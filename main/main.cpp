/**
 * FB2 Reader — ESP-IDF entry point for M5 Paper S3
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "platform.h"
#include "scene_manager.h"
#include "splash_scene.h"
#include "bookshelf_scene.h"
#include "browser_scene.h"
#include "book_scene.h"
#include "gestures.h"
#include "settings_scene.h"
#include "root_scene.h"
#include "power_off_scene.h"
#include "font_renderer.h"
#include "config.h"
#include "clock.h"
#include "layout_cache.h"
#include "history.h"
#include "ui_overlay.h"
#include "filesys/filesystem.h"

static const char* TAG = "fbr";

// While a touch event is within this window, keep polling inputs at a tight
// rate so gesture timing (double-tap window, swipe max duration) stays
// accurate. Only after this much idle time does the loop enter light sleep.
static constexpr uint32_t kTouchActiveIdleMs = 1200;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "FB2 Reader starting on M5 Paper S3");
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // ── NVS init ──
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── Platform init (display + touch) ──
    Platform::init();
    if (!Platform::instance()) {
        ESP_LOGE(TAG, "Platform init failed");
        return;
    }

    ESP_LOGI(TAG, "Display: %dx%d", Platform::instance()->width(), Platform::instance()->height());

    // ── Mount SD card ──
    fs::FileSystem* fs = fs::FileSystem::mount_any();
    if (fs) {
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "No SD card found — running without filesystem");
    }

    cfg::init(fs);
    LayoutCache::set_fs(fs);
    History::set_fs(fs);
    clock_init();

    // ── Font loading ──
    FontRenderer ui_font;
    FontRenderer reader_font;
    bool font_ok = false;
    bool ui_font_ok = false;

    // UI font, static for all UI scenes. 540-line 4.7" screen: 26px keeps
    // labels readable and drives 50px+ touch targets.
    constexpr int kUiFontSize = 26;

    if (fs) {
        fs::File* uf = fs->open(".fonts/arial.ttf");
        if (!uf) uf = fs->open(".fonts/Arial.ttf");
        if (uf) {
            size_t sz = uf->size();
            if (sz > 0) {
                unsigned char* buf = (unsigned char*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buf) buf = (unsigned char*)malloc(sz);
                if (buf) {
                    uf->read(buf, sz);
                    ui_font_ok = ui_font.load_font_from_memory(buf, sz, "", kUiFontSize);
                    if (!ui_font_ok) free(buf);
                }
            }
            delete uf;
        }
    }
    // Fallback: if no Arial, use the first available font for UI
    if (!ui_font_ok && fs) {
        fs::DirEntry entries[16];
        size_t n = fs->read_dir(".fonts", entries, 16);
        for (size_t i = 0; i < n; ++i) {
            if (entries[i].is_dir) continue;
            const char* name = entries[i].name;
            size_t len = std::strlen(name);
            if (len > 4 && (std::strcmp(name + len - 4, ".ttf") == 0 ||
                            std::strcmp(name + len - 4, ".TTF") == 0)) {
                std::string path = std::string(".fonts/") + name;
                fs::File* f = fs->open(path.c_str());
                if (f) {
                    size_t sz = f->size();
                    if (sz > 0) {
                        unsigned char* buf = (unsigned char*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!buf) buf = (unsigned char*)malloc(sz);
                        if (buf) {
                            f->read(buf, sz);
                            ui_font_ok = ui_font.load_font_from_memory(buf, sz, "", kUiFontSize);
                            if (!ui_font_ok) free(buf);
                        }
                    }
                    delete f;
                }
                if (ui_font_ok) break;
            }
        }
    }

    // Load reader font (user-configurable from INI)
    if (fs) {
        std::string font_file = cfg::get_str("font_file");
        std::string path = std::string(".fonts/") + font_file;
        fs::File* f = fs->open(path.c_str());
        if (!f) {
            f = fs->open(".fonts/arial.ttf");
            if (!f) f = fs->open(".fonts/Arial.ttf");
        }
        if (f) {
            size_t sz = f->size();
            if (sz > 0) {
                unsigned char* buf = (unsigned char*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buf) buf = (unsigned char*)malloc(sz);
                if (buf) {
                    f->read(buf, sz);
                    int user_fs = cfg::get_int("font_size");
                    font_ok = reader_font.load_font_from_memory(buf, sz, "", user_fs);
                    if (!font_ok) free(buf);
                }
            }
            delete f;
        }
    }

    ESP_LOGI(TAG, "stack high water: %lu bytes", (unsigned long)uxTaskGetStackHighWaterMark(NULL));

    if (!font_ok)
        ESP_LOGE(TAG, "Failed to load reader font");

    // ── Экран загрузки ────────────────────────────────────────
    // До цикла сцен: пока декодируется логотип выключения, видно только
    // это. Шрифт интерфейса предпочтительнее книжного — его размер не
    // зависит от настройки читателя. present() в epd_text асинхронный,
    // поэтому ждём отрисовки: иначе тяжёлый warmup успевает стартовать,
    // пока на стекле ещё пусто.
    {
        int w = Platform::instance()->width();
        int h = Platform::instance()->height();
        uint8_t* fb = Platform::instance()->framebuffer();
        draw_loading_screen(fb, w, h,
                            ui_font_ok ? &ui_font : (font_ok ? &reader_font : nullptr));
        Platform::instance()->present();
        Platform::instance()->wait_display();
    }
    const uint32_t loading_at = Platform::instance()->tick_ms();

    // Pre-decode the power-off logo while heap is still large (~4MB) — a
    // double hardware-button press can follow heavy reading when only
    // ~0.5MB is free and on-demand decode would fail.
    ESP_LOGI(TAG, "warmup: before logo heap=%lu", (unsigned long)esp_get_free_heap_size());
    PowerOffScene::warmup();
    ESP_LOGI(TAG, "warmup: after logo heap=%lu", (unsigned long)esp_get_free_heap_size());

    // Экран загрузки не должен мелькать: если декодирование уложилось в
    // меньше секунды, досчитываем остаток перед появлением полки.
    {
        const uint32_t hold = loading_hold_ms(loading_at, Platform::instance()->tick_ms());
        if (hold) {
            ESP_LOGI(TAG, "loading screen: hold %u ms", (unsigned)hold);
            Platform::instance()->delay_ms(hold);
        }
    }

    // ── Scene manager ──
    SceneManager mgr;
    mgr.set_font(&reader_font);
    mgr.set_ui_font(&ui_font);
    mgr.set_fs(fs);
    ui_overlay_set_font(&ui_font);

    mgr.start(new RootScene);
    mgr.push(new SplashScene(&ui_font));
    if (fs)
        mgr.push(new BookshelfScene(&ui_font, fs));
    else
        mgr.push(new NoFsScene(&ui_font));

    ESP_LOGI(TAG, "Entering main loop");

    // ── Main loop ──
    uint32_t diag_start = Platform::instance()->tick_ms();
    uint32_t diag_renders = 0;
    uint32_t diag_touches = 0;
    uint32_t diag_presents = 0;
    while (!mgr.quit_requested()) {

        uint32_t diag_now = Platform::instance()->tick_ms();
        if (diag_now - diag_start >= 3000) {
            ESP_LOGI(TAG, "diag: 3s: touches=%lu renders=%lu presents=%lu",
                     (unsigned long)diag_touches, (unsigned long)diag_renders,
                     (unsigned long)diag_presents);
            diag_start = diag_now;
            diag_renders = 0;
            diag_touches = 0;
            diag_presents = 0;
        }
        // Multifunction button (via PMS150G): single click → soft reset,
        // double click → Power Off scene.
        if (mgr.top() && std::strcmp(mgr.top()->name(), "poweroff") != 0 &&
            Platform::instance()->power_button_single_clicked()) {
            ESP_LOGI(TAG, "Button: single click → reset requested");
            esp_restart();
        }
        if (mgr.top() && std::strcmp(mgr.top()->name(), "poweroff") != 0 &&
            Platform::instance()->power_button_double_clicked()) {
            ESP_LOGI(TAG, "Button: double click → power off scene");
            mgr.push(new PowerOffScene);
        }

        // Touch input → mouse events
        int tx, ty;
        if (Platform::instance()->get_touch(tx, ty)) {
            diag_touches++;
            mgr.on_mouse_down(tx, ty, 1);
        }

        // Swipe detection → page turn / menu. Разбор жеста общий с
        // симулятором: копия в src/sims3/main.cpp уже расходилась с этой.
        int dx, dy, sx, sy;
        if (Platform::instance()->get_swipe(dx, dy, sx, sy)) {
            dispatch_swipe(mgr, dx, dy, sx, sy, &ui_font, fs);
        }

        mgr.update();

        if (mgr.needs_render()) {
            diag_renders++;
            uint8_t* fb = Platform::instance()->framebuffer();
            int w = Platform::instance()->width();
            int h = Platform::instance()->height();
            mgr.render(fb, w, h);
            Platform::instance()->present();
            diag_presents++;
        } else {
            // Idle frame on bistable e-paper.
            // epd_text present() is async — the IT8951 finishes writing the
            // frame in the background. Keep the panel write going before we
            // power down into light sleep, or the frame stays half-drawn on
            // screen until the next touch forces a re-render.
            Platform::instance()->wait_display();

            // Adaptive idle: gestures (double-tap 700ms window, swipe max
            // 300ms) need a tight touch sample rate. The GT911 caches
            // coordinates while a finger is held, so a 150ms poll misses
            // finger movement between samples and the swipe delta never
            // accumulates. While the user is interacting, keep polling fast;
            // only light-sleep once the screen has been idle for a while.
            if (Platform::instance()->touch_recent(kTouchActiveIdleMs)) {
                Platform::instance()->delay_ms(20);
            } else {
                Platform::instance()->light_sleep(150);
            }
        }

        // Настройки и состояние пишутся в SD не чаще раза на кадр и только
        // если что-то изменилось: сеттеры лишь меняют значения в памяти.
        // Раньше каждое изменение перезаписывало файл целиком, а всплески
        // тока на SD приводили к brownout.
        cfg::flush();
    }

    // ── Cleanup ──
    ESP_LOGI(TAG, "Shutting down");
    cfg::flush();
    delete fs;
    Platform::instance()->deep_sleep();
}

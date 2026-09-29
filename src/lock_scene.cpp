#include "lock_scene.h"
#include "scene_manager.h"
#include "font_renderer.h"
#include "palette.h"
#include "fb_helpers.h"
#include "platform.h"
#include "power_off_scene.h"
#include "config.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(...) ((void)0)
#endif
#include <cstdlib>
#include <cstdio>
#include <algorithm>

namespace {
    const char* const TAG = "lock";

    // Плашка паузы. Рост взят под три строки текста.
    constexpr int kPlaqueW = 400;
    constexpr int kPlaqueH = 170;
    constexpr int kPlaqueLines = 3;
    constexpr int kPlaquePitch = 40;   // шаг между строками

    // Русский текст в исходнике храним escape-последовательностями UTF-8:
    // так файл остаётся чистым ASCII и одинаково читается и gcc, и MSVC.
    const char* const kAutoOffPrefix =
        "\xD0\x90\xD0\xB2\xD1\x82\xD0\xBE\xD0\xB2\xD1\x8B\xD0\xBA\xD0\xBB\xD1\x8E"
        "\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5: ";      // Автовыключение:
    const char* const kAutoOffMin = "\xD0\xBC\xD0\xB8\xD0\xBD";  // мин
    const char* const kAutoOffOff = "\xD0\xB2\xD1\x8B\xD0\xBA\xD0\xBB";  // выкл
}

LockScene::LockScene(FontRenderer* font) : font_(font) {}

bool LockScene::auto_off_due(uint32_t elapsed_ms, int minutes) {
    if (minutes <= 0) return false;                 // 0 = выключено
    return elapsed_ms >= (uint32_t)minutes * 60u * 1000u;
}

void LockScene::on_enter() {
    // Читаем настройку при входе, а не при создании: пользователь мог
    // поменять её, пока сцена уже лежала в стеке.
    auto_off_min_ = cfg::get_int("auto_off_min");
    enter_ms_ = Platform::instance()->tick_ms();
    fired_ = false;
    ESP_LOGI(TAG, "pause: auto-off=%d min, enter_ms=%u", auto_off_min_, enter_ms_);
}

void LockScene::update() {
    if (fired_ || auto_off_min_ <= 0) return;

    const uint32_t now = Platform::instance()->tick_ms();
    if (!auto_off_due(now - enter_ms_, auto_off_min_)) return;

    // Один раз: push() не удаляет текущую сцену, но следующий update() уже
    // достанется PowerOffScene, и повторный пуш продублировал бы выключение.
    fired_ = true;
    ESP_LOGI(TAG, "pause: %d min elapsed, powering off", auto_off_min_);
    mgr_->push(new PowerOffScene);
    // Дальше поля не трогаем: после push сцена остаётся живой, но полагаться
    // на это не стоит.
}

void LockScene::plaque_rect(int fb_w, int fb_h, int* x, int* y, int* w, int* h) const {
    *w = kPlaqueW;
    *h = kPlaqueH;
    *x = (fb_w - *w) / 2;
    *y = (fb_h - *h) / 2;
}

void LockScene::render(uint8_t* fb, int fb_w, int fb_h) {
    for (int i = 0; i < fb_w * fb_h; ++i) {
        fb[i] = fb_dim(fb[i], 8);
    }

    int dx = 0, dy = 0, dw = kPlaqueW, dh = kPlaqueH;
    plaque_rect(fb_w, fb_h, &dx, &dy, &dw, &dh);

    fb_fill_rect(fb, fb_w, dx, dy, dx + dw - 1, dy + dh - 1, kCream);

    for (int x = dx; x < dx + dw; ++x) {
        if (x >= 0 && x < fb_w) {
            fb[dy * fb_w + x] = kBrown;
            fb[(dy + dh - 1) * fb_w + x] = kBrown;
        }
    }
    for (int y = dy; y < dy + dh; ++y) {
        if (y >= 0 && y < fb_h) {
            fb[y * fb_w + dx] = kBrown;
            fb[y * fb_w + dx + dw - 1] = kBrown;
        }
    }

    // Строки центрируем и по горизонтали, и по вертикали: draw_text()
    // трактует y как базовую линию, поэтому базовая считается от центра
    // строки через метрики шрифта, а не «минус половина line_height».
    const int block_h = kPlaqueLines * kPlaquePitch;
    const int block_top = dy + (dh - block_h) / 2;
    auto line = [&](int idx, const char* text, uint8_t color) {
        const int tw = font_->text_width(text);
        font_->draw_text(fb, fb_w, fb_h, dx + (dw - tw) / 2,
                         font_->baseline_for_center(block_top + idx * kPlaquePitch,
                                                    kPlaquePitch),
                         text, color);
    };

    line(0, "PAUSED", kMidDark);
    line(1, "Double-click to resume", kGray);

    // Отсчёта на экране нет намеренно: каждое обновление e-ink стоит
    // почти секунду и оставляет призраки. Показываем, сколько ждать.
    char hint[96];
    if (auto_off_min_ > 0)
        std::snprintf(hint, sizeof(hint), "%s%d %s",
                      kAutoOffPrefix, auto_off_min_, kAutoOffMin);
    else
        std::snprintf(hint, sizeof(hint), "%s%s", kAutoOffPrefix, kAutoOffOff);
    line(2, hint, kBrown);
}

void LockScene::on_mouse_down(int x, int y, int button) {
    if (button != 1) return;

    uint32_t now = Platform::instance()->tick_ms();
    // 700ms — как в SceneManager::kDoubleTapMs: первый клик не делает
    // present (см. scene_manager), окно должно переживать медленный e-ink.
    if (last_click_ != 0 && now - last_click_ < 700 &&
        std::abs(x - last_x_) < 20 && std::abs(y - last_y_) < 20) {
        int fb_w = Platform::instance()->width();
        int fb_h = Platform::instance()->height();
        int dx = 0, dy = 0, dw = 0, dh = 0;
        plaque_rect(fb_w, fb_h, &dx, &dy, &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy && y < dy + dh) {
            mgr_->go_back();   // pop() удаляет this — после этого нельзя трогать поля
            return;
        }
        last_click_ = 0;
        return;
    }
    last_click_ = now;
    last_x_ = x;
    last_y_ = y;
}

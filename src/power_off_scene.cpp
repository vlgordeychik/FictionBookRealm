#include "power_off_scene.h"
#include "scene_manager.h"
#include "platform.h"
#include "palette.h"
#include "config.h"
#include "swofflogo_jpg.h"

#include <cstring>
#include <cstdlib>

#if !defined(ESP_PLATFORM)
#include "stb_image.h"
#endif

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "rom/tjpgd.h"
#define POWER_LOGI(fmt, ...) ESP_LOGI("power", fmt, ##__VA_ARGS__)
#define POWER_HEAP() ((unsigned)esp_get_free_heap_size())
#else
#define POWER_LOGI(fmt, ...) printf("fbr: " fmt "\n", ##__VA_ARGS__)
#define POWER_HEAP() 0u
#endif

// Real power-off is enabled. The PMS150G cuts the supply from Platform::power_off();
// set to 1 to keep the logo on screen and skip it again while debugging.
#define DEBUG_SKIP_POWER_OFF 0

// ─── Decoded image buffer helpers ───────────────────────────────
// The decoded frame is ~0.5 MB; on ESP32 place it in PSRAM.
static uint8_t* alloc_img(size_t n) {
#if defined(ESP_PLATFORM)
    uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_8BIT);
    if (p) return p;
    return (uint8_t*)malloc(n);
#else
    return (uint8_t*)malloc(n);
#endif
}

static void free_img(uint8_t* p) {
#if defined(ESP_PLATFORM)
    if (p) heap_caps_free(p);
#else
    free(p);
#endif
}

// ─── Pre-decoded logo cache ─────────────────────────────────────
// Power-off can happen right after heavy reading when internal heap is
// ~0.5MB — decoding 540x960 on demand (2× ~0.5MB stbi buffers) then
// fails and the logo never shows. Decode once at boot in PSRAM instead.
static uint8_t* g_power_logo = nullptr;
static int      g_power_logo_w = 0;
static int      g_power_logo_h = 0;

// ─── ESP32: streaming TJpgDec decode (no stb) ──────────────────
// swofflogo.jpg is baseline now (see assets/swofflogo.jpg), so ROM
// tjpgd can stream it MCU-by-MCU straight into the rotated 960x540
// palette buffer — no full-size intermediate ever exists.
#if defined(ESP_PLATFORM)

namespace {

struct LogoJpgCtx {
    const uint8_t* src;
    size_t         src_n;
    size_t         src_pos;
    uint8_t*       dst;      // 960x540 palette indices, rotated 90° CW
    int            out_w;    // = src height (960)
    int            out_h;    // = src width  (540)
    int            src_w;    // 540
    int            src_h;    // 960
};

static UINT logo_jpg_in(JDEC* jd, BYTE* buf, UINT len) {
    auto* s = static_cast<LogoJpgCtx*>(jd->device);
    if (!s || s->src_pos >= s->src_n) return 0;
    size_t rem = s->src_n - s->src_pos;
    if (len > rem) len = static_cast<UINT>(rem);
    if (buf) std::memcpy(buf, s->src + s->src_pos, len);
    s->src_pos += len;
    return len;
}

// MCU block → rotated palette indices. No intermediate RGB buffer.
static UINT logo_jpg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* s = static_cast<LogoJpgCtx*>(jd->device);
    if (!s || !s->dst || !s->out_w || !s->out_h) return 0;
    const auto* rgb = static_cast<const uint8_t*>(bitmap);
    const int w = rect->right - rect->left + 1;
    const int h = rect->bottom - rect->top + 1;
    for (int y = 0; y < h; ++y) {
        const int sy = rect->top + y;               // source row
        for (int x = 0; x < w; ++x) {
            const int sx = rect->left + x;          // source col
            const uint8_t* p = rgb + (size_t)(y * w + x) * 3;
            const uint32_t lum =
                ((uint32_t)p[0] * 77 + (uint32_t)p[1] * 151 +
                 (uint32_t)p[2] * 29) >> 8;
            // Rotate 90° CW: out(Y=sx, X=h-1-sy) = src(sx, sy).
            const int X = s->src_h - 1 - sy;
            const int Y = sx;
            if (X >= 0 && X < s->out_w && Y >= 0 && Y < s->out_h)
                s->dst[(size_t)Y * s->out_w + X] =
                    (uint8_t)((lum + 8) / 17);
        }
    }
    return 1;
}

static bool decode_logo_tjpgd(uint8_t* out, int out_w, int out_h,
                              int src_w, int src_h) {
    if (!out || out_w <= 0 || out_h <= 0) return false;

    LogoJpgCtx ctx{};
    ctx.src = swofflogo_jpg;
    ctx.src_n = swofflogo_jpg_len;
    ctx.src_pos = 0;
    ctx.dst = out;
    ctx.out_w = out_w;
    ctx.out_h = out_h;
    ctx.src_w = src_w;
    ctx.src_h = src_h;

    constexpr UINT kPool = 4096;
    JDEC jd;
    void* pool = std::malloc(kPool);
    if (!pool) {
        POWER_LOGI("logo pool alloc FAIL");
        return false;
    }
    POWER_LOGI("logo jd_prepare enter src=%u", (unsigned)ctx.src_n);
    JRESULT jr = jd_prepare(&jd, logo_jpg_in, pool, kPool, &ctx);
    if (jr != JDR_OK) {
        std::free(pool);
        POWER_LOGI("logo prepare jr=%d w=%u h=%u heap=%u",
                   (int)jr, (unsigned)jd.width, (unsigned)jd.height,
                   (unsigned)esp_get_free_heap_size());
        return false;
    }
    POWER_LOGI("logo jd_prepare ok %ux%u, decomp enter", (unsigned)jd.width, (unsigned)jd.height);
    jr = jd_decomp(&jd, logo_jpg_out, 0);
    POWER_LOGI("logo jd_decomp done jr=%d", (int)jr);
    std::free(pool);
    if (jr != JDR_OK && jr != JDR_INTR) {
        POWER_LOGI("logo decomp jr=%d", (int)jr);
        return false;
    }
    return true;
}

} // namespace

#endif // ESP_PLATFORM

// Decode swofflogo.jpg (baseline, 540x960) → rotated 960x540 palette
// indices. Called once at boot when heap is large; reused by every
// PowerOffScene instance.
static void decode_logo_once() {
    if (g_power_logo) {
        POWER_LOGI("logo already cached %dx%d", g_power_logo_w, g_power_logo_h);
        return;
    }

    const int src_w = 540;
    const int src_h = 960;
    int ow = src_h;  // output width  = source height (960)
    int oh = src_w;  // output height = source width  (540)

    uint8_t* out = alloc_img((size_t)ow * oh);
    if (!out) {
        POWER_LOGI("logo alloc FAIL %dx%d heap=%u",
                   ow, oh, POWER_HEAP());
        return;
    }

#if defined(ESP_PLATFORM)
    if (!decode_logo_tjpgd(out, ow, oh, src_w, src_h)) {
        free_img(out);
        POWER_LOGI("logo tjpgd FAIL w=%d h=%d heap=%u",
                   src_w, src_h, POWER_HEAP());
        return;
    }
#else
    int w = 0, h = 0, ch = 0;
    unsigned char* src = stbi_load_from_memory(
        swofflogo_jpg, (int)swofflogo_jpg_len, &w, &h, &ch, 1);
    if (!src || w <= 0 || h <= 0 || w != src_w || h != src_h) {
        stbi_image_free(src);
        free_img(out);
        return;
    }
    for (int X = 0; X < ow; ++X) {
        int sy = src_h - 1 - X;           // source row
        const unsigned char* srow = src + (size_t)sy * src_w;
        for (int Y = 0; Y < oh; ++Y) {
            unsigned char lum = srow[Y];
            out[(size_t)Y * ow + X] = (uint8_t)((lum + 8) / 17);
        }
    }
    stbi_image_free(src);
#endif

    g_power_logo = out;
    g_power_logo_w = ow;
    g_power_logo_h = oh;
    POWER_LOGI("logo decoded %dx%d heap=%u",
               ow, oh, POWER_HEAP());
}

// ─── Decoding ───────────────────────────────────────────────────
bool PowerOffScene::decode() {
    if (decoded_) return true;

    if (!g_power_logo) decode_logo_once();
    if (!g_power_logo) return false;

    img_ = g_power_logo;
    img_w_ = g_power_logo_w;
    img_h_ = g_power_logo_h;
    cached_ = true;
    decoded_ = true;
    return true;
}

void PowerOffScene::warmup() {
    decode_logo_once();
}

void PowerOffScene::free_image() {
    if (img_ && !cached_) free_img(img_);
    img_ = nullptr;
    img_w_ = 0;
    img_h_ = 0;
    decoded_ = false;
}

// ─── Scene lifecycle ─────────────────────────────────────────────
PowerOffScene::~PowerOffScene() {
    free_image();
}

void PowerOffScene::on_enter() {
    decode();
    // Full-screen logo needs a clearing (quality) refresh, not a partial
    // epd_text update — otherwise the old page ghosts under the logo.
    if (Platform::instance())
        Platform::instance()->force_quality_next_present();
}

void PowerOffScene::on_exit() {
    free_image();
    time_base_set_ = false;
    powering_off_ = false;
    presented_ = false;
}

void PowerOffScene::update() {
    if (powering_off_) return;

    uint32_t now = Platform::instance()->tick_ms();

    // Start the countdown only after the logo frame has actually been
    // presented — update() runs before render/present in the main loop,
    // so a slow decode/first frame must not eat into the EPD refresh time.
    if (!presented_) return;

    if (!time_base_set_) {
        time_base_set_ = true;
        enter_ms_ = now;
#ifdef ESP_PLATFORM
        printf("fbr: poweroff countdown start heap=%u\n",
               (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
    }
    // Let the EPD finish (re)drawing the logo before cutting power.
    if (now - enter_ms_ >= (uint32_t)kOffDelayMs) {
        powering_off_ = true;
#if defined(ESP_PLATFORM)
        printf("fbr: poweroff firing heap=%u\n",
               (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
        Platform::instance()->wait_display();
        // Последний шанс записать состояние: после обрыва питания
        // несохранённая страница потеряется. Настройки/состояние пишутся
        // лениво, поэтому без этого flush() они не успеют попасть на SD.
        cfg::flush();
#if DEBUG_SKIP_POWER_OFF
        // Debug: stay awake so the splash + logs can be checked.
        printf("fbr: DEBUG_SKIP_POWER_OFF — not powering off\n");
        fflush(stdout);
        powering_off_ = false; // allow repeated inspection (no state change)
#else
        Platform::instance()->power_off();
#endif
    }
}

void PowerOffScene::on_mouse_down(int, int, int) {
    mgr_->go_back();        // win32: dismiss; ESP never receives touches here
}

void PowerOffScene::on_key_down(int) {
    mgr_->go_back();
}

void PowerOffScene::render(uint8_t* fb, int fb_w, int fb_h) {
    if (!decode()) {
        // No logo available — show a plain dark cover instead.
        std::memset(fb, kDark2, (size_t)fb_w * fb_h);
        presented_ = true; // still a frame; start the off countdown
#ifdef ESP_PLATFORM
        printf("fbr: poweroff render NO LOGO %dx%d heap=%u\n",
               fb_w, fb_h, (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
        return;
    }

    int x0 = (fb_w - img_w_) / 2;
    int y0 = (fb_h - img_h_) / 2;
    int y1 = y0 + img_h_ - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (y1 >= fb_h) y1 = fb_h - 1;

    int row_w = img_w_;
    int skip_src_x = 0;
    if (row_w > fb_w - x0) row_w = fb_w - x0;

    for (int y = y0; y <= y1; ++y) {
        std::memcpy(fb + (size_t)y * fb_w + x0,
                    img_ + (size_t)y * img_w_ + skip_src_x, (size_t)row_w);
    }

    presented_ = true;
#ifdef ESP_PLATFORM
    printf("fbr: poweroff render LOGO %dx%d heap=%u\n",
           img_w_, img_h_, (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif
}
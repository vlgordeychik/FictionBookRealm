#include "platform.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "soc/rtc.h"
#include "soc/adc_channel.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define LGFX_USE_V1
#include "M5GFX.h"
#include <M5Unified.h>

#include "palette.h"
#include "bm8563.h"

static const char* TAG = "platform";

// ─── Часы BM8563 ────────────────────────────────────────────────
// Чип сидит на той же шине I2C, что и тач GT911: SDA 41, SCL 42,
// порт I2C_NUM_1. Отдельный I2C-перифетал на эти пины запускать нельзя,
// но lgfx::i2c::init с уже поднятой шиной и теми же пинами — no-op
// (ранний return в setPins), так что шина тача не страдает.
static constexpr int kRtcI2cPort = 1;
static constexpr int kRtcSda     = 41;
static constexpr int kRtcScl     = 42;

// HAL для драйвера: две транзакции, адрес и регистр задаёт сам драйвер.
// ВАЖНО: драйвер сравнивает результат с BM8563_OK, а это 0, и трактует
// ненулевое значение как ошибку. Поэтому успех — это ровно 0, а не длина.
static int32_t rtc_read(void*, uint8_t address, uint8_t reg,
                        uint8_t* buffer, uint16_t size) {
    auto r = lgfx::i2c::transactionWriteRead(kRtcI2cPort, address, &reg, 1, buffer, size);
    return r.has_value() ? BM8563_OK : -1;
}

static int32_t rtc_write(void*, uint8_t address, uint8_t reg,
                         const uint8_t* buffer, uint16_t size) {
    uint8_t tmp[1 + 16];
    if (size > sizeof(tmp) - 1) return -1;
    tmp[0] = reg;
    std::memcpy(tmp + 1, buffer, size);
    auto r = lgfx::i2c::transactionWrite(kRtcI2cPort, address, tmp, (uint8_t)(size + 1));
    return r.has_value() ? BM8563_OK : -1;
}

// ─── Hardware pins (Paper S3) ──────────────────────────────────
// Multifunction button on Paper S3 goes through the PMS150G power
// controller: OFF = ESP GPIO44 → PMS150G, and the button sense line
// (PMS150G "G0") arrives at PIN_BTN_PRESS (verify on hardware).
static constexpr gpio_num_t PIN_BTN_PRESS  = GPIO_NUM_0;
static constexpr gpio_num_t PIN_PWR_OFF    = GPIO_NUM_44;
static constexpr gpio_num_t PIN_TOUCH_INT  = GPIO_NUM_48;

// ─── Display dimensions ────────────────────────────────────────
static constexpr int DISP_W = 960;
static constexpr int DISP_H = 540;

// ─── Swipe detection thresholds ────────────────────────────────
static constexpr int SWIPE_THRESHOLD     = 50;   // min px to count as swipe
static constexpr int SWIPE_MAX_TIME_MS   = 300;  // max duration of swipe gesture
static constexpr int TOUCH_DEBOUNCE_MS   = 80;   // debounce between taps

// ─── Power button thresholds (PMS150G G0) ──────────────────────
static constexpr bool     BTN_PRESSED_LEVEL = false; // pressed = LOW (verify)
static constexpr uint32_t BTN_DEBOUNCE_MS   = 20;    // debounce
static constexpr uint32_t BTN_MIN_PRESS_MS  = 30;    // min hold to count as a click
static constexpr uint32_t BTN_DOUBLE_MIN_MS = 40;    // min gap between the two clicks
static constexpr uint32_t BTN_DOUBLE_MAX_MS = 400;   // max gap to be a double-click

// ─── Palette for EPD push ──────────────────────────────────────
// Maps palette index → rgb888_t color (R=G=B=gray_value).
// M5GFX converts this to grayscale_8bit internally.
static lgfx::rgb888_t s_palette[256];

static void init_palette() {
    for (int i = 0; i < 256; ++i) {
        uint8_t gray = (i < kPaletteSize) ? kPaletteValues[i] : 0;
        s_palette[i] = lgfx::rgb888_t{gray, gray, gray};
    }
}

// ─── Concrete platform implementation ──────────────────────────
class Platform_PaperS3 : public Platform {
public:
    bool init() {
        // ── Power button input (multifunction button, PMS150G G0) ──
        gpio_reset_pin(PIN_BTN_PRESS);
        gpio_set_direction(PIN_BTN_PRESS, GPIO_MODE_INPUT);
        gpio_set_pull_mode(PIN_BTN_PRESS, GPIO_PULLUP_ONLY);

        // ── Power off pin ──
        gpio_reset_pin(PIN_PWR_OFF);
        gpio_set_direction(PIN_PWR_OFF, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_PWR_OFF, 0);

        // ── Init M5GFX (autodetects EPD + touch) ──
        gfx_.init();
        gfx_.setRotation(3); // landscape: 960x540
        gfx_.setEpdMode(epd_mode_t::epd_text); // fast partial update for text
        gfx_.fillScreen(TFT_WHITE);
        gfx_.display();
        gfx_.waitDisplay();

        // ── Battery ADC (PaperS3: GPIO3 / ADC1 ch2, divider ratio 2.0) ──
        // M5.Power.begin() alone leaves M5.getBoard()==unknown, so
        // Power_Class never enables the PaperS3 pmic_adc path → level -2.
        init_battery_adc();
        {
            int mv = read_battery_mv();
            int bl = battery_level_from_mv(mv);
            ESP_LOGI(TAG, "Battery after ADC init: mv=%d level=%d cali=%d",
                     mv, bl, batt_cali_ok_ ? 1 : 0);
        }

        // Keep M5.Power for board power-off pinmap (no-op for unknown board).
        M5.Power.begin();

        ESP_LOGI(TAG, "Display: %dx%d, board=%d", gfx_.width(), gfx_.height(), gfx_.getBoard());

        // ── Init palette ──
        init_palette();

        // ── Allocate palette-indexed framebuffer (in PSRAM) ──
        fb_ = (uint8_t*)heap_caps_malloc(DISP_W * DISP_H, MALLOC_CAP_SPIRAM);
        if (!fb_) {
            ESP_LOGE(TAG, "Failed to allocate framebuffer (%d bytes)", DISP_W * DISP_H);
            return false;
        }
        std::memset(fb_, kWhite, DISP_W * DISP_H); // fill white
        ESP_LOGI(TAG, "Framebuffer: %d bytes at %p", DISP_W * DISP_H, fb_);

        // ── Touch state init ──
        touch_active_ = false;
        touch_reported_ = false;
        touch_x_ = 0;
        touch_y_ = 0;
        swipe_dx_ = 0;
        swipe_dy_ = 0;
        swipe_pending_ = false;
        last_touch_time_ = 0;
        last_tap_time_ = 0;
        last_activity_ms_ = 0;

        btn_db_lvl_ = gpio_get_level(PIN_BTN_PRESS) == (BTN_PRESSED_LEVEL ? 1 : 0);
        btn_db_since_ = tick_ms();
        btn_prev_pressed_ = false;
        btn_press_stamp_ = 0;
        btn_press_locked_ = false;
        btn_last_release_ = 0;
        btn_single_pending_ = 0;
        btn_action_lock_until_ = 0;

        return true;
    }

    // ─── Display ──────────────────────────────────────────────
    int width() const override { return DISP_W; }
    int height() const override { return DISP_H; }

    uint8_t* framebuffer() override { return fb_; }

    void force_quality_next_present() override {
        force_quality_ = true;
    }

    void present() override {
        if (!fb_) return;

        // Push palette-indexed buffer to EPD.
        // M5GFX reads each byte as palette index, looks up rgb888_t color,
        // converts to grayscale_8bit, then Panel_EPD dithers 8→4 bit.
        const bool quality = force_quality_;
        if (quality) {
            gfx_.waitDisplay();
            gfx_.setEpdMode(epd_mode_t::epd_quality);
            force_quality_ = false;
        }
        gfx_.pushImage(0, 0, DISP_W, DISP_H, fb_,
                       lgfx::color_depth_t::palette_8bit, s_palette);
        gfx_.display();
        if (quality) {
            gfx_.waitDisplay();
            gfx_.setEpdMode(epd_mode_t::epd_text);
        }
    }

    void present_area(int x, int y, int w, int h) override {
        if (!fb_) return;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0) return;
        if (x + w > DISP_W) w = DISP_W - x;
        if (y + h > DISP_H) h = DISP_H - y;
        if (w <= 0 || h <= 0) return;

        // Частичное обновление IT8951: pushImage для palette_8bit ожидает
        // плотный w×h буфер, а fb_ имеет шаг DISP_W — копируем строки по одной.
        gfx_.waitDisplay();
        for (int row = 0; row < h; ++row) {
            gfx_.pushImage(x, y + row, w, 1,
                           &fb_[(y + row) * DISP_W + x],
                           lgfx::color_depth_t::palette_8bit, s_palette);
        }
        gfx_.display(x, y, w, h);
        gfx_.waitDisplay();
    }

    void wait_display() override {
        gfx_.waitDisplay();
    }

    // ─── Touch input ──────────────────────────────────────────
    bool get_touch(int& x, int& y) override {
        lgfx::touch_point_t tp;
        uint8_t count = gfx_.getTouch(&tp);

        if (count > 0) {
            // M5GFX returns touch coords already mapped to display rotation
            uint32_t now = tick_ms();
            last_activity_ms_ = now;

            if (!touch_active_) {
                // Touch start — report exactly once per press (press-edge),
                // otherwise a held finger floods on_mouse_down and the
                // full e-ink present() between frames loses the 2nd tap.
                touch_active_ = true;
                touch_reported_ = false;
                touch_start_x_ = tp.x;
                touch_start_y_ = tp.y;
                touch_start_time_ = now;
            }

            touch_x_ = tp.x;
            touch_y_ = tp.y;

            // Debounce: ignore a new press too close to the last completed tap
            if (!touch_reported_ && now - last_tap_time_ < TOUCH_DEBOUNCE_MS)
                return false;

            if (touch_reported_)
                return false;

            touch_reported_ = true;
            x = touch_x_;
            y = touch_y_;
            return true;
        }

        if (touch_active_) {
            // Touch released — check for swipe
            uint32_t now = tick_ms();
            last_activity_ms_ = now;
            int dx = touch_x_ - touch_start_x_;
            int dy = touch_y_ - touch_start_y_;
            uint32_t elapsed = now - touch_start_time_;

            if (elapsed < SWIPE_MAX_TIME_MS) {
                if (std::abs(dx) > SWIPE_THRESHOLD || std::abs(dy) > SWIPE_THRESHOLD) {
                    swipe_dx_ = dx;
                    swipe_dy_ = dy;
                    swipe_pending_ = true;
                }
            }

            // If no swipe and quick tap — it's a tap
            if (!swipe_pending_ && elapsed < 200) {
                last_tap_time_ = now;
            }

            touch_active_ = false;
            touch_reported_ = false;
        }

        return false;
    }

    bool get_swipe(int& dx, int& dy, int& sx, int& sy) override {
        if (!swipe_pending_) return false;
        dx = swipe_dx_;
        dy = swipe_dy_;
        sx = touch_start_x_;
        sy = touch_start_y_;
        swipe_pending_ = false;
        swipe_dx_ = 0;
        swipe_dy_ = 0;
        last_activity_ms_ = tick_ms();
        return true;
    }

    bool touch_recent(uint32_t window_ms) const override {
        return tick_ms() - last_activity_ms_ < window_ms;
    }

    // ─── Timing ───────────────────────────────────────────────
    uint32_t tick_ms() const override {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void delay_ms(uint32_t ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    // ─── Power ────────────────────────────────────────────────
    void light_sleep(uint32_t timeout_ms) override {
        // Light sleep, timer-only wake. The e-paper holds the frame
        // (bistable), so nothing needs redrawing after wake — the main loop
        // just re-polls touch/power button. Timer-only avoids level-triggered
        // GPIO wake (a held finger would busy-loop waking) and sidesteps the
        // ext0 RTC-GPIO restriction (touch INT is GPIO48, not an RTC pin).
        // NOTE: USB-Serial/JTAG console goes silent after the first light
        // sleep and only returns after a full chip reset (S3 USB quirk).
        esp_sleep_enable_timer_wakeup((uint64_t)timeout_ms * 1000);
        esp_light_sleep_start();
    }

    void deep_sleep() override {
        ESP_LOGI(TAG, "Deep sleep");

        // Turn off EPD
        gfx_.sleep();

        // Configure touch wake
        esp_sleep_enable_ext0_wakeup(PIN_TOUCH_INT, 0);

        esp_deep_sleep_start();
    }

    int battery_level() const override {
        auto now = tick_ms();
        if (batt_level_ < 0 || now - last_batt_read_ > 60000) {
            int mv = read_battery_mv();
            int bl = battery_level_from_mv(mv);
            batt_level_ = bl;
            last_batt_read_ = now;
            ESP_LOGI(TAG, "battery_level=%d (mv=%d cali=%d)",
                     bl, mv, batt_cali_ok_ ? 1 : 0);
        }
        return (int)batt_level_;
    }

    // ─── Часы BM8563 ────────────────────────────────────────────
    // Проба чтения делается один раз: при неудаче чип считается отсутствующим
    // и на каждом кадре дёргать шину незачем.
    bool rtc_probe() const {
        if (rtc_ready_) return true;

        rtc_.read = rtc_read;
        rtc_.write = rtc_write;
        rtc_.handle = nullptr;

        struct tm t;
        std::memset(&t, 0, sizeof(t));

        // Сначала читаем как есть. Вызов i2c::init() внутри M5GFX делает
        // release() и переустанавливает драйвер, а эту шину держит тач, —
        // трогать её на ровном месте незачем.
        bm8563_err_t st = bm8563_read(&rtc_, &t);
        if (st != BM8563_OK && st != BM8563_ERR_LOW_VOLTAGE) {
            if (lgfx::i2c::init(kRtcI2cPort, kRtcSda, kRtcScl).has_error()) {
                ESP_LOGE(TAG, "BM8563: шина I2C не поднялась");
                return false;
            }
            std::memset(&t, 0, sizeof(t));
            st = bm8563_read(&rtc_, &t);
            if (st != BM8563_OK && st != BM8563_ERR_LOW_VOLTAGE) {
                ESP_LOGW(TAG, "BM8563: не отвечает (status=%d)", (int)st);
                return false;
            }
            ESP_LOGI(TAG, "BM8563: шина поднята с нуля, ответили");
        }
        if (st == BM8563_ERR_LOW_VOLTAGE)
            ESP_LOGW(TAG, "BM8563: низкое напряжение, батарея часов села");

        // Проверяем только то, на что опираемся. Дату не смотрим: в неё никто
        // не заглядывает, и мусор в ней не должен гасить часы.
        if (t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59 ||
            t.tm_sec < 0 || t.tm_sec > 59) {
            ESP_LOGW(TAG, "BM8563: неправдоподобное время %02d:%02d:%02d "
                          "ymd=%d-%d-%d wday=%d",
                     t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900,
                     t.tm_mon + 1, t.tm_mday, t.tm_wday);
            return false;
        }
        ESP_LOGI(TAG, "BM8563: %02d:%02d:%02d, дата %d-%02d-%02d",
                 t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        // Переводим чип в 24-часовой режим и снимаем флаги. Без этого чтение
        // может вернуть час в 12-часовом виде. Инициализация идемпотентна.
        if (bm8563_init(&rtc_) != BM8563_OK)
            ESP_LOGW(TAG, "BM8563: не удалось включить 24-часовой режим");
        rtc_ready_ = true;
        return true;
    }

    bool wall_clock(int& hh, int& mm) const override {
        if (!rtc_probe()) return false;
        struct tm t;
        std::memset(&t, 0, sizeof(t));
        const bm8563_err_t st = bm8563_read(&rtc_, &t);
        if ((st != BM8563_OK && st != BM8563_ERR_LOW_VOLTAGE) ||
            t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59)
            return false;
        hh = t.tm_hour;
        mm = t.tm_min;
        return true;
    }

    bool set_wall_clock(int hh, int mm) const override {
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
        if (!rtc_probe()) return false;

        // Драйвер пишет все семь байт разом, поэтому читаем текущее и меняем
        // только нужное поле: иначе на секунде в строке занулилась бы дата.
        struct tm t;
        std::memset(&t, 0, sizeof(t));
        const bm8563_err_t st = bm8563_read(&rtc_, &t);
        // Низкое напряжение временем не мешает: часы в чипе есть. А вот дата
        // нужна правдоподобная — драйвер отдаёт её в BCD и считает через
        // mktime, мусор там ломает запись.
        const bool date_ok = (st == BM8563_OK || st == BM8563_ERR_LOW_VOLTAGE) &&
                             t.tm_year >= 100 && t.tm_year <= 200 &&
                             t.tm_mon >= 0 && t.tm_mon <= 11 &&
                             t.tm_mday >= 1 && t.tm_mday <= 31;
        if (!date_ok) {
            // Часы в чипе нечитаемы (никогда не выставлялись). Подставляем
            // безопасную дату: в часах она не показывается, но драйвер
            // передаёт её в BCD, и нули там недопустимы для mktime.
            t.tm_year = 124;   // 2024
            t.tm_mon = 0;
            t.tm_mday = 1;
            t.tm_wday = 1;
            if (t.tm_sec < 0 || t.tm_sec > 59) t.tm_sec = 0;
        }
        t.tm_hour = hh;
        t.tm_min = mm;
        if (t.tm_sec > 59) t.tm_sec = 0;
        return bm8563_write(&rtc_, &t) == BM8563_OK;
    }

    // ─── Power button / device power off ────────────────────────
    bool power_button_double_clicked() override {
        uint32_t now = tick_ms();

        // Debounce the raw line.
        bool raw = gpio_get_level(PIN_BTN_PRESS) == (BTN_PRESSED_LEVEL ? 1 : 0);
        if (raw != btn_db_lvl_) {
            btn_db_lvl_ = raw;
            btn_db_since_ = now;
            ESP_LOGI(TAG, "btn raw -> %d at %u ms", raw ? 1 : 0, (unsigned)now);
            return false;
        }
        if (now - btn_db_since_ < BTN_DEBOUNCE_MS)
            return false;
        bool pressed = raw;

        if (pressed && !btn_prev_pressed_) {
            // Press edge.
            btn_press_stamp_ = now;
            btn_prev_pressed_ = true;
            btn_press_locked_ = (now < btn_action_lock_until_);
        } else if (!pressed && btn_prev_pressed_) {
            // Release edge → one completed click.
            bool locked = btn_press_locked_;
            btn_prev_pressed_ = false;
            btn_press_locked_ = false;
            if (locked)
                return false; // the click started inside a cooldown — ignore it
            if (now - btn_press_stamp_ < BTN_MIN_PRESS_MS)
                return false; // too short (glitch)
            if (btn_last_release_ != 0 &&
                now - btn_last_release_ >= BTN_DOUBLE_MIN_MS &&
                now - btn_last_release_ <= BTN_DOUBLE_MAX_MS) {
                // Double click — fire once and cancel single-click handling.
                btn_last_release_ = 0;
                btn_single_pending_ = 0;
                btn_action_lock_until_ = now + BTN_DOUBLE_MAX_MS;
                return true;
            }
            btn_last_release_ = now;
            btn_single_pending_ = now; // waiting for the double-click window to pass
        }
        return false;
    }

    bool power_button_single_clicked() override {
        uint32_t now = tick_ms();
        if (btn_single_pending_ == 0)
            return false;
        if (now < btn_action_lock_until_)
            return false; // still inside the double-click window
        if (now - btn_single_pending_ < BTN_DOUBLE_MAX_MS)
            return false;
        // Single click confirmed — fire once.
        btn_single_pending_ = 0;
        btn_action_lock_until_ = now + BTN_DOUBLE_MAX_MS;
        return true;
    }

    void power_off() override {
        ESP_LOGI(TAG, "Hardware power off (pulse OFF:G44 to PMS150G)");

        // Ensure the last frame (power-off logo) is fully drawn on the EPD —
        // the bistable panel keeps it as a "cover" after the device is off.
        gfx_.waitDisplay();

        // Exact M5Unified procedure for PaperS3 (Power_Class::_powerOff):
        // repeatedly pulse the OFF line so the PMS150G cuts the supply.
        for (int i = 0; i < 5; ++i) {
            gpio_set_level(PIN_PWR_OFF, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(PIN_PWR_OFF, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        ESP_LOGW(TAG, "PMS150G did not respond — falling back to deep sleep");
        deep_sleep();
    }

private:
    // PaperS3 battery: ADC1 GPIO3 = channel 2, divider ×2.0 (M5Unified).
    void init_battery_adc() {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_oneshot_unit_init_cfg_t unit_cfg = {};
        unit_cfg.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&unit_cfg, &batt_adc_) != ESP_OK || !batt_adc_) {
            ESP_LOGE(TAG, "battery ADC unit init failed");
            return;
        }
        adc_oneshot_chan_cfg_t ch_cfg = {};
        ch_cfg.atten = ADC_ATTEN_DB_12;
        ch_cfg.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(batt_adc_,
                                       (adc_channel_t)ADC1_GPIO3_CHANNEL,
                                       &ch_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "battery ADC channel config failed");
            return;
        }
        adc_cali_curve_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id = ADC_UNIT_1;
        cali_cfg.chan = (adc_channel_t)ADC1_GPIO3_CHANNEL;
        cali_cfg.atten = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &batt_cali_) == ESP_OK &&
            batt_cali_) {
            batt_cali_ok_ = true;
            ESP_LOGI(TAG, "battery ADC curve-fitting cali ready");
        } else {
            batt_cali_ok_ = false;
            ESP_LOGW(TAG, "battery ADC cali unavailable (eFuse?) — raw fallback");
        }
#endif
    }

    // Millivolts at the cell (divider ×2), or 0 if unreadable.
    int read_battery_mv() const {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (!batt_adc_) return 0;
        int raw = 0;
        if (adc_oneshot_read(batt_adc_,
                             (adc_channel_t)ADC1_GPIO3_CHANNEL, &raw) != ESP_OK)
            return 0;
        int pin_mv = 0;
        if (batt_cali_ok_ && batt_cali_ &&
            adc_cali_raw_to_voltage(batt_cali_, raw, &pin_mv) == ESP_OK) {
            return pin_mv * 2;
        }
        // No cali: rough 12 dB curve (adequate for a bar indicator).
        // raw 0..4095 → ~0..~3100 mV at pin, ×2 divider.
        return (int)((int64_t)raw * 3100 * 2 / 4095);
#else
        return 0;
#endif
    }

    static int battery_level_from_mv(int mv) {
        if (mv <= 0) return 0;
        // Same mapping as M5Unified Power_Class (3300–4150 mV → 0–100%).
        int level = (int)((mv - 3300) * 100.0f / (4150 - 3350));
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        return level;
    }

    M5GFX gfx_;
    uint8_t* fb_ = nullptr;  // palette-indexed framebuffer
    bool force_quality_ = false;

    // Battery ADC (lazy-stable handles; const battery_level uses them)
    mutable adc_oneshot_unit_handle_t batt_adc_ = nullptr;
    mutable adc_cali_handle_t batt_cali_ = nullptr;
    bool batt_cali_ok_ = false;

    // Часы BM8563
    mutable bm8563_t rtc_ = { nullptr, nullptr, nullptr };
    mutable bool rtc_ready_ = false;

    // Power button (PMS150G G0) state
    bool     btn_db_lvl_ = false;
    uint32_t btn_db_since_ = 0;
    bool     btn_prev_pressed_ = false;
    uint32_t btn_press_stamp_ = 0;
    bool     btn_press_locked_ = false;
    uint32_t btn_last_release_ = 0;
    uint32_t btn_single_pending_ = 0;  // release time of a click awaiting confirmation
    uint32_t btn_action_lock_until_ = 0; // suppress click handling while this runs

    // Touch state
    bool touch_active_ = false;
    bool touch_reported_ = false;
    int touch_x_ = 0, touch_y_ = 0;
    int touch_start_x_ = 0, touch_start_y_ = 0;
    uint32_t touch_start_time_ = 0;
    uint32_t last_touch_time_ = 0;
    uint32_t last_tap_time_ = 0;
    uint32_t last_activity_ms_ = 0;

    // Swipe state
    int swipe_dx_ = 0, swipe_dy_ = 0;
    bool swipe_pending_ = false;

    // Battery cache
    mutable int32_t batt_level_ = -1;
    mutable uint32_t last_batt_read_ = 0;
};

// ─── Singleton ─────────────────────────────────────────────────
static Platform_PaperS3 s_platform;
static bool s_initialized = false;

Platform* Platform::instance() {
    return s_initialized ? &s_platform : nullptr;
}

void Platform::init() {
    if (s_initialized) return;
    s_initialized = s_platform.init();
    if (!s_initialized) {
        ESP_LOGE(TAG, "Platform init failed!");
    }
}

#pragma once
#include <cstdint>

/**
 * Platform abstraction for M5 Paper S3.
 * Replaces ugui_win32.h from the Win32 build.
 *
 * Provides: framebuffer access, touch input, timing, power management.
 *
 * Framebuffer is uint8_t[] — each byte is a palette index (0-15).
 * See palette.h for color definitions.
 */

class Platform {
public:
    virtual ~Platform() = default;

    // ─── Display ──────────────────────────────────────────────
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual uint8_t* framebuffer() = 0;
    virtual void present() = 0;
    // Обновление только области (x,y,w,h) — для E-ink это быстрый
    // partial update; на симуляторе по умолчанию сводится к present().
    virtual void present_area(int x, int y, int w, int h) { present(); }
    virtual void wait_display() = 0;
    // Следующий present() уйдёт в epd_quality (полное очищающее обновление)
    // и затем автоматически вернёт epd_text. Гасит призраки после прогресса.
    virtual void force_quality_next_present() {}

    // ─── Touch input ──────────────────────────────────────────
    virtual bool get_touch(int& x, int& y) = 0;
    // sx, sy — точка, с которой жест начался. Без неё зону, из которой
    // свайп запущен, определить нельзя: дельта говорит только о направлении.
    virtual bool get_swipe(int& dx, int& dy, int& sx, int& sy) = 0;
    // Подстановка касания для проверок в симуляторе. На устройстве
    // ничего не делает: там состояние приходит из драйвера.
    virtual void inject_touch(int, int, bool) {}
    // True if a touch event (press/release/swipe) happened within the
    // last window_ms. Used by the main loop to keep polling fast during
    // gestures (double-tap/swipe need a tight sample rate) and only
    // light-sleep once the screen has been idle for a while.
    virtual bool touch_recent(uint32_t window_ms) const { (void)window_ms; return false; }

    // ─── Timing ───────────────────────────────────────────────
    virtual uint32_t tick_ms() const = 0;
    virtual void delay_ms(uint32_t ms) = 0;

    // ─── Power ────────────────────────────────────────────────
    virtual void light_sleep(uint32_t timeout_ms) = 0;
    virtual void deep_sleep() = 0;
    virtual int  battery_level() const = 0;

    // ─── Часы ───────────────────────────────────────────────────
    // Время суток в местном времени. false — время неизвестно, тогда
    // вызывающий берёт запасной источник (см. clock_now в общем слое).
    // Читается при каждой отрисовке, поэтому ничего кэшировать не нужно.
    // const — как battery_level(): состояние часов внутри помечено mutable,
    // иначе пробу в const-методе негде было бы запомнить.
    virtual bool wall_clock(int& hh, int& mm) const { (void)hh; (void)mm; return false; }
    // Записать время суток в аппаратные часы. Вызывается из настроек сразу
    // по нажатию, без ожидания выхода с экрана.
    virtual bool set_wall_clock(int hh, int mm) const { (void)hh; (void)mm; return false; }

    // ─── Power button / device power off ──────────────────────
    // Multifunction button (via PMS150G). Defaults: no button, no-op off.
    virtual bool power_button_single_clicked() { return false; }
    virtual bool power_button_double_clicked() { return false; }
    virtual void power_off() {}

    // ─── Singleton ────────────────────────────────────────────
    static Platform* instance();
    static void init();
};

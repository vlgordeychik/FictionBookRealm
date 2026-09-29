#include "clock.h"
#include "config.h"
#include "platform.h"
#include <cstdio>

namespace {
    // Запасное время: минуты от полуночи на момент загрузки плюс сдвиг по
    // tick_ms(). Оно же переживает перезагрузку.
    constexpr int kMinutesPerDay = 24 * 60;

    bool     g_started = false;
    int      g_base_minutes = -1;    // -1 — время не задано
    uint32_t g_base_ms = 0;

    // 32-битный счётчик перескакивает через ноль, поэтому разность
    // беззнаковая: «прошло» корректно и на переходе через ~49 дней.
    int minutes_since(uint32_t from_ms, uint32_t now_ms) {
        return (int)((uint32_t)(now_ms - from_ms) / 60000u);
    }
}

void clock_init() {
    g_base_minutes = cfg::get_int("clock_minutes");
    if (g_base_minutes < 0 || g_base_minutes >= kMinutesPerDay) g_base_minutes = -1;
    g_base_ms = Platform::instance() ? Platform::instance()->tick_ms() : 0;
    g_started = true;
}

bool clock_now(int& hh, int& mm) {
    if (!g_started) clock_init();

    int h = 0, m = 0;
    if (Platform::instance() && Platform::instance()->wall_clock(h, m) &&
        h >= 0 && h < 24 && m >= 0 && m < 60) {
        hh = h;
        mm = m;
        return true;
    }

    if (g_base_minutes < 0) return false;

    int total = g_base_minutes + minutes_since(g_base_ms, Platform::instance()->tick_ms());
    total %= kMinutesPerDay;
    if (total < 0) total += kMinutesPerDay;
    hh = total / 60;
    mm = total % 60;
    return true;
}

bool clock_set(int hh, int mm) {
    if (!g_started) clock_init();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;

    const int total = hh * 60 + mm;
    bool stored = false;
    if (Platform::instance()) {
        stored = Platform::instance()->set_wall_clock(hh, mm);
        if (!stored)
            std::printf("clock: аппаратные часы недоступны, время в запасном источнике\n");
    }

    // Запасной источник обновляем всегда: даже если чип ответил, полезно
    // иметь согласованное значение на случай, если чип не проснётся.
    g_base_minutes = total;
    g_base_ms = Platform::instance() ? Platform::instance()->tick_ms() : 0;
    cfg::set_int("clock_minutes", total);
    return true;
}

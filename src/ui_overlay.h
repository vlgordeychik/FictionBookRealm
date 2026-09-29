#pragma once

class FontRenderer;

// UI-шрифт для оверлеев (передаётся из main после загрузки).
void ui_overlay_set_font(FontRenderer* ui_font);

// Показывает ошибку поверх текущего кадра framebuffer:
// затемнённая плашка + текст, полный epd_quality present,
// ожидание тапа (на устройстве нет клавиатуры), затем возврат.
void show_error_overlay(const char* title, const char* detail = nullptr);

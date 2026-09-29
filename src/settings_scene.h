#pragma once
#include "scene.h"
#include <string>
#include <vector>

class FontRenderer;
namespace fs { class FileSystem; }

class SettingsScene : public Scene {
public:
    // Вкладки. Список открыт для расширения: новая вкладка — это одна
    // строка здесь плюс её строки в render(). Полоса вкладок занимает
    // место заголовка, поэтому координаты существующих строк не сдвигаются.
    enum class Tab { Reading = 0, System = 1 };

    SettingsScene(FontRenderer* font, fs::FileSystem* fs);
    const char* name() const override { return "settings"; }
    void on_enter() override;
    void on_exit() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
private:
    FontRenderer* font_;
    fs::FileSystem* fs_;
    std::vector<std::string> font_files_;
    int font_index_ = -1;
    int font_size_ = 18;
    float line_spacing_ = 1.45f;
    float para_spacing_ = 1.5f;
    int para_indent_ = 30;
    bool night_mode_ = false;
    int refresh_pages_ = 0; // full-quality redraw every N page turns (0=off)
    int auto_off_min_ = 10; // автовыключение после паузы, мин (0=выкл)
    Tab active_tab_ = Tab::Reading;
    int clock_hh_ = -1, clock_mm_ = -1; // -1 — время неизвестно

    void load();
    void save();
    void apply(int row); // 0=font file, 1=size, 2..4=spacing — no SD write
    void scan_fonts();
    void refresh_clock();
    // Сдвинуть часы или минуты и СРАЗУ записать в RTC: ждать выхода с
    // экрана не требуется, значение применяется на месте.
    void bump_clock(bool hours, int delta);
    bool dirty_ = false; // deferred: one ini write on exit
};

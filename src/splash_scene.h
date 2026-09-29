#pragma once
#include "scene.h"

class FontRenderer;

// Экран загрузки: «ЗАГРУЗКА» по центру на белом фоне. Вынесено отдельной
// функцией, потому что экран рисуется до запуска цикла сцен, из app_main,
// где сам SplashScene ещё не в стеке.
void draw_loading_screen(uint8_t* fb, int fb_w, int fb_h, FontRenderer* font);

// Экран не должен мелькать: на e-ink вспышка в четверть секунды хуже, чем
// его отсутствие.
constexpr uint32_t kLoadingMinMs = 1000;

// Сколько ещё ждать, чтобы экран держался не меньше kLoadingMinMs.
uint32_t loading_hold_ms(uint32_t drawn_at_ms, uint32_t now_ms);

class SplashScene : public Scene {
public:
    SplashScene(FontRenderer* font) : font_(font) {}
    const char* name() const override { return "splash"; }
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
private:
    FontRenderer* font_;
};

class NoFsScene : public Scene {
public:
    NoFsScene(FontRenderer* font) : font_(font) {}
    const char* name() const override { return "nofs"; }
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
private:
    void     retry_mount();
    bool     last_failed_ = false;
    uint32_t last_click_ms_ = 0;
    FontRenderer* font_;
};

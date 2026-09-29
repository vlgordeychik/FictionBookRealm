#pragma once
#include "scene.h"
#include <cstdint>

class FontRenderer;

class LockScene : public Scene {
public:
    LockScene(FontRenderer* font);
    ~LockScene() = default;
    const char* name() const override { return "lock"; }
    bool is_lock_scene() const override { return true; }
    void on_enter() override;
    void update() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;

    // Чистая проверка дедлайна. Вынесена отдельно от update(), чтобы её можно
    // было проверить, не ожидая минуты реального времени: minutes == 0 значит
    // «выключено» и не срабатывает никогда.
    static bool auto_off_due(uint32_t elapsed_ms, int minutes);

private:
    FontRenderer* font_;
    uint32_t last_click_ = 0;
    int last_x_ = 0, last_y_ = 0;

    // Автовыключение по паузе. enter_ms_ — момент постановки на паузу,
    // fired_ — защита от повторного запуска, пока сцена уже в стеке.
    uint32_t enter_ms_ = 0;
    int auto_off_min_ = 0;
    bool fired_ = false;

    // Геометрия плашки в одной точке правды: иначе при росте плашки под
    // новую строку текст разъедется с областью тапа.
    void plaque_rect(int fb_w, int fb_h, int* x, int* y, int* w, int* h) const;
};

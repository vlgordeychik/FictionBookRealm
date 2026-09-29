#pragma once
#include "scene.h"
#include <vector>
#include <string>
#include <cstdint>

class FontRenderer;
class BookScene;
namespace fs { class FileSystem; }

class SceneManager {
    std::vector<Scene*> stack_;
    bool dirty_ = true;
    bool quit_ = false;
    FontRenderer* font_ = nullptr;
    FontRenderer* ui_font_ = nullptr;
    fs::FileSystem* fs_ = nullptr;
    uint32_t last_click_time_ = 0;
    int last_click_x_ = 0, last_click_y_ = 0;
public:
    void set_font(FontRenderer* f) { font_ = f; }
    void set_ui_font(FontRenderer* f) { ui_font_ = f; }
    void set_fs(fs::FileSystem* f) { fs_ = f; }
    FontRenderer* font() const { return font_; }
    FontRenderer* ui_font() const { return ui_font_; }
    ~SceneManager();
    void start(Scene* root);
    void replace(Scene* s);
    void push(Scene* s);
    void pop();
    void go_back();
    Scene* top();
    // Topmost BookScene with the same cache_key, or nullptr.
    BookScene* find_book(const std::string& cache_key);
    // Remove every BookScene from the stack (frees index+layout before reopen).
    void close_books();
    // Pop until target is top (target must be in the stack).
    void pop_to(Scene* target);
    bool quit_requested() const { return quit_; }
    void request_quit() { quit_ = true; }
    bool needs_render() const { return dirty_; }
    // Сцена изменила состояние сама (например, клик по сношке в middle-third
    // книги) — вне общего dirty после on_mouse_down.
    void request_render() { dirty_ = true; }

    void update();
    void render(uint8_t* fb, int fb_w, int fb_h);
    void on_mouse_down(int x, int y, int button);
    void on_key_down(int key);
    void on_mouse_move(int x, int y);
};

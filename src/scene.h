#pragma once
#include <cstdint>

class SceneManager;

class Scene {
public:
    Scene*      back_   = nullptr;
    SceneManager* mgr_ = nullptr;
    virtual ~Scene() = default;
    virtual const char* name() const = 0;
    virtual void on_enter() {}
    virtual void on_exit() {}
    virtual void on_mouse_down(int x, int y, int button) {}
    virtual void on_key_down(int key) {}
    virtual void on_mouse_move(int x, int y) {}
    virtual void update() {}
    virtual void render(uint8_t* fb, int fb_w, int fb_h) = 0;
    virtual bool is_lock_scene() const { return false; }

    void go_back();
};

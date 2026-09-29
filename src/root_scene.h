#pragma once
#include "scene.h"

class RootScene : public Scene {
public:
    const char* name() const override { return "root"; }
    void render(uint8_t*, int, int) override {}
    void on_key_down(int key) override {
        if (key == 27) mgr_->request_quit();
    }
};

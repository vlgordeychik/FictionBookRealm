#pragma once
#include "scene.h"
#include "history.h"
#include <string>
#include <vector>

class FontRenderer;
namespace fs { class FileSystem; }

class HistoryScene : public Scene {
public:
    HistoryScene(FontRenderer* font, fs::FileSystem* fs);
    const char* name() const override { return "history"; }
    void on_enter() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
private:
    FontRenderer* font_;
    fs::FileSystem* fs_;
    std::vector<HistoryEntry> entries_;
    int scroll_ = 0;
    int sel_ = -1;
    int window_w_ = 0;
    int window_h_ = 0;

    int item_h() const;
    int max_visible(int fb_h) const;
};

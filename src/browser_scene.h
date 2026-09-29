#pragma once
#include "scene.h"
#include <string>
#include <vector>
#include <cstdint>

class FontRenderer;
namespace fs { class FileSystem; }

class BrowserScene : public Scene {
public:
    BrowserScene(FontRenderer* font, fs::FileSystem* fs);
    const char* name() const override { return "browser"; }
    void on_enter() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
private:
    struct Entry { bool is_dir; std::string name; };
    FontRenderer* font_;
    fs::FileSystem* fs_;
    std::string cur_dir_;
    std::vector<Entry> entries_;
    int scroll_ = 0;
    int sel_ = 0;
    int window_w_ = 0;
    int window_h_ = 0;
    void refresh();
    int max_visible(int fb_h) const;
    int row_h() const;
    std::string parent(const std::string& p) const;
    void open_selected_file();
};

#pragma once
#include "scene.h"
#include "image_decoder.h"
#include <string>
#include <vector>
#include <cstdint>

class FontRenderer;
namespace fs { class FileSystem; }

class BookshelfScene : public Scene {
public:
    BookshelfScene(FontRenderer* font, fs::FileSystem* fs);
    const char* name() const override { return "bookshelf"; }
    void on_enter() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
private:
    void load_book_info();
    void draw_cover(uint8_t* fb, int fb_w, int fb_h, int x, int y, int w, int h);
    int  wrap_text(const std::string& text, int max_w, int max_lines,
                   std::vector<std::string>& lines) const;

    FontRenderer* font_;
    fs::FileSystem* fs_;
    std::string last_file_;
    std::string cache_key_;
    std::string title_;
    std::string author_;
    std::string annotation_;
    int current_page_ = 0;
    int total_pages_ = 0;
    bool has_book_ = false;

    // Обложка: 16-цветные индексы, как в буфере кадра. Хранить палитру, а не
    // RGBA вчетверо дешевле (56 КБ против 224 КБ) и рисуется без конвертации
    // каждого пикселя.
    PaletteImage cover_pal_;
    bool cover_loaded_ = false;

    // Button rectangles
    struct Btn { int x1, y1, x2, y2; const char* label; };
    Btn btn_browse_ = {};
    Btn btn_history_ = {};
    Btn btn_settings_ = {};

    // Cover hitbox
    int cover_x1_ = 0, cover_y1_ = 0, cover_x2_ = 0, cover_y2_ = 0;

    // Hitbox of the right column (title/author/annotation/progress). Tapping
    // it opens the book too, otherwise a right-handed user has to reach for
    // the cover on the far left. Filled in render() so it can never drift
    // from the drawn column; the bottom edge stops right above the buttons.
    int info_x1_ = 0, info_y1_ = 0, info_x2_ = 0, info_y2_ = 0;
};

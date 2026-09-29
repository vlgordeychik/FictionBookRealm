#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include "font_renderer.h"
#include "image_decoder.h"
#include "fb2/fb2_document.h"

class Fb2PageView {
public:
    using ProgressCb = std::function<void(int)>;
public:
    static constexpr int kDefaultWidth  = 960;
    static constexpr int kDefaultHeight = 540;
    static constexpr int kDefaultFontSize = 18;
    static constexpr int kOrphanMinLines = 3;
    static constexpr float kTitleScale0 = 1.35f;
    static constexpr float kTitleScale1 = 1.20f;
    static constexpr float kTitleScaleN = 1.10f;
    Fb2PageView(FontRenderer* font, FontRenderer* ui_font, const fb2::Fb2Document* doc);

    void set_size(int w, int h);
    void set_font_size(int pts);
    // Имя файла, из которого реально загружен шрифт: входит в ключ кэша
    // раскладки (метрики шрифта определяют и переносы, и выключку).
    void set_font_file(const std::string& f) { font_file_ = f; space_w_ = 0; }
    void set_layout_params(float line_spacing, float para_spacing, int para_indent);
    void set_night_mode(bool on);
    void layout(ProgressCb cb = nullptr);

    // Pre-decode the cover image (if any) while the open-time heap is still
    // large (~3.8MB). Without this, render_image → read_binary of a ~1.66MB
    // base64 cover fails the OOM guard after load_cache drops heap to ~0.5MB
    // and page 0 renders as a white page.
    void predecode_cover();

    // Layout cache
    bool save_cache(const std::string& file_path);
    bool load_cache(const std::string& file_path);
    bool has_cache(const std::string& file_path) const;

    void render(uint8_t* fb, int fb_w, int fb_h);

    // Диагностика вёрстки: печатает строки с их шириной и растяжкой пробелов.
    // Используется headless-проверкой на хосте и разбором вёрстки на устройстве.
    void dump_layout(int max_pages, const char* tag) const;

    // Хеш всей раскладки (текст строк + растяжка пробелов + разбивка на
    // страницы). Нужен, чтобы сверить результат layout() с результатом
    // load_cache(): при рассогласовании полей кэша хеши разойдутся.
    uint32_t layout_signature() const;

    bool next_page();
    bool prev_page();
    bool go_to_page(int page);
    int  current_page() const { return cur_page_; }
    int  total_pages() const { return (int)pages_.size(); }
    int  current_page_block() const;
    int  page_for_block(int block_idx) const;

    // Footnotes
    bool has_footnote_click(int x, int y) const; // processes click + navigates
    // То же без перехода: нужно, чтобы решить судьбу жеста, не выполняя
    // его. has_footnote_click не годится для проверки — он переходит.
    bool is_footnote_at(int x, int y) const;
    bool is_footnote_active() const { return fn_nav_.active; }
    bool is_first_footnote_page() const;
    void exit_footnote();
    int  first_footnote_page() const;

    int  hit_test(int x, int y) const;
    int  toc_hit_test(int x, int y) const;

    void toc_scroll_up()   { if (toc_scroll_ > 0) --toc_scroll_; }
    void toc_scroll_down() { toc_scroll_ = std::min(toc_scroll_ + 1, max_toc_scroll()); }
    int  max_toc_scroll() const;

    void toggle_toc() { toc_visible_ = !toc_visible_; }
    bool is_toc_visible() const { return toc_visible_; }

    const std::vector<int>& get_toc_pages() const { return toc_entry_page_; }
    void follow_footnote(const std::string& id);

private:
    struct FootnoteLink {
        int         start;          // char offset in line.text
        int         end;            // char offset after link text
        std::string id;             // footnote id (without #)
    };

    struct Line {
        std::string text;
        bool        para_indent;
        int         block_type;     // fb2::BlockType
        bool        para_break;     // paragraph separator
        int         block_index;    // which block (0..), or image index
        int         depth;          // TOC depth (for title sizing)
        int         image_index = -1;         // image index, -1 = none
        // Выключка по правому краю: сколько пикселей добавить к каждому
        // пробелу. 0 = строка не выключена. Остаток, который не удалось
        // распределить без «решётки», намеренно остаётся справа.
        int16_t     space_extra = 0;
        std::vector<FootnoteLink> footnotes; // footnote links in this line
    };

    // Pages store only a contiguous index range into all_lines_ —
    // no second copy of Line texts (OOM on push_back without exceptions).
    struct Page {
        int               first_line_idx = 0;
        int               last_line_idx = -1;   // last_line < first_line ⇒ empty
        int               first_block = -1;
        int               last_block = -1;
        bool              is_image_page = false;
        bool empty() const { return last_line_idx < first_line_idx; }
        int  line_count() const {
            return empty() ? 0 : (last_line_idx - first_line_idx + 1);
        }
    };

    struct TocLayout {
        std::vector<fb2::TocEntry> entries;
        std::vector<int>           pages;
    };

    int  text_width(const std::string& s) const;
    int  ui_text_width(const std::string& s) const;
    void layout_blocks(ProgressCb cb = nullptr);
    void render_header(uint8_t* fb, int fb_w, int fb_h);
    void render_footer(uint8_t* fb, int fb_w, int fb_h);
    void render_toc(uint8_t* fb, int fb_w, int fb_h);
    void render_image(uint8_t* fb, int fb_w, int fb_h, const Page& pg);

    // Ширина пробела и знака переноса для текущего шрифта. Считается один
    // раз на раскладку; используется и вёрсткой, и рендером выключенных строк.
    void measure_justify();
    // X символа с заданным смещением в строке — с учётом раздвинутых пробелов.
    int  x_of_offset(const Line& ln, int start_x, int off) const;

    FontRenderer*         font_;
    FontRenderer*         ui_font_;
    const fb2::Fb2Document* doc_;

    std::vector<Line> all_lines_;
    std::vector<Page> pages_;
    int               cur_page_ = 0;
    int               width_ = 960;
    int               height_ = 540;
    int               font_size_ = 18;
    bool              toc_visible_ = false;

    int margin_x_, margin_y_;
    int header_h_, footer_h_;
    std::string font_file_ = "arial.ttf";
    int content_x_, content_y_, content_w_, content_h_;
    int para_indent_;
    int line_height_;
    int para_spacing_;
    int space_w_ = 0;    // ширина пробела текущего шрифта
    int hyphen_w_ = 0;   // ширина знака переноса
    bool have_hyphen_glyph_ = false;  // есть ли в шрифте U+2010
    float line_spacing_ratio_ = 1.45f;
    float para_spacing_ratio_ = 1.5f;
    int para_indent_px_ = 30;
    bool night_mode_ = false;

    // TOC view state
    int               toc_scroll_ = 0;
    bool              toc_inited_ = false;
    std::vector<int>  toc_entry_page_;

    // Block metadata for layout
    std::vector<int>  block_depth_;      // TOC depth per block
    std::vector<int>  block_first_line_; // first all_lines_ index per block

    // Footnote navigation state
    struct FootnoteNav {
        bool active = false;
        int  original_page = 0;
        int  first_block = 0;
        int  last_block = 0;
    };
    FootnoteNav fn_nav_;

    // Hit-test rectangles for footnote links (rebuilt each render)
    struct FootHitRect {
        int x1, y1, x2, y2;
        std::string id;
    };
    mutable std::vector<FootHitRect> foot_hit_rects_;

    // Image rendering — 16-color palette indices (no truecolor cache)
    PaletteImage current_image_;
    int          current_image_idx_ = -1;

    // Обложка, декодированная при открытии книги, когда куча ещё свободна.
    // Хранится отдельно от current_image_: render_image перезатирает последнюю
    // при показе страницы с картинкой, и обложка была бы потеряна. Книжная
    // полка берёт её отсюда, когда книга ещё открыта и повторный декод
    // base64 (~1.6 МБ) не помещается в кучу.
    PaletteImage cover_palette_;

public:
    // Декодированная обложка книги (пустая, если не декодировалась).
    const PaletteImage& cached_cover() const { return cover_palette_; }

    // Unified per-line vertical geometry. Every pass (layout, render, footnote
    // underlines) must use this so the screen position of a line always matches
    // the space layout() reserved for it (esp. Subtitles drawn at 1.1x).
    int line_advance(int block_type, int depth) const;

    static float title_scale(int depth) {
        if (depth == 0) return kTitleScale0;
        if (depth == 1) return kTitleScale1;
        return kTitleScaleN;
    }
};

#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_CACHE_H
#include <string_view>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Was 1536KB — too large next to doc + layout (peak → OOM/abort).
#define FTC_MAX_BYTES (512 * 1024)

class FontRenderer {
public:
    FontRenderer();
    ~FontRenderer();

    bool load_font(const char* path, int pixel_size);
    bool load_font_from_memory(const void* data, size_t size,
                               const char* name, int pixel_size);
    void set_fallback_font(const void* data, size_t size);
    bool is_loaded() const { return font_ok_; }
    // Rescale the currently loaded face without re-reading the font file.
    bool set_pixel_size(int pixel_size);
    int  pixel_size() const { return pixel_size_; }

    void draw_text(uint8_t* fb, int fb_w, int fb_h,
                   int x, int y, std::string_view text, uint8_t color_idx);

    void draw_text_wrapped(uint8_t* fb, int fb_w, int fb_h,
                           int x, int y, std::string_view text,
                           int max_width, int max_height, uint8_t color_idx);

    int  text_width(std::string_view text) const;
    int  line_height() const { return line_height_; }
    int  ascent() const { return ascent_; }
    int  descent() const { return descent_; }

    // Базовая линия для текста, отцентрованного по вертикали в блоке
    // высотой h, начинающемся в top. Нужна потому, что draw_text() трактует
    // y как базовую линию, а не как верх бокса: формула «top + (h -
    // line_height)/2» прижимает текст к верху примерно на половину высоты
    // заглавных букв.
    int  baseline_for_center(int top, int h) const {
        return top + h / 2 + (ascent_ - descent_) / 2;
    }

private:
    struct MemoryFont {
        std::unique_ptr<unsigned char[]> data;
        size_t size = 0;
    };

    static uint32_t decode_utf8(const char*& s);
    static FT_Error face_requester(FTC_FaceID face_id, FT_Library library,
                                   FT_Pointer req_data, FT_Face* aface);

    FT_Library              lib_ = nullptr;
    std::vector<std::unique_ptr<MemoryFont>> memory_fonts_;
    mutable FTC_Manager     manager_ = nullptr;
    mutable FTC_CMapCache   cmap_cache_ = nullptr;
    mutable FTC_ImageCache  image_cache_ = nullptr;
    mutable FTC_ScalerRec   scaler_{};
    int                     line_height_ = 0;
    int                     ascent_ = 0;      // пиксели над базовой линией
    int                     descent_ = 0;     // пиксели под базовой линией
    int                     cmap_index_ = 0;
    int                     pixel_size_ = 0;
    bool                    font_ok_ = false;
    std::vector<unsigned char> fallback_font_data_;
};

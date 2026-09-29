#include "font_renderer.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include <new>
#endif
#ifdef ESP_PLATFORM
#define FR_PROBE(...) ESP_LOGI("frt", __VA_ARGS__)
#else
#define FR_PROBE(...) ((void)0)
#endif
#ifndef ESP_PLATFORM
#include <new>
#endif
#include "palette.h"
#include "fb_helpers.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
    constexpr int kFallbackLineHeightPad = 4;
    constexpr int kAlphaMax = 255;
    constexpr unsigned kReplacementChar = 0xFFFD;
    constexpr int kLineHeightPad = 4;

    // Blend two palette indices with 8-bit alpha (0=bg, 255=fg)
    uint8_t blend_palette(uint8_t fg_idx, uint8_t bg_idx, uint8_t alpha) {
        int fg_gray = kPaletteValues[fg_idx];
        int bg_gray = kPaletteValues[bg_idx];
        int gray = (fg_gray * alpha + bg_gray * (255 - alpha)) / 255;
        return (uint8_t)((gray + 8) / 17);
    }

    // Метрики шрифта одним куском: высота строки, восходящий и нисходящий
    // элементы в пикселях. Считается в двух местах (загрузка и смена
    // размера), поэтому держим в одной функции.
    void compute_metrics(FT_Face face, int pixel_size,
                         int* line_height, int* ascent, int* descent) {
        if (face && face->units_per_EM > 0) {
            *line_height = (int)(face->height * (FT_Pos)pixel_size / face->units_per_EM);
            *ascent  = (int)( face->ascender * (FT_Pos)pixel_size / face->units_per_EM);
            *descent = (int)(-face->descender * (FT_Pos)pixel_size / face->units_per_EM);
        } else {
            *line_height = pixel_size + kFallbackLineHeightPad;
            *ascent = 0;
            *descent = 0;
        }
        if (*line_height < 1) *line_height = pixel_size + kFallbackLineHeightPad;
        if (*ascent < 1 || *descent < 0) {
            // Без реальных метрик берём типовые пропорции: иначе текст,
            // отцентрованный по формуле ниже, уезжает вверх или вниз.
            *ascent = pixel_size;
            *descent = pixel_size / 4;
        }
    }
}

FontRenderer::FontRenderer() {
    if (FT_Init_FreeType(&lib_)) return;
    FTC_Manager_New(lib_, 0, 0, FTC_MAX_BYTES, face_requester, this, &manager_);
    FTC_CMapCache_New(manager_, &cmap_cache_);
    FTC_ImageCache_New(manager_, &image_cache_);
}

FontRenderer::~FontRenderer() {
    if (manager_) FTC_Manager_Done(manager_);
    if (lib_)  FT_Done_FreeType(lib_);
}

FT_Error FontRenderer::face_requester(FTC_FaceID face_id, FT_Library library,
                                      FT_Pointer req_data, FT_Face* aface) {
    auto* self = (FontRenderer*)req_data;
    auto* mf = (MemoryFont*)face_id;
    FT_Error err = FT_New_Memory_Face(library, mf->data.get(), (FT_Long)mf->size, 0, aface);
    if (err) return err;
    FT_Face face = *aface;

    // Find charmap that has Cyrillic 'Рђ' (0x0410)
    for (int i = 0; i < face->num_charmaps; ++i) {
        FT_CharMap cm = face->charmaps[i];
        FT_Set_Charmap(face, cm);
        if (FT_Get_Char_Index(face, 0x0410) != 0) {
            self->cmap_index_ = i;
            return 0;
        }
    }

    // None have Cyrillic в†’ load fallback font
    FT_Done_Face(face);
    *aface = nullptr;
    if (!self->fallback_font_data_.empty()) {
        err = FT_New_Memory_Face(library, self->fallback_font_data_.data(),
                                 (FT_Long)self->fallback_font_data_.size(), 0, aface);
        if (err) return err;
        FT_Select_Charmap(*aface, FT_ENCODING_UNICODE);
        for (int i = 0; i < (*aface)->num_charmaps; ++i) {
            if ((*aface)->charmaps[i] == (*aface)->charmap) {
                self->cmap_index_ = i;
                break;
            }
        }
        return 0;
    }
    // No fallback вЂ” recreate original with Unicode default
    err = FT_New_Memory_Face(library, mf->data.get(), (FT_Long)mf->size, 0, aface);
    if (err) return err;
    FT_Select_Charmap(*aface, FT_ENCODING_UNICODE);
    for (int i = 0; i < (*aface)->num_charmaps; ++i) {
        if ((*aface)->charmaps[i] == (*aface)->charmap) {
            self->cmap_index_ = i;
            break;
        }
    }
    return 0;
}

bool FontRenderer::load_font(const char* path, int pixel_size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    auto* raw = new (std::nothrow) unsigned char[(size_t)sz];
    if (!raw) { std::fclose(f); return false; }
    if (std::fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        delete[] raw;
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    bool ok = load_font_from_memory(raw, (size_t)sz, path, pixel_size);
    delete[] raw;
    return ok;
}

bool FontRenderer::load_font_from_memory(const void* data, size_t size,
                                         const char*, int pixel_size) {
    if (!data || size == 0 || !manager_) return false;

    // Drop cached faces/glyphs BEFORE freeing old MemoryFont pointers —
    // face_id in the FTC cache aliases those pointers.
    FTC_Manager_Reset(manager_);
    memory_fonts_.clear();

    auto* mf = new (std::nothrow) MemoryFont();
    if (!mf) return false;
    mf->data.reset(new (std::nothrow) unsigned char[size]);
    if (!mf->data) {
        delete mf;
        return false;
    }
    std::memcpy(mf->data.get(), data, size);
    mf->size = size;

    scaler_.face_id = (FTC_FaceID)mf;
    scaler_.width = (FT_UInt)pixel_size;
    scaler_.height = (FT_UInt)pixel_size;
    scaler_.pixel = 1;
    scaler_.x_res = 72;
    scaler_.y_res = 72;

    FT_Face face;
    if (FTC_Manager_LookupFace(manager_, scaler_.face_id, &face)) {
        delete mf;
        scaler_.face_id = nullptr;
        font_ok_ = false;
        return false;
    }
    // Size comes only from scaler_ (FTC owns the face — no FT_Set_Pixel_Sizes).
    compute_metrics(face, pixel_size, &line_height_, &ascent_, &descent_);
    if (memory_fonts_.capacity() == 0) {
        memory_fonts_.reserve(1);
        if (memory_fonts_.capacity() == 0) {
            FTC_Manager_Reset(manager_);
            delete mf;
            scaler_.face_id = nullptr;
            font_ok_ = false;
            return false;
        }
    }
    memory_fonts_.emplace_back(std::unique_ptr<MemoryFont>(mf));
    pixel_size_ = pixel_size;
    font_ok_ = true;
    return true;
}

bool FontRenderer::set_pixel_size(int pixel_size) {
    if (!font_ok_ || !manager_ || memory_fonts_.empty() || pixel_size < 1)
        return false;
    if (pixel_size == pixel_size_) return true;

    // FreeType cache owns the FT_Face: never FT_Set_Pixel_Sizes on it
    // (that path corrupted the heap → CORRUPT HEAP on next render).
    // Only the scaler changes size; flush stale glyph nodes first.
    FTC_Manager_Reset(manager_);

    scaler_.width = (FT_UInt)pixel_size;
    scaler_.height = (FT_UInt)pixel_size;
    scaler_.pixel = 1;

    FT_Face face = nullptr;
    if (FTC_Manager_LookupFace(manager_, scaler_.face_id, &face) || !face)
        return false;
    int lh = line_height_;
    compute_metrics(face, pixel_size, &lh, &ascent_, &descent_);
    line_height_ = lh;
    pixel_size_ = pixel_size;
    FR_PROBE("set_pixel_size %d lh=%d", pixel_size, lh);
    return true;
}

void FontRenderer::set_fallback_font(const void* data, size_t size) {
    fallback_font_data_.assign((const unsigned char*)data, (const unsigned char*)data + size);
}

uint32_t FontRenderer::decode_utf8(const char*& s) {
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) { ++s; return c; }
    if (c < 0xC0) { ++s; return kReplacementChar; }

    uint32_t cp;
    int trail;
    if (c < 0xE0) { cp = c & 0x1F; trail = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; trail = 2; }
    else if (c < 0xF8) { cp = c & 0x07; trail = 3; }
    else { ++s; return kReplacementChar; }

    for (int i = 0; i < trail; ++i) {
        ++s;
        if (((uint8_t)*s & 0xC0) != 0x80) { return kReplacementChar; }
        cp = (cp << 6) | ((uint8_t)*s & 0x3F);
    }
    ++s;
    return cp;
}

void FontRenderer::draw_text(uint8_t* fb, int fb_w, int fb_h,
                              int x, int y, std::string_view text,
                              uint8_t color_idx) {
    if (!font_ok_ || text.empty()) return;
    int pen_x = x;
    int pen_y = y;
    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += line_height_;
            ++p;
            continue;
        }

uint32_t cp = decode_utf8(p);
        if (cp == kReplacementChar) continue;

        FT_UInt gi = FTC_CMapCache_Lookup(cmap_cache_, scaler_.face_id, cmap_index_, cp);
        if (!gi) continue;

        FT_Glyph glyph;
        if (FTC_ImageCache_LookupScaler(image_cache_, &scaler_,
                                        FT_LOAD_RENDER | FT_LOAD_MONOCHROME | FT_LOAD_NO_HINTING, gi, &glyph, nullptr))
            continue;
        if (glyph->format != FT_GLYPH_FORMAT_BITMAP) continue;

        FT_BitmapGlyph bg = (FT_BitmapGlyph)glyph;
        FT_Bitmap& bm = bg->bitmap;
        int gx = pen_x + bg->left;
        int gy = pen_y - bg->top;

        for (int by = 0; by < (int)bm.rows; ++by) {
            for (int bx = 0; bx < (int)bm.width; ++bx) {
                int px = gx + bx;
                int py = gy + by;
                if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;

                unsigned char a = bm.buffer[by * bm.pitch + (bx >> 3)];
                if (!(a & (0x80 >> (bx & 7)))) continue;

                uint8_t bg_idx = fb[py * fb_w + px];
                fb[py * fb_w + px] = color_idx;
            }
        }

        pen_x += glyph->advance.x >> 16;
        // glyph is owned by the FTC cache node — do NOT FT_Done_Glyph it
    }
}

void FontRenderer::draw_text_wrapped(uint8_t* fb, int fb_w, int fb_h,
                                      int x, int y, std::string_view text,
                                      int max_width, int max_height,
                                      uint8_t color_idx) {
    if (!font_ok_ || text.empty()) return;
    int pen_x = x;
    int pen_y = y;
    int limit_y = y + max_height;
    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end && pen_y + line_height_ <= limit_y) {
        if (*p == '\n') {
            pen_x = x;
            pen_y += line_height_;
            ++p;
            continue;
        }

        const char* word_start = p;
        const char* word_end = p;
        int word_width = 0;

        while (word_end < end && *word_end != '\n' && *word_end != ' ' && *word_end != '\t') {
            ++word_end;
        }

        const char* wp = word_start;
        int ww = 0;
        while (wp < word_end) {
            uint32_t cp = decode_utf8(wp);
            FT_UInt gi = FTC_CMapCache_Lookup(cmap_cache_, scaler_.face_id, cmap_index_, cp);
            if (gi) {
                FT_Glyph glyph;
                if (FTC_ImageCache_LookupScaler(image_cache_, &scaler_,
                                                FT_LOAD_DEFAULT, gi, &glyph, nullptr) == 0) {
                    ww += glyph->advance.x >> 16;
                }
            }
        }

        if (pen_x + ww > x + max_width && pen_x > x) {
            pen_x = x;
            pen_y += line_height_;
            if (pen_y + line_height_ > limit_y) break;
        }

        draw_text(fb, fb_w, fb_h, pen_x, pen_y,
                  std::string_view(word_start, word_end - word_start), color_idx);

        wp = word_start;
        while (wp < word_end) {
            uint32_t cp = decode_utf8(wp);
            FT_UInt gi = FTC_CMapCache_Lookup(cmap_cache_, scaler_.face_id, cmap_index_, cp);
            if (gi) {
                FT_Glyph glyph;
                if (FTC_ImageCache_LookupScaler(image_cache_, &scaler_,
                                                FT_LOAD_DEFAULT, gi, &glyph, nullptr) == 0) {
                    pen_x += glyph->advance.x >> 16;
                }
            }
        }

        p = word_end;

        while (p < end && (*p == ' ' || *p == '\t')) {
            if (*p == ' ') {
                uint32_t cp = decode_utf8(p);
                FT_UInt gi = FTC_CMapCache_Lookup(cmap_cache_, scaler_.face_id, 0, cp);
                if (gi) {
                FT_Glyph glyph;
                if (FTC_ImageCache_LookupScaler(image_cache_, &scaler_,
                                                FT_LOAD_DEFAULT, gi, &glyph, nullptr) == 0) {
                    pen_x += glyph->advance.x >> 16;
                }
                }
            } else {
                ++p;
            }
        }
    }
}

int FontRenderer::text_width(std::string_view text) const {
    if (!font_ok_) return 0;
    int w = 0;
    const char* p = text.data();
    const char* end = p + text.size();

    while (p < end) {
        if (*p == '\n') break;
        uint32_t cp = decode_utf8(p);
        if (cp == kReplacementChar) continue;
        FT_UInt gi = FTC_CMapCache_Lookup(cmap_cache_, scaler_.face_id, cmap_index_, cp);
        if (gi) {
            FT_Glyph glyph;
            if (FTC_ImageCache_LookupScaler(image_cache_, &scaler_,
                                            FT_LOAD_DEFAULT, gi, &glyph, nullptr) == 0) {
                w += glyph->advance.x >> 16;
            }
        }
    }
    return w;
}

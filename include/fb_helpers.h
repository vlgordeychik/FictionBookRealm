#pragma once
#include <cstdint>
#include <cstring>
#include "palette.h"

/**
 * Palette-indexed framebuffer helper.
 *
 * Provides fb_write() and fb_fill() that accept ARGB colors
 * and convert them to palette indices on write. This lets
 * scene code keep using ARGB constants with a uint8_t* buffer.
 */

inline uint8_t* fb_ptr(uint8_t* fb, int fb_w, int x, int y) {
    return &fb[y * fb_w + x];
}

inline void fb_write(uint8_t* fb, int fb_w, int x, int y, uint32_t argb) {
    fb[y * fb_w + x] = argb_to_palette(argb);
}

inline void fb_write(uint8_t* fb, int fb_w, int x, int y, uint8_t idx) {
    fb[y * fb_w + x] = idx;
}

inline uint8_t fb_read(const uint8_t* fb, int fb_w, int x, int y) {
    return fb[y * fb_w + x];
}

inline void fb_fill(uint8_t* fb, int fb_w, int fb_h, uint8_t idx) {
    std::memset(fb, idx, fb_w * fb_h);
}

inline void fb_fill_rect(uint8_t* fb, int fb_w,
                          int x0, int y0, int x1, int y1, uint8_t idx) {
    for (int y = y0; y <= y1; ++y) {
        std::memset(&fb[y * fb_w + x0], idx, x1 - x0 + 1);
    }
}

inline void fb_fill_rect(uint8_t* fb, int fb_w,
                          int x0, int y0, int x1, int y1, uint32_t argb) {
    fb_fill_rect(fb, fb_w, x0, y0, x1, y1, argb_to_palette(argb));
}

// Horizontal line
inline void fb_hline(uint8_t* fb, int fb_w,
                      int x0, int x1, int y, uint8_t idx) {
    std::memset(&fb[y * fb_w + x0], idx, x1 - x0 + 1);
}

// Blend two palette indices: result = a * t + b * (1-t), t in [0..16]
inline uint8_t fb_blend(uint8_t a, uint8_t b, int t) {
    return (uint8_t)((a * t + b * (16 - t) + 8) / 16);
}

// Dim a palette index by factor (0=black, 16=original)
inline uint8_t fb_dim(uint8_t idx, int factor) {
    return (uint8_t)((idx * factor + 8) / 16);
}

// ─── Icon blitting ──────────────────────────────────────────────
struct Icon {
    const uint8_t* data;
    int w;
    int h;
};

// Blit a palette-indexed icon onto the framebuffer.
// icon.data[] contains palette indices 0-15 (row-major).
// Index 15 = white = transparent (skipped).
// Other indices blend toward 'color': index 0 = fully color,
// index 14 = very faint.
// If invert=true, indices are flipped (0→15, 1→14, etc.)
// for night mode (white shape on dark background).
inline void fb_blit_icon(uint8_t* fb, int fb_w, int fb_h,
                         int x, int y, const Icon& icon, uint8_t color,
                         bool invert = false) {
    for (int iy = 0; iy < icon.h; ++iy) {
        int dy = y + iy;
        if (dy < 0 || dy >= fb_h) continue;
        for (int ix = 0; ix < icon.w; ++ix) {
            int dx = x + ix;
            if (dx < 0 || dx >= fb_w) continue;
            uint8_t idx = icon.data[iy * icon.w + ix];
            if (invert) idx = 15 - idx;
            if (idx >= 15) continue;  // white = transparent
            uint8_t bg = fb[dy * fb_w + dx];
            int t = 16 - (int)idx;  // 16 for black, 1 for index 15 (skipped above)
            fb[dy * fb_w + dx] = fb_blend(color, bg, t);
        }
    }
}

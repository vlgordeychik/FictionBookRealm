#pragma once
#include <cstdint>

/**
 * Palette for 16-shade EPD (M5 Paper S3).
 *
 * Palette maps 4-bit index → 8-bit grayscale value.
 * Index 0 = black, index 15 = white.
 *
 * All scene colors are defined as palette indices here.
 * Framebuffer is uint8_t[] — each byte is a palette index.
 */

// ─── Palette size ────────────────────────────────────────────
static constexpr int kPaletteSize = 16;

// ─── Grayscale palette: index → gray value (0x00..0xFF) ──────
// Linear mapping: index * 17 (0, 17, 34, ..., 255)
static constexpr uint8_t kPaletteValues[kPaletteSize] = {
    0x00,  //  0: black
    0x11,  //  1
    0x22,  //  2: very dark
    0x33,  //  3: dark
    0x44,  //  4: medium dark
    0x55,  //  5: medium
    0x66,  //  6: medium light
    0x77,  //  7
    0x88,  //  8: gray
    0x99,  //  9: light
    0xAA,  // 10: lighter
    0xBB,  // 11
    0xCC,  // 12: light border
    0xDD,  // 13: very light
    0xEE,  // 14: near-white
    0xFF,  // 15: white
};

// ─── Named palette indices (scene colors) ────────────────────

// Blacks and darks
static constexpr uint8_t kBlack      =  0;  // 0x00 — pure black
static constexpr uint8_t kDark2      =  2;  // 0x22 — main text
static constexpr uint8_t kDark1      =  3;  // 0x33 — button text
static constexpr uint8_t kMidDark    =  4;  // 0x44 — section headings, links, TOC text
static constexpr uint8_t kMid        =  5;  // 0x55 — epigraphs, subtext
static constexpr uint8_t kMidLight   =  6;  // 0x66 — subtitle

// Lights
static constexpr uint8_t kGray       =  8;  // 0x88 — version, hints, scroll buttons
static constexpr uint8_t kLight2     =  9;  // 0x99 — empty states, navigation hints
static constexpr uint8_t kLight1     = 10;  // 0xAA — page numbers

// Very light
static constexpr uint8_t kBorder     = 12;  // 0xCC — borders
static constexpr uint8_t kButton     = 13;  // 0xDD — button backgrounds
static constexpr uint8_t kCream      = 14;  // 0xEE — panel backgrounds (warm cream)
static constexpr uint8_t kOffWhite   = 15;  // 0xFF — page backgrounds (off-white)
static constexpr uint8_t kWhite      = 15;  // 0xFF — pure white

// Color-approximated → gray (on 16-shade EPD these become their luminance)
static constexpr uint8_t kBrown      =  7;  // 0x8B7355 → ~0x71 → index 7
static constexpr uint8_t kRed        =  6;  // 0xCC4444 → ~0x6A → index 6
static constexpr uint8_t kGreen      =  8;  // 0x50B86C → ~0x84 → index 8
static constexpr uint8_t kBlue       =  4;  // 0x4A90D9 → ~0x83 → index 8 (close to kMidDark)
static constexpr uint8_t kOrange     = 12;  // 0xE6A23C → ~0xC4 → index 12
static constexpr uint8_t kPurple     =  7;  // 0x8B5CF6 → ~0x7B → index 7

// Transparency helpers (for lock_scene dimming, fb2_page_view darkening)
static constexpr uint8_t kTransparentBlack = 0;  // 0x40000000 → dimmed → near-black

// ─── ARGB → palette index conversion (for image decoding) ────
// luminance = (R*77 + G*151 + B*29) >> 8
// index = (luminance + 8) / 17  (round to nearest palette entry)
inline uint8_t argb_to_palette(uint32_t argb) {
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >>  8) & 0xFF;
    uint32_t b = (argb      ) & 0xFF;
    uint32_t lum = (r * 77 + g * 151 + b * 29) >> 8;
    return (uint8_t)((lum + 8) / 17);
}

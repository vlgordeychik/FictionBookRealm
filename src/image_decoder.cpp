#include "image_decoder.h"
#include "palette.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>

#ifndef ESP_PLATFORM
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "rom/tjpgd.h"
#endif

static const unsigned char base64_table[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,
    0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
    0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static std::vector<uint8_t> base64_decode(std::string_view src) {
    if (src.empty()) return {};
    std::vector<uint8_t> out;
    out.reserve(src.size() / 4 * 3);
    int val = 0, valb = -8;
    for (unsigned char c : src) {
        if (c == '=') break;
        unsigned char d = base64_table[c];
        if (d == 0 && c != 'A') continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

size_t ImageDecoder::base64_decode_inplace(uint8_t* data, size_t n) {
    if (!data || n == 0) return 0;
    size_t out = 0;
    int val = 0, valb = -8;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = data[i];
        if (c == '=') break;
        unsigned char d = base64_table[c];
        if (d == 0 && c != 'A') continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            data[out++] = (uint8_t)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return out;
}

DecodedImageBuffer ImageDecoder::decode(const std::string& base64_data) {
    if (base64_data.empty()) return {};
    auto raw = base64_decode(base64_data);
    return decode_raw(raw.data(), raw.size());
}

// Вписать src в max_w×max_h по большей стороне, nearest neighbour.
// Индексы палитры копируются напрямую: буфер кадра тоже палитровый, так
// что конвертация RGB→палитра не нужна (и дорога).
bool ImageDecoder::downscale_palette(const PaletteImage& src, int max_w, int max_h,
                                     PaletteImage& out) {
    out.indices.clear();
    out.width = out.height = 0;
    if (src.empty() || max_w <= 0 || max_h <= 0) return false;
    if ((size_t)src.width * (size_t)src.height > src.indices.size()) return false;

    int fit_w, fit_h;
    if (src.width <= max_w && src.height <= max_h) {
        fit_w = src.width;      // мельче бокса — не увеличиваем
        fit_h = src.height;
    } else {
        const int64_t by_h = (int64_t)src.width * max_h / src.height;
        if (by_h <= max_w) {
            fit_h = max_h;
            fit_w = (int)by_h;
        } else {
            fit_w = max_w;
            fit_h = (int)((int64_t)src.height * max_w / src.width);
        }
    }
    if (fit_w < 1) fit_w = 1;
    if (fit_h < 1) fit_h = 1;

    out.indices.assign((size_t)fit_w * fit_h, 15 /* kOffWhite */);
    for (int y = 0; y < fit_h; ++y) {
        int sy = (int)((int64_t)y * src.height / fit_h);
        if (sy >= src.height) sy = src.height - 1;
        const uint8_t* srow = &src.indices[(size_t)sy * src.width];
        uint8_t* drow = &out.indices[(size_t)y * fit_w];
        for (int x = 0; x < fit_w; ++x) {
            int sx = (int)((int64_t)x * src.width / fit_w);
            if (sx >= src.width) sx = src.width - 1;
            drow[x] = srow[sx];
        }
    }
    out.width = fit_w;
    out.height = fit_h;
    return true;
}

bool ImageDecoder::rgba_to_palette(const std::vector<uint8_t>& rgba, int w, int h,
                                   int max_w, int max_h, PaletteImage& out) {
    out.indices.clear();
    out.width = out.height = 0;
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0) return false;
    if (rgba.size() < (size_t)w * h * 4) return false;

    int fit_w, fit_h;
    if (w <= max_w && h <= max_h) {
        fit_w = w; fit_h = h;
    } else if ((int64_t)w * max_h / h <= max_w) {
        fit_h = max_h;
        fit_w = (int)((int64_t)w * max_h / h);
    } else {
        fit_w = max_w;
        fit_h = (int)((int64_t)h * max_w / w);
    }
    if (fit_w < 1) fit_w = 1;
    if (fit_h < 1) fit_h = 1;

    out.indices.assign((size_t)fit_w * fit_h, 15 /* kOffWhite */);
    for (int y = 0; y < fit_h; ++y) {
        int sy = (int)((int64_t)y * h / fit_h);
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < fit_w; ++x) {
            int sx = (int)((int64_t)x * w / fit_w);
            if (sx >= w) sx = w - 1;
            const uint8_t* s = &rgba[((size_t)sy * w + sx) * 4];
            // a < 128 — прозрачный пиксель, оставляем фон полки
            if (s[3] < 128) continue;
            out.indices[(size_t)y * fit_w + x] =
                argb_to_palette(0xFF000000u |
                                ((uint32_t)s[2] << 16) |   // R
                                ((uint32_t)s[1] << 8) |    // G
                                 (uint32_t)s[0]);          // B
        }
    }
    out.width = fit_w;
    out.height = fit_h;
    return true;
}

bool ImageDecoder::probe_raw(const uint8_t* data, size_t n, int& w, int& h) {
#if defined(ESP_PLATFORM)
    // No stb on ESP32 — the reader decodes covers/illustrations only
    // through decode_fit_palette()'s tjpgd/DC path. probe_raw is a
    // win32-only helper for the win32 stb fallbacks.
    (void)data; (void)n;
    w = h = 0;
    return false;
#else
    w = h = 0;
    if (!data || n == 0) return false;
    return stbi_info_from_memory(data, (int)n, &w, &h, nullptr) != 0 &&
           w > 0 && h > 0;
#endif
}

DecodedImageBuffer ImageDecoder::decode_raw(const uint8_t* data, size_t n) {
#if defined(ESP_PLATFORM)
    // No stb on ESP32 (memory-heavy, OOM risk); the palette paths stream
    // with tjpgd/DC instead. This full-color helper is win32-only.
    (void)data; (void)n;
    return DecodedImageBuffer{};
#else
    DecodedImageBuffer result;
    if (!data || n == 0) return result;

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, (int)n, &w, &h, &ch, 4);

    if (!pixels || w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return result;
    }

    result.width = w;
    result.height = h;
    result.pixels.resize((size_t)w * h * 4);
    std::memcpy(result.pixels.data(), pixels, result.pixels.size());
    stbi_image_free(pixels);

    // BGRA swizzle: stb outputs RGBA, framebuffer is BGRA
    for (size_t i = 0; i < result.pixels.size(); i += 4) {
        std::swap(result.pixels[i], result.pixels[i + 2]);
    }

    return result;
#endif
}

// ─── 16-color fit decode (reader covers / illustrations) ─────────

#ifdef ESP_PLATFORM
namespace {

struct PalJpgCtx {
    const uint8_t* src;
    size_t src_n;
    size_t src_pos;
    uint8_t* dst;   // palette indices, fit_w × fit_h
    int fit_w;
    int fit_h;
    int out_w;
    int out_h;
};

static UINT pal_jpg_in(JDEC* jd, BYTE* buf, UINT len) {
    auto* s = static_cast<PalJpgCtx*>(jd->device);
    if (!s || s->src_pos >= s->src_n) return 0;
    size_t rem = s->src_n - s->src_pos;
    if (len > rem) len = static_cast<UINT>(rem);
    if (buf) std::memcpy(buf, s->src + s->src_pos, len);
    s->src_pos += len;
    return len;
}

// MCU block → nearest 16-gray index. No truecolor intermediate.
static UINT pal_jpg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* c = static_cast<PalJpgCtx*>(jd->device);
    if (!c || !c->dst || c->out_w <= 0 || c->out_h <= 0) return 0;
    const auto* rgb = static_cast<const uint8_t*>(bitmap);
    const uint32_t w = rect->right - rect->left + 1;
    const uint32_t h = rect->bottom - rect->top + 1;
    for (uint32_t y = 0; y < h; ++y) {
        const int sy = static_cast<int>(rect->top) + static_cast<int>(y);
        if (sy < 0 || sy >= c->out_h) continue;
        const int dy = static_cast<int>(
            static_cast<int64_t>(sy) * c->fit_h / c->out_h);
        if (dy < 0 || dy >= c->fit_h) continue;
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = static_cast<int>(rect->left) + static_cast<int>(x);
            if (sx < 0 || sx >= c->out_w) continue;
            const int dx = static_cast<int>(
                static_cast<int64_t>(sx) * c->fit_w / c->out_w);
            if (dx < 0 || dx >= c->fit_w) continue;
            const uint8_t* p = rgb + (y * w + x) * 3;
            c->dst[static_cast<size_t>(dy) * c->fit_w + dx] =
                argb_to_palette(0xFF000000u |
                                ((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8) |
                                (uint32_t)p[2]);
        }
    }
    return 1;
}

static bool decode_jpeg_palette_tjpgd(const uint8_t* raw, size_t raw_n,
                                      int max_w, int max_h,
                                      PaletteImage& out) {
    if (!raw || raw_n < 4 || raw[0] != 0xFF || raw[1] != 0xD8) return false;
    if (max_w <= 0 || max_h <= 0) return false;

    constexpr UINT kPool = 4096;
    PalJpgCtx ctx{};
    ctx.src = raw;
    ctx.src_n = raw_n;
    ctx.src_pos = 0;

    JDEC jd;
    void* pool = std::malloc(kPool);
    if (!pool) return false;
    JRESULT jr = jd_prepare(&jd, pal_jpg_in, pool, kPool, &ctx);
    if (jr != JDR_OK) {
        std::free(pool);
        return false;
    }
    if (jd.width == 0 || jd.height == 0) {
        std::free(pool);
        return false;
    }

    const int src_w = jd.width;
    const int src_h = jd.height;
    int fit_h = max_h;
    int fit_w = static_cast<int>(
        static_cast<int64_t>(src_w) * max_h / src_h + 0.5);
    if (fit_w > max_w) {
        fit_w = max_w;
        fit_h = static_cast<int>(
            static_cast<int64_t>(src_h) * max_w / src_w + 0.5);
    }
    if (fit_w < 1) fit_w = 1;
    if (fit_h < 1) fit_h = 1;

    uint8_t scale = 0;
    for (uint8_t s = 1; s <= 3; ++s) {
        const int sw = (src_w + (1 << s) - 1) >> s;
        const int sh = (src_h + (1 << s) - 1) >> s;
        if (sw >= fit_w && sh >= fit_h) scale = s;
        else break;
    }
    ctx.fit_w = fit_w;
    ctx.fit_h = fit_h;
    ctx.out_w = (src_w + (1 << scale) - 1) >> scale;
    ctx.out_h = (src_h + (1 << scale) - 1) >> scale;

    const size_t need = static_cast<size_t>(fit_w) * fit_h;
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < need + kPool + 64 * 1024) {
        std::free(pool);
        return false;
    }

    out.indices.assign(need, kOffWhite);
    ctx.dst = out.indices.data();
    jr = jd_decomp(&jd, pal_jpg_out, scale);
    std::free(pool);
    if (jr != JDR_OK && jr != JDR_INTR) {
        out.indices.clear();
        out.width = out.height = 0;
        return false;
    }
    out.width = fit_w;
    out.height = fit_h;
    return true;
}

} // namespace
#endif // ESP_PLATFORM

// ─── Progressive JPEG: DC-only decode → low-res RGB ──────────────
// tjpgd returns JDR_FMT3 for progressive; full stb needs w*h*4 (OOM on
// 1500×2376). First progressive scans are DC (Se=0): one Huffman category
// per 8×8 block → image ≈ JPEG/8, ~w*h/64 RAM. Enough for 16-color covers.

namespace {

struct Jhuff {
    uint8_t bits[17]{};
    uint8_t vals[256]{};
    bool ok = false;
};

struct Jbr {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint32_t acc = 0;
    int nbits = 0;
    bool err = false;

    int bit() {
        if (nbits == 0) {
            if (p >= end) { err = true; return 0; }
            uint8_t c = *p++;
            if (c == 0xFF) {
                if (p >= end) { err = true; return 0; }
                uint8_t n = *p++;
                if (n == 0x00) {
                    c = 0xFF;
                } else if (n >= 0xD0 && n <= 0xD7) {
                    // RSTn — byte-aligned restart; drop bits, caller resets preds
                    nbits = 0;
                    acc = 0;
                    // Re-process this marker byte path: treat as resync
                    // (entropy continues after RSTn)
                    return bit();
                } else {
                    // Real marker (EOI/etc.) — push back and fail this scan
                    p -= 2;
                    err = true;
                    return 0;
                }
            }
            acc = c;
            nbits = 8;
        }
        --nbits;
        return (acc >> nbits) & 1;
    }

    int receive(int n) {
        int v = 0;
        while (n-- > 0) {
            v = (v << 1) | bit();
            if (err) return 0;
        }
        return v;
    }
};

static int j_extend(int v, int n) {
    if (n <= 0) return 0;
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static bool j_huff_dec(Jbr& br, const Jhuff& h, int& sym) {
    if (!h.ok || br.err) return false;
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 16; ++len) {
        code = (code << 1) | br.bit();
        if (br.err) return false;
        int count = h.bits[len];
        if (count > 0) {
            int d = code - first;
            if (d >= 0 && d < count) {
                sym = h.vals[index + d];
                return true;
            }
        }
        index += count;
        first = (first + count) << 1;
    }
    return false;
}

static uint8_t j_clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

} // namespace

bool ImageDecoder::decode_jpeg_dc_rgb(const uint8_t* raw, size_t n,
                                      std::vector<uint8_t>& rgb,
                                      int& w, int& h) {
    rgb.clear();
    w = h = 0;
    if (!raw || n < 4 || raw[0] != 0xFF || raw[1] != 0xD8) return false;

    int qt[4][64];
    std::memset(qt, 0, sizeof(qt));
    bool qt_has[4] = {};
    Jhuff dc_tbl[4];
    Jhuff ac_tbl[4]; // parsed but unused (DC-only)

    int jw = 0, jh = 0, ncomp = 0;
    int cid[3] = {}, ch[3] = {}, cv[3] = {}, cqt[3] = {};
    bool have_sof = false;
    bool progressive = false;

    int16_t* dc[3] = {};
    int bw[3] = {}, bh[3] = {};
    bool dc_got[3] = {};

    const uint8_t* p = raw;
    const uint8_t* end = raw + n;
    auto rd16 = [&](const uint8_t* q) {
        return (q[0] << 8) | q[1];
    };

    // Free helper for all early-outs
    auto cleanup = [&]() {
        for (int c = 0; c < 3; ++c) {
            delete[] dc[c];
            dc[c] = nullptr;
        }
    };

    while (p + 4 <= end) {
        if (p[0] != 0xFF) { ++p; continue; }
        uint8_t m = p[1];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;
            continue;
        }
        if (m == 0xD9) break; // EOI
        if (p + 4 > end) break;
        int seg = rd16(p + 2);
        if (seg < 2 || p + 2 + seg > end) break;
        const uint8_t* seg_p = p + 4;
        const uint8_t* seg_end = p + 2 + seg;

        if (m == 0xDB) { // DQT
            const uint8_t* q = seg_p;
            while (q + 1 < seg_end) {
                int pq = q[0] >> 4;
                int tq = q[0] & 15;
                q++;
                if (tq > 3) break;
                if (pq == 0) {
                    if (q + 64 > seg_end) break;
                    for (int i = 0; i < 64; ++i) qt[tq][i] = q[i];
                    q += 64;
                } else {
                    if (q + 128 > seg_end) break;
                    for (int i = 0; i < 64; ++i)
                        qt[tq][i] = rd16(q + i * 2);
                    q += 128;
                }
                qt_has[tq] = true;
            }
        } else if (m == 0xC4) { // DHT
            const uint8_t* q = seg_p;
            while (q + 17 <= seg_end) {
                int tc = q[0] >> 4;
                int th = q[0] & 15;
                q++;
                if (th > 3 || tc > 1) break;
                Jhuff* dst = tc ? &ac_tbl[th] : &dc_tbl[th];
                int total = 0;
                for (int i = 1; i <= 16; ++i) {
                    dst->bits[i] = q[i - 1];
                    total += q[i - 1];
                }
                q += 16;
                if (total > 256 || q + total > seg_end) break;
                for (int i = 0; i < total; ++i) dst->vals[i] = q[i];
                q += total;
                dst->ok = true;
            }
        } else if (m == 0xC2 || m == 0xC0 || m == 0xC1) { // SOF2/SOF0/SOF1
            progressive = (m == 0xC2);
            if (seg < 8) break;
            int prec = seg_p[0];
            jh = rd16(seg_p + 1);
            jw = rd16(seg_p + 3);
            ncomp = seg_p[5];
            if (prec != 8 || jw <= 0 || jh <= 0 || ncomp < 1 || ncomp > 3)
                { cleanup(); return false; }
            const uint8_t* c = seg_p + 6;
            if (c + ncomp * 3 > seg_end) { cleanup(); return false; }
            int hmax = 1, vmax = 1;
            for (int i = 0; i < ncomp; ++i) {
                cid[i] = c[i * 3 + 0];
                ch[i] = c[i * 3 + 1] >> 4;
                cv[i] = c[i * 3 + 1] & 15;
                cqt[i] = c[i * 3 + 2];
                if (ch[i] < 1 || ch[i] > 4 || cv[i] < 1 || cv[i] > 4) {
                    cleanup(); return false;
                }
                if (ch[i] > hmax) hmax = ch[i];
                if (cv[i] > vmax) vmax = cv[i];
            }
            int mcux = (jw + 8 * hmax - 1) / (8 * hmax);
            int mcuy = (jh + 8 * vmax - 1) / (8 * vmax);
            for (int i = 0; i < ncomp; ++i) {
                bw[i] = mcux * ch[i];
                bh[i] = mcuy * cv[i];
                size_t cells = (size_t)bw[i] * bh[i];
                if (cells == 0 || cells > 400000) { cleanup(); return false; }
                dc[i] = new (std::nothrow) int16_t[cells];
                if (!dc[i]) { cleanup(); return false; }
                std::memset(dc[i], 0, cells * sizeof(int16_t));
            }
            have_sof = true;
        } else if (m == 0xDA) { // SOS
            if (!have_sof || seg < 4) { p = seg_end; continue; }
            int ns = seg_p[0];
            if (ns < 1 || ns > ncomp || 1 + ns * 2 + 3 > seg) {
                p = seg_end; continue;
            }
            int scid[3] = {}, ssel[3] = {}; // sel = (td<<4)|ta
            for (int i = 0; i < ns; ++i) {
                scid[i] = seg_p[1 + i * 2];
                ssel[i] = seg_p[2 + i * 2];
            }
            int ss = seg_p[1 + ns * 2];
            int se = seg_p[2 + ns * 2];
            int ahal = seg_p[3 + ns * 2];
            int ah = ahal >> 4, al = ahal & 15;
            const uint8_t* scan_start = seg_end;

            // DC-only initial scans only (progressive refinement skipped)
            bool dc_scan = (ss == 0 && se == 0 && ah == 0);
            if (!progressive) {
                // Baseline entropy — still DC+AC in one scan; we only pull
                // DC then abandon AC (acceptable fallback when tjpgd failed).
                dc_scan = true;
                ss = 0; se = 0;
            }

            if (dc_scan) {
                // Map SOS component ids → indices
                int pred[3] = {};
                int comp_idx[3] = {};
                bool skip_scan = false;
                for (int i = 0; i < ns; ++i) {
                    int idx = -1;
                    for (int c = 0; c < ncomp; ++c)
                        if (cid[c] == scid[i]) { idx = c; break; }
                    if (idx < 0 || !dc[idx]) { skip_scan = true; break; }
                    comp_idx[i] = idx;
                }

                if (!skip_scan) {
                    // Decode interleaved DC (or single-component scan)
                    // Number of blocks in this scan = max over components
                    // in MCU order. Progressive non-interleaved: ns==1.
                    int total_blocks = 0;
                    if (ns == 1) {
                        total_blocks = bw[comp_idx[0]] * bh[comp_idx[0]];
                    } else {
                        // Interleaved: MCU count × ncomp-in-scan
                        int hmax = 1, vmax = 1;
                        for (int i = 0; i < ncomp; ++i) {
                            if (ch[i] > hmax) hmax = ch[i];
                            if (cv[i] > vmax) vmax = cv[i];
                        }
                        int mcux = (jw + 8 * hmax - 1) / (8 * hmax);
                        int mcuy = (jh + 8 * vmax - 1) / (8 * vmax);
                        total_blocks = mcux * mcuy * ns; // handled below
                        // Use MCU loop instead
                        Jbr br;
                        br.p = scan_start;
                        br.end = end;
                        for (int y = 0; y < mcuy && !br.err; ++y) {
                            for (int x = 0; x < mcux && !br.err; ++x) {
                                for (int s = 0; s < ns; ++s) {
                                    int ci = comp_idx[s];
                                    int td = ssel[s] >> 4;
                                    if (td > 3) { br.err = true; break; }
                                    for (int by = 0; by < cv[ci]; ++by) {
                                        for (int bx = 0; bx < ch[ci]; ++bx) {
                                            int blk_x = x * ch[ci] + bx;
                                            int blk_y = y * cv[ci] + by;
                                            if (blk_x >= bw[ci] || blk_y >= bh[ci])
                                                continue;
                                            int sym = 0;
                                            if (!j_huff_dec(br, dc_tbl[td], sym)) {
                                                br.err = true; break;
                                            }
                                            int diff = 0;
                                            if (sym > 0) {
                                                int v = br.receive(sym);
                                                if (br.err) { break; }
                                                diff = j_extend(v, sym);
                                            }
                                            pred[s] = (int16_t)(pred[s] + diff);
                                            dc[ci][(size_t)blk_y * bw[ci] + blk_x] =
                                                (int16_t)pred[s];
                                            dc_got[ci] = true;
                                        }
                                        if (br.err) break;
                                    }
                                    if (br.err) break;
                                }
                            }
                        }
                        p = br.err && br.p < end ? br.p : scan_start;
                        // Resync: find next marker from after SOS header
                        // (br.p may sit on marker already)
                        if (br.err && br.p + 2 <= end && br.p[0] == 0xFF) {
                            // already at marker
                        } else {
                            const uint8_t* q = scan_start;
                            while (q + 1 < end) {
                                if (q[0] == 0xFF && q[1] != 0x00 &&
                                    !(q[1] >= 0xD0 && q[1] <= 0xD7)) {
                                    p = q;
                                    break;
                                }
                                ++q;
                            }
                            if (q + 1 >= end) { cleanup(); return false; }
                        }
                        p = scan_start;
                        // Scan entropy until marker
                        {
                            const uint8_t* q = scan_start;
                            while (q + 1 < end) {
                                if (q[0] == 0xFF) {
                                    uint8_t nx = q[1];
                                    if (nx != 0x00 &&
                                        !(nx >= 0xD0 && nx <= 0xD7)) {
                                        p = q;
                                        break;
                                    }
                                    q += (nx == 0x00) ? 2 : 1;
                                } else {
                                    ++q;
                                }
                            }
                            if (q + 1 >= end) { cleanup(); return false; }
                        }
                        continue;
                    }

                    if (ns == 1) {
                        int ci = comp_idx[0];
                        int td = ssel[0] >> 4;
                        if (td > 3) { p = seg_end; continue; }
                        Jbr br;
                        br.p = scan_start;
                        br.end = end;
                        int idx = 0;
                        int cells = bw[ci] * bh[ci];
                        // Progressive non-interleaved: raster of blocks
                        for (int by = 0; by < bh[ci] && !br.err && idx < cells; ++by) {
                            for (int bx = 0; bx < bw[ci] && !br.err && idx < cells;
                                 ++bx, ++idx) {
                                int sym = 0;
                                if (!j_huff_dec(br, dc_tbl[td], sym)) {
                                    br.err = true; break;
                                }
                                int diff = 0;
                                if (sym > 0) {
                                    int v = br.receive(sym);
                                    if (br.err) break;
                                    diff = j_extend(v, sym);
                                }
                                pred[0] += diff;
                                dc[ci][(size_t)by * bw[ci] + bx] =
                                    (int16_t)pred[0];
                            }
                        }
                        dc_got[ci] = true;
                        // Skip to next marker
                        const uint8_t* q = br.p;
                        if (q < scan_start) q = scan_start;
                        while (q + 1 < end) {
                            if (q[0] == 0xFF) {
                                uint8_t nx = q[1];
                                if (nx != 0x00 &&
                                    !(nx >= 0xD0 && nx <= 0xD7)) {
                                    p = q;
                                    break;
                                }
                                q += (nx == 0x00) ? 2 : 1;
                            } else {
                                ++q;
                            }
                        }
                        if (q + 1 >= end) { cleanup(); return false; }
                        continue;
                    }
                }
            }

            // Skip non-DC / unknown scan entropy
            {
                const uint8_t* q = scan_start;
                while (q + 1 < end) {
                    if (q[0] == 0xFF) {
                        uint8_t nx = q[1];
                        if (nx != 0x00 && !(nx >= 0xD0 && nx <= 0xD7)) {
                            p = q;
                            break;
                        }
                        q += (nx == 0x00) ? 2 : 1;
                    } else {
                        ++q;
                    }
                }
                if (q + 1 >= end) { cleanup(); return false; }
                continue;
            }
        }

        p = seg_end;
    }

    if (!have_sof || jw <= 0 || jh <= 0) { cleanup(); return false; }

    // Need DC for component 0 (luma / only component)
    if (!dc[0] || !dc_got[0]) {
        // Single-component JPEGs sometimes use non-1 ids; accept any got
        bool any = false;
        for (int i = 0; i < ncomp; ++i)
            if (dc_got[i]) { any = true; break; }
        if (!any) { cleanup(); return false; }
    }

    // Output at luma block resolution
    int ow = bw[0];
    int oh = bh[0];
    if (ow <= 0 || oh <= 0 || (size_t)ow * oh > 500000) {
        cleanup(); return false;
    }

#ifdef ESP_PLATFORM
    const size_t need = (size_t)ow * oh * 3 + 64 * 1024;
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < need) { cleanup(); return false; }
#endif

    rgb.assign((size_t)ow * oh * 3, 0);

    // Decode each luma block DC → gray; chroma DC → upsample
    auto dc_to_spatial = [&](int comp, int blk_x, int blk_y) -> int {
        int16_t d = dc[comp][(size_t)blk_y * bw[comp] + blk_x];
        int q0 = qt[cqt[comp]][0];
        if (q0 <= 0) q0 = 1;
        int f = (int)d * q0;
        return j_clamp8((f / 8) + 128);
    };

    if (ncomp == 1) {
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                uint8_t v = (uint8_t)dc_to_spatial(0, x, y);
                size_t o = ((size_t)y * ow + x) * 3;
                rgb[o] = rgb[o + 1] = rgb[o + 2] = v;
            }
        }
    } else {
        // Component 0 = Y (or R), 1 = Cb, 2 = Cr — standard JPEG order
        // Chroma block coords scale by sampling ratio
        for (int y = 0; y < oh; ++y) {
            for (int x = 0; x < ow; ++x) {
                int yy = dc_to_spatial(0, x, y);
                int cb = 128, cr = 128;
                if (ncomp >= 2 && dc_got[1] && bw[1] > 0 && bh[1] > 0) {
                    int cx = x * bw[1] / ow;
                    int cy = y * bh[1] / oh;
                    if (cx >= bw[1]) cx = bw[1] - 1;
                    if (cy >= bh[1]) cy = bh[1] - 1;
                    cb = dc_to_spatial(1, cx, cy);
                }
                if (ncomp >= 3 && dc_got[2] && bw[2] > 0 && bh[2] > 0) {
                    int cx = x * bw[2] / ow;
                    int cy = y * bh[2] / oh;
                    if (cx >= bw[2]) cx = bw[2] - 1;
                    if (cy >= bh[2]) cy = bh[2] - 1;
                    cr = dc_to_spatial(2, cx, cy);
                }
                int r = yy + ((1436 * (cr - 128)) >> 10);
                int g = yy - ((352 * (cb - 128)) >> 10) - ((731 * (cr - 128)) >> 10);
                int b = yy + ((1815 * (cb - 128)) >> 10);
                size_t o = ((size_t)y * ow + x) * 3;
                rgb[o] = (uint8_t)j_clamp8(r);
                rgb[o + 1] = (uint8_t)j_clamp8(g);
                rgb[o + 2] = (uint8_t)j_clamp8(b);
            }
        }
    }

    cleanup();
    w = ow;
    h = oh;
    return true;
}

// (stb fallback removed — ESP32 has no stb; the palette path is
// tjpgd/DC streaming only. win32 covers non-JPEG via ImageDecoder::decode.)

bool ImageDecoder::decode_fit_palette(std::string& b64, int max_w, int max_h,
                                      PaletteImage& out) {
    out.indices.clear();
    out.width = out.height = 0;
    if (b64.empty() || max_w <= 0 || max_h <= 0) return false;

#ifdef ESP_PLATFORM
    const size_t b64_len = b64.size();
    size_t raw_n = base64_decode_inplace(
        reinterpret_cast<uint8_t*>(&b64[0]), b64.size());
    if (raw_n < 4) return false;
    b64.resize(raw_n); // size only — capacity kept, no realloc

    if (b64[0] == '\xFF' && b64[1] == '\xD8') {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(b64.data());
        if (decode_jpeg_palette_tjpgd(raw, raw_n, max_w, max_h, out)) {
#ifdef ESP_PLATFORM
            printf("fbr: pal_jpeg ok %dx%d b64=%u heap=%u\n",
                   out.width, out.height, (unsigned)b64_len,
                   (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
            return true;
        }
        // Progressive (tjpgd JDR_FMT3) or tjpgd fail → DC-only ~1/8 res
        std::vector<uint8_t> rgb;
        int rw = 0, rh = 0;
        if (decode_jpeg_dc_rgb(raw, raw_n, rgb, rw, rh) && rw > 0 && rh > 0) {
            // Nearest fit into max_w×max_h
            int fit_h = max_h;
            int fit_w = (int)((int64_t)rw * max_h / rh + 0.5);
            if (fit_w > max_w) {
                fit_w = max_w;
                fit_h = (int)((int64_t)rh * max_w / rw + 0.5);
            }
            if (fit_w < 1) fit_w = 1;
            if (fit_h < 1) fit_h = 1;
            out.indices.assign((size_t)fit_w * fit_h, kOffWhite);
            for (int y = 0; y < fit_h; ++y) {
                int sy = (int)((int64_t)y * rh / fit_h);
                if (sy >= rh) sy = rh - 1;
                for (int x = 0; x < fit_w; ++x) {
                    int sx = (int)((int64_t)x * rw / fit_w);
                    if (sx >= rw) sx = rw - 1;
                    const uint8_t* px = &rgb[((size_t)sy * rw + sx) * 3];
                    out.indices[(size_t)y * fit_w + x] = argb_to_palette(
                        0xFF000000u | ((uint32_t)px[0] << 16) |
                        ((uint32_t)px[1] << 8) | (uint32_t)px[2]);
                }
            }
            out.width = fit_w;
            out.height = fit_h;
#ifdef ESP_PLATFORM
            printf("fbr: pal_dc_jpeg ok %dx%d→%dx%d b64=%u heap=%u\n",
                   rw, rh, fit_w, fit_h, (unsigned)b64_len,
                   (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
            return true;
        }
#ifdef ESP_PLATFORM
        printf("fbr: pal tjpgd+dc fail raw=%u heap=%u\n",
               (unsigned)raw_n, (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
    }
    // Non-JPEG, or tjpgd+DC both failed. ESP32 has no stb (memory-heavy)
    // — the palette path is streaming-only, so fail. win32 uses stb below.
    printf("fbr: pal decode FAIL raw=%u heap=%u\n",
           (unsigned)raw_n,
#if defined(ESP_PLATFORM)
           (unsigned)esp_get_free_heap_size()
#else
           0u
#endif
    );
    fflush(stdout);
    return false;
#else
    auto decoded = decode(b64);
    if (decoded.pixels.empty()) return false;
    auto resized = resize(decoded, max_h, max_w);
    if (resized.pixels.empty()) return false;
    const size_t n = (size_t)resized.width * resized.height;
    out.indices.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t si = i * 4;
        out.indices[i] = argb_to_palette(
            0xFF000000u |
            ((uint32_t)resized.pixels[si + 2] << 16) |
            ((uint32_t)resized.pixels[si + 1] << 8) |
            (uint32_t)resized.pixels[si + 0]);
    }
    out.width = resized.width;
    out.height = resized.height;
    return true;
#endif
}

DecodedImageBuffer ImageDecoder::resize(const DecodedImageBuffer& src,
                                         int target_h, int content_w) {
    DecodedImageBuffer result;
    if (src.width <= 0 || src.height <= 0) return result;

    float scale = (float)target_h / src.height;
    int new_w = (int)(src.width * scale + 0.5f);
    int new_h = target_h;
    if (new_w > content_w) {
        new_w = content_w;
        scale = (float)new_w / src.width;
        new_h = (int)(src.height * scale + 0.5f);
    }

    result.width = new_w;
    result.height = new_h;
    result.pixels.resize((size_t)new_w * new_h * 4);

    // Bilinear resize (much better quality than nearest-neighbor)
    for (int y = 0; y < new_h; ++y) {
        float syf = y / scale;
        int sy0 = (int)syf;
        int sy1 = sy0 + 1;
        if (sy0 >= src.height) sy0 = src.height - 1;
        if (sy1 >= src.height) sy1 = src.height - 1;
        float fy = syf - sy0;
        for (int x = 0; x < new_w; ++x) {
            float sxf = x / scale;
            int sx0 = (int)sxf;
            int sx1 = sx0 + 1;
            if (sx0 >= src.width) sx0 = src.width - 1;
            if (sx1 >= src.width) sx1 = src.width - 1;
            float fx = sxf - sx0;

            size_t s00 = ((size_t)sy0 * src.width + sx0) * 4;
            size_t s01 = ((size_t)sy0 * src.width + sx1) * 4;
            size_t s10 = ((size_t)sy1 * src.width + sx0) * 4;
            size_t s11 = ((size_t)sy1 * src.width + sx1) * 4;
            size_t dd  = ((size_t)y * new_w + x) * 4;

            for (int c = 0; c < 4; ++c) {
                float v = (1-fx)*(1-fy)*src.pixels[s00+c]
                        +    fx *(1-fy)*src.pixels[s01+c]
                        + (1-fx)*   fy *src.pixels[s10+c]
                        +    fx *   fy *src.pixels[s11+c];
                result.pixels[dd+c] = (uint8_t)(v + 0.5f);
            }
        }
    }

    return result;
}

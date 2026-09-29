#include "cover_loader.h"
#include "image_decoder.h"
#include "filesys/filesystem.h"
#include "fb2zip.h"
#include "config.h"
#include "pugixml.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

#ifdef ESP_PLATFORM
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "rom/tjpgd.h"
#define COVER_LOGI(fmt, ...) ESP_LOGI("cover", fmt, ##__VA_ARGS__)
#else
#define COVER_LOGI(fmt, ...) printf("fbr: " fmt "\n", ##__VA_ARGS__)
#endif

// Streaming cover load: never keep the whole FB2 in RAM.
// 1) read description head only (</description> or cap)
// 2) pugi-parse that head for annotation + cover href
// 3) scan for matching <binary id="..."> and collect base64 with a hard cap
namespace {

constexpr size_t kHeadCap  = 128 * 1024;
// Perumov cover base64 >768KB; shelf has ~3.2MB free. Reserve once from
// largest free block so appends never re-grow into bad_alloc/abort.
constexpr size_t kB64Cap   = 2 * 1024 * 1024;
constexpr size_t kChunk    = 16 * 1024;
constexpr size_t kB64Headroom = 512 * 1024;

static unsigned heap_u32() {
#ifdef ESP_PLATFORM
    return (unsigned)esp_get_free_heap_size();
#else
    return 0u;
#endif
}

// Portable substring search (no memmem — missing on MSVC / some newlib).
static const char* find_bytes(const char* hay, size_t hay_len,
                              const char* needle, size_t needle_len) {
    if (needle_len == 0) return hay;
    if (hay_len < needle_len) return nullptr;
    const char* end = hay + (hay_len - needle_len + 1);
    for (const char* p = hay; p < end; ++p) {
        if (*p == needle[0] && std::memcmp(p, needle, needle_len) == 0)
            return p;
    }
    return nullptr;
}

static void extract_text(pugi::xml_node node, std::string& out, size_t cap) {
    for (auto child : node.children()) {
        if (out.size() >= cap) return;
        if (child.type() == pugi::node_pcdata) {
            const char* v = child.value();
            size_t len = std::strlen(v);
            if (out.size() + len > cap) len = cap - out.size();
            out.append(v, len);
        } else {
            const char* name = child.name();
            if (std::strcmp(name, "p") == 0 || std::strcmp(name, "emphasis") == 0 ||
                std::strcmp(name, "strong") == 0 || std::strcmp(name, "a") == 0 ||
                std::strcmp(name, "title") == 0 || std::strcmp(name, "subtitle") == 0 ||
                std::strcmp(name, "epigraph") == 0 || std::strcmp(name, "cite") == 0 ||
                std::strcmp(name, "poem") == 0 || std::strcmp(name, "stanza") == 0 ||
                std::strcmp(name, "v") == 0 || std::strcmp(name, "text-author") == 0 ||
                std::strcmp(name, "date") == 0 || std::strcmp(name, "annotation") == 0) {
                extract_text(child, out, cap);
                if (std::strcmp(name, "p") == 0 && !out.empty() &&
                    out.size() < cap && out.back() != '\n')
                    out += '\n';
            }
        }
    }
}

// Fallback when description was cut before </description>.
static std::string crude_cover_id(const char* buf, size_t sz) {
    static const char kCp[] = "coverpage";
    const char* end = buf + sz;
    const char* p = buf;
    while (p < end) {
        const char* hit = find_bytes(p, (size_t)(end - p),
                                     kCp, sizeof(kCp) - 1);
        if (!hit) break;
        size_t window = (size_t)(end - hit);
        if (window > 512) window = 512;
        const char* lhref = find_bytes(hit, window, "l:href=", 7);
        const char* href = find_bytes(hit, window, "href=", 5);
        const char* qpos = nullptr;
        if (lhref && (!href || lhref < href)) qpos = lhref + 7;
        else if (href) qpos = href + 5;
        if (qpos && qpos < end) {
            char quote = *qpos;
            if (quote == '"' || quote == '\'') {
                const char* vstart = qpos + 1;
                const char* vend = static_cast<const char*>(
                    std::memchr(vstart, quote, (size_t)(end - vstart)));
                if (vend && vend > vstart) {
                    std::string id(vstart, (size_t)(vend - vstart));
                    if (!id.empty() && id[0] == '#') id.erase(0, 1);
                    if (!id.empty()) return id;
                }
            }
        }
        p = hit + 1;
    }
    return {};
}

// Parse description head (may be truncated) → annotation + cover_image_id.
static bool parse_cover_head(const char* buf, size_t sz, CoverResult& out) {
    if (sz == 0) return false;

    std::string xml;
    xml.reserve(sz + 64);
    xml.assign(buf, sz);
    if (xml.find("</description>") == std::string::npos)
        xml += "</description>";
    if (xml.find("</FictionBook>") == std::string::npos)
        xml += "</FictionBook>";

    pugi::xml_document doc;
    auto result = doc.load_buffer(xml.data(), xml.size(),
                                  pugi::parse_default | pugi::parse_declaration |
                                      pugi::parse_fragment);
    if (!result) {
        result = doc.load_buffer(xml.data(), xml.size(),
                                 pugi::parse_default | pugi::parse_declaration);
    }

    pugi::xml_node fb{};
    pugi::xml_node desc{};
    pugi::xml_node ti{};
    // Статус разбора НЕ проверяем намеренно. head обрезан на границе
    // kChunk, поэтому pugi возвращает «Start-end tags mismatch» — но дерево
    // он всё равно строит, и нужные узлы в нём есть. Раньше проверка
    // result глушила весь разбор, и аннотация не выводилась никогда, хотя
    // разметка была корректной.
    auto pick = [](pugi::xml_node root, const char* name) {
        for (pugi::xml_node n : root.children()) {
            if (n.type() != pugi::node_element) continue;
            if (std::strcmp(n.name(), name) == 0) return n;
            for (pugi::xml_node c : n.children())
                if (c.type() == pugi::node_element &&
                    std::strcmp(c.name(), name) == 0)
                    return c;
        }
        return pugi::xml_node{};
    };
    desc = pick(doc, "description");
    if (desc) {
        ti = desc.child("title-info");
        if (!ti) ti = desc;
    }

    if (ti) {
        auto annotation = ti.child("annotation");
        if (annotation) {
            extract_text(annotation, out.annotation, kAnnCap);
            while (!out.annotation.empty() &&
                   (out.annotation.back() == '\n' || out.annotation.back() == '\r'))
                out.annotation.pop_back();
        }
        auto cover_img = ti.child("coverpage").child("image");
        if (cover_img) {
            const char* href = cover_img.attribute("l:href").value();
            if (!href || !*href) href = cover_img.attribute("href").value();
            if (href && *href)
                out.cover_image_id = (href[0] == '#') ? (href + 1) : href;
        }
    }

    if (out.cover_image_id.empty())
        out.cover_image_id = crude_cover_id(buf, sz);

    return !out.annotation.empty() || !out.cover_image_id.empty();
}

struct ByteReader {
    virtual ~ByteReader() = default;
    virtual size_t size() const = 0;
    virtual bool read_at(size_t off, void* dst, size_t n, size_t* got) = 0;
};

struct FsReader final : ByteReader {
    fs::File* f;
    size_t sz;
    FsReader(fs::File* file, size_t s) : f(file), sz(s) {}
    size_t size() const override { return sz; }
    bool read_at(size_t off, void* dst, size_t n, size_t* got) override {
        if (!f->seek(static_cast<int64_t>(off), SEEK_SET)) return false;
        *got = f->read(dst, n);
        return true;
    }
};

struct FileReader final : ByteReader {
    FILE* f;
    size_t sz;
    FileReader(FILE* file, size_t s) : f(file), sz(s) {}
    size_t size() const override { return sz; }
    bool read_at(size_t off, void* dst, size_t n, size_t* got) override {
        if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) return false;
        *got = std::fread(dst, 1, n, f);
        return true;
    }
};

// Attribute id=... in a binary open-tag body (after "<binary").
static bool binary_tag_id_match(const char* tag, size_t tag_len,
                                const std::string& want) {
    static const char kKey[] = "id=";
    const char* end = tag + tag_len;
    const char* p = tag;
    while (p < end) {
        const char* hit = find_bytes(p, (size_t)(end - p), kKey, 3);
        if (!hit) return false;
        if (hit > tag) {
            char c = hit[-1];
            if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
                p = hit + 1;
                continue;
            }
        }
        const char* v = hit + 3;
        while (v < end && (*v == ' ' || *v == '\t')) ++v;
        if (v >= end) return false;
        if (*v == '"' || *v == '\'') {
            char quote = *v;
            const char* vs = v + 1;
            const char* ve = static_cast<const char*>(
                std::memchr(vs, quote, (size_t)(end - vs)));
            if (!ve) return false;
            if ((size_t)(ve - vs) == want.size() &&
                std::memcmp(vs, want.data(), want.size()) == 0)
                return true;
            p = ve + 1;
        } else {
            const char* vs = v;
            while (v < end && *v != ' ' && *v != '\t' && *v != '\n' &&
                   *v != '\r' && *v != '/' && *v != '>')
                ++v;
            if ((size_t)(v - vs) == want.size() &&
                std::memcmp(vs, want.data(), want.size()) == 0)
                return true;
            p = (v > vs) ? v : (vs + 1);
        }
    }
    return false;
}

// Grow-safe append: never let std::string reallocate into bad_alloc.
static bool b64_append(std::string& b64, const char* data, size_t n) {
    if (n == 0) return true;
    if (b64.size() + n > kB64Cap) return false;
#ifndef _WIN32
    if (b64.capacity() < b64.size() + n) {
        // Prefer largest free block (string needs one contiguous alloc).
        size_t largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        size_t need = (b64.size() + n) * 2 + 64;
        if (largest < need + kB64Headroom) {
            COVER_LOGI("b64_append OOM guard size=%u cap=%u need=%u largest=%u",
                       (unsigned)b64.size(), (unsigned)b64.capacity(),
                       (unsigned)need, (unsigned)largest);
            return false;
        }
    }
#endif
    b64.append(data, n);
    return true;
}

// One-shot reserve so Collect does not double-realloc (exceptions off).
static void b64_reserve_for_scan(std::string& b64) {
#ifndef _WIN32
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t want = kB64Cap;
    if (largest > kB64Headroom + 64 * 1024) {
        if (want > largest - kB64Headroom)
            want = largest - kB64Headroom;
    } else {
        want = 0;
    }
    if (want >= 64 * 1024) {
        b64.reserve(want); // size checked → must not throw/abort
        COVER_LOGI("scan: reserve want=%u largest=%u cap=%u",
                   (unsigned)want, (unsigned)largest,
                   (unsigned)b64.capacity());
    } else {
        COVER_LOGI("scan: reserve skip largest=%u", (unsigned)largest);
    }
#else
    b64.reserve(kB64Cap);
#endif
}

// Scan for <binary id="want"> and collect base64 (≤ kB64Cap).
static bool scan_cover_b64(ByteReader& rd, const std::string& want,
                           std::string& b64) {
    if (want.empty()) {
        COVER_LOGI("scan: empty want id");
        return false;
    }
    const size_t fsize = rd.size();
    std::vector<char> buf(kChunk);
    std::string carry;
    carry.reserve(kChunk + 64);

    enum class St { SeekOpen, InOpenTag, Collect, SkipBin };
    St st = St::SeekOpen;
    std::string tag_acc;
    tag_acc.reserve(256);

    size_t pos = 0;
    size_t bins_seen = 0;
    while (pos < fsize || !carry.empty()) {
        if (pos < fsize) {
            size_t want_n = kChunk;
            if (want_n > fsize - pos) want_n = fsize - pos;
            size_t got = 0;
            if (!rd.read_at(pos, buf.data(), want_n, &got)) return false;
            if (got == 0) break;
            carry.append(buf.data(), got);
            pos += got;
        }

        size_t scan = 0;
        bool need_more = false;
        while (scan < carry.size()) {
            if (st == St::SeekOpen) {
                const char* hit = find_bytes(carry.data() + scan,
                                             carry.size() - scan,
                                             "<binary", 7);
                if (!hit) {
                    // keep tail for a split "<binary"
                    size_t len = carry.size() - scan;
                    size_t keep = (len < 16) ? len : 16;
                    carry.erase(0, carry.size() - keep);
                    scan = 0;
                    need_more = (pos < fsize);
                    break;
                }
                scan = (size_t)(hit - carry.data()) + 7;
                tag_acc.clear();
                st = St::InOpenTag;
                ++bins_seen;
            } else if (st == St::InOpenTag) {
                const char* data = carry.data() + scan;
                size_t len = carry.size() - scan;
                const char* gt = static_cast<const char*>(
                    std::memchr(data, '>', len));
                if (!gt) {
                    tag_acc.append(data, len);
                    if (tag_acc.size() > 1024) {
                        tag_acc.clear();
                        st = St::SeekOpen;
                        carry.erase(0, carry.size());
                        scan = 0;
                        break;
                    }
                    if (pos >= fsize) {
                        need_more = false;
                        break; // EOF mid-tag
                    }
                    // drop consumed prefix, wait for more bytes
                    carry.erase(0, carry.size());
                    scan = 0;
                    need_more = true;
                    break;
                }
                tag_acc.append(data, (size_t)(gt - data));
                bool match = binary_tag_id_match(tag_acc.data(),
                                                 tag_acc.size(), want);
                bool self_close =
                    !tag_acc.empty() &&
                    tag_acc[tag_acc.size() - 1] == '/' &&
                    (tag_acc.size() < 2 || tag_acc[tag_acc.size() - 2] != '/');
                // self_close: last char before '>' was '/', which we did NOT
                // append (we stop at '>'), so check last appended char.
                // Actually tag_acc excludes '>', so body ends with '/' for
                // <binary ... />. Correct.
                scan = (size_t)(gt - carry.data()) + 1;
                tag_acc.clear();
                if (match && !self_close) {
                    b64.clear();
                    b64_reserve_for_scan(b64);
                    st = St::Collect;
                    COVER_LOGI("scan: match id=%s at pos=%u bins=%u",
                               want.c_str(), (unsigned)pos,
                               (unsigned)bins_seen);
                } else if (match && self_close) {
                    COVER_LOGI("scan: matched empty self-close id=%s",
                               want.c_str());
                    return false;
                } else {
                    st = St::SkipBin;
                }
            } else if (st == St::Collect) {
                const char* data = carry.data() + scan;
                size_t len = carry.size() - scan;
                const char* close_hit = find_bytes(data, len, "</binary>", 9);
                if (!close_hit) {
                    // leave 8 bytes for a possible split "</binary>"
                    if (len <= 8) {
                        if (pos >= fsize) break;
                        size_t keep = len;
                        carry.erase(0, carry.size() - keep);
                        scan = 0;
                        need_more = true;
                        break;
                    }
                    size_t take = len - 8;
                    size_t room = kB64Cap - b64.size();
                    if (take > room) take = room;
                    if (take == 0) {
                        COVER_LOGI("scan: over cap b64=%u room=0 — drop",
                                   (unsigned)b64.size());
                        b64.clear();
                        st = St::SkipBin;
                        scan += (len > 8 ? len - 8 : 0);
                        continue;
                    }
                    if (!b64_append(b64, data, take)) {
                        COVER_LOGI("scan: append fail size=%u take=%u",
                                   (unsigned)b64.size(), (unsigned)take);
                        b64.clear();
                        st = St::SkipBin;
                        scan += take;
                        continue;
                    }
                    scan += take;
                    if (pos >= fsize) break;
                    // compact: consumed prefix
                    if (scan > 0) {
                        carry.erase(0, scan);
                        scan = 0;
                    }
                    need_more = true;
                    break;
                }
                size_t content = (size_t)(close_hit - data);
                size_t room = kB64Cap - b64.size();
                if (content > room) {
                    COVER_LOGI("scan: final over cap content=%u room=%u",
                               (unsigned)content, (unsigned)room);
                    b64.clear();
                    st = St::SkipBin;
                    scan += content;
                    continue;
                }
                if (!b64_append(b64, data, content)) {
                    COVER_LOGI("scan: final append fail content=%u",
                               (unsigned)content);
                    b64.clear();
                    st = St::SkipBin;
                    scan += content;
                    continue;
                }
                scan = (size_t)(close_hit - carry.data()) + 9;
                st = St::SeekOpen;
                if (!b64.empty()) {
                    COVER_LOGI("scan: collected b64=%u bins=%u",
                               (unsigned)b64.size(), (unsigned)bins_seen);
                    return true;
                }
            } else { // SkipBin
                const char* data = carry.data() + scan;
                size_t len = carry.size() - scan;
                const char* close_hit = find_bytes(data, len, "</binary>", 9);
                if (!close_hit) {
                    size_t keep = (len < 8) ? len : 8;
                    carry.erase(0, carry.size() - keep);
                    scan = 0;
                    if (pos >= fsize) {
                        carry.clear();
                        break;
                    }
                    need_more = true;
                    break;
                }
                scan = (size_t)(close_hit - carry.data()) + 9;
                st = St::SeekOpen;
            }
        }

        if (need_more && pos >= fsize) need_more = false;
        if (scan > 0 && scan < carry.size()) {
            // keep unconsumed tail
            carry.erase(0, scan);
        } else if (scan >= carry.size()) {
            carry.clear();
        }
        if (!need_more && pos >= fsize && carry.empty()) break;
        if (!need_more && pos >= fsize && scan == 0 && carry.size() < 16 &&
            st == St::SeekOpen && !find_bytes(carry.data(), carry.size(),
                                              "<binary", 7)) {
            break; // EOF and nothing pending
        }
    }
    COVER_LOGI("scan: done no match b64=%u bins=%u pos=%u/%u st=%d",
               (unsigned)b64.size(), (unsigned)bins_seen,
               (unsigned)pos, (unsigned)fsize, (int)st);
    return !b64.empty();
}

#ifdef ESP_PLATFORM
// tjpgd: stream JPEG → MCU blocks → dest BGRA (no full-res RGBA alloc).
// Peak: ~4KB pool + max_w*max_h*4. Fits when free heap is only ~1MB with
// the 2MB b64 string still held.
// jd.device is shared by in/out callbacks — one context holds both.
struct CoverJpgCtx {
    const uint8_t* src;
    size_t src_n;
    size_t src_pos;
    uint8_t* dst;
    int fit_w;
    int fit_h;
    int out_w;
    int out_h;
};

static UINT cover_jpg_in(JDEC* jd, BYTE* buf, UINT len) {
    auto* s = static_cast<CoverJpgCtx*>(jd->device);
    if (!s || s->src_pos >= s->src_n) return 0;
    size_t rem = s->src_n - s->src_pos;
    if (len > rem) len = static_cast<UINT>(rem);
    if (buf) std::memcpy(buf, s->src + s->src_pos, len); // buf==0 → skip
    s->src_pos += len;
    return len;
}

static UINT cover_jpg_out(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* c = static_cast<CoverJpgCtx*>(jd->device);
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
            uint8_t* d = c->dst +
                         (static_cast<size_t>(dy) * c->fit_w + dx) * 4;
            d[0] = p[2]; // BGRA
            d[1] = p[1];
            d[2] = p[0];
            d[3] = 255;
        }
    }
    return 1;
}
#endif // ESP_PLATFORM

// b64 is mutated in-place on ESP (base64 → raw JPEG in the same buffer).
static bool decode_cover_b64(std::string& b64, int max_w, int max_h,
                             CoverResult& out) {
    if (b64.empty() || max_w <= 0 || max_h <= 0) return false;
    const size_t b64_len = b64.size();

#ifdef ESP_PLATFORM
    size_t raw_n = ImageDecoder::base64_decode_inplace(
        reinterpret_cast<uint8_t*>(&b64[0]), b64.size());
    if (raw_n < 4) {
        COVER_LOGI("decode: b64 too small in=%u raw=%u",
                   (unsigned)b64_len, (unsigned)raw_n);
        return false;
    }
    b64.resize(raw_n); // size only — capacity kept, no realloc
    if (b64[0] != '\xFF' || b64[1] != '\xD8') {
        COVER_LOGI("decode: not jpeg magic raw=%u", (unsigned)raw_n);
        return false;
    }

    constexpr uint16_t kPool = 4096;
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t pix_cap = (size_t)max_w * max_h * 4;
    if (largest < kPool + pix_cap + 64 * 1024) {
        COVER_LOGI("decode: OOM guard raw=%u pix=%u largest=%u free=%u",
                   (unsigned)raw_n, (unsigned)pix_cap,
                   (unsigned)largest, (unsigned)heap_u32());
        return false;
    }

    // Fit box / scale first — fill ctx before prepare (device = &ctx).
    int src_w = 0;
    int src_h = 0;
    CoverJpgCtx ctx{};
    ctx.src = reinterpret_cast<const uint8_t*>(b64.data());
    ctx.src_n = raw_n;
    ctx.src_pos = 0;

    JDEC jd;
    void* pool = std::malloc(kPool);
    if (!pool) {
        COVER_LOGI("decode: pool OOM kPool=%u", (unsigned)kPool);
        return false;
    }
    JRESULT jr = jd_prepare(&jd, cover_jpg_in, pool, kPool, &ctx);
    if (jr != JDR_OK) {
        COVER_LOGI("decode: prepare jr=%d w=%u h=%u free=%u — try dc/stb",
                   static_cast<int>(jr),
                   static_cast<unsigned>(jd.width),
                   static_cast<unsigned>(jd.height),
                   (unsigned)heap_u32());
        std::free(pool);

        // Progressive (JDR_FMT3): DC-only ~1/8 res RGB → fit BGRA.
        std::vector<uint8_t> rgb;
        int rw = 0, rh = 0;
        if (ImageDecoder::decode_jpeg_dc_rgb(
                reinterpret_cast<const uint8_t*>(b64.data()), raw_n,
                rgb, rw, rh) &&
            rw > 0 && rh > 0) {
            int fit_h = max_h;
            int fit_w = (int)((int64_t)rw * max_h / rh + 0.5);
            if (fit_w > max_w) {
                fit_w = max_w;
                fit_h = (int)((int64_t)rh * max_w / rw + 0.5);
            }
            if (fit_w < 1) fit_w = 1;
            if (fit_h < 1) fit_h = 1;
            out.pixels.assign((size_t)fit_w * fit_h * 4, 0);
            for (int y = 0; y < fit_h; ++y) {
                int sy = (int)((int64_t)y * rh / fit_h);
                if (sy >= rh) sy = rh - 1;
                for (int x = 0; x < fit_w; ++x) {
                    int sx = (int)((int64_t)x * rw / fit_w);
                    if (sx >= rw) sx = rw - 1;
                    const uint8_t* px = &rgb[((size_t)sy * rw + sx) * 3];
                    uint8_t* d = &out.pixels[((size_t)y * fit_w + x) * 4];
                    d[0] = px[2]; // BGRA
                    d[1] = px[1];
                    d[2] = px[0];
                    d[3] = 255;
                }
            }
            out.width = fit_w;
            out.height = fit_h;
            COVER_LOGI("decode: ok(dc) %dx%d from %dx%d raw=%u free=%u",
                       out.width, out.height, rw, rh, (unsigned)raw_n,
                       (unsigned)heap_u32());
            return true;
        }

        // Progressive / unsupported SOF → no stb on ESP32. The DC path
        // above already handled progressive covers at ~1/8 res, so a
        // total failure here means an unsupported image.
        COVER_LOGI("decode: tjpgd+dc fail raw=%u free=%u",
                   (unsigned)raw_n, (unsigned)heap_u32());
        return false;
    }

    src_w = jd.width;
    src_h = jd.height;
    if (src_w <= 0 || src_h <= 0) {
        COVER_LOGI("decode: bad dims %dx%d", src_w, src_h);
        std::free(pool);
        return false;
    }

    // Aspect-fit into max_w×max_h (same rule as ImageDecoder::resize).
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

    // Largest tjpgd scale (1/1..1/8) whose output still covers the fit box.
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

    out.pixels.resize(static_cast<size_t>(fit_w) * fit_h * 4);
    std::memset(out.pixels.data(), 0, out.pixels.size());
    ctx.dst = out.pixels.data();

    jr = jd_decomp(&jd, cover_jpg_out, scale);
    std::free(pool);
    if (jr != JDR_OK && jr != JDR_INTR) {
        COVER_LOGI("decode: decomp jr=%d", static_cast<int>(jr));
        out.pixels.clear();
        return false;
    }

    out.width = fit_w;
    out.height = fit_h;
    COVER_LOGI("decode: ok %dx%d from %dx%d scale=%u raw=%u b64=%u free=%u",
               out.width, out.height, src_w, src_h,
               static_cast<unsigned>(scale),
               (unsigned)raw_n, (unsigned)b64_len, (unsigned)heap_u32());
    return true;
#else
    auto decoded = ImageDecoder::decode(b64);
    if (decoded.pixels.empty()) {
        COVER_LOGI("decode: stb fail b64=%u", (unsigned)b64.size());
        return false;
    }
    auto resized = ImageDecoder::resize(decoded, max_h, max_w);
    if (resized.pixels.empty()) {
        COVER_LOGI("decode: resize fail %dx%d", decoded.width, decoded.height);
        return false;
    }
    out.pixels = std::move(resized.pixels);
    out.width = resized.width;
    out.height = resized.height;
    COVER_LOGI("decode: ok %dx%d from %dx%d b64=%u",
               out.width, out.height, decoded.width, decoded.height,
               (unsigned)b64.size());
    return true;
#endif
}

static bool read_desc_head(ByteReader& rd, std::vector<char>& head) {
    const size_t fsize = rd.size();
    const size_t limit = (fsize < kHeadCap) ? fsize : kHeadCap;
    head.clear();
    head.reserve(limit + 1);
    static const char kClose[] = "</description>";
    size_t pos = 0;
    while (pos < limit) {
        size_t old = head.size();
        head.resize(old + kChunk); // temporary; shrink to actual below
        if (head.size() > limit) head.resize(limit);
        size_t want = head.size() - old;
        size_t got = 0;
        if (!rd.read_at(pos, head.data() + old, want, &got)) return false;
        head.resize(old + got);
        if (got == 0) break;
        pos += got;
        if (head.size() >= sizeof(kClose) - 1) {
            size_t from = 0;
            if (head.size() > kChunk + sizeof(kClose))
                from = head.size() - kChunk - (sizeof(kClose) - 1);
            if (find_bytes(head.data() + from, head.size() - from,
                           kClose, sizeof(kClose) - 1))
                break;
        }
    }
    return !head.empty();
}


static bool load_cover_stream(ByteReader& rd, int max_w, int max_h,
                              CoverResult& out) {
    if (rd.size() == 0) {
        COVER_LOGI("stream empty size heap=%u", heap_u32());
        return false;
    }

    std::vector<char> head;
    if (!read_desc_head(rd, head)) {
        COVER_LOGI("stream head read FAIL size=%u heap=%u",
                   (unsigned)rd.size(), heap_u32());
        return false;
    }

    bool meta_ok = parse_cover_head(head.data(), head.size(), out);
    head.clear();
    head.shrink_to_fit();

    if (!out.cover_image_id.empty()) {
        std::string b64;
        if (scan_cover_b64(rd, out.cover_image_id, b64)) {
            decode_cover_b64(b64, max_w, max_h, out);
            b64.clear();
            b64.shrink_to_fit();
        }
    }

    COVER_LOGI("stream head_ok=%d id=%s ann=%u px=%u heap=%u",
               (int)meta_ok, out.cover_image_id.c_str(),
               (unsigned)out.annotation.size(),
               (unsigned)out.pixels.size(), heap_u32());
    return meta_ok;
}
} // namespace

bool load_cover_from_fb2(const std::string& path,
                         int max_w, int max_h, CoverResult& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    FileReader rd(f, (size_t)sz);
    bool ok = load_cover_stream(rd, max_w, max_h, out);
    std::fclose(f);
    return ok;
}

bool load_cover_from_fb2_fs(fs::FileSystem* fs, const std::string& path,
                            int max_w, int max_h, CoverResult& out) {
    if (!fs) return false;

    if (fb2zip::is_zip_path(path)) {
        // Prefer the already-staged book temp (same file BookScene extracts
        // from the zip). Re-extract only when temp is missing/invalid.
        static const char* kBookTemp = "__temp_book.fb2";
        const char* use_path = nullptr;
        bool temp_ok = false;

        fs::File* probe = fs->open(kBookTemp);
        if (probe && *probe) {
            size_t psz = probe->size();
            delete probe;
            if (psz > 0 && psz <= fb2zip::kMaxUncompressed &&
                cfg::get_str("temp_source") == path) {
                temp_ok = true;
                use_path = kBookTemp;
                COVER_LOGI("use staged temp=%s size=%u",
                           kBookTemp, (unsigned)psz);
            } else {
                COVER_LOGI("staged temp stale size=%u src=%s want=%s",
                           (unsigned)psz,
                           cfg::get_str("temp_source").c_str(),
                           path.c_str());
            }
        } else {
            delete probe;
        }

        if (!temp_ok) {
            COVER_LOGI("zip enter path=%s heap=%u", path.c_str(), heap_u32());
            if (!fb2zip::extract_fb2_to_file(fs, path, kBookTemp)) {
                COVER_LOGI("zip extract FAIL path=%s err=%s heap=%u",
                           path.c_str(), fb2zip::last_error(), heap_u32());
                return false;
            }
            use_path = kBookTemp;
            cfg::set_str("temp_source", path.c_str());
            COVER_LOGI("zip extract OK, temp=%s", kBookTemp);
        }

        fs::File* tf = fs->open(use_path);
        if (!tf || !*tf) {
            COVER_LOGI("tmp open FAIL path=%s heap=%u", use_path, heap_u32());
            delete tf;
            if (!temp_ok) fs->remove(use_path);
            return false;
        }
        size_t tsz = tf->size();
        if (tsz == 0 || tsz > fb2zip::kMaxUncompressed) {
            COVER_LOGI("tmp size invalid=%u path=%s", (unsigned)tsz, use_path);
            delete tf;
            if (!temp_ok) fs->remove(use_path);
            return false;
        }
        {
            FsReader rd(tf, tsz);
            load_cover_stream(rd, max_w, max_h, out);
            COVER_LOGI("after stream id=%s ann=%u px=%u",
                       out.cover_image_id.c_str(),
                       (unsigned)out.annotation.size(),
                       (unsigned)out.pixels.size());
        }
        delete tf;
        // Keep staged temp: BookScene reuses/overwrites __temp_book.fb2.
        COVER_LOGI("zip done ann=%u px=%u",
                   (unsigned)out.annotation.size(),
                   (unsigned)out.pixels.size());
        return !out.annotation.empty() || !out.pixels.empty();
    }

    fs::File* f = fs->open(path.c_str());
    if (!f || !*f) {
        COVER_LOGI("fs open FAIL path=%s heap=%u", path.c_str(), heap_u32());
        delete f;
        return false;
    }
    size_t sz = f->size();
    if (sz == 0 || sz > fb2zip::kMaxBookBytes) {
        COVER_LOGI("fs size invalid=%u path=%s", (unsigned)sz, path.c_str());
        delete f;
        return false;
    }
    bool ok;
    {
        FsReader rd(f, sz);
        ok = load_cover_stream(rd, max_w, max_h, out);
    }
    delete f;
    return ok;
}

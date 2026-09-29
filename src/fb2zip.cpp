#include "fb2zip.h"
#include "filesys/filesystem.h"
#include "lgfx/utility/lgfx_miniz.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// ─── stb_zip configuration ─────────────────────────────────────
// Win32 sim: desktop preset (stdio, no mmap).
// ESP32: embedded config; uncompressed limit under free heap.
#ifdef _WIN32
#define STBZ_MODE_DESKTOP
#ifndef STBZ_MAX_UNCOMPRESSED_SIZE
#define STBZ_MAX_UNCOMPRESSED_SIZE (3ULL * 1024 * 1024)
#endif
#else
#define STBZ_NO_SIMD
#define STBZ_NO_TIME
#define STBZ_USE_STDIO 0
#define STBZ_USE_MMAP 0
#define STBZ_ENABLE_SECURITY_CHECKS 1
#define STBZ_MAX_UNCOMPRESSED_SIZE (3ULL * 1024 * 1024)
#define STBZ_NO_AES
#define STBZ_MAX_FILES 512
#endif

#define STB_ZIP_IMPLEMENTATION
#include "stb_zip.h"

namespace fb2zip {

namespace {
std::string g_err;
}

void FreeDeleter::operator()(char* p) const noexcept {
    std::free(p);
}

const char* last_error() { return g_err.c_str(); }
void clear_error() { g_err.clear(); }

static void set_err(const char* msg) {
    g_err = msg ? msg : "";
    printf("fbr: zip ERR %s\n", g_err.c_str());
}

static void set_errf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_err(buf);
}

bool is_zip_path(const std::string& path) {
    if (path.size() < 4) return false;
    size_t p = path.size() - 4;
    const char& a = path[p];
    const char& b = path[p + 1];
    const char& c = path[p + 2];
    const char& d = path[p + 3];
    return a == '.' &&
           (b == 'z' || b == 'Z') &&
           (c == 'i' || c == 'I') &&
           (d == 'p' || d == 'P');
}

static bool ends_with_ignore_case(const char* name, const char* suffix) {
    size_t nl = std::strlen(name);
    size_t sl = std::strlen(suffix);
    if (nl < sl) return false;
    for (size_t i = 0; i < sl; ++i) {
        char a = name[nl - sl + i];
        char b = suffix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

static bool is_book_entry(const char* name) {
    if (!name || !*name) return false;
    // book.fb2 / book.FB2 / book.fb2.xml
    return ends_with_ignore_case(name, ".fb2") ||
           ends_with_ignore_case(name, ".fb2.xml") ||
           ends_with_ignore_case(name, ".xml");
}

bool extract_first_fb2_buf(const uint8_t* data, size_t size,
                           ZipBuffer& out, size_t& out_size) {
    clear_error();
    out.reset();
    out_size = 0;

    if (!data || size == 0) {
        set_err("пустой архив");
        return false;
    }
    if (size > kMaxBookBytes) {
        set_errf("архив %u > лимит %u", (unsigned)size, (unsigned)kMaxBookBytes);
        return false;
    }

    stb_zip_archive za;
    if (!stb_zip_parse(&za, data, size)) {
        set_errf("stb_zip_parse FAIL size=%u", (unsigned)size);
        stb_zip_free(&za);
        return false;
    }
    printf("fbr: zip parsed files=%d heap_hint\n", za.num_files);

    const stb_zip_file_entry* entry = nullptr;
    const stb_zip_file_entry* fb2_only = nullptr;
    for (int i = 0; i < za.num_files && za.files; ++i) {
        const stb_zip_file_entry& e = za.files[i];
        if (!e.name || e.is_symlink) continue;
        size_t nl = std::strlen(e.name);
        if (nl == 0 || e.name[nl - 1] == '/') continue;
        if (is_book_entry(e.name)) {
            if (ends_with_ignore_case(e.name, ".fb2")) {
                fb2_only = &e;
                if (!entry) entry = &e;
            } else if (!entry) {
                entry = &e;
            }
        }
        if (i < 8)
            printf("fbr: zip entry[%d]=%s\n", i, e.name);
    }
    if (fb2_only) entry = fb2_only;

    if (!entry) {
        set_errf("нет .fb2 в архиве (files=%d)", za.num_files);
        for (int i = 0; i < za.num_files && za.files && i < 8; ++i)
            if (za.files[i].name)
                printf("fbr: zip skip[%d]=%s\n", i, za.files[i].name);
        stb_zip_free(&za);
        return false;
    }
    printf("fbr: zip entry=%s\n", entry->name ? entry->name : "?");

    if (entry->uncomp_size == 0 || entry->uncomp_size > kMaxUncompressed) {
        set_errf("размер fb2=%u вне лимита", (unsigned)entry->uncomp_size);
        stb_zip_free(&za);
        return false;
    }

    stb_zip_file got = stb_zip_extract(&za, entry, nullptr);
    stb_zip_free(&za);

    if (got.error != STBZ_OK || !got.data || got.size == 0) {
        set_errf("extract err=%d size=%u", (int)got.error, (unsigned)got.size);
        STBZ_FREE(got.data);
        return false;
    }
    if (got.size > kMaxUncompressed) {
        set_errf("extract size=%u > лимит", (unsigned)got.size);
        STBZ_FREE(got.data);
        return false;
    }

    // stb выделил через malloc — забираем без копии.
    out.reset(reinterpret_cast<char*>(got.data));
    out_size = got.size;
    printf("fbr: zip extract OK size=%u\n", (unsigned)got.size);
    return true;
}

bool extract_first_fb2(const uint8_t* data, size_t size, std::vector<char>& out) {
    ZipBuffer buf;
    size_t buf_size = 0;
    if (!extract_first_fb2_buf(data, size, buf, buf_size)) return false;
    out.assign(buf.get(), buf.get() + buf_size);
    return !out.empty();
}

static bool read_all_fs(fs::FileSystem* fs, const std::string& path,
                        ZipBuffer& out, size_t& out_size) {
    if (!fs) {
        set_err("fs=null");
        return false;
    }

    fs::File* file = fs->open(path.c_str());
    if (!file) {
        set_errf("fs->open FAIL path=%s", path.c_str());
        return false;
    }

    size_t sz = file->size();
    if (sz == 0) {
        delete file;
        set_err("пустой файл");
        return false;
    }
    if (sz > kMaxBookBytes) {
        delete file;
        set_errf("файл %u > лимит %u", (unsigned)sz, (unsigned)kMaxBookBytes);
        return false;
    }

    out.reset(static_cast<char*>(std::malloc(sz)));
    if (!out) {
        delete file;
        set_errf("OOM zip raw %u", (unsigned)sz);
        return false;
    }
    size_t rd = file->read(out.get(), sz);
    delete file;
    if (rd != sz) {
        out.reset();
        set_errf("short read %u/%u", (unsigned)rd, (unsigned)sz);
        return false;
    }
    out_size = sz;
    return true;
}

// ─── Streaming extract: zip (SD) → temp fb2 (SD) ──────────────
// lgfx_miniz has MINIZ_NO_ARCHIVE_APIS → no zip reader API.
// Hand-roll EOCD/CD + tinfl (raw deflate). Peak RAM ≈ 64KB CD
// + 16KB IO + 32KB tinfl dict ≈ ~112KB.

namespace {

// ZIP signatures / header sizes
constexpr uint32_t kSigEocd = 0x06054b50u;
constexpr uint32_t kSigCdh  = 0x02014b50u;
constexpr uint32_t kSigLdh  = 0x04034b50u;
constexpr size_t kEocdMin   = 22;
constexpr size_t kCdhMin    = 46;
constexpr size_t kLdhMin    = 30;
constexpr size_t kCdMax     = 64 * 1024;  // central directory cap
constexpr size_t kIoChunk   = 16 * 1024;  // read/write chunk
constexpr size_t kTailScan  = 22 + 65535; // max EOCD+comment from end

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    uint16_t method;      // 0=store, 8=deflate
    uint16_t flags;
    uint32_t crc32;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_ofs;
    char     name[256];
};

static bool seek_read(fs::File* f, size_t ofs, void* buf, size_t n) {
    if (!f || !*f) return false;
    if (!f->seek((int64_t)ofs, SEEK_SET)) return false;
    return f->read(buf, n) == n;
}

// Scan file tail for EOCD, then CD; pick first book entry.
// 'cd' must be at least kCdMax; returns bytes used in 'cd'.
static bool find_book_entry(fs::File* zf, size_t zsize,
                            uint8_t* cd, size_t cd_cap,
                            size_t& cd_len, ZipEntry& out) {
    if (zsize < kEocdMin) return false;

    size_t tail_n = zsize < kTailScan ? zsize : kTailScan;
    size_t tail_ofs = zsize - tail_n;
    std::vector<uint8_t> tail(tail_n);
    if (!seek_read(zf, tail_ofs, tail.data(), tail_n)) return false;

    // Find EOCD (sig at start of record; comment may follow)
    long eocd_rel = -1;
    for (long i = (long)tail_n - (long)kEocdMin; i >= 0; --i) {
        if (rd32(tail.data() + i) == kSigEocd) {
            uint16_t comment = rd16(tail.data() + i + 20);
            if ((size_t)i + kEocdMin + comment == tail_n) {
                eocd_rel = i;
                break;
            }
        }
    }
    if (eocd_rel < 0) return false;

    const uint8_t* e = tail.data() + eocd_rel;
    uint16_t total = rd16(e + 10);
    uint32_t cd_size = rd32(e + 12);
    uint32_t cd_ofs  = rd32(e + 16);
    if (total == 0 || cd_size == 0 || cd_size > cd_cap) return false;
    if ((size_t)cd_ofs + cd_size > zsize) return false;
    if (cd_size > cd_cap) return false;

    if (!seek_read(zf, cd_ofs, cd, cd_size)) return false;
    cd_len = cd_size;

    // Walk CD entries; prefer first pure .fb2
    size_t pos = 0;
    bool have = false, have_fb2 = false;
    ZipEntry best{};
    for (uint16_t i = 0; i < total && pos + kCdhMin <= cd_size; ++i) {
        const uint8_t* p = cd + pos;
        if (rd32(p) != kSigCdh) break;
        uint16_t nlen = rd16(p + 28);
        uint16_t elen = rd16(p + 30);
        uint16_t clen = rd16(p + 32);
        size_t rec = kCdhMin + nlen + elen + clen;
        if (pos + rec > cd_size) break;

        ZipEntry e{};
        e.flags     = rd16(p + 8);
        e.method    = rd16(p + 10);
        e.crc32     = rd32(p + 16);
        e.comp_size = rd32(p + 20);
        e.uncomp_size = rd32(p + 24);
        e.local_ofs = rd32(p + 42);
        size_t ncopy = nlen < sizeof(e.name) - 1 ? nlen : sizeof(e.name) - 1;
        std::memcpy(e.name, p + kCdhMin, ncopy);
        e.name[ncopy] = '\0';
        if (i < 8)
            printf("fbr: zip entry[%u]=%s\n", (unsigned)i, e.name);

        if (is_book_entry(e.name)) {
            bool fb2 = ends_with_ignore_case(e.name, ".fb2");
            if (fb2 && !have_fb2) {
                out = e;
                have = true;
                have_fb2 = true;
                break;
            }
            if (!have) {
                best = e;
                have = true;
            }
        }
        pos += rec;
    }
    if (have && !have_fb2) out = best;
    return have;
}

// Stream raw-deflate (method 8) or store (method 0) from zip into out.
static bool stream_entry(fs::File* zf, const ZipEntry& e,
                         fs::File* of) {
    // Local header: skip name+extra, locate data start
    uint8_t lh[kLdhMin];
    if (!seek_read(zf, e.local_ofs, lh, kLdhMin)) return false;
    if (rd32(lh) != kSigLdh) return false;
    uint16_t ln = rd16(lh + 26);
    uint16_t le = rd16(lh + 28);
    size_t data_ofs = (size_t)e.local_ofs + kLdhMin + ln + le;
    if (data_ofs + e.comp_size > zf->size()) return false;

    // Reject encrypted / patch
    if (e.flags & 0x0001) return false;

    // Drop stale output position
    if (!of->seek(0, SEEK_SET)) return false;

    uint32_t expect = e.uncomp_size;
    if (expect == 0 || expect > kMaxUncompressed) return false;

    if (e.method == 0) {
        // Store: copy in chunks
        if (e.comp_size != e.uncomp_size) return false;
        if (!zf->seek((int64_t)data_ofs, SEEK_SET)) return false;
        uint8_t buf[kIoChunk];
        uint32_t left = e.comp_size;
        lgfx_mz_ulong mzcrc = 0;
        while (left) {
            size_t n = left < sizeof(buf) ? left : sizeof(buf);
            if (zf->read(buf, n) != n) return false;
            if (of->write(buf, n) != n) return false;
            mzcrc = lgfx_mz_crc32(mzcrc, buf, n);
            left -= (uint32_t)n;
        }
        if ((uint32_t)mzcrc != e.crc32) return false;
        return true;
    }

    if (e.method != 8) return false;

    // Deflate: tinfl with 32KB wrapping dict + 16KB IO
    auto* dict = (uint8_t*)std::malloc(TINFL_LZ_DICT_SIZE);
    auto* iobuf = (uint8_t*)std::malloc(kIoChunk);
    if (!dict || !iobuf) {
        std::free(dict);
        std::free(iobuf);
        return false;
    }

    lgfx_tinfl_decompressor infl;
    lgfx_tinfl_init(&infl);

    if (!zf->seek((int64_t)data_ofs, SEEK_SET)) {
        std::free(dict);
        std::free(iobuf);
        return false;
    }

    uint64_t out_ofs = 0;
    uint64_t comp_left = e.comp_size;
    size_t read_avail = 0, read_ofs = 0;
    lgfx_mz_ulong mzcrc = 0;
    bool ok = false;

    auto refill = [&](void) -> bool {
        if (read_avail) return true;
        if (!comp_left) return true;
        size_t n = comp_left < kIoChunk ? (size_t)comp_left : kIoChunk;
        if (zf->read(iobuf, n) != n) return false;
        read_avail = n;
        read_ofs = 0;
        comp_left -= n;
        return true;
    };

    lgfx_tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    for (;;) {
        if (!refill()) break;
        uint8_t* dict_cur = dict + (out_ofs & (TINFL_LZ_DICT_SIZE - 1));
        size_t out_space = TINFL_LZ_DICT_SIZE - (out_ofs & (TINFL_LZ_DICT_SIZE - 1));
        size_t in_size = read_avail;
        uint32_t flags = comp_left ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        status = lgfx_tinfl_decompress(
            &infl, iobuf + read_ofs, &in_size,
            dict, dict_cur, &out_space, flags);
        read_avail -= in_size;
        read_ofs += in_size;

        if (out_space) {
            if (of->write(dict_cur, out_space) != out_space) break;
            mzcrc = lgfx_mz_crc32(mzcrc, dict_cur, out_space);
            out_ofs += out_space;
            if (out_ofs > expect) break;
        }
        if (status == TINFL_STATUS_DONE) {
            ok = (out_ofs == expect) &&
                 ((uint32_t)mzcrc == e.crc32);
            break;
        }
        if (status < 0) break; // FAILED / BAD_PARAM
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && !comp_left && !read_avail)
            break;
    }

    std::free(dict);
    std::free(iobuf);
    return ok;
}

} // namespace

bool extract_fb2_to_file(fs::FileSystem* fs,
                         const std::string& zip_path,
                         const std::string& out_path) {
    clear_error();
    if (!fs) {
        set_err("fs=null");
        return false;
    }

    fs::File* zf = fs->open(zip_path.c_str());
    if (!zf || !*zf) {
        delete zf;
        set_errf("zip open FAIL path=%s", zip_path.c_str());
        return false;
    }
    size_t zsize = zf->size();
    if (zsize == 0 || zsize > kMaxBookBytes) {
        size_t bad = zsize;
        delete zf;
        set_errf("zip size=%u invalid", (unsigned)bad);
        return false;
    }

    auto* cd = (uint8_t*)std::malloc(kCdMax);
    if (!cd) {
        delete zf;
        set_err("OOM CD buffer");
        return false;
    }

    ZipEntry ent{};
    size_t cd_len = 0;
    if (!find_book_entry(zf, zsize, cd, kCdMax, cd_len, ent)) {
        std::free(cd);
        delete zf;
        set_err("нет .fb2 в архиве / CD parse FAIL");
        return false;
    }
    std::free(cd);
    printf("fbr: zip stream CD ok size=%u entry=%s method=%u uncomp=%u\n",
           (unsigned)zsize, ent.name, (unsigned)ent.method,
           (unsigned)ent.uncomp_size);

    if (ent.uncomp_size == 0 || ent.uncomp_size > kMaxUncompressed) {
        delete zf;
        set_errf("размер fb2=%u вне лимита", (unsigned)ent.uncomp_size);
        return false;
    }
    if (ent.comp_size == 0xFFFFFFFFu || ent.uncomp_size == 0xFFFFFFFFu) {
        delete zf;
        set_err("ZIP64 не поддерживается");
        return false;
    }

    fs->remove(out_path.c_str());
    fs::File* of = fs->create(out_path.c_str());
    if (!of || !*of) {
        delete of;
        delete zf;
        set_err("temp create FAIL");
        return false;
    }

    bool ok = stream_entry(zf, ent, of);

    of->sync();
    of->close();
    delete of;
    delete zf;

    if (!ok) {
        fs->remove(out_path.c_str());
        set_err("stream extract FAIL");
        return false;
    }

    printf("fbr: zip stream extract OK size=%u\n",
           (unsigned)ent.uncomp_size);
    return true;
}

bool load_book_fb2(fs::FileSystem* fs, const std::string& path,
                   bool& in_place, ZipBuffer& out, size_t& out_size) {
    clear_error();
    in_place = false;
    out.reset();
    out_size = 0;

    FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe) {
        std::fclose(probe);
        if (!is_zip_path(path)) {
            in_place = true;
            return true;
        }
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            set_errf("fopen FAIL path=%s", path.c_str());
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0 || (unsigned long)sz > kMaxBookBytes) {
            std::fclose(f);
            set_errf("fopen size=%ld invalid", sz);
            return false;
        }
        ZipBuffer raw(static_cast<char*>(std::malloc((size_t)sz)));
        if (!raw) {
            std::fclose(f);
            set_errf("OOM zip raw %ld", sz);
            return false;
        }
        size_t rd = std::fread(raw.get(), 1, (size_t)sz, f);
        std::fclose(f);
        if (rd != (size_t)sz) {
            set_errf("fread short %u/%ld", (unsigned)rd, sz);
            return false;
        }
        printf("fbr: zip fopen path=%s size=%ld\n", path.c_str(), sz);
        return extract_first_fb2_buf((const uint8_t*)raw.get(), (size_t)sz,
                                     out, out_size);
    }

    if (!fs) {
        set_errf("fopen FAIL and fs=null path=%s", path.c_str());
        return false;
    }

    ZipBuffer raw;
    size_t raw_size = 0;
    if (!read_all_fs(fs, path, raw, raw_size)) {
        if (g_err.empty())
            set_errf("read_all_fs FAIL path=%s", path.c_str());
        return false;
    }
    printf("fbr: zip read_all ok size=%u path=%s\n",
           (unsigned)raw_size, path.c_str());

    if (is_zip_path(path))
        return extract_first_fb2_buf((const uint8_t*)raw.get(), raw_size,
                                     out, out_size);

    out = std::move(raw);
    out_size = raw_size;
    return true;
}

} // namespace fb2zip

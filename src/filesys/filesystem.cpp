#include "filesys/filesystem.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include "filesys/image_backend.h"
#else
#include "filesys/sd_backend.h"
#endif

extern "C" {
#include "ff.h"
}

/* ══════════════════════════════════════════════════════════════
 *  Mojibake normalization
 *
 *  Some SD cards / images were created by treating each UTF-8 byte
 *  of a filename as one Latin-1 code point and re-encoding to UTF-8.
 *  E.g. D0 92 D0 B0 ("Ва") was stored as C3 90 C2 92 C3 90 C2 B0.
 *  read_dir() undoes this so names display correctly; all path
 *  operations fall back to the encoded form so those names can be
 *  looked up on disk.
 * ══════════════════════════════════════════════════════════════ */

namespace {

struct Utf8Result {
    bool      ok = true;
    std::vector<uint32_t> cps;
};

Utf8Result utf8_decode(const char* s) {
    Utf8Result r;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
        else { r.ok = false; return r; }
        for (int i = 1; i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) { r.ok = false; return r; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += len;
        r.cps.push_back(cp);
    }
    return r;
}

/* Undo "each UTF-8 byte stored as a Latin-1 code point".
 * Returns false when the name is not double encoded (plain ASCII,
 * real Cyrillic, or Latin-1 text), so those pass through unchanged. */
bool decode_mojibake(const char* src, std::string& out) {
    Utf8Result r = utf8_decode(src);
    if (!r.ok) return false;

    bool has_high = false;
    for (uint32_t cp : r.cps) { if (cp > 0xFF) return false; if (cp >= 0x80) has_high = true; }
    if (!has_high) return false;               /* pure ASCII */
    if (r.cps.empty()) return false;

    /* The code-point values must themselves form valid UTF-8 text. */
    std::string raw;
    raw.reserve(r.cps.size());
    for (uint32_t cp : r.cps) raw.push_back((char)cp);
    Utf8Result re = utf8_decode(raw.c_str());
    if (!re.ok) return false;

    bool still_has_high = false;
    for (uint32_t cp : re.cps) if (cp >= 0x80) { still_has_high = true; break; }
    if (!still_has_high) return false;

    out = std::move(raw);
    return true;
}

/* Inverse of decode_mojibake: encode each byte >= 0x80 of the path as
 * its Latin-1 code point, matching how corrupt images store names. */
std::string encode_mojibake(const std::string& path) {
    std::string out;
    out.reserve(path.size() + path.size() / 2 + 2);
    for (unsigned char b : path) {
        if (b < 0x80) {
            out.push_back((char)b);
        } else if (b <= 0xBF) {               /* U+0080..U+00BF -> C2 xx */
            out.push_back((char)0xC2);
            out.push_back((char)b);
        } else {                              /* U+00C0..U+00FF -> C3 (b-0x40) */
            out.push_back((char)0xC3);
            out.push_back((char)(b - 0x40));
        }
    }
    return out;
}

} // namespace

/* ══════════════════════════════════════════════════════════════
 *  File implementation
 * ══════════════════════════════════════════════════════════════ */

fs::File::File()
    : open_(false)
    , fil_(new FIL)
{
    std::memset(fil_, 0, sizeof(FIL));
}

fs::File::~File() {
    close();
    delete static_cast<FIL*>(fil_);
}

fs::File::File(File&& o) noexcept
    : open_(o.open_)
    , fil_(o.fil_)
{
    o.open_ = false;
    o.fil_  = nullptr;
}

fs::File& fs::File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        delete static_cast<FIL*>(fil_);
        open_ = o.open_;
        fil_  = o.fil_;
        o.open_ = false;
        o.fil_  = nullptr;
    }
    return *this;
}

size_t fs::File::read(void* buf, size_t size) {
    if (!open_ || !buf) return 0;
    UINT br = 0;
    if (f_read(static_cast<FIL*>(fil_), buf, (UINT)size, &br) != FR_OK)
        return 0;
    return br;
}

size_t fs::File::write(const void* buf, size_t size) {
    if (!open_ || !buf) return 0;
    UINT bw = 0;
    if (f_write(static_cast<FIL*>(fil_), buf, (UINT)size, &bw) != FR_OK)
        return 0;
    return bw;
}

bool fs::File::sync() {
    if (!open_) return false;
    return f_sync(static_cast<FIL*>(fil_)) == FR_OK;
}

bool fs::File::seek(int64_t offset, int whence) {
    if (!open_) return false;
    /* Convert whence to FatFs FR_SEEK_SET/END/etc */
    switch (whence) {
    case SEEK_SET: break; /* default is from start */
    case SEEK_CUR: offset += (int64_t)tell(); break;
    case SEEK_END: offset += (int64_t)size(); break;
    default: return false;
    }
    return f_lseek(static_cast<FIL*>(fil_), (DWORD)offset) == FR_OK;
}

size_t fs::File::tell() {
    if (!open_) return 0;
    return (size_t)f_tell(static_cast<FIL*>(fil_));
}

size_t fs::File::size() {
    if (!open_) return 0;
    return (size_t)f_size(static_cast<FIL*>(fil_));
}

void fs::File::close() {
    if (open_) {
        f_close(static_cast<FIL*>(fil_));
        open_ = false;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  FileSystem implementation
 * ══════════════════════════════════════════════════════════════ */

fs::FileSystem::FileSystem()
    : mounted_(false)
    , fs_(new FATFS)
{
    std::memset(fs_, 0, sizeof(FATFS));
}

fs::FileSystem::~FileSystem() {
    if (mounted_) {
        f_mount(nullptr, "", 1);
    }
    delete static_cast<FATFS*>(fs_);
#ifdef _WIN32
    image_backend_shutdown();
#else
    sd_backend_shutdown();
#endif
}

fs::FileSystem* fs::FileSystem::mount(const char* mount_point) {
    /* Initialize backend (sector-level) */
#ifdef _WIN32
    image_backend_init(mount_point);
    if (!image_backend_is_ready()) return nullptr;
#else
    sd_backend_init();
    if (!sd_backend_is_ready()) return nullptr;
#endif

    auto* self = new FileSystem();
    FRESULT fr = f_mount(static_cast<FATFS*>(self->fs_), "", 1);
    if (fr != FR_OK) {
        delete self;
#ifdef _WIN32
        image_backend_shutdown();
#else
        sd_backend_shutdown();
#endif
        return nullptr;
    }
    self->mounted_ = true;
    return self;
}

fs::FileSystem* fs::FileSystem::mount_any() {
#ifdef _WIN32
    static const char* kCandidates[] = {
        "../sdimage/fatimage.img",
        "sdimage/fatimage.img",
        nullptr
    };
    for (int i = 0; kCandidates[i]; ++i) {
        fs::FileSystem* fs = mount(kCandidates[i]);
        if (fs) return fs;
    }
#else
    fs::FileSystem* fs = mount("/sdcard");
    if (fs) return fs;
#endif
    return nullptr;
}

fs::File* fs::FileSystem::open(const char* path) {
    if (!mounted_) return nullptr;
    auto* f = new File();
    FRESULT fr = f_open(static_cast<FIL*>(f->fil_), path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        std::string enc = encode_mojibake(path);
        if (enc != path)
            fr = f_open(static_cast<FIL*>(f->fil_), enc.c_str(), FA_READ);
    }
    if (fr != FR_OK) {
        delete f;
        return nullptr;
    }
    f->open_ = true;
    return f;
}

fs::File* fs::FileSystem::create(const char* path) {
    if (!mounted_) return nullptr;
    auto* f = new File();
    FRESULT fr = f_open(static_cast<FIL*>(f->fil_), path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        std::string enc = encode_mojibake(path);
        if (enc != path)
            fr = f_open(static_cast<FIL*>(f->fil_), enc.c_str(), FA_WRITE | FA_CREATE_ALWAYS);
    }
    if (fr != FR_OK) {
        delete f;
        return nullptr;
    }
    f->open_ = true;
    return f;
}

bool fs::FileSystem::remove(const char* path) {
    if (!mounted_) return false;
    FRESULT fr = f_unlink(path);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        std::string enc = encode_mojibake(path);
        if (enc != path)
            fr = f_unlink(enc.c_str());
    }
    return fr == FR_OK;
}

bool fs::FileSystem::mkdir(const char* path) {
    if (!mounted_) return false;
    FRESULT fr = f_mkdir(path);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        std::string enc = encode_mojibake(path);
        if (enc != path)
            fr = f_mkdir(enc.c_str());
    }
    return fr == FR_OK;
}

size_t fs::FileSystem::read_dir(const char* path, DirEntry* entries, size_t max_count) {
    if (!mounted_ || !entries || max_count == 0) return 0;

    DIR dir;
    FILINFO fno;
    size_t count = 0;

    FRESULT fr = f_opendir(&dir, path);
    std::string enc_path;
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        enc_path = encode_mojibake(path);
        if (enc_path != path)
            fr = f_opendir(&dir, enc_path.c_str());
    }
    if (fr != FR_OK)
        return 0;

    while (count < max_count && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        std::string name;
        if (!decode_mojibake(fno.fname, name))
            name = fno.fname;
        std::strncpy(entries[count].name, name.c_str(), kMaxName - 1);
        entries[count].name[kMaxName - 1] = '\0';
        entries[count].size   = fno.fsize;
        entries[count].is_dir = (fno.fattrib & AM_DIR) != 0;
        ++count;
    }

    f_closedir(&dir);
    return count;
}

bool fs::FileSystem::exists(const char* path) {
    if (!mounted_) return false;
    FILINFO fno;
    FRESULT fr = f_stat(path, &fno);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        std::string enc = encode_mojibake(path);
        if (enc != path)
            fr = f_stat(enc.c_str(), &fno);
    }
    return fr == FR_OK;
}

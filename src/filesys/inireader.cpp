#include "filesys/inireader.h"
#include "filesys/filesystem.h"
#include <cstring>
#include <cstdio>
#include <cctype>
#include <new>

/* ══════════════════════════════════════════════════════════════
 *  IniReader
 * ══════════════════════════════════════════════════════════════ */

namespace {
    void copy_bounded(char* dst, size_t dst_size, const char* src) {
        if (dst_size == 0) return;
        std::strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = 0;
    }
}

fs::IniReader::IniReader()
    : count_(0)
{
    cur_sec_[0] = 0;
}

fs::IniReader::~IniReader() {
}

void fs::IniReader::create() {
    count_ = 0;
    cur_sec_[0] = 0;
}

/* ── Trim leading/trailing whitespace (in-place) ──────────── */

void fs::IniReader::trim(char* s) const {
    char* end;
    while (std::isspace((unsigned char)*s)) ++s;
    if (*s == 0) { s[0] = 0; return; }
    end = s + std::strlen(s) - 1;
    while (end > s && std::isspace((unsigned char)*end)) --end;
    *(end + 1) = 0;
}

/* ── Find entry by section+key ────────────────────────────── */

int fs::IniReader::find(const char* sec, const char* key) const {
    for (int i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].section, sec) == 0 &&
            std::strcmp(entries_[i].key, key) == 0)
            return i;
    }
    return -1;
}

/* ── Get string value ─────────────────────────────────────── */

const char* fs::IniReader::get(const char* section, const char* key, const char* def) {
    int i = find(section, key);
    return i >= 0 ? entries_[i].value : def;
}

int fs::IniReader::get_int(const char* section, const char* key, int def) {
    int i = find(section, key);
    if (i < 0) return def;
    int v = 0;
    if (std::sscanf(entries_[i].value, "%d", &v) == 1) return v;
    return def;
}

/* ── Set string value ─────────────────────────────────────── */

bool fs::IniReader::set(const char* section, const char* key, const char* value) {
    if (count_ >= kIniMaxEntries) return false;
    int i = find(section, key);
    if (i < 0) {
        i = count_++;
        copy_bounded(entries_[i].section, sizeof(entries_[i].section), section);
    }
    copy_bounded(entries_[i].key, sizeof(entries_[i].key), key);
    copy_bounded(entries_[i].value, sizeof(entries_[i].value), value);
    return true;
}

bool fs::IniReader::set_int(const char* section, const char* key, int value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", value);
    return set(section, key, buf);
}

/* ── Open and parse INI file ──────────────────────────────── */

bool fs::IniReader::open(FileSystem* fs, const char* path) {
    auto* f = fs->open(path);
    if (!f) return false;

    size_t sz = f->size();
    if (sz == 0) { delete f; return false; }

    // Read whole file into memory
    char* data = new (std::nothrow) char[sz + 1];
    if (!data) { delete f; return false; }
    size_t rd = f->read(data, sz);
    data[rd] = 0;
    delete f;

    if (rd == 0) { delete[] data; return false; }

    // Parse line by line
    count_ = 0;
    cur_sec_[0] = 0;

    char* line = data;
    while (line && *line) {
        char* nl = std::strchr(line, '\n');
        if (nl) *nl = 0;

        // Remove \r if present
        char* cr = std::strchr(line, '\r');
        if (cr) *cr = 0;

        trim(line);

        if (line[0] == ';' || line[0] == '#' || line[0] == 0) {
            // Comment or empty line
        }
        else if (line[0] == '[') {
            char* end = std::strchr(line + 1, ']');
            if (end) {
                *end = 0;
                copy_bounded(cur_sec_, sizeof(cur_sec_), line + 1);
            }
        }
        else {
            char* eq = std::strchr(line, '=');
            if (eq && cur_sec_[0] && count_ < kIniMaxEntries) {
                *eq = 0;
                char* k = line;
                char* v = eq + 1;
                trim(k);
                trim(v);
                copy_bounded(entries_[count_].section, sizeof(entries_[count_].section), cur_sec_);
                copy_bounded(entries_[count_].key, sizeof(entries_[count_].key), k);
                copy_bounded(entries_[count_].value, sizeof(entries_[count_].value), v);
                ++count_;
            }
        }

        if (!nl) break;
        line = nl + 1;
    }

    delete[] data;
    return true;
}

/* ── Serialize to buffer ──────────────────────────────────── */

size_t fs::IniReader::serialize(char* buf, size_t size) {
    // First pass: compute needed size
    size_t need = 0;
    const char* last_sec = "";

    for (int i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].section, last_sec) != 0) {
            need += 2 + std::strlen(entries_[i].section) + 1;  // [sec]\n
            last_sec = entries_[i].section;
        }
        need += std::strlen(entries_[i].key) + 1 + std::strlen(entries_[i].value) + 1;  // key=val\n
    }
    ++need; // trailing \0

    if (!buf || size == 0) return need;

    // Second pass: write
    size_t pos = 0;
    last_sec = "";
    for (int i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].section, last_sec) != 0) {
            int n = std::snprintf(buf + pos, size - pos, "[%s]\n", entries_[i].section);
            if (n > 0) pos += (size_t)(n < (int)(size - pos) ? n : size - pos - 1);
            last_sec = entries_[i].section;
        }
        int n = std::snprintf(buf + pos, size - pos, "%s=%s\n", entries_[i].key, entries_[i].value);
        if (n > 0) pos += (size_t)(n < (int)(size - pos) ? n : size - pos - 1);
    }

    if (pos < size) buf[pos] = 0;
    return need;
}

/* ── Save to file ──────────────────────────────────────────── */

bool fs::IniReader::save(FileSystem* fs, const char* path) {
    size_t need = serialize(nullptr, 0);
    char* data = new (std::nothrow) char[need];
    if (!data) return false;
    serialize(data, need);

    auto* f = fs->create(path);
    if (!f) { delete[] data; return false; }

    size_t len = std::strlen(data);
    bool ok = f->write(data, len) == len;
    if (ok) f->sync();
    delete f;
    delete[] data;
    return ok;
}

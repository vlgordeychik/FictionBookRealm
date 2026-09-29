#include "history.h"
#include "filesys/filesystem.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

static fs::FileSystem* g_fs = nullptr;

static bool write_u8(fs::File* f, uint8_t v) { return f->write(&v, 1) == 1; }
static bool write_u16(fs::File* f, uint16_t v) { return f->write(&v, 2) == 2; }
static bool write_i32(fs::File* f, int32_t v) { return f->write(&v, 4) == 4; }

static bool read_u8(fs::File* f, uint8_t& v) { return f->read(&v, 1) == 1; }
static bool read_u16(fs::File* f, uint16_t& v) { return f->read(&v, 2) == 2; }
static bool read_i32(fs::File* f, int32_t& v) { return f->read(&v, 4) == 4; }

static bool write_str(fs::File* f, const std::string& s) {
    uint16_t len = (uint16_t)s.size();
    if (!write_u16(f, len)) return false;
    if (len > 0 && f->write(s.data(), len) != len) return false;
    return true;
}

static bool read_str(fs::File* f, std::string& s) {
    uint16_t len;
    if (!read_u16(f, len)) return false;
    s.resize(len);
    if (len > 0 && f->read(&s[0], len) != len) return false;
    return true;
}

void History::set_fs(fs::FileSystem* fs) {
    g_fs = fs;
}

bool History::load(std::vector<HistoryEntry>& entries) {
    entries.clear();
    if (!g_fs) return false;

    fs::File* f = g_fs->open(kFilePath);
    if (!f) return false;

    char magic[4];
    if (f->read(magic, 4) != 4 || std::memcmp(magic, "HIST", 4) != 0) {
        delete f;
        return false;
    }

    int32_t version;
    read_i32(f, version);
    if (version != 1) { delete f; return false; }

    int32_t count;
    read_i32(f, count);
    if (count < 0 || count > 200) { delete f; return false; }

    entries.reserve(count);
    for (int32_t i = 0; i < count; ++i) {
        HistoryEntry e;
        if (!read_str(f, e.title)) break;
        if (!read_str(f, e.author)) break;
        if (!read_str(f, e.file_path)) break;
        if (!read_str(f, e.cache_key)) break;
        int32_t tmp;
        if (!read_i32(f, tmp)) break;
        e.current_page = tmp;
        if (!read_i32(f, tmp)) break;
        e.total_pages = tmp;
        entries.push_back(std::move(e));
    }

    delete f;

    // Cleanup: remove entries with temp file path and dedup by file_path
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [](const HistoryEntry& e) { return e.file_path.find("__temp_book") != std::string::npos; }),
        entries.end());

    // Dedup: keep first occurrence of each file_path
    std::vector<HistoryEntry> deduped;
    for (auto& e : entries) {
        bool found = false;
        for (const auto& d : deduped) {
            if (d.file_path == e.file_path) { found = true; break; }
        }
        if (!found) deduped.push_back(std::move(e));
    }
    entries = std::move(deduped);

    return true;
}

bool History::save(const std::vector<HistoryEntry>& entries) {
    if (!g_fs) return false;

    fs::File* f = g_fs->create(kFilePath);
    if (!f) return false;

    f->write("HIST", 4);
    int32_t version = 1;
    write_i32(f, version);
    int32_t count = (int32_t)entries.size();
    write_i32(f, count);

    for (const auto& e : entries) {
        write_str(f, e.title);
        write_str(f, e.author);
        write_str(f, e.file_path);
        write_str(f, e.cache_key);
        int32_t tmp;
        tmp = e.current_page; write_i32(f, tmp);
        tmp = e.total_pages;  write_i32(f, tmp);
    }

    bool ok = f->sync();
    delete f;
    return ok;
}

void History::add(const HistoryEntry& entry) {
    // В историю попадают только реально открытые книги.
    // Промежуточный временный файл (__temp_book.fb2) не записываем,
    // а расширение файла должно быть .fb2 или .zip (.fb2.zip).
    auto is_temp_path = [](const std::string& s) {
        return s.find("__temp_book") != std::string::npos;
    };
    if (is_temp_path(entry.file_path) || is_temp_path(entry.cache_key))
        return;

    auto ends_with_ignore_case = [](const std::string& s, const char* suffix) {
        size_t sl = std::strlen(suffix);
        if (s.size() < sl) return false;
        for (size_t i = 0; i < sl; ++i) {
            char a = s[s.size() - sl + i];
            char b = suffix[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) return false;
        }
        return true;
    };
    if (!ends_with_ignore_case(entry.file_path, ".fb2") &&
        !ends_with_ignore_case(entry.file_path, ".zip"))
        return;

    std::vector<HistoryEntry> entries;
    load(entries);

    // Remove existing entry for the same file
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [&](const HistoryEntry& e) { return e.file_path == entry.file_path; }),
        entries.end());

    // Prepend new entry
    entries.insert(entries.begin(), entry);

    // Trim to max
    if ((int)entries.size() > kMaxEntries)
        entries.resize(kMaxEntries);

    save(entries);
}

void History::update_page(const std::string& file_path, int current_page) {
    std::vector<HistoryEntry> entries;
    load(entries);

    for (auto& e : entries) {
        if (e.file_path == file_path) {
            e.current_page = current_page;
            break;
        }
    }

    save(entries);
}

bool History::find(const std::string& file_path, HistoryEntry& out) {
    std::vector<HistoryEntry> entries;
    if (!load(entries)) return false;

    for (const auto& e : entries) {
        if (e.file_path == file_path ||
            (e.cache_key == file_path && !e.cache_key.empty())) {
            out = e;
            return true;
        }
    }
    return false;
}

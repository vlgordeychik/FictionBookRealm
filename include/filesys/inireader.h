#pragma once

#include <cstddef>

namespace fs {

class FileSystem;

static constexpr int kIniMaxEntries = 64;
static constexpr int kIniMaxKey   = 64;
static constexpr int kIniMaxVal   = 256;
static constexpr int kIniMaxSec   = 64;

struct IniEntry {
    char section[kIniMaxSec];
    char key[kIniMaxKey];
    char value[kIniMaxVal];
};

class IniReader {
public:
    IniReader();
    ~IniReader();

    bool open(FileSystem* fs, const char* path);
    void create();

    const char* get(const char* section, const char* key, const char* def = "");
    int  get_int(const char* section, const char* key, int def = 0);

    bool set(const char* section, const char* key, const char* value);
    bool set_int(const char* section, const char* key, int value);

    bool save(FileSystem* fs, const char* path);

    size_t serialize(char* buf, size_t size);

    IniReader(const IniReader&) = delete;
    IniReader& operator=(const IniReader&) = delete;

private:
    int  find(const char* sec, const char* key) const;
    void trim(char* s) const;

    IniEntry entries_[kIniMaxEntries];
    int  count_;
    char cur_sec_[kIniMaxSec];
};

} // namespace fs

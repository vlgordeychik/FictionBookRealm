#pragma once

#include <cstddef>
#include <cstdint>

namespace fs {

static constexpr size_t kMaxName = 256;

struct DirEntry {
    char name[kMaxName];
    size_t size;
    bool   is_dir;
};

/* ─── File handle ───────────────────────────────────────────── */

class File {
public:
    ~File();
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    size_t read(void* buf, size_t size);
    size_t write(const void* buf, size_t size);
    bool   sync();
    bool   seek(int64_t offset, int whence);
    size_t tell();
    size_t size();
    void   close();
    explicit operator bool() const { return open_; }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

private:
    friend class FileSystem;
    File();
    bool   open_ = false;
    void*  fil_;   // FIL* (opaque)
};

/* ─── Filesystem (mounted FAT volume) ──────────────────────── */

class FileSystem {
public:
    static FileSystem* mount(const char* image_path);
    static FileSystem* mount_any();
    ~FileSystem();

    File*  open(const char* path);
    File*  create(const char* path);
    bool   remove(const char* path);
    bool   mkdir(const char* path);
    size_t read_dir(const char* path, DirEntry* entries, size_t max_count);
    bool   exists(const char* path);

    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

private:
    FileSystem();
    bool   mounted_ = false;
    void*  fs_;     // FATFS* (opaque)
};

} // namespace fs

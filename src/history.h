#pragma once
#include <string>
#include <vector>

namespace fs { class FileSystem; }

struct HistoryEntry {
    std::string title;
    std::string author;
    std::string file_path;
    std::string cache_key;
    int current_page = 0;
    int total_pages = 0;
};

class History {
public:
    static void set_fs(fs::FileSystem* fs);
    static bool load(std::vector<HistoryEntry>& entries);
    static bool save(const std::vector<HistoryEntry>& entries);
    static void add(const HistoryEntry& entry);
    static void update_page(const std::string& file_path, int current_page);
    // Look up a previously opened book by file_path (falls back to cache_key).
    // Returns false if the book is not in history.
    static bool find(const std::string& file_path, HistoryEntry& out);
private:
    static constexpr int kMaxEntries = 50;
    static constexpr const char* kFilePath = ".history";
};

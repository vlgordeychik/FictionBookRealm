#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace fs { class FileSystem; }

// Предел длины аннотации в байтах. Общий для обоих источников: и разбор
// .fb2, и уже разобранный DOM открытой книги. Иначе текст на полке
// отличался бы до и после перезагрузки устройства.
constexpr size_t kAnnCap = 2048;

struct CoverResult {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    std::string annotation;
    std::string cover_image_id;
};

bool load_cover_from_fb2(const std::string& path,
                         int max_w, int max_h, CoverResult& out);

bool load_cover_from_fb2_fs(fs::FileSystem* fs, const std::string& path,
                            int max_w, int max_h, CoverResult& out);

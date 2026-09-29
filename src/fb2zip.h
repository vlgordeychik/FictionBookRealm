#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace fs { class FileSystem; }

// ─── Чтение книг из ZIP-архивов (FB2.ZIP) ────────────────────
// Обёртка над stb_zip.h. stb_zip компилируется только в fb2zip.cpp
// (STB_ZIP_IMPLEMENTATION), чтобы не плодить дубликаты символов.
namespace fb2zip {

// Лимиты под free heap ESP32 (~3.3MB после загрузки шрифтов).
// Пик open: raw zip + uncompressed FB2 (без лишней копии).
constexpr size_t kMaxBookBytes = 3 * 1024 * 1024;          // raw .zip
constexpr size_t kMaxUncompressed = 3 * 1024 * 1024;       // распакованный .fb2

// Истина, если путь оканчивается на .zip (без учёта регистра).
bool is_zip_path(const std::string& path);

// Последняя строковая ошибка (для UI/лога). Пустая — если не было ошибок.
const char* last_error();
void clear_error();

// Разбирает ZIP-архив в памяти и извлекает первый файл .fb2 / .fb2.xml
// в 'out' (владение: free()). Возвращает false при любой ошибке.
bool extract_first_fb2(const uint8_t* data, size_t size, std::vector<char>& out);

// То же, но буфер сразу в 'out' (custom deleter = free), без копирования.
// out_size — размер в байтах.
struct FreeDeleter {
    void operator()(char* p) const noexcept;
};
using ZipBuffer = std::unique_ptr<char, FreeDeleter>;

bool extract_first_fb2_buf(const uint8_t* data, size_t size,
                           ZipBuffer& out, size_t& out_size);

// Загружает FB2-контент книги по пути 'path'.
//  - in_place == true : обычный fopen-able .fb2 — парсить напрямую (out пуст).
//  - иначе: распакованный zip (или raw через fs) в 'out' (free-владение).
bool load_book_fb2(fs::FileSystem* fs, const std::string& path,
                   bool& in_place, ZipBuffer& out, size_t& out_size);

// Потоково распаковывает первый .fb2 из zip-архива 'zip_path'
// в файл 'out_path' на том же volume (SD / FAT image).
// Пик RAM: ~4KB (CD scan) + 64KB (IO) + 32KB (tinfl dict) ≈ ~100KB.
// При ошибке out_path удаляется. Возвращает false и last_error().
bool extract_fb2_to_file(fs::FileSystem* fs,
                         const std::string& zip_path,
                         const std::string& out_path);

} // namespace fb2zip

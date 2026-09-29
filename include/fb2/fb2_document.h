#pragma once
#include "fb2_types.h"
#include "fb2_metadata.h"
#include "fb2_index.h"
#include <string>
#include <memory>
#include <cstdlib>

namespace fs { class FileSystem; }

namespace fb2 {

// free()/malloc() — не new: на ESP new(nothrow) при OOM abort() (нет исключений)
struct FbrBufDeleter {
    void operator()(char* p) const noexcept { std::free(p); }
};
using FbrBuf = std::unique_ptr<char, FbrBufDeleter>;

// ─── Управление документом ─────────────────────────────────────
// Fb2Document — центральный класс фреймворка.
// Отвечает за:
//   - открытие FB2-файла
//   - построение индекса (однократный проход)
//   - чтение фрагментов XML с диска по запросу
//   - хранение метаданных
//
// Поток данных:
//   load(path)
//     ├── прочитать файл в буфер
//     ├── распарсить PugiXML (DOM)
//     ├── обойти DOM → собрать BookMeta + DocumentIndex
//     ├── освободить DOM
//     └── закрыть буфер (держать файл открытым для seek+read)
//
//   read_block(block_index) → std::string
//     ├── найти BlockSpan по индексу
//     ├── seek → read → вернуть XML-фрагмент
//     └── (без парсинга — вызывающий разбирает inline-разметку)

class Fb2Document {
public:
    Fb2Document() noexcept;
    ~Fb2Document();

    // Загрузка запрещена — используйте open()
    Fb2Document(const Fb2Document&) = delete;
    Fb2Document& operator=(const Fb2Document&) = delete;
    Fb2Document(Fb2Document&&) noexcept;
    Fb2Document& operator=(Fb2Document&&) noexcept;

    // ─── Загрузка ──────────────────────────────────────────────
    bool open(const char* path);
    // Открытие через пользовательскую FS (FatFs/SD). Файл читается в буфер
    // только на время построения индекса; после этого буфер освобождается,
    // а блоки читаются по требованию через открытый fs::File* (seek+read).
    // 'temp_to_remove' — опциональный временный файл (для .zip-стеджинга),
    // который будет удалён в close() после закрытия fs::File.
    bool open_fs(fs::FileSystem* fs, const char* path, const char* temp_to_remove = nullptr);
    // Открытие из уже загруженного буфера (не требует FILE*/VFS).
    // Принимает владение буфером (malloc/free); буфер остаётся резидентным
    // для последующих read_block/read_binary (память-режим).
    bool open_memory(FbrBuf buf, size_t size);
    void close();
    bool is_open() const noexcept { return file_ != nullptr || fs_file_ != nullptr || load_buffer_ != nullptr; }

    // ─── Доступ к метаданным ───────────────────────────────────
    const BookMeta&      meta()  const noexcept { return meta_; }
    const DocumentIndex& index() const noexcept { return index_; }

    // Последняя строковая ошибка open/open_fs/open_memory (пустая — нет ошибок).
    const char* last_error() const noexcept { return last_error_.c_str(); }

    // ─── Чтение блоков ─────────────────────────────────────────
    // Возвращает XML-фрагмент блока (включая inline-разметку).
    // При ошибке возвращает пустую строку.
    std::string read_block(size_t block_index) const;

    // ─── Чтение бинарных данных (изображений) ──────────────────
    // Возвращает сырые base64-данные.
    std::string read_binary(size_t image_index) const;

    // ─── Информация ────────────────────────────────────────────
    const char* path() const noexcept { return path_; }

private:
    bool build_index(const char* xml_data, size_t xml_size);
    // Потоковое построение индекса: читает файл порциями (~16KB),
    // не аллоцирует его целиком. Используется в open()/open_fs().
    bool read_file_into_buffer(const char* path);
    void set_error(const char* msg);
    void set_errorf(const char* fmt, ...);

    char        path_[512];
    void*       file_;           // FILE* (скрыто для избежания #include <cstdio> в заголовке)
    void*       fs_file_;        // fs::File* (открытый FatFs-хендл для seek+read)
    void*       fs_owner_;       // fs::FileSystem* (для удаления temp-файла в close())
    std::string temp_path_;      // временный файл (.zip-стединг) — удаляется в close()
    BookMeta    meta_;
    DocumentIndex index_;
    std::string last_error_;

    // Временный буфер для загрузки файла (освобождается после построения индекса)
    // После build_index() остается только FILE* для seek+read.
    // Для open/open_fs (streaming) остаётся nullptr — индекс строится без полного буфера.
    FbrBuf      load_buffer_;
    size_t load_size_ = 0;
};

} // namespace fb2

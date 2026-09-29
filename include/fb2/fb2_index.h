#pragma once
#include "fb2_types.h"
#include <string>
#include <vector>
#include <cstdint>

namespace fb2 {

// ─── Запись оглавления ────────────────────────────────────────
// Хранит заголовок секции + диапазон блоков, которые в неё входят.
// Вложенные секции образуют дерево.
struct TocEntry {
    std::string title;               // заголовок секции
    size_t      first_block;         // индекс первого блока секции
    size_t      last_block;          // индекс последнего блока
    int         depth;               // уровень вложенности (0 = корневая)
    std::vector<TocEntry> children;

    TocEntry() noexcept : first_block(0), last_block(0), depth(0) {}
};

// ─── Запись сноски ─────────────────────────────────────────────
struct FootnoteEntry {
    std::string id;                  // идентификатор сноски (без #)
    size_t      first_block;         // блоки текста сноски
    size_t      last_block;
    // Список блоков в основном тексте, где встречается ссылка на сноску
    // Используется для подсветки / навигации.
    std::vector<size_t> ref_blocks;
};

// ─── Запись изображения ────────────────────────────────────────
struct ImageEntry {
    std::string id;                  // идентификатор (совпадает с атрибутом id в <binary>)
    std::string content_type;        // MIME-тип (image/jpeg, image/png)
    FileOffset  binary_offset;       // смещение к содержимому <binary> (base64)
    FileLength  binary_length;       // длина base64-строки
    size_t      decoded_size;        // размер после декодирования (0 если не вычислен)

    // Блоки, которые ссылаются на это изображение
    std::vector<size_t> ref_blocks;
};

// ─── Индекс документа ─────────────────────────────────────────
// Собирается при загрузке и остаётся в памяти.
// Размер индекса пропорционален количеству структурных элементов, а не размеру текста.
// Для книги в 500KB типичный размер индекса — 10-30KB.
struct DocumentIndex {
    // Все блоки контента в порядке чтения.
    // body[0] (основной текст), затем body[1] (сноски).
    // Позволяет линейную навигацию без загрузки всего XML.
    std::vector<BlockSpan> blocks;

    // --- Оглавление ---
    TocEntry toc_root;               // корень дерева разделов
    std::vector<const TocEntry*> toc_flat;  // плоский список для быстрого доступа

    // --- Сноски ---
    std::vector<FootnoteEntry> footnotes;
    size_t notes_body_start = 0;     // индекс первого блока в body[name="notes"]

    // --- Изображения ---
    std::vector<ImageEntry> images;

    // --- Вспомогательное ---
    size_t total_blocks() const noexcept { return blocks.size(); }
    bool   has_footnotes() const noexcept { return !footnotes.empty(); }
    bool   has_images() const noexcept { return !images.empty(); }

    void clear();
};

} // namespace fb2

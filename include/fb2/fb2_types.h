#pragma once
#include <cstdint>
#include <cstddef>

// ─── Режимы памяти ─────────────────────────────────────────────
// Фреймворк спроектирован для маломощных устройств.
// Основные приёмы экономии памяти:
//   1. Индекс хранит только смещения + длины блоков — сами данные не кешируются.
//   2. Страницы формируются по требованию, LRU-кеш ограничен.
//   3. Изображения декодируются лениво, кеш ограничен.
//   4. Файл не отображается в память целиком — только seek+read.

namespace fb2 {

// ─── Базовые типы ──────────────────────────────────────────────
using FileOffset = uint64_t;
using FileLength = uint32_t;

// ─── Типы блоков содержимого ───────────────────────────────────
enum class BlockType : uint8_t {
    Section     = 0,  // <section>
    Title       = 1,  // <title>
    Paragraph   = 2,  // <p>
    Subtitle    = 3,  // <subtitle>
    Poem        = 4,  // <poem>
    Stanza      = 5,  // <stanza>
    Verse       = 6,  // <v>
    Cite        = 7,  // <cite>
    Epigraph    = 8,  // <epigraph>
    EmptyLine   = 9,  // <empty-line/>
    Image       = 10, // <image>
    Table       = 11, // <table>
    TextAuthor  = 12, // <text-author>
    Annotation  = 13, // <annotation>
};

// ─── Флаги оформления текста ───────────────────────────────────
enum class StyleFlags : uint16_t {
    None          = 0,
    Strong        = 1 << 0,  // жирный
    Emphasis      = 1 << 1,  // курсив
    Strikethrough = 1 << 2,  // зачёркнутый
    Subscript     = 1 << 3,  // нижний индекс
    Superscript   = 1 << 4,  // верхний индекс
    Code          = 1 << 5,  // моноширинный
    HasFootnote   = 1 << 6,  // содержит сноску
    HasImage      = 1 << 7,  // содержит встроенное изображение
};

constexpr StyleFlags operator|(StyleFlags a, StyleFlags b) {
    return static_cast<StyleFlags>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr StyleFlags operator&(StyleFlags a, StyleFlags b) {
    return static_cast<StyleFlags>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}

// ─── Позиция блока в файле ─────────────────────────────────────
// Каждый блок контента представлен смещением и длиной в исходном XML.
// Это позволяет читать только нужные фрагменты с диска.
struct BlockSpan {
    FileOffset offset;
    FileLength length;
    BlockType  type;
    int16_t    image_index = -1;  // index into DocumentIndex::images, -1 = none

    BlockSpan() noexcept : offset(0), length(0), type(BlockType::Paragraph) {}
    BlockSpan(FileOffset off, FileLength len, BlockType t) noexcept
        : offset(off), length(len), type(t) {}
};

// ─── Параметры экрана ──────────────────────────────────────────
struct ScreenMetrics {
    int width_chars  = 80;   // ширина в символах
    int height_lines = 40;   // высота в строках
};

// ─── Параметры кеша ────────────────────────────────────────────
struct CacheLimits {
    int max_pages     = 10;  // макс. страниц в текстовом кеше
    int max_images    = 3;   // макс. декодированных изображений
    int max_images_kb = 512; // макс. суммарный вес изображений (KB)
};

} // namespace fb2

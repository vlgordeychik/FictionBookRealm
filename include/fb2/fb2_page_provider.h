#pragma once
#include "fb2_types.h"
#include "fb2_document.h"
#include <string>
#include <vector>
#include <cstdint>

namespace fb2 {

// ─── Отформатированная страница ────────────────────────────────
// Результат рендеринга одного экрана текста.
struct Page {
    std::vector<std::string> lines;    // строки с ANSI-разметкой
    size_t first_block;                // индекс первого блока на странице
    size_t last_block;                 // индекс последнего блока

    bool has_footnotes = false;        // страница содержит сноски
    bool has_images    = false;        // страница содержит изображения

    void clear() { lines.clear(); first_block = last_block = 0; }
};

// ─── Поставщик страниц ────────────────────────────────────────
// Преобразует индекс в отформатированные страницы.
// Страницы формируются по требованию и кешируются (LRU).
//
// Принцип работы:
//   1. getPage(n) вызывается навигатором
//   2. Если страница n в кеше → вернуть
//   3. Иначе: найти диапазон блоков для страницы n
//      (итеративно рендерить блоки, пока не наберётся экран)
//   4. Для каждого блока: read_block() → распарсить inline-разметку
//      → применить word-wrap → добавить строки на страницу
//   5. Записать в кеш, вернуть
//
// Кеш: фиксированное количество страниц (LRU).
// При переполнении вытесняется самая старая.

class PageProvider {
public:
    explicit PageProvider(const Fb2Document* doc,
                          ScreenMetrics screen = ScreenMetrics{},
                          CacheLimits limits = CacheLimits{});
    ~PageProvider();

    PageProvider(const PageProvider&) = delete;
    PageProvider& operator=(const PageProvider&) = delete;

    // ─── Получение страницы ────────────────────────────────────
    const Page* getPage(int number);
    int total_pages() const noexcept { return total_pages_; }

    // ─── Перенастройка ─────────────────────────────────────────
    void set_screen(const ScreenMetrics& s);
    void set_cache_limits(const CacheLimits& l);
    void invalidate_cache();

private:
    // Внутренняя структура кеша (скрыта от заголовка)
    struct PageCache;

    int         build_page(int number, Page& out);
    int         find_page_for_block(size_t block_index) const;

    const Fb2Document* doc_;
    ScreenMetrics      screen_;
    CacheLimits        limits_;
    PageCache*         cache_;
    int                total_pages_ = 0;
};

} // namespace fb2

#pragma once
#include "fb2_types.h"
#include "fb2_index.h"
#include "fb2_page_provider.h"
#include <vector>
#include <utility>

namespace fb2 {

// ─── Навигация по документу ────────────────────────────────────
// Управляет положением читателя в книге.
//
// Возможности:
//   - Листание вперёд/назад (постранично)
//   - Переход к разделу по оглавлению
//   - Переход по сноске и возврат назад
//   - История навигации (стек позиций)
//
// Состояние: номер текущей страницы.
// Navigator не хранит текст — он делегирует PageProvider.

class Navigator {
public:
    explicit Navigator(const Fb2Document* doc);

    // ─── Навигация ─────────────────────────────────────────────
    bool open();
    bool next_page();
    bool prev_page();
    bool go_to_page(int page_number);
    int  current_page() const noexcept { return page_; }
    int  total_pages() const noexcept;

    // ─── Оглавление ────────────────────────────────────────────
    const TocEntry& toc() const;
    bool go_to_toc_entry(size_t toc_index);
    int  toc_entry_page(size_t toc_index) const;

    // ─── Сноски ────────────────────────────────────────────────
    bool has_footnotes_on_page();
    bool follow_footnote(const std::string& note_id);
    bool go_back();     // вернуться из сноски

    // ─── Изображения ───────────────────────────────────────────
    bool has_images_on_page();
    std::vector<size_t> images_on_page();

    // ─── Состояние ─────────────────────────────────────────────
    void reset();        // сбросить на первую страницу
    const Page* current_page_data();

private:
    struct HistoryEntry {
        int  page;
        int  scroll_y;   // для будущей прокрутки внутри страницы
    };

    const Fb2Document* doc_;
    PageProvider       provider_;
    int                page_ = 0;
    std::vector<HistoryEntry> history_;
};

} // namespace fb2

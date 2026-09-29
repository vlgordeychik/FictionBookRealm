#include "fb2/fb2_navigator.h"

namespace fb2 {

Navigator::Navigator(const Fb2Document* doc)
    : doc_(doc)
    , provider_(doc)
{
}

bool Navigator::open() {
    reset();
    return total_pages() > 0;
}

bool Navigator::next_page() {
    if (page_ + 1 >= total_pages()) return false;
    page_++;
    return true;
}

bool Navigator::prev_page() {
    if (page_ <= 0) return false;
    page_--;
    return true;
}

bool Navigator::go_to_page(int page_number) {
    if (page_number < 0 || page_number >= total_pages()) return false;
    page_ = page_number;
    return true;
}

int Navigator::total_pages() const noexcept {
    return provider_.total_pages();
}

const Page* Navigator::current_page_data() {
    return provider_.getPage(page_);
}

// ─── Оглавление ────────────────────────────────────────────────

const TocEntry& Navigator::toc() const {
    return doc_->index().toc_root;
}

bool Navigator::go_to_toc_entry(size_t toc_index) {
    const auto& flat = doc_->index().toc_flat;
    if (toc_index >= flat.size()) return false;

    // Ищем страницу, на которой расположен first_block данной секции
    size_t block_idx = flat[toc_index]->first_block;
    // В реальном проекте: binary search по page_map
    // Пока используем provider_.find_page_for_block
    int target_page = 0;
    // Приблизительно
    if (doc_->index().total_blocks() > 0) {
        target_page = static_cast<int>(
            block_idx * provider_.total_pages() / doc_->index().total_blocks());
    }
    return go_to_page(target_page);
}

int Navigator::toc_entry_page(size_t toc_index) const {
    const auto& flat = doc_->index().toc_flat;
    if (toc_index >= flat.size()) return -1;
    size_t block_idx = flat[toc_index]->first_block;
    if (doc_->index().total_blocks() == 0) return 0;
    return static_cast<int>(
        block_idx * provider_.total_pages() / doc_->index().total_blocks());
}

// ─── Сноски ────────────────────────────────────────────────────

bool Navigator::has_footnotes_on_page() {
    auto* p = provider_.getPage(page_);
    return p && p->has_footnotes;
}

bool Navigator::follow_footnote(const std::string& note_id) {
    // Сохраняем текущую позицию в истории
    history_.push_back({page_, 0});

    // Ищем сноску в индексе
    for (const auto& fn : doc_->index().footnotes) {
        if (fn.id == note_id) {
            // Переход к первому блоку сноски
            size_t block_idx = fn.first_block;
            int target_page = 0;
            if (doc_->index().total_blocks() > 0) {
                target_page = static_cast<int>(
                    block_idx * provider_.total_pages() / doc_->index().total_blocks());
            }
            return go_to_page(target_page);
        }
    }
    return false;
}

bool Navigator::go_back() {
    if (history_.empty()) return false;
    auto prev = history_.back();
    history_.pop_back();
    return go_to_page(prev.page);
}

// ─── Изображения ───────────────────────────────────────────────

bool Navigator::has_images_on_page() {
    auto* page = provider_.getPage(page_);
    return page && page->has_images;
}

std::vector<size_t> Navigator::images_on_page() {
    // Заглушка: в реальном проекте — поиск image-блоков на текущей странице
    return {};
}

// ─── Состояние ─────────────────────────────────────────────────

void Navigator::reset() {
    page_ = 0;
    history_.clear();
    provider_.invalidate_cache();
}

} // namespace fb2

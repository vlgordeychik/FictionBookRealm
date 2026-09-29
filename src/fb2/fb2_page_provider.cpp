#include "fb2/fb2_page_provider.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <list>

namespace fb2 {

// ─── LRU-кеш страниц ──────────────────────────────────────────
struct PageProvider::PageCache {
    struct Entry {
        int   number;
        Page  page;
    };

    std::list<Entry> entries;              // front = MRU, back = LRU
    std::unordered_map<int, decltype(entries)::iterator> map;

    int max_size;

    explicit PageCache(int max) : max_size(max) {}

    const Page* find(int n) {
        auto it = map.find(n);
        if (it == map.end()) return nullptr;
        // Move to front (MRU)
        entries.splice(entries.begin(), entries, it->second);
        return &it->second->page;
    }

    void insert(int n, Page page) {
        auto it = map.find(n);
        if (it != map.end()) {
            it->second->page = std::move(page);
            entries.splice(entries.begin(), entries, it->second);
            return;
        }
        // Evict if full
        while (static_cast<int>(entries.size()) >= max_size && !entries.empty()) {
            auto& last = entries.back();
            map.erase(last.number);
            entries.pop_back();
        }
        entries.emplace_front(Entry{n, std::move(page)});
        map[n] = entries.begin();
    }

    void clear() {
        entries.clear();
        map.clear();
    }
};

// ─── PageProvider ──────────────────────────────────────────────

PageProvider::PageProvider(const Fb2Document* doc, ScreenMetrics screen, CacheLimits limits)
    : doc_(doc)
    , screen_(screen)
    , limits_(limits)
{
    cache_ = new PageCache(limits.max_pages);
}

PageProvider::~PageProvider() {
    delete cache_;
}

void PageProvider::set_screen(const ScreenMetrics& s) {
    screen_ = s;
    invalidate_cache();
}

void PageProvider::set_cache_limits(const CacheLimits& l) {
    limits_ = l;
    cache_->max_size = l.max_pages;
    invalidate_cache();
}

void PageProvider::invalidate_cache() {
    cache_->clear();
    total_pages_ = 0;
}

const Page* PageProvider::getPage(int number) {
    if (number < 0) return nullptr;

    // Проверка кеша
    const Page* cached = cache_->find(number);
    if (cached) return cached;

    // Построить страницу
    Page page;
    int actual_num = build_page(number, page);

    if (page.lines.empty())
        return nullptr;

    // Обновить total_pages при необходимости
    if (actual_num >= total_pages_)
        total_pages_ = actual_num + 1;

    cache_->insert(actual_num, std::move(page));
    return cache_->find(actual_num);
}

// ─── Построение страницы ──────────────────────────────────────
// Формирует страницу, начиная с block_index = number * blocks_per_estimate.
// Алгоритм приблизительный — в реальном проекте требуется точный
// подсчёт строк с учётом word-wrap и стилей.

int PageProvider::build_page(int number, Page& out) {
    const auto& blocks = doc_->index().blocks;
    if (blocks.empty()) return 0;

    out.lines.clear();
    out.first_block = 0;

    // Грубая оценка: блоки распределяются равномерно
    const int est_blocks_per_page = std::max(1,
        static_cast<int>(blocks.size()) / std::max(1, total_pages_));
    size_t start = static_cast<size_t>(number) * est_blocks_per_page;
    if (start >= blocks.size()) start = blocks.size() - 1;
    if (number == 0) start = 0;

    out.first_block = start;
    int lines_added = 0;

    for (size_t i = start; i < blocks.size(); ++i) {
        const auto& span = blocks[i];

        switch (span.type) {
        case BlockType::Paragraph:
        case BlockType::Subtitle:
        case BlockType::Verse:
        case BlockType::TextAuthor: {
            std::string xml_text = doc_->read_block(i);
            if (!xml_text.empty()) {
                // Заглушка: извлекаем текст между > и </
                auto text_start = xml_text.find('>');
                auto text_end   = xml_text.rfind("</");
                if (text_start != std::string::npos && text_end != std::string::npos &&
                    text_end > text_start) {
                    std::string content = xml_text.substr(text_start + 1,
                        text_end - text_start - 1);
                    // Простейший word-wrap (без учёта стилей)
                    while (!content.empty() && lines_added < screen_.height_lines) {
                        auto line = content.substr(0, screen_.width_chars);
                        out.lines.push_back(line);
                        lines_added++;
                        if (content.size() > static_cast<size_t>(screen_.width_chars))
                            content = content.substr(screen_.width_chars);
                        else
                            content.clear();
                    }
                }
            }
            break;
        }
        case BlockType::EmptyLine:
            if (lines_added < screen_.height_lines) {
                out.lines.emplace_back();
                lines_added++;
            }
            break;
        case BlockType::Title:
        case BlockType::Epigraph:
        case BlockType::Cite:
            // Рекурсивно разобрать содержимое
            break;
        default:
            break;
        }

        if (lines_added >= screen_.height_lines) {
            out.last_block = i;
            return number;
        }
    }

    out.last_block = blocks.size() - 1;
    return number;
}

int PageProvider::find_page_for_block(size_t block_index) const {
    // В реальном проекте здесь должен быть бинарный поиск по page_map
    // Пока возвращаем приблизительный номер
    if (total_pages_ <= 1) return 0;
    return static_cast<int>(block_index * total_pages_ / doc_->index().total_blocks());
}

} // namespace fb2

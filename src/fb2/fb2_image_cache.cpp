#include "fb2/fb2_image_cache.h"
#include "image_decoder.h"
#include <list>
#include <unordered_map>
#include <string>
#include <algorithm>

namespace fb2 {

// ─── Внутренняя структура LRU-кеша ─────────────────────────────
struct ImageCache::Impl {
    struct Entry {
        size_t       image_index;
        DecodedImage image;
    };

    const Fb2Document* doc;
    CacheLimits limits;

    std::list<Entry> entries;
    std::unordered_map<size_t, decltype(entries)::iterator> map;

    size_t current_memory = 0;

    Impl(const Fb2Document* d, CacheLimits l)
        : doc(d), limits(l) {}

    const DecodedImage* find(size_t idx) {
        auto it = map.find(idx);
        if (it == map.end()) return nullptr;
        entries.splice(entries.begin(), entries, it->second);
        return &it->second->image;
    }

    void insert(size_t idx, DecodedImage img) {
        size_t mem = img.memory_usage();

        // Вытесняем, пока не влезем
        while (!entries.empty() &&
               (static_cast<int>(entries.size()) >= limits.max_images ||
                current_memory + mem > static_cast<size_t>(limits.max_images_kb) * 1024))
        {
            auto& last = entries.back();
            current_memory -= last.image.memory_usage();
            map.erase(last.image_index);
            entries.pop_back();
        }

        entries.emplace_front(Entry{idx, std::move(img)});
        map[idx] = entries.begin();
        current_memory += mem;
    }

    void clear() {
        entries.clear();
        map.clear();
        current_memory = 0;
    }
};

// ─── ImageCache ────────────────────────────────────────────────

ImageCache::ImageCache(const Fb2Document* doc, CacheLimits limits) {
    impl_ = new Impl(doc, limits);
}

ImageCache::~ImageCache() {
    delete impl_;
}

void ImageCache::set_limits(const CacheLimits& l) {
    impl_->limits = l;
    clear();
}

void ImageCache::clear() {
    impl_->clear();
}

size_t ImageCache::current_memory_usage() const noexcept {
    return impl_->current_memory;
}

const DecodedImage* ImageCache::get(size_t image_index) {
    // Проверка кеша
    const DecodedImage* cached = impl_->find(image_index);
    if (cached) return cached;

    // Загрузка из документа
    const auto& images = impl_->doc->index().images;
    if (image_index >= images.size()) return nullptr;

    std::string base64_data = impl_->doc->read_binary(image_index);
    if (base64_data.empty()) return nullptr;

    // ─── Декодирование base64 → stb_image ──────────────────────
    auto raw = ImageDecoder::decode(base64_data);
    if (raw.pixels.empty()) return nullptr;

    DecodedImage img;
    img.id = images[image_index].id;
    img.width  = raw.width;
    img.height = raw.height;
    img.pixels = std::move(raw.pixels);
    img.channels = 4;

    impl_->insert(image_index, std::move(img));
    return impl_->find(image_index);
}

} // namespace fb2

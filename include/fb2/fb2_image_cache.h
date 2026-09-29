#pragma once
#include "fb2_types.h"
#include "fb2_document.h"
#include <string>
#include <vector>
#include <cstdint>

namespace fb2 {

// ─── Декодированное изображение ────────────────────────────────
struct DecodedImage {
    std::string id;              // идентификатор
    int         width  = 0;
    int         height = 0;
    // Пиксели: 4 байта на пиксель (RGBA) или 3 (RGB).
    // Для маломощных устройств хранение в сыром виде —
    // самый быстрый способ отрисовки.
    std::vector<uint8_t> pixels;
    int channels = 4;

    bool valid() const noexcept { return !pixels.empty(); }
    size_t memory_usage() const noexcept { return pixels.size(); }
};

// ─── Кеш изображений (LRU) ────────────────────────────────────
// Изображения хранятся в FB2 как base64 в элементах <binary>.
// Декодирование дорогое — необходим кеш.
//
// Стратегия:
//   - Ленивое декодирование: изображение декодируется при первом запросе
//   - LRU-вытеснение по количеству и по суммарному размеру
//   - Кеш можно очистить при нехватке памяти

class ImageCache {
public:
    explicit ImageCache(const Fb2Document* doc, CacheLimits limits = CacheLimits{});
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // ─── Получение изображения ─────────────────────────────────
    // Если не в кеше — декодировать из <binary>.
    const DecodedImage* get(size_t image_index);

    // ─── Управление кешем ──────────────────────────────────────
    void set_limits(const CacheLimits& l);
    void clear();
    size_t current_memory_usage() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace fb2

#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DecodedImageBuffer {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
};

// 16-color EPD image: one palette index (0..15) per pixel.
struct PaletteImage {
    std::vector<uint8_t> indices;
    int width = 0;
    int height = 0;
    bool empty() const { return indices.empty() || width <= 0 || height <= 0; }
};

class ImageDecoder {
public:
    static DecodedImageBuffer decode(const std::string& base64_data);
    // Decode already-raw image bytes (JPEG/PNG/…), not base64.
    static DecodedImageBuffer decode_raw(const uint8_t* data, size_t n);
    // Peek dimensions without decoding (stbi_info). false if unsupported.
    static bool probe_raw(const uint8_t* data, size_t n, int& w, int& h);
    static DecodedImageBuffer resize(const DecodedImageBuffer& src, int target_h, int content_w);
    // Decode base64 in-place into data[0..n). Returns raw byte count.
    // Output is always ≤ 3/4 of input — safe to overwrite the same buffer.
    static size_t base64_decode_inplace(uint8_t* data, size_t n);

    // Aspect-fit decode → 16-color palette indices (1 byte/pixel).
    // b64 is mutated in-place on ESP (base64 → raw JPEG) to avoid a second buffer.
    // JPEG: tjpgd streams MCU blocks → palette (no full-res RGBA).
    // Progressive (tjpgd JDR_FMT3): DC-only decoder → ~1/8 res, then palette.
    // Other / no tjpgd: heap-guarded stb path, then quantize.
    static bool decode_fit_palette(std::string& b64, int max_w, int max_h,
                                   PaletteImage& out);

    // Progressive/baseline JPEG → RGB888 at block (DC) resolution.
    // Used when tjpgd cannot decode (progressive) and full stb would OOM.
    // On success: rgb is w*h*3, w/h ≈ ceil(JPEG/8).
    static bool decode_jpeg_dc_rgb(const uint8_t* raw, size_t n,
                                   std::vector<uint8_t>& rgb,
                                   int& w, int& h);

    // Уменьшить палитровое изображение до max_w×max_h с сохранением
    // пропорций (вписывает по большей стороне, nearest neighbour). Индексы
    // палитры переносятся как есть — конвертация не нужна. out всегда ровно
    // max_w×max_h. false, если src пуста или выход выходит за пределы int.
    static bool downscale_palette(const PaletteImage& src, int max_w, int max_h,
                                  PaletteImage& out);

    // RGBA (формат CoverResult) → 16-цветные индексы, вписав в max_w×max_h.
    // Нужна холодному старту книжной полки: путь из файла отдаёт обложку в
    // RGBA, а рисовать её в буфер кадра дешевле сразу палитрой.
    static bool rgba_to_palette(const std::vector<uint8_t>& rgba, int w, int h,
                                int max_w, int max_h, PaletteImage& out);
};

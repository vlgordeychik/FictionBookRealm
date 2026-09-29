#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

class FontRenderer;
namespace fb2 { class Fb2Document; }
namespace fs { class FileSystem; }

struct LayoutFnSpan { int start, end; std::string id; };

struct CachedLayout {
    std::string file_path;
    int font_size = 0;
    float line_spacing = 0;
    float para_spacing = 0;
    int para_indent = 0;
    int width = 0;
    int height = 0;

    struct CachedLine {
        std::string text;
        bool para_indent = false;
        int block_type = 0;
        bool para_break = false;
        int block_index = -1;
        int depth = 0;
        int image_index = -1;
        int16_t space_extra = 0;   // доп. пиксели на пробел (выключка)
        struct FnLink { int start, end; std::string id; };
        std::vector<FnLink> footnotes;
    };

    struct CachedPage {
        std::vector<int> line_indices;
        int first_line_idx = 0;
        int last_line_idx = 0;
        int first_block = -1;
        int last_block = -1;
        bool is_image_page = false;
    };

    std::vector<CachedLine> lines;
    std::vector<CachedPage> pages;
    std::vector<int> block_depth;
    std::vector<int> block_first_line;
    std::vector<int> toc_entry_page;
};

// Streaming line view — avoids a second full copy of all line texts
// (OOM / abort when exceptions are off).
struct LayoutLineView {
    const std::string* text = nullptr;
    bool para_indent = false;
    bool para_break = false;
    int block_type = 0;
    int block_index = -1;
    int depth = 0;
    int image_index = -1;
    int16_t space_extra = 0;
    const std::vector<LayoutFnSpan>* footnotes = nullptr;
};

class LayoutCache {
public:
    // Версия 6: исправлено распознавание самозакрытых тегов (<empty-line/>), из-за
    // чего из индекса выпадали блоки целых секций. Число блоков книги изменилось
    // (у «Нейроманта» 632 -> 3395), поэтому старый кэш описывал бы обрезанный
    // текст. Дополнительно load_cache сверяет число блоков с индексом.
    // Версия 5: оглавление стало длиннее — в оглавление попадают <subtitle>.
    // В кэш пишется toc_entry_page, и его длина изменилась: со старым кэшем
    // сопоставление записей съезжает, а страницы уходят в 0.
    // Версия 4: добавлен space_extra (выключка строк) и font_file в ключ кэша.
    static constexpr int kVersion = 6;

    static void set_fs(fs::FileSystem* fs);
    // Stream lines from callback (index → view). Never builds CachedLayout.
    static bool save_stream(const std::string& file_path,
                            const std::string& font_file,
                            int font_size, float line_spacing, float para_spacing, int para_indent,
                            int width, int height,
                            int num_lines,
                            const std::function<bool(int idx, LayoutLineView& out)>& get_line,
                            const std::vector<CachedLayout::CachedPage>& pages,
                            const std::vector<int>& block_depth,
                            const std::vector<int>& block_first_line,
                            const std::vector<int>& toc_entry_page);
    static bool load(const std::string& file_path,
                     const std::string& font_file,
                     int font_size, float line_spacing, float para_spacing, int para_indent,
                     int width, int height,
                     CachedLayout& data);
    static std::string cache_dir();
};

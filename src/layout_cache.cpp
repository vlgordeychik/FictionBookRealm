#include "layout_cache.h"
#include "filesys/filesystem.h"
#include <cstring>
#include <cstdio>

static fs::FileSystem* g_cache_fs = nullptr;

static uint32_t fnv1a_hash(const void* data, size_t len) {
    uint32_t h = 2166136261u;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t compute_hash(const std::string& file_path,
                             const std::string& font_file,
                             int font_size, float line_spacing, float para_spacing,
                             int para_indent, int width, int height) {
    uint32_t h = fnv1a_hash(file_path.data(), file_path.size());
    // Шрифт влияет на раскладку напрямую (ширина слов, точки переноса), поэтому
    // раньше его отсутствие в ключе давало устаревшую раскладку после смены
    // font_file при тех же прочих параметрах.
    h = fnv1a_hash(font_file.data(), font_file.size()) ^ (h * 11);
    h = fnv1a_hash(&font_size, sizeof(font_size)) ^ (h * 13);
    h = fnv1a_hash(&line_spacing, sizeof(line_spacing)) ^ (h * 17);
    h = fnv1a_hash(&para_spacing, sizeof(para_spacing)) ^ (h * 19);
    h = fnv1a_hash(&para_indent, sizeof(para_indent)) ^ (h * 23);
    h = fnv1a_hash(&width, sizeof(width)) ^ (h * 29);
    h = fnv1a_hash(&height, sizeof(height)) ^ (h * 31);
    return h;
}

static std::string cache_filename(uint32_t hash) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08lx.fb2c", (unsigned long)hash);
    return std::string(buf);
}

void LayoutCache::set_fs(fs::FileSystem* fs) {
    g_cache_fs = fs;
}

std::string LayoutCache::cache_dir() {
    return ".cache";
}

static bool write_u8(fs::File* f, uint8_t v) { return f->write(&v, 1) == 1; }
static bool write_u16(fs::File* f, uint16_t v) { return f->write(&v, 2) == 2; }
static bool write_i16(fs::File* f, int16_t v) { return f->write(&v, 2) == 2; }
static bool write_i32(fs::File* f, int32_t v) { return f->write(&v, 4) == 4; }
static bool write_u32(fs::File* f, uint32_t v) { return f->write(&v, 4) == 4; }
static bool write_float(fs::File* f, float v) { return f->write(&v, 4) == 4; }

static bool write_str(fs::File* f, const std::string& s) {
    uint16_t len = (uint16_t)s.size();
    if (!write_u16(f, len)) return false;
    if (len > 0 && f->write(s.data(), len) != len) return false;
    return true;
}

static bool read_u8(fs::File* f, uint8_t& v) { return f->read(&v, 1) == 1; }
static bool read_u16(fs::File* f, uint16_t& v) { return f->read(&v, 2) == 2; }
static bool read_i16(fs::File* f, int16_t& v) { return f->read(&v, 2) == 2; }
static bool read_i32(fs::File* f, int32_t& v) { return f->read(&v, 4) == 4; }
static bool read_u32(fs::File* f, uint32_t& v) { return f->read(&v, 4) == 4; }
static bool read_float(fs::File* f, float& v) { return f->read(&v, 4) == 4; }

static bool read_str(fs::File* f, std::string& s) {
    uint16_t len;
    if (!read_u16(f, len)) return false;
    s.resize(len);
    if (len > 0 && f->read(&s[0], len) != len) return false;
    return true;
}

static bool ensure_cache_dir(fs::FileSystem* fs) {
    if (!fs) return false;
    std::string dir = LayoutCache::cache_dir();
    if (fs->exists(dir.c_str())) return true;
    fs->mkdir(dir.c_str());
    return fs->exists(dir.c_str());
}

bool LayoutCache::save_stream(const std::string& file_path,
                              const std::string& font_file,
                              int font_size, float line_spacing, float para_spacing, int para_indent,
                              int width, int height,
                              int num_lines,
                              const std::function<bool(int idx, LayoutLineView& out)>& get_line,
                              const std::vector<CachedLayout::CachedPage>& pages,
                              const std::vector<int>& block_depth,
                              const std::vector<int>& block_first_line,
                              const std::vector<int>& toc_entry_page) {
    if (!g_cache_fs) return false;
    if (!ensure_cache_dir(g_cache_fs)) return false;

    uint32_t hash = compute_hash(file_path, font_file, font_size, line_spacing, para_spacing,
                                  para_indent, width, height);
    std::string path = cache_dir() + "/" + cache_filename(hash);

    fs::File* f = g_cache_fs->create(path.c_str());
    if (!f) return false;

    const char magic[4] = {'F','B','2','C'};
    f->write(magic, 4);

    write_u32(f, kVersion);
    write_float(f, line_spacing);
    write_float(f, para_spacing);
    write_i32(f, font_size);
    write_i32(f, para_indent);
    write_i32(f, width);
    write_i32(f, height);

    int32_t nl = num_lines;
    int32_t np = (int32_t)pages.size();
    int32_t nb = (int32_t)block_depth.size();
    int32_t nt = (int32_t)toc_entry_page.size();
    write_i32(f, nl);
    write_i32(f, np);
    write_i32(f, nb);
    write_i32(f, nt);

    // Stream lines one-by-one — no second copy of all texts in RAM.
    for (int i = 0; i < num_lines; ++i) {
        LayoutLineView lv;
        if (!get_line || !get_line(i, lv) || !lv.text) { delete f; return false; }
        write_str(f, *lv.text);
        uint8_t flags = (lv.para_indent ? 1 : 0) | (lv.para_break ? 2 : 0);
        write_u8(f, flags);
        write_u8(f, (uint8_t)lv.block_type);
        int32_t tmp;
        tmp = lv.block_index; write_i32(f, tmp);
        tmp = lv.depth;       write_i32(f, tmp);
        tmp = lv.image_index; write_i32(f, tmp);
        write_i16(f, lv.space_extra);
        uint16_t fn_count = lv.footnotes ? (uint16_t)lv.footnotes->size() : 0;
        write_u16(f, fn_count);
        if (fn_count > 0 && lv.footnotes) {
            for (const auto& fn : *lv.footnotes) {
                tmp = fn.start; write_i32(f, tmp);
                tmp = fn.end;   write_i32(f, tmp);
                write_str(f, fn.id);
            }
        }
    }

    for (const auto& pg : pages) {
        uint16_t line_count = (uint16_t)pg.line_indices.size();
        write_u16(f, line_count);
        int32_t tmp;
        tmp = pg.first_line_idx; write_i32(f, tmp);
        tmp = pg.last_line_idx;  write_i32(f, tmp);
        tmp = pg.first_block;    write_i32(f, tmp);
        tmp = pg.last_block;     write_i32(f, tmp);
        write_u8(f, pg.is_image_page ? 1 : 0);
        for (int idx : pg.line_indices) {
            tmp = idx;
            write_i32(f, tmp);
        }
    }

    for (int d : block_depth) {
        int32_t tmp = d;
        write_i32(f, tmp);
    }
    for (int fl : block_first_line) {
        int32_t tmp = fl;
        write_i32(f, tmp);
    }
    for (int p : toc_entry_page) {
        int32_t tmp = p;
        write_i32(f, tmp);
    }

    bool ok = f->sync();
    delete f;
    return ok;
}

bool LayoutCache::load(const std::string& file_path,
                       const std::string& font_file,
                       int font_size, float line_spacing, float para_spacing, int para_indent,
                       int width, int height,
                       CachedLayout& data) {
    if (!g_cache_fs) return false;

    uint32_t hash = compute_hash(file_path, font_file, font_size, line_spacing, para_spacing,
                                  para_indent, width, height);
    std::string path = cache_dir() + "/" + cache_filename(hash);

    fs::File* f = g_cache_fs->open(path.c_str());
    if (!f) return false;

    // Read header
    char magic[4];
    if (f->read(magic, 4) != 4 || std::memcmp(magic, "FB2C", 4) != 0) { delete f; return false; }

    uint32_t version;
    read_u32(f, version);
    if (version != kVersion) { delete f; return false; }

    float ls, ps;
    int32_t fs_val, pi, w, h;
    read_float(f, ls);
    read_float(f, ps);
    read_i32(f, fs_val);
    read_i32(f, pi);
    read_i32(f, w);
    read_i32(f, h);

    if (fs_val != font_size || ls != line_spacing || ps != para_spacing ||
        pi != para_indent || w != width || h != height) {
        delete f;
        return false;
    }

    int32_t num_lines, num_pages, num_blocks, num_toc;
    read_i32(f, num_lines);
    read_i32(f, num_pages);
    read_i32(f, num_blocks);
    read_i32(f, num_toc);

    data.file_path = file_path;
    data.font_size = font_size;
    data.line_spacing = line_spacing;
    data.para_spacing = para_spacing;
    data.para_indent = para_indent;
    data.width = width;
    data.height = height;

    // Lines
    data.lines.resize(num_lines);
    for (int i = 0; i < num_lines; ++i) {
        auto& ln = data.lines[i];
        if (!read_str(f, ln.text)) { delete f; return false; }
        uint8_t flags;
        read_u8(f, flags);
        ln.para_indent = (flags & 1) != 0;
        ln.para_break = (flags & 2) != 0;
        uint8_t bt;
        read_u8(f, bt);
        ln.block_type = bt;
        int32_t tmp;
        read_i32(f, tmp); ln.block_index = tmp;
        read_i32(f, tmp); ln.depth = tmp;
        read_i32(f, tmp); ln.image_index = tmp;
        int16_t se = 0;
        read_i16(f, se); ln.space_extra = se;
        uint16_t fn_count;
        read_u16(f, fn_count);
        ln.footnotes.resize(fn_count);
        for (int j = 0; j < fn_count; ++j) {
            read_i32(f, tmp); ln.footnotes[j].start = tmp;
            read_i32(f, tmp); ln.footnotes[j].end = tmp;
            read_str(f, ln.footnotes[j].id);
        }
    }

    // Pages
    data.pages.resize(num_pages);
    for (int i = 0; i < num_pages; ++i) {
        auto& pg = data.pages[i];
        uint16_t line_count;
        int32_t tmp;
        read_u16(f, line_count);
        read_i32(f, tmp); pg.first_line_idx = tmp;
        read_i32(f, tmp); pg.last_line_idx = tmp;
        read_i32(f, tmp); pg.first_block = tmp;
        read_i32(f, tmp); pg.last_block = tmp;
        uint8_t is_img;
        read_u8(f, is_img);
        pg.is_image_page = is_img != 0;
        pg.line_indices.resize(line_count);
        for (int j = 0; j < line_count; ++j) {
            read_i32(f, tmp);
            pg.line_indices[j] = tmp;
        }
    }

    // Block metadata
    data.block_depth.resize(num_blocks);
    for (int i = 0; i < num_blocks; ++i) {
        int32_t tmp;
        read_i32(f, tmp);
        data.block_depth[i] = tmp;
    }

    data.block_first_line.resize(num_blocks);
    for (int i = 0; i < num_blocks; ++i) {
        int32_t tmp;
        read_i32(f, tmp);
        data.block_first_line[i] = tmp;
    }

    // TOC pages
    data.toc_entry_page.resize(num_toc);
    for (int i = 0; i < num_toc; ++i) {
        int32_t tmp;
        read_i32(f, tmp);
        data.toc_entry_page[i] = tmp;
    }

    delete f;
    return true;
}

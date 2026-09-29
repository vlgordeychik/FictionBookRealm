#include "fb2/fb2_document.h"
#include "filesys/filesystem.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <new>
#include <vector>
#include <utility>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define FBR_FSEEK64(f,o,w) _fseeki64(f,o,w)
  #define FBR_FTELL64(f)     _ftelli64(f)
#else
  #define FBR_FSEEK64(f,o,w) fseeko(f,o,w)
  #define FBR_FTELL64(f)     ftello(f)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "esp_system.h"
  #include "esp_heap_caps.h"
#endif

// Periodic yield during long index build: lets IDLE tasks run (task_wdt)
// and catches stack canary checks on context switch.
static void fb2_index_yield() {
#ifndef _WIN32
    static uint32_t counter = 0;
    if ((++counter & 0xFFu) == 0)
        vTaskDelay(1);
#endif
}

#include "pugixml.hpp"

namespace fb2 {

namespace {
    constexpr size_t kMaxPathLen = 512;
    constexpr size_t kStreamChunk  = 16 * 1024;
    constexpr size_t kDescHeadCap  = 128 * 1024;
    constexpr size_t kMaxTagScan   = 8 * 1024;
    constexpr size_t kMaxTitleText = 512;

    static unsigned heap_free_u32() {
#ifdef ESP_PLATFORM
        return (unsigned)esp_get_free_heap_size();
#else
        return 0u;
#endif
    }

    static BlockType block_type_from_name(const char* name) {
        if (!name) return BlockType::Paragraph;
        if (std::strcmp(name, "p") == 0)           return BlockType::Paragraph;
        if (std::strcmp(name, "poem") == 0)        return BlockType::Poem;
        if (std::strcmp(name, "subtitle") == 0)    return BlockType::Subtitle;
        if (std::strcmp(name, "cite") == 0)        return BlockType::Cite;
        if (std::strcmp(name, "epigraph") == 0)    return BlockType::Epigraph;
        if (std::strcmp(name, "empty-line") == 0)  return BlockType::EmptyLine;
        if (std::strcmp(name, "table") == 0)       return BlockType::Table;
        if (std::strcmp(name, "image") == 0)       return BlockType::Image;
        if (std::strcmp(name, "annotation") == 0)  return BlockType::Annotation;
        if (std::strcmp(name, "title") == 0)       return BlockType::Title;
        return BlockType::Paragraph; // caller must check is_block_type first
    }

    static bool is_main_block_type(const char* name) {
        if (!name) return false;
        return std::strcmp(name, "p") == 0 ||
               std::strcmp(name, "poem") == 0 ||
               std::strcmp(name, "subtitle") == 0 ||
               std::strcmp(name, "cite") == 0 ||
               std::strcmp(name, "epigraph") == 0 ||
               std::strcmp(name, "empty-line") == 0 ||
               std::strcmp(name, "table") == 0 ||
               std::strcmp(name, "image") == 0 ||
               std::strcmp(name, "annotation") == 0;
    }

    static bool is_notes_block_type(const char* name) {
        if (!name) return false;
        return std::strcmp(name, "p") == 0 ||
               std::strcmp(name, "title") == 0 ||
               std::strcmp(name, "empty-line") == 0;
    }

    // Обрезка краёв и пропуск пустых подзаголовков. Проверять содержимое на
    // «смысловые буквы» не нужно: в старых русских книгах <subtitle> — это в
    // том числе звёздочки-разделители «* * *», и они несут смысл (границы
    // рассказов: в «Сказках Упорядоченного» Перумова их 16, и каждая попадает
    // на свою страницу), поэтому в оглавление попадают наравне с осмысленными.
    // Края убираются потому, что в pretty-printed XML первый текстовый узел
    // внутри <subtitle> — это перевод строки с отступом перед <p>, и без
    // обрезки он уехал бы вправо в списке.
    static std::string trim_ws(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
        while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' ||
                         s[e-1] == '\n')) --e;
        return s.substr(b, e - b);
    }

    // ─── Byte source abstraction ────────────────────────────────
    struct ByteSource {
        virtual ~ByteSource() = default;
        virtual size_t size() const = 0;
        // Read up to n bytes at absolute offset. Returns bytes read (0 at EOF).
        virtual size_t read_at(size_t offset, void* buf, size_t n) = 0;
    };

    struct FsByteSource final : ByteSource {
        fs::File* f;
        size_t sz;
        explicit FsByteSource(fs::File* file) : f(file), sz(file->size()) {}
        size_t size() const override { return sz; }
        size_t read_at(size_t offset, void* buf, size_t n) override {
            if (!f->seek(static_cast<int64_t>(offset), SEEK_SET)) return 0;
            return f->read(buf, n);
        }
    };

    struct FileByteSource final : ByteSource {
        FILE* f;
        size_t sz;
        FileByteSource(FILE* file, size_t s) : f(file), sz(s) {}
        size_t size() const override { return sz; }
        size_t read_at(size_t offset, void* buf, size_t n) override {
            if (FBR_FSEEK64(f, static_cast<long long>(offset), SEEK_SET) != 0) return 0;
            return std::fread(buf, 1, n, f);
        }
    };

    // ─── XML tag scanner (streaming, chunked) ───────────────────
    // Yields tags with absolute file offsets. offset = position of '<'
    // (renderer prepends '<' if missing — we always include it).
    struct ScanTag {
        enum Kind : uint8_t { Open, Close, SelfClose, Other };
        Kind kind = Other;
        // name points into scanner's working buffer; copy before next().
        const char* name = nullptr;
        size_t name_len = 0;
        size_t offset = 0;  // offset of '<'
        size_t end = 0;     // offset after '>'
        // Attribute values (points into working buffer, valid until next())
        const char* attr_id = nullptr;         size_t attr_id_len = 0;
        const char* attr_name = nullptr;       size_t attr_name_len = 0;
        const char* attr_href = nullptr;       size_t attr_href_len = 0;
        const char* attr_ct = nullptr;         size_t attr_ct_len = 0;

        std::string name_str() const { return name ? std::string(name, name_len) : std::string(); }
        std::string id_str() const { return attr_id ? std::string(attr_id, attr_id_len) : std::string(); }
        std::string name_attr_str() const { return attr_name ? std::string(attr_name, attr_name_len) : std::string(); }
        std::string href_str() const { return attr_href ? std::string(attr_href, attr_href_len) : std::string(); }
        std::string ct_str() const { return attr_ct ? std::string(attr_ct, attr_ct_len) : std::string(); }
    };

    class TagScanner {
    public:
        TagScanner(ByteSource& src, std::string* err_out)
            : src_(src), err_(err_out) {
            buf_.resize(kStreamChunk);
            // Pre-read head for description extraction
            head_.clear();
        }

        // Scan entire file, invoking cb for each tag.
        // Returns false on I/O or structural error (sets *err_).
        template <typename Fn>
        bool scan_all(Fn&& cb) {
            size_t pos = 0;
            const size_t fsize = src_.size();
            while (pos < fsize) {
                fb2_index_yield();
                size_t want = std::min(kStreamChunk, fsize - pos);
                size_t got = src_.read_at(pos, buf_.data(), want);
                if (got == 0) {
                    if (err_) *err_ = "Ошибка чтения файла";
                    return false;
                }
                if (!scan_chunk(pos, got, fsize, cb)) return false;
                pos += got;
            }
            return true;
        }

        // Scan with both tag and text callbacks.
        // cb_tag(const ScanTag&), cb_text(const char*, size_t)
        template <typename FnTag, typename FnText>
        bool scan_all_ex(FnTag&& cb_tag, FnText&& cb_text) {
            size_t pos = 0;
            const size_t fsize = src_.size();
            while (pos < fsize) {
                fb2_index_yield();
                size_t want = std::min(kStreamChunk, fsize - pos);
                size_t got = src_.read_at(pos, buf_.data(), want);
                if (got == 0) {
                    if (err_) *err_ = "Ошибка чтения файла";
                    return false;
                }
                if (!scan_chunk_ex(pos, got, fsize, cb_tag, cb_text)) return false;
                pos += got;
            }
            return true;
        }

        // Head bytes collected up to and including </description> (or full head if shorter).
        const std::string& head() const { return head_; }
        bool has_desc_end() const { return desc_end_found_; }

    private:
        ByteSource& src_;
        std::string* err_;
        std::vector<char> buf_;
        std::string head_;
        bool desc_end_found_ = false;

        // Carry-over state across chunks
        bool in_tag_ = false;          // currently inside a tag (after '<')
        bool in_comment_ = false;
        bool in_cdata_ = false;
        bool in_pi_ = false;
        size_t tag_start_abs_ = 0;     // absolute offset of '<'
        std::string tag_acc_;          // accumulated tag text (without leading '<')
        bool head_done_ = false;       // stopped accumulating head (desc ended)
        size_t global_pos_ = 0;        // absolute offset of next byte to process

        // Find attribute value for key in current tag_acc_ (which excludes '<').
        // Returns pointer into tag_acc_ data.
        static bool find_attr(const std::string& tag, const char* key,
                              size_t& out_pos, size_t& out_len) {
            size_t klen = std::strlen(key);
            size_t search = 0;
            while (search < tag.size()) {
                // Match key as attribute name: start or preceded by whitespace
                size_t p = tag.find(key, search);
                if (p == std::string::npos) return false;
                // Boundary before
                if (p > 0) {
                    char c = tag[p - 1];
                    if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
                        search = p + 1;
                        continue;
                    }
                }
                // Boundary after key must be '='
                size_t after = p + klen;
                if (after < tag.size() && tag[after] == '=') {
                    // Skip '=' and whitespace
                    size_t v = after + 1;
                    while (v < tag.size() && (tag[v] == ' ' || tag[v] == '\t')) ++v;
                    if (v >= tag.size()) return false;
                    char q = tag[v];
                    if (q == '"' || q == '\'') {
                        size_t endq = tag.find(q, v + 1);
                        if (endq == std::string::npos) return false;
                        out_pos = v + 1;
                        out_len = endq - (v + 1);
                        return true;
                    } else {
                        // Unquoted value
                        size_t endv = v;
                        while (endv < tag.size() && tag[endv] != ' ' &&
                               tag[endv] != '\t' && tag[endv] != '\n' &&
                               tag[endv] != '\r' && tag[endv] != '>' &&
                               tag[endv] != '/')
                            ++endv;
                        out_pos = v;
                        out_len = endv - v;
                        return true;
                    }
                }
                search = p + 1;
            }
            return false;
        }

        template <typename Fn>
        void emit_tag(Fn&& cb, bool is_end, bool self_close,
                      size_t abs_start, size_t abs_end,
                      const std::string& tag_body) {
            // tag_body is everything after '<' up to but not including '>'
            // e.g. "section id=\"x\"" or "/section" or "?xml ..."
            ScanTag t;
            t.offset = abs_start;
            t.end = abs_end;
            size_t name_start = 0;
            if (is_end) {
                t.kind = ScanTag::Close;
                name_start = 1; // skip '/'
            } else {
                t.kind = self_close ? ScanTag::SelfClose : ScanTag::Open;
            }
            // Determine name end (first whitespace, '/', or '>')
            size_t ns = name_start;
            while (ns < tag_body.size() && tag_body[ns] != ' ' &&
                   tag_body[ns] != '\t' && tag_body[ns] != '\n' &&
                   tag_body[ns] != '\r' && tag_body[ns] != '/' && tag_body[ns] != '>')
                ++ns;
            if (ns > name_start) {
                t.name = tag_body.data() + name_start;
                t.name_len = ns - name_start;
                // Attributes only for open/self-close
                if (t.kind != ScanTag::Close) {
                    size_t ap, al;
                    if (find_attr(tag_body, "id", ap, al)) {
                        t.attr_id = tag_body.data() + ap;
                        t.attr_id_len = al;
                    }
                    if (find_attr(tag_body, "name", ap, al)) {
                        t.attr_name = tag_body.data() + ap;
                        t.attr_name_len = al;
                    }
                    // href: prefer l:href, fallback href
                    if (find_attr(tag_body, "l:href", ap, al)) {
                        t.attr_href = tag_body.data() + ap;
                        t.attr_href_len = al;
                    } else if (find_attr(tag_body, "href", ap, al)) {
                        t.attr_href = tag_body.data() + ap;
                        t.attr_href_len = al;
                    }
                    if (find_attr(tag_body, "content-type", ap, al)) {
                        t.attr_ct = tag_body.data() + ap;
                        t.attr_ct_len = al;
                    }
                }
            }
            cb(t);
        }

        template <typename Fn>
        bool scan_chunk(size_t chunk_abs, size_t chunk_len, size_t fsize, Fn&& cb) {
            return scan_chunk_ex(chunk_abs, chunk_len, fsize, cb,
                                 [](const char*, size_t) {});
        }

        template <typename FnTag, typename FnText>
        bool scan_chunk_ex(size_t chunk_abs, size_t chunk_len, size_t fsize,
                           FnTag&& cb_tag, FnText&& cb_text) {
            (void)fsize;
            const char* data = buf_.data();
            size_t i = 0;
            while (i < chunk_len) {
                if (in_comment_) {
                    size_t p = i;
                    while (p + 2 < chunk_len) {
                        if (data[p] == '-' && data[p+1] == '-' && data[p+2] == '>') {
                            p += 3;
                            in_comment_ = false;
                            i = p;
                            goto next_loop;
                        }
                        ++p;
                    }
                    i = chunk_len;
                    continue;
                }
                if (in_cdata_) {
                    size_t p = i;
                    while (p + 2 < chunk_len) {
                        if (data[p] == ']' && data[p+1] == ']' && data[p+2] == '>') {
                            p += 3;
                            in_cdata_ = false;
                            i = p;
                            goto next_loop;
                        }
                        ++p;
                    }
                    i = chunk_len;
                    continue;
                }
                if (in_pi_) {
                    size_t p = i;
                    while (p + 1 < chunk_len) {
                        if (data[p] == '?' && data[p+1] == '>') {
                            p += 2;
                            in_pi_ = false;
                            i = p;
                            goto next_loop;
                        }
                        ++p;
                    }
                    i = chunk_len;
                    continue;
                }
                if (in_tag_) {
                    size_t p = i;
                    while (p < chunk_len) {
                        if (data[p] == '>') {
                            tag_acc_.append(data + i, p - i);
                            size_t abs_start = tag_start_abs_;
                            size_t abs_end = chunk_abs + p + 1;
                            bool is_end = !tag_acc_.empty() && tag_acc_[0] == '/';
                            // tag_acc_ — всё после '<' до '>' (не включая '>'),
                            // поэтому у «empty-line/» последний символ и есть
                            // '/'. Проверять нужно последний байт: со сдвигом на
                            // единицу самозакрытый тег считался открывающимся, его
                            // кадр не закрывался и глотал все следующие
                            // соседние блоки секции до её конца.
                            bool self_close = !tag_acc_.empty() &&
                                              tag_acc_.back() == '/' &&
                                              !is_end;
                            emit_tag(cb_tag, is_end, self_close, abs_start, abs_end, tag_acc_);
                            if (!head_done_) {
                                head_.push_back('<');
                                head_.append(tag_acc_);
                                head_.push_back('>');
                                if (is_end) {
                                    std::string nm(tag_acc_.size() > 1 ? tag_acc_.data()+1 : "",
                                                   tag_acc_.size() > 1 ? tag_acc_.size()-1 : 0);
                                    if (nm == "description") {
                                        desc_end_found_ = true;
                                        head_done_ = true;
                                    }
                                }
                                if (head_.size() >= kDescHeadCap) {
                                    head_done_ = true;
                                }
                            }
                            in_tag_ = false;
                            tag_acc_.clear();
                            i = p + 1;
                            goto next_loop;
                        }
                        ++p;
                    }
                    tag_acc_.append(data + i, chunk_len - i);
                    if (tag_acc_.size() > kMaxTagScan) {
                        if (err_) *err_ = "Слишком длинный XML-тег";
                        return false;
                    }
                    i = chunk_len;
                    continue;
                }

                // Not in any special state — look for '<'
                {
                    const char* lt = static_cast<const char*>(
                        std::memchr(data + i, '<', chunk_len - i));
                    if (!lt) {
                        // Text run until end of chunk
                        if (chunk_len > i) {
                            cb_text(data + i, chunk_len - i);
                        }
                        if (!head_done_) {
                            head_.append(data + i, chunk_len - i);
                            if (head_.size() >= kDescHeadCap) head_done_ = true;
                        }
                        i = chunk_len;
                        continue;
                    }
                    size_t lt_off = static_cast<size_t>(lt - data);
                    if (lt_off > i) {
                        cb_text(data + i, lt_off - i);
                        if (!head_done_) {
                            head_.append(data + i, lt_off - i);
                            if (head_.size() >= kDescHeadCap) head_done_ = true;
                        }
                    }
                    i = lt_off + 1; // past '<'
                    tag_start_abs_ = chunk_abs + lt_off;
                    tag_acc_.clear();
                    if (i >= chunk_len) {
                        in_tag_ = true;
                        continue;
                    }
                    char c = data[i];
                    if (c == '!' && i + 2 < chunk_len && data[i+1] == '-' && data[i+2] == '-') {
                        in_comment_ = true;
                        i += 3;
                        continue;
                    }
                    if (c == '!' && i + 7 < chunk_len && data[i+1] == '[' &&
                        std::memcmp(data + i + 1, "[CDATA[", 7) == 0) {
                        in_cdata_ = true;
                        i += 8;
                        continue;
                    }
                    if (c == '?') {
                        in_pi_ = true;
                        ++i;
                        continue;
                    }
                    // Regular tag — accumulate starting from current i
                    in_tag_ = true;
                    continue;
                }
            next_loop:
                (void)0;
            }
            global_pos_ = chunk_abs + chunk_len;
            return true;
        }
    };

    // ─── Parse metadata from head buffer ────────────────────────
    static bool parse_meta_from_head(const std::string& head, BookMeta& meta) {
        if (head.empty()) return false;

        // Ensure well-formed: wrap with FictionBook if we cut mid-document.
        // head starts with optional <?xml?> and <FictionBook ...> ... </description>
        std::string xml = head;
        // Ensure description is closed
        if (xml.find("</description>") == std::string::npos)
            xml += "</description>";
        // Ensure FictionBook is closed
        if (xml.find("</FictionBook>") == std::string::npos)
            xml += "</FictionBook>";

        pugi::xml_document doc;
        pugi::xml_parse_result result = doc.load_buffer(
            xml.data(), xml.size(),
            pugi::parse_default | pugi::parse_declaration | pugi::parse_fragment);
        if (!result) {
            // Try without fragment flag
            result = doc.load_buffer(xml.data(), xml.size(),
                                     pugi::parse_default | pugi::parse_declaration);
            if (!result) return false;
        }

        auto fb = doc.child("FictionBook");
        if (!fb) {
            // Fragment mode may not have FictionBook wrapper — try first child
            fb = doc.first_child();
            if (fb && std::strcmp(fb.name(), "FictionBook") != 0) {
                // Description might be root fragment
                if (std::strcmp(fb.name(), "description") == 0) {
                    // Re-parse differently — extract description fields manually
                    // Fallback: treat fb as description
                    auto ti = fb.child("title-info");
                    if (ti) {
                        meta.title_info.book_title = ti.child("book-title").text().as_string();
                        meta.title_info.lang = ti.child("lang").text().as_string();
                    }
                    return !meta.title_info.book_title.empty();
                }
                return false;
            }
            if (!fb) return false;
        }

        auto desc = fb.child("description");
        auto ti   = desc.child("title-info");
        auto di   = desc.child("document-info");
        auto pi   = desc.child("publish-info");

        if (ti) {
            meta.title_info.book_title = ti.child("book-title").text().as_string();
            meta.title_info.lang = ti.child("lang").text().as_string();
            meta.title_info.src_lang = ti.child("src-lang").text().as_string();
            meta.title_info.keywords = ti.child("keywords").text().as_string();
            meta.title_info.date_text = ti.child("date").text().as_string();
            meta.title_info.date_value = ti.child("date").attribute("value").as_string();

            for (auto g : ti.children("genre"))
                meta.title_info.genres.push_back(g.text().as_string());

            for (auto a : ti.children("author")) {
                AuthorInfo ai;
                ai.first_name  = a.child("first-name").text().as_string();
                ai.middle_name = a.child("middle-name").text().as_string();
                ai.last_name   = a.child("last-name").text().as_string();
                ai.nickname    = a.child("nickname").text().as_string();
                meta.title_info.authors.push_back(std::move(ai));
            }

            for (auto t : ti.children("translator")) {
                AuthorInfo ai;
                ai.first_name  = t.child("first-name").text().as_string();
                ai.middle_name = t.child("middle-name").text().as_string();
                ai.last_name   = t.child("last-name").text().as_string();
                ai.nickname    = t.child("nickname").text().as_string();
                meta.title_info.translators.push_back(std::move(ai));
            }

            auto cover_img = ti.child("coverpage").child("image");
            if (cover_img) {
                const char* href = cover_img.attribute("l:href").value();
                if (!href || !*href)
                    href = cover_img.attribute("href").value();
                if (href && href[0] == '#')
                    meta.title_info.cover_image_id = href + 1;
                else if (href && *href)
                    meta.title_info.cover_image_id = href;
            }
        }

        if (di) {
            meta.document_info.id = di.child("id").text().as_string();
            meta.document_info.version = di.child("version").text().as_float();
            meta.document_info.program_used = di.child("program-used").text().as_string();
            meta.document_info.date_text = di.child("date").text().as_string();
            meta.document_info.date_value = di.child("date").attribute("value").as_string();
            meta.document_info.src_ocr = di.child("src-ocr").text().as_string();
        }

        if (pi) {
            meta.publish_info.book_name = pi.child("book-name").text().as_string();
            meta.publish_info.publisher = pi.child("publisher").text().as_string();
            meta.publish_info.city = pi.child("city").text().as_string();
            meta.publish_info.isbn = pi.child("isbn").text().as_string();
        }

        return true;
    }

    // ─── Streaming index builder ────────────────────────────────
    // Builds DocumentIndex without loading the whole file into RAM.
    // Single forward pass with a small chunk buffer.
    struct StreamIndexBuilder {
        DocumentIndex& index;
        size_t file_size;
        std::string* err;

        StreamIndexBuilder(DocumentIndex& idx, size_t sz, std::string* e)
            : index(idx), file_size(sz), err(e) {}

        // Element stack for depth tracking (names only, no offsets needed for blocks)
        // We only need direct-child-of-section context.
        struct Frame {
            std::string name;
            // For sections:
            bool is_section = false;
            bool is_notes_section = false;
            // TOC entry being built (main body only)
            TocEntry* toc = nullptr;
            // Footnote entry (notes body only)
            FootnoteEntry* fn = nullptr;
            // First block index when section opened
            size_t first_block = 0;
            // Pending last-block finalization: index of last block added in this section
            // that still needs its length fixed when section_end is known.
            bool has_pending_last = false;
            size_t pending_last_idx = 0;
            FileOffset pending_last_start = 0;
            // Title capture
            bool in_title = false;
            bool title_p_open = false;
            std::string title_text;
            // <subtitle> как запись оглавления: смысловой разделитель, а не
            // орнамент. Захват текста ведётся на кадре секции, отметка и
            // индекс блока — на собственном кадре <subtitle>.
            bool is_subtitle_head = false;
            size_t subtitle_block = 0;
            bool subtitle_capture = false;
            bool subtitle_p_done = false;
            std::string subtitle_text;
            // For body:
            bool is_body = false;
            bool is_notes_body = false;
        };

        std::vector<Frame> stack;

        // Main body / notes body flags
        bool in_main_body = false;
        bool in_notes_body = false;
        size_t notes_body_start_offset = 0;
        bool notes_body_seen = false;

        // Body-level section list for computing section_end (offsets of top-level sections)
        // We finalize last-block lengths when we see the next top-level section,
        // notes body, or binary/EOF.

        // Deferred finalization for last block of a section:
        // when we know section_end, patch blocks[idx].length.
        struct PendingFix {
            size_t block_idx;
            FileOffset start;
            int section_depth; // depth of owning section on stack when pending
        };
        std::vector<PendingFix> pending_fixes;

        // Depth of current section context (number of section frames on stack)
        int section_depth() const {
            int d = 0;
            for (const auto& f : stack)
                if (f.is_section) ++d;
            return d;
        }

        Frame* top() { return stack.empty() ? nullptr : &stack.back(); }
        Frame* top_section() {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                if (it->is_section) return &*it;
            return nullptr;
        }
        Frame* top_body() {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                if (it->is_body) return &*it;
            return nullptr;
        }

        // Is current position a direct child of a section (not inside title, not nested deeper)?
        bool in_section_child_ctx() const {
            if (stack.empty()) return false;
            const Frame& t = stack.back();
            if (t.is_section) return true; // we're about to see a child (stack has section on top means we're between its tags... actually no)
            // If top is section, next open tag is its child — but we call this
            // when handling an OPEN tag, before pushing. So top is the parent.
            if (t.in_title || t.title_p_open) return false;
            return t.is_section;
        }

        // Called when an OPEN tag arrives (before push). parent = stack.back()
        void on_open(const ScanTag& t) {
            std::string name = t.name_str();

            // Title text capture: inside <title>, first <p>, collect text until </p>
            Frame* sec = top_section();
            if (sec && sec->in_title && !sec->title_p_open && name == "p") {
                sec->title_p_open = true;
                sec->title_text.clear();
            }

            // Also: if we're opening title's p via the content-block path below,
            // ensure title_p_open is set on the section.
            // (handled in content block branch)

            // Section open
            if (name == "section") {
                Frame f;
                f.name = name;
                f.is_section = true;
                f.first_block = index.blocks.size();

                Frame* body = top_body();
                bool notes = body && body->is_notes_body;
                f.is_notes_section = notes;

                if (!notes) {
                    // Main body section → TOC
                    TocEntry* parent_toc = nullptr;
                    // Find nearest section frame with toc (or root)
                    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                        if (it->is_section && it->toc) { parent_toc = it->toc; break; }
                    }
                    if (!parent_toc) parent_toc = &index.toc_root;
                    parent_toc->children.emplace_back();
                    f.toc = &parent_toc->children.back();
                    f.toc->first_block = index.blocks.size();
                    f.toc->depth = 0;
                    // Compute depth
                    int d = 0;
                    for (const auto& fr : stack) if (fr.is_section) ++d;
                    f.toc->depth = d;
                } else {
                    // Notes section → FootnoteEntry
                    FootnoteEntry fe;
                    fe.id = t.id_str();
                    fe.first_block = index.blocks.size();
                    index.footnotes.push_back(std::move(fe));
                    f.fn = &index.footnotes.back();
                }

                // Finalize pending last-block of previous same-depth section if any
                finalize_pending_for_new_section();

                stack.push_back(std::move(f));
                return;
            }

            // Body open
            if (name == "body") {
                Frame f;
                f.name = name;
                f.is_body = true;
                std::string bname = t.name_attr_str();
                f.is_notes_body = (bname == "notes");
                if (f.is_notes_body) {
                    in_notes_body = true;
                    if (!notes_body_seen) {
                        notes_body_seen = true;
                        notes_body_start_offset = t.offset;
                    }
                } else if (!in_main_body) {
                    in_main_body = true;
                }
                stack.push_back(std::move(f));
                return;
            }

            // Binary open
            if (name == "binary") {
                ImageEntry ie;
                ie.id = t.id_str();
                ie.content_type = t.ct_str();
                ie.binary_offset = static_cast<FileOffset>(t.offset);
                ie.binary_length = 0; // set on close
                index.images.push_back(std::move(ie));
                Frame f;
                f.name = name;
                stack.push_back(std::move(f));
                return;
            }

            // Content blocks (direct child of section)
            Frame* parent = top();
            if (parent && parent->is_section && !parent->in_title) {
                bool in_notes = parent->is_notes_section;
                bool is_block = in_notes ? is_notes_block_type(name.c_str())
                                         : is_main_block_type(name.c_str());
                // Also allow <title> in main sections
                if (!is_block && !in_notes && name == "title")
                    is_block = true;

                if (is_block) {
                    BlockType bt = block_type_from_name(name.c_str());
                    // Title inside section: only first direct title child
                    if (name == "title") {
                        // Mark title capture on the section frame
                        parent->in_title = true;
                        parent->title_p_open = false;
                        parent->title_text.clear();
                    }

                    // Finalize previous pending last-block if this is a sibling
                    finalize_pending_for_sibling();

                    BlockSpan span(static_cast<FileOffset>(t.offset), 0, bt);
                    size_t idx = index.blocks.size();
                    index.blocks.push_back(span);

                    if (parent->has_pending_last) {
                        // Should not happen — we finalize before adding
                    }
                    parent->has_pending_last = true;
                    parent->pending_last_idx = idx;
                    parent->pending_last_start = static_cast<FileOffset>(t.offset);

                    // Image href resolution deferred until binaries known
                    // (binaries come after body). Store href in ref via span.
                    // We'll patch image_index after all binaries scanned —
                    // but binaries are after body, so at EOF we resolve.
                    // For now store href on a side list keyed by block idx.
                    if (bt == BlockType::Image) {
                        pending_image_refs.emplace_back(idx, t.href_str());
                    }

                    Frame f;
                    f.name = name;
                    // A <subtitle> that is a direct child of a body section is a
                    // navigation marker in its own right. Its own span was added
                    // above exactly like any other block, so rendering is
                    // untouched; the TOC entry is created on </subtitle>, when the
                    // captured text is already known.
                    if (bt == BlockType::Subtitle && !in_notes && parent->toc) {
                        f.is_subtitle_head = true;
                        f.subtitle_block = idx;
                        parent->subtitle_capture = true;
                        parent->subtitle_p_done = false;
                        parent->subtitle_text.clear();
                    }
                    if (name == "title") {
                        // title frame sits on stack for text capture
                        f.in_title = true;
                        // Don't use f.in_title for capture — we use parent section's flag
                        // Actually simpler: push title frame and capture into it
                    }
                    stack.push_back(std::move(f));
                    // For self-close, on_close will be called immediately after
                    return;
                }
            }

            // Inline/other elements — just push for depth tracking (no index)
            Frame f;
            f.name = name;
            stack.push_back(std::move(f));
        }

        // Called for SelfClose (open+close in one)
        void on_self_close(const ScanTag& t) {
            on_open(t);
            on_close(t);
        }

        void on_close(const ScanTag& t) {
            std::string name = t.name_str();

            if (stack.empty()) return;

            // Match top frame (lenient: search from top for matching name)
            int match = -1;
            for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
                if (stack[i].name == name) { match = i; break; }
            }
            if (match < 0) return; // stray close tag

            // If closing a title's p while capturing, stop capture
            Frame* sec = top_section();
            if (sec && sec->title_p_open && name == "p") {
                sec->title_p_open = false;
                // Copy title text to TOC entry if this is the first p
                if (sec->toc && sec->title_text.empty()) {
                    sec->toc->title = sec->title_text;
                }
                if (sec->fn && sec->title_text.empty()) {
                    // Footnotes don't use toc title
                }
            }
            // First </p> of a <subtitle> closes its capture: anything after it
            // (a second <p>) is ignored.
            if (sec && sec->subtitle_capture && !sec->subtitle_p_done && name == "p")
                sec->subtitle_p_done = true;

            Frame& f = stack[match];

            if (f.is_section) {
                // Section closing: finalize pending last block with section_end.
                // section_end = next sibling section offset OR notes_body OR file_size.
                // We don't know next sibling yet — defer until we see it or body/binary/EOF.
                // Instead: patch length now using best known end:
                // If this section will be followed by sibling/notes/EOF, the
                // pending fix waits. Move pending to global list with depth info.
                if (f.has_pending_last) {
                    PendingFix fix;
                    fix.block_idx = f.pending_last_idx;
                    fix.start = f.pending_last_start;
                    fix.section_depth = 0;
                    for (const auto& fr : stack) if (fr.is_section) ++fix.section_depth;
                    pending_fixes.push_back(fix);
                    f.has_pending_last = false;
                }

                // Set TOC last_block
                if (f.toc) {
                    if (index.blocks.empty())
                        f.toc->last_block = f.toc->first_block;
                    else
                        f.toc->last_block = index.blocks.size() - 1;
                }
                if (f.fn) {
                    if (index.blocks.empty())
                        f.fn->last_block = f.fn->first_block;
                    else
                        f.fn->last_block = index.blocks.size() - 1;
                }

                // Copy captured title text to TOC entry if not yet set
                if (f.toc && f.toc->title.empty() && !f.title_text.empty()) {
                    f.toc->title = f.title_text;
                }

                // Remove frame and all children above `match`
                stack.resize(match);
                return;
            }

            if (f.is_body) {
                // Body closing: finalize all pending fixes for sections inside this body
                // that haven't been finalized — use notes offset or file_size.
                finalize_all_pending(f.is_notes_body ? notes_body_start_offset
                                                     : static_cast<FileOffset>(file_size));
                in_main_body = false;
                in_notes_body = false;
                stack.resize(match);
                return;
            }

            if (f.is_body == false && !f.is_section && f.name == "title") {
                // Title element closing in section — handled by section's in_title flag
                Frame* parent_sec = nullptr;
                for (int i = match - 1; i >= 0; --i) {
                    if (stack[i].is_section) { parent_sec = &stack[i]; break; }
                }
                if (parent_sec) parent_sec->in_title = false;
            }

            if (f.is_subtitle_head) {
                // Subtitle element closing — create its TOC entry, if it carries
                // anything but ornament. No early return: the span of this very
                // block is still pending finalization further down, and the frame
                // is popped there too.
                Frame* sec = top_section();
                if (sec) {
                    const std::string t = trim_ws(sec->subtitle_text);
                    if (sec->toc && !t.empty()) {
                        TocEntry e;
                        e.title = t;
                        // The range is deliberately one block. set_depth() paints
                        // block_depth_ over the whole first_block..last_block of
                        // every entry, and a wide range would overwrite the depth
                        // of nested section titles that follow the subtitle.
                        e.first_block = f.subtitle_block;
                        e.last_block  = f.subtitle_block;
                        e.depth = sec->toc->depth + 1;
                        sec->toc->children.emplace_back(std::move(e));
                    }
                    sec->subtitle_capture = false;
                    sec->subtitle_p_done = false;
                    sec->subtitle_text.clear();
                }
            }

            // Binary close: set length
            if (f.name == "binary" && !index.images.empty()) {
                // Find the binary frame's corresponding image (last one)
                // Actually we pushed image on binary open — the last image entry
                ImageEntry& ie = index.images.back();
                if (ie.binary_length == 0) {
                    ie.binary_length = static_cast<FileLength>(
                        static_cast<FileOffset>(t.end) - ie.binary_offset);
                }
            }

            // Content block close: length = end - start (exact element span)
            Frame* parent = nullptr;
            for (int i = match - 1; i >= 0; --i) {
                if (stack[i].is_section) { parent = &stack[i]; break; }
            }
            // Only finalize if THIS frame is the pending block itself
            // (not a nested inline child like <strong> inside <p>)
            if (parent && parent->is_section && parent->has_pending_last) {
                bool is_block_frame = is_main_block_type(stack[match].name.c_str()) ||
                                      is_notes_block_type(stack[match].name.c_str()) ||
                                      stack[match].name == "title";
                // Confirm this frame is the pending block (same name/type)
                if (is_block_frame && parent->pending_last_idx < index.blocks.size()) {
                    BlockSpan& span = index.blocks[parent->pending_last_idx];
                    if (span.length == 0) {
                        span.length = static_cast<FileLength>(
                            static_cast<FileOffset>(t.end) - span.offset);
                    }
                    parent->has_pending_last = false;
                }
            }

            stack.resize(match);
        }

        // ─── Text capture for title ─────────────────────────────
        // Called for character data between tags (outside tags).
        void on_text(const char* text, size_t len) {
            if (len == 0) return;
            // Find if we're inside a <p> that's first child of <title> in a section
            Frame* sec = top_section();

            // Subtitle text: both inside its <p> and directly in the element
            // (<subtitle>Part one</subtitle> is legal). Stops after the first
            // </p>, so a subtitle with two <p> keeps the first — the same rule
            // the section title follows. Collected on the section frame, as the
            // title text is, so the <p> depth is irrelevant.
            if (sec && sec->subtitle_capture && !sec->subtitle_p_done) {
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (it->is_subtitle_head) {
                        if (sec->subtitle_text.size() < kMaxTitleText) {
                            size_t room = kMaxTitleText - sec->subtitle_text.size();
                            sec->subtitle_text.append(text, std::min(len, room));
                        }
                        return;
                    }
                    if (it->is_section || it->is_body) break;
                }
            }

            if (!sec || !sec->in_title || !sec->title_p_open) return;
            if (sec->title_text.size() >= kMaxTitleText) return;
            // Only capture if stack top is the <p> (or inline inside first p)
            // top_section finds nearest section; we need top frame to be p or inline in title
            // Check: nearest non-inline ancestor from top should be p in title
            bool capture = false;
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                if (it->name == "p") { capture = true; break; }
                if (it->is_section || it->is_body) break;
                // inline tags (strong, emphasis, etc.) — continue up
            }
            if (!capture) return;
            size_t room = kMaxTitleText - sec->title_text.size();
            sec->title_text.append(text, std::min(len, room));
        }

        void finalize_pending_for_sibling() {
            // Called when adding a new block sibling in current section:
            // previous pending block's length already set on its own close.
            // Nothing to do here for exact-close semantics.
            // (kept for structural compatibility / future sibling-offset logic)
        }

        void finalize_pending_for_new_section() {
            // When opening a new section at depth D, any pending last-block
            // in a section at depth >= D that's still open... actually pending
            // blocks are closed when their element closes (exact length).
            // Pending fixes (deferred section_end) are for cases where we
            // used section_end as length — with exact-close we don't need them
            // for blocks. But notes/main last-block that spans to section_end
            // (pugixml semantics for last child) is now exact-close instead.
            // Renderer truncates to </tag> anyway, so exact-close is compatible.
        }

        void finalize_all_pending(FileOffset end_pos) {
            for (const auto& fix : pending_fixes) {
                if (fix.block_idx < index.blocks.size()) {
                    BlockSpan& span = index.blocks[fix.block_idx];
                    if (span.length == 0 && end_pos > fix.start) {
                        span.length = static_cast<FileLength>(end_pos - fix.start);
                    }
                }
            }
            pending_fixes.clear();
        }

        // Resolve image hrefs after binaries are indexed
        std::vector<std::pair<size_t, std::string>> pending_image_refs;

        void resolve_images() {
            for (auto& ref : pending_image_refs) {
                size_t bi = ref.first;
                std::string href = ref.second;
                if (href.empty()) continue;
                if (href[0] == '#') href.erase(0, 1);
                for (size_t ii = 0; ii < index.images.size(); ++ii) {
                    if (index.images[ii].id == href) {
                        if (bi < index.blocks.size())
                            index.blocks[bi].image_index = static_cast<int16_t>(ii);
                        index.images[ii].ref_blocks.push_back(bi);
                        break;
                    }
                }
            }
            pending_image_refs.clear();
        }

        // Called at EOF to finalize any remaining pending fixes
        void finish() {
            finalize_all_pending(static_cast<FileOffset>(file_size));
            // Fix last binary length if unclosed
            if (!index.images.empty() && index.images.back().binary_length == 0) {
                ImageEntry& ie = index.images.back();
                if (file_size > ie.binary_offset)
                    ie.binary_length = static_cast<FileLength>(file_size - ie.binary_offset);
            }
            // notes_body_start: if notes seen, it's the offset; blocks index
            // was recorded when? We need block index at notes body start.
            // Track separately:
            if (notes_body_seen && index.notes_body_start == 0 && !index.footnotes.empty()) {
                index.notes_body_start = index.footnotes.front().first_block;
            } else if (notes_body_seen && index.notes_body_start == 0 && index.footnotes.empty()) {
                index.notes_body_start = index.blocks.size();
            }
            resolve_images();
        }
    };

    // Text scanning for body content: we need character data between tags
    // for title capture. Extend TagScanner to also yield text runs.
    // Simpler: integrate text into scan via callback overload.

    // ─── Unified stream open ────────────────────────────────────
    static bool stream_open(ByteSource& src, size_t file_size,
                            BookMeta& meta, DocumentIndex& index,
                            std::string* err_out) {
        if (file_size == 0) {
            if (err_out) *err_out = "Пустой файл";
            return false;
        }

        // Pass 1: scan for tags + collect head for metadata
        TagScanner scanner(src, err_out);

        StreamIndexBuilder builder(index, file_size, err_out);

        // We need text events for title capture — wrap scan with a custom
        // scanner that also extracts text between tags in title context.
        // For simplicity, use TagScanner.scan_all for tags, and separately
        // extract title texts by re-scanning... that's expensive.
        // Better: enhance TagScanner to call on_text.

        // Actually let's do a single custom scan here that handles both.
        // Reuse TagScanner but add text callback via a second template param.

        bool meta_parsed = false;
        bool ok = scanner.scan_all_ex(
            [&](const ScanTag& t) {
                if (t.kind == ScanTag::Other || !t.name) return;
                if (t.kind == ScanTag::Open) {
                    builder.on_open(t);
                } else if (t.kind == ScanTag::SelfClose) {
                    builder.on_self_close(t);
                } else if (t.kind == ScanTag::Close) {
                    builder.on_close(t);
                }
            },
            [&](const char* text, size_t len) {
                builder.on_text(text, len);
            });

        if (!ok) return false;

        builder.finish();

        // Parse metadata from collected head
        if (!scanner.head().empty()) {
            meta_parsed = parse_meta_from_head(scanner.head(), meta);
        }
        if (!meta_parsed) {
            // Non-fatal: book can open without full metadata
            // but title is important — try minimal extract
            if (err_out && err_out->empty()) {
                // leave empty — open succeeds
            }
        }

        // Basic sanity: need at least some blocks or images
        if (index.blocks.empty() && index.images.empty() && index.footnotes.empty()) {
            if (err_out) *err_out = "Файл повреждён или не является FB2";
            return false;
        }

        return true;
    }
}

// ─── Constructors / destructors ────────────────────────────────────

Fb2Document::Fb2Document() noexcept : file_(nullptr), fs_file_(nullptr), fs_owner_(nullptr) {
    path_[0] = '\0';
}

Fb2Document::~Fb2Document() {
    close();
}

Fb2Document::Fb2Document(Fb2Document&& other) noexcept
    : file_(other.file_)
    , fs_file_(other.fs_file_)
    , fs_owner_(other.fs_owner_)
    , temp_path_(std::move(other.temp_path_))
    , meta_(std::move(other.meta_))
    , index_(std::move(other.index_))
    , last_error_(std::move(other.last_error_))
    , load_buffer_(std::move(other.load_buffer_))
    , load_size_(other.load_size_)
{
    std::memcpy(path_, other.path_, sizeof(path_));
    other.file_ = nullptr;
    other.fs_file_ = nullptr;
    other.fs_owner_ = nullptr;
    other.load_size_ = 0;
}

Fb2Document& Fb2Document::operator=(Fb2Document&& other) noexcept {
    if (this != &other) {
        close();
        file_ = other.file_;
        fs_file_ = other.fs_file_;
        fs_owner_ = other.fs_owner_;
        temp_path_ = std::move(other.temp_path_);
        meta_ = std::move(other.meta_);
        index_ = std::move(other.index_);
        last_error_ = std::move(other.last_error_);
        load_buffer_ = std::move(other.load_buffer_);
        load_size_ = other.load_size_;
        std::memcpy(path_, other.path_, sizeof(path_));
        other.file_ = nullptr;
        other.fs_file_ = nullptr;
        other.fs_owner_ = nullptr;
        other.load_size_ = 0;
    }
    return *this;
}

void Fb2Document::set_error(const char* msg) {
    last_error_ = msg ? msg : "";
    printf("fbr: doc ERR %s\n", last_error_.c_str());
}

void Fb2Document::set_errorf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_error(buf);
}

// ─── Loading ───────────────────────────────────────────────────────

bool Fb2Document::open(const char* path) {
    close();
    last_error_.clear();

    std::strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    FILE* f = std::fopen(path, "rb");
    if (!f) {
        set_errorf("Не удалось открыть файл: %s", path);
        return false;
    }

    FBR_FSEEK64(f, 0, SEEK_END);
    auto fsize = static_cast<size_t>(FBR_FTELL64(f));
    FBR_FSEEK64(f, 0, SEEK_SET);

    if (fsize == 0) {
        std::fclose(f);
        set_error("Пустой файл");
        return false;
    }

    // Keep FILE* for seek+read after index build
    file_ = f;

    FileByteSource src(f, fsize);
    if (!stream_open(src, fsize, meta_, index_, &last_error_)) {
        close();
        if (last_error_.empty())
            set_error("Файл повреждён или не является FB2");
        return false;
    }

    // No full buffer — read_block/read_binary use FILE* seek+read
    load_buffer_.reset();
    load_size_ = 0;
    return true;
}

bool Fb2Document::open_memory(FbrBuf buf, size_t size) {
    close();
    last_error_.clear();

    load_buffer_ = std::move(buf);
    load_size_ = size;
    path_[0] = '\0';

    if (!build_index(load_buffer_.get(), load_size_)) {
        if (last_error_.empty())
            set_error("Файл повреждён или не является FB2");
        close();
        return false;
    }

    // Buffer stays resident for read_block/read_binary (memory mode)
    return true;
}

bool Fb2Document::open_fs(fs::FileSystem* fs, const char* path, const char* temp_to_remove) {
    close();
    last_error_.clear();
    if (!fs || !path) {
        set_error("Файловая система недоступна");
        return false;
    }

    temp_path_ = temp_to_remove ? temp_to_remove : "";
    fs_owner_ = fs;

    std::strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    fs::File* f = fs->open(path);
    if (!f) {
        printf("fbr: open_fs FAIL fs->open path=%s\n", path);
        set_errorf("Не удалось открыть файл: %s", path);
        close();
        return false;
    }
    fs_file_ = f;

    size_t size = f->size();
    printf("fbr: open_fs opened path=%s size=%u\n", path, (unsigned)size);
    if (size == 0) {
        set_error("Пустой файл");
        close();
        return false;
    }

    // Streaming index build: NO full-file allocation.
    // Read only ~16KB chunks + ~128KB description head.
    FsByteSource src(f);
    if (!stream_open(src, size, meta_, index_, &last_error_)) {
        printf("fbr: open_fs FAIL stream_open size=%u heap=%u\n",
               (unsigned)size, heap_free_u32());
        close();
        if (last_error_.empty())
            set_error("Файл повреждён или не является FB2");
        return false;
    }

    printf("fbr: open_fs stream OK size=%u blocks=%u imgs=%u heap=%u\n",
           (unsigned)size,
           (unsigned)index_.blocks.size(),
           (unsigned)index_.images.size(),
           heap_free_u32());
    return true;
}

void Fb2Document::close() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
    if (fs_file_) {
        delete static_cast<fs::File*>(fs_file_);
        fs_file_ = nullptr;
    }
    if (fs_owner_ && !temp_path_.empty()) {
        static_cast<fs::FileSystem*>(fs_owner_)->remove(temp_path_.c_str());
    }
    fs_owner_ = nullptr;
    temp_path_.clear();
    load_buffer_.reset();
    load_size_ = 0;
    meta_ = BookMeta{};
    index_.clear();
    last_error_.clear();
    path_[0] = '\0';
}

// ─── Block reading ────────────────────────────────────────────────

static size_t min30(size_t a, size_t b) { return a < b ? a : b; }

// Exceptions are off: operator new / string growth on OOM → abort().
// Always check the largest free block before a heap string allocation.
// SPIRAM_USE_MALLOC: 8BIT cap includes SPIRAM + internal.
static bool heap_can_alloc(size_t n) {
#ifdef ESP_PLATFORM
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    return largest >= n + 64;
#else
    (void)n;
    return true;
#endif
}

// One <p>/<title>/… element. layout_blocks also builds int[xml.size()]
// maps (~4× peak) — keep well under free heap after parse (~2.8MB).
static constexpr size_t kMaxBlockBytes = 64 * 1024;

std::string Fb2Document::read_block(size_t block_index) const {
    if (block_index >= index_.blocks.size())
        return {};

    const auto& span = index_.blocks[block_index];
    if (span.length == 0) return {};

    size_t len = static_cast<size_t>(span.length);
    if (len > kMaxBlockBytes) {
        printf("fbr: read_block reject huge bi=%u len=%u heap=%u\n",
               (unsigned)block_index, (unsigned)len, heap_free_u32());
        return {};
    }

    if (load_buffer_) {
        if (span.offset >= load_size_) return {};
        size_t n = min30(len, load_size_ - static_cast<size_t>(span.offset));
        if (n > kMaxBlockBytes) n = kMaxBlockBytes;
        if (!heap_can_alloc(n)) {
            printf("fbr: read_block OOM buf bi=%u n=%u heap=%u\n",
                   (unsigned)block_index, (unsigned)n, heap_free_u32());
            return {};
        }
        return std::string(load_buffer_.get() + span.offset, n);
    }

    if (fs_file_) {
        fs::File* f = static_cast<fs::File*>(fs_file_);
        if (!heap_can_alloc(len)) {
            printf("fbr: read_block OOM fs bi=%u len=%u heap=%u\n",
                   (unsigned)block_index, (unsigned)len, heap_free_u32());
            return {};
        }
        // Single allocation: resize once, read into &s[0] (no malloc+string peak).
        std::string out;
        out.resize(len);
        f->seek(static_cast<int64_t>(span.offset), SEEK_SET);
        size_t actually_read = f->read(&out[0], len);
        out.resize(actually_read);
        return out;
    }

    if (!file_) return {};

    if (!heap_can_alloc(len)) {
        printf("fbr: read_block OOM file bi=%u len=%u heap=%u\n",
               (unsigned)block_index, (unsigned)len, heap_free_u32());
        return {};
    }
    FILE* f = static_cast<FILE*>(file_);
    std::string out;
    out.resize(len);
    FBR_FSEEK64(f, static_cast<long long>(span.offset), SEEK_SET);
    size_t actually_read = std::fread(&out[0], 1, len, f);
    out.resize(actually_read);
    return out;
}

std::string Fb2Document::read_binary(size_t image_index) const {
    if (image_index >= index_.images.size())
        return {};

    const auto& img = index_.images[image_index];
    // Raw <binary>… element: ONE pre-sized string buffer, then strip the
    // <binary …>…</binary> wrapper in place — no second full-size copy.
    // (malloc + string copy peaked at 2×blen and OOM'd on the ~1.66MB
    // Perumov cover with ~2.2MB free.) Heap-guarded, fails gracefully.
    constexpr size_t kMaxElem = 2048 * 1024;
    constexpr size_t kGuard = 64 * 1024;

    auto strip_wrapper = [](std::string& s) -> bool {
        size_t lt = s.find('>');              // end of "<binary id=…>"
        if (lt == std::string::npos || lt + 1 >= s.size()) return false;
        ++lt;
        size_t rt = s.rfind('<');             // start of "</binary>"
        if (rt == std::string::npos || rt <= lt) return false;
        size_t clen = rt - lt;
        std::memmove(&s[0], s.data() + lt, clen);
        s.resize(clen);
        return true;
    };

#ifndef _WIN32
    // Largest CONTIGUOUS allocable block (total free can be fragmented and
    // still abort on a single big new[]→operator new→throw).
    size_t free_h = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
    size_t free_h = 0x7fffffff;
#endif

    if (load_buffer_) {
        if (img.binary_offset >= load_size_) return {};
        size_t n = min30(static_cast<size_t>(img.binary_length),
                         load_size_ - static_cast<size_t>(img.binary_offset));
        if (n == 0 || n > kMaxElem) return {};
        if (n + kGuard > free_h) return {};
        std::string s(load_buffer_.get() + img.binary_offset, n);
        if (!strip_wrapper(s)) return {};
        return s;
    }

    if (fs_file_) {
        fs::File* f = static_cast<fs::File*>(fs_file_);
        size_t blen = static_cast<size_t>(img.binary_length);
        if (blen == 0 || blen > kMaxElem) {
            printf("fbr: read_binary reject blen=%u off=%u free=%u\n",
                   (unsigned)blen, (unsigned)img.binary_offset,
                   (unsigned)free_h);
            return {};
        }
        if (blen + kGuard > free_h) {
            printf("fbr: read_binary OOM guard blen=%u free=%u\n",
                   (unsigned)blen, (unsigned)free_h);
            return {};
        }
        std::string s;
        s.resize(blen);
        f->seek(static_cast<int64_t>(img.binary_offset), SEEK_SET);
        size_t actually_read = f->read(&s[0], blen);
        if (actually_read == 0) {
            printf("fbr: read_binary read0 blen=%u\n", (unsigned)blen);
            return {};
        }
        s.resize(actually_read);
        if (!strip_wrapper(s)) return {};
        printf("fbr: read_binary ok blen=%u got=%u out=%u\n",
               (unsigned)blen, (unsigned)actually_read, (unsigned)s.size());
        return s;
    }

    if (!file_) return {};

    FILE* f = static_cast<FILE*>(file_);
    size_t blen = static_cast<size_t>(img.binary_length);
    if (blen == 0 || blen > kMaxElem) return {};
    if (blen + kGuard > free_h) return {};
    std::string s;
    s.resize(blen);
    FBR_FSEEK64(f, static_cast<long long>(img.binary_offset), SEEK_SET);
    size_t actually_read = std::fread(&s[0], 1, blen, f);
    if (actually_read == 0) return {};
    s.resize(actually_read);
    if (!strip_wrapper(s)) return {};
    return s;
}

// ─── Internal methods ─────────────────────────────────────────────

bool Fb2Document::read_file_into_buffer(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    FBR_FSEEK64(f, 0, SEEK_END);
    auto size = static_cast<size_t>(FBR_FTELL64(f));
    FBR_FSEEK64(f, 0, SEEK_SET);

    load_buffer_.reset(static_cast<char*>(std::malloc(size + 1)));
    if (!load_buffer_) {
        std::fclose(f);
        return false;
    }
    load_size_ = size;

    size_t read = std::fread(load_buffer_.get(), 1, size, f);
    load_buffer_.get()[read] = '\0';

    file_ = f;
    return read == size;
}

// ─── Build index from in-memory buffer (open_memory path) ────────

static FileOffset element_length(pugi::xml_node node) {
    auto next = node.next_sibling();
    while (next && next.type() != pugi::node_element)
        next = next.next_sibling();
    if (next)
        return static_cast<FileOffset>(next.offset_debug() - node.offset_debug());

    auto parent = node.parent();
    if (parent) {
        auto pnext = parent.next_sibling();
        while (pnext && pnext.type() != pugi::node_element)
            pnext = pnext.next_sibling();
        if (pnext)
            return static_cast<FileOffset>(pnext.offset_debug() - node.offset_debug());
    }

    return 0;
}

static void indexSection(
    pugi::xml_node section_node,
    DocumentIndex& index,
    int depth,
    TocEntry& toc_parent,
    size_t& block_counter,
    FileOffset section_end)
{
    auto title_node = section_node.child("title");
    if (title_node) {
        BlockSpan span;
        span.offset = title_node.offset_debug();
        span.type = BlockType::Title;
        auto len = element_length(title_node);
        if (len > 0)
            span.length = static_cast<FileLength>(len);
        else
            span.length = static_cast<FileLength>(section_end - title_node.offset_debug());
        index.blocks.push_back(span);
        block_counter++;
    }

    for (auto child : section_node.children()) {
        const char* name = child.name();
        fb2_index_yield();

        if (std::strcmp(name, "section") == 0 && child.type() == pugi::node_element) {
            TocEntry entry;
            entry.first_block = index.blocks.size();
            entry.depth = depth + 1;
            auto sub_title = child.child("title").child("p").text().as_string();
            entry.title = sub_title;

            auto next_sub = child.next_sibling("section");
            FileOffset sub_end = next_sub
                ? static_cast<FileOffset>(next_sub.offset_debug())
                : section_end;

            indexSection(child, index, depth + 1, entry, block_counter, sub_end);
            entry.last_block = index.blocks.empty() ? 0 : index.blocks.size() - 1;
            toc_parent.children.push_back(std::move(entry));
            continue;
        }

        if (child.type() != pugi::node_element)
            continue;

        BlockType bt = BlockType::Paragraph;
        if (std::strcmp(name, "p") == 0)          bt = BlockType::Paragraph;
        else if (std::strcmp(name, "poem") == 0)    bt = BlockType::Poem;
        else if (std::strcmp(name, "subtitle") == 0) bt = BlockType::Subtitle;
        else if (std::strcmp(name, "cite") == 0)    bt = BlockType::Cite;
        else if (std::strcmp(name, "epigraph") == 0) bt = BlockType::Epigraph;
        else if (std::strcmp(name, "empty-line") == 0) bt = BlockType::EmptyLine;
        else if (std::strcmp(name, "table") == 0)   bt = BlockType::Table;
        else if (std::strcmp(name, "image") == 0)   bt = BlockType::Image;
        else if (std::strcmp(name, "annotation") == 0) bt = BlockType::Annotation;
        else continue;

        auto elen = element_length(child);
        FileLength len;
        if (elen > 0)
            len = static_cast<FileLength>(elen);
        else
            len = static_cast<FileLength>(section_end - child.offset_debug());
        BlockSpan span(child.offset_debug(), len, bt);

        if (bt == BlockType::Image) {
            const char* href = child.attribute("l:href").value();
            if (!href || !*href)
                href = child.attribute("href").value();
            if (href && *href) {
                if (href[0] == '#') ++href;
                for (size_t ii = 0; ii < index.images.size(); ++ii) {
                    if (index.images[ii].id == href) {
                        span.image_index = static_cast<int16_t>(ii);
                        index.images[ii].ref_blocks.push_back(block_counter);
                        break;
                    }
                }
            }
        }

        index.blocks.push_back(span);
        block_counter++;
    }
}

bool Fb2Document::build_index(const char* xml_data, size_t xml_size) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer_inplace(
        const_cast<char*>(xml_data), xml_size,
        pugi::parse_default | pugi::parse_declaration);

    if (!result) {
        printf("fbr: build_index parse FAIL st=%d off=%u '%s' size=%u heap=%u\n",
               (int)result.status, (unsigned)result.offset,
               result.description() ? result.description() : "?",
               (unsigned)xml_size, heap_free_u32());
        set_errorf("Ошибка парсинга FB2 (offset %u)", (unsigned)result.offset);
        return false;
    }

    auto fb = doc.child("FictionBook");
    if (!fb) {
        pugi::xml_node first = doc.first_child();
        printf("fbr: build_index no FictionBook first='%s' decl=%d heap=%u\n",
               first.name() ? first.name() : "?",
               (int)first.type(), heap_free_u32());
        set_error("Файл повреждён или не является FB2");
        return false;
    }

    auto desc = fb.child("description");
    auto ti   = desc.child("title-info");
    auto di   = desc.child("document-info");
    auto pi   = desc.child("publish-info");

    // ─── Metadata ───────────────────────────────────────────

    if (ti) {
        meta_.title_info.book_title = ti.child("book-title").text().as_string();
        meta_.title_info.lang = ti.child("lang").text().as_string();
        meta_.title_info.src_lang = ti.child("src-lang").text().as_string();
        meta_.title_info.keywords = ti.child("keywords").text().as_string();
        meta_.title_info.date_text = ti.child("date").text().as_string();
        meta_.title_info.date_value = ti.child("date").attribute("value").as_string();

        for (auto g : ti.children("genre"))
            meta_.title_info.genres.push_back(g.text().as_string());

        for (auto a : ti.children("author")) {
            AuthorInfo ai;
            ai.first_name  = a.child("first-name").text().as_string();
            ai.middle_name = a.child("middle-name").text().as_string();
            ai.last_name   = a.child("last-name").text().as_string();
            ai.nickname    = a.child("nickname").text().as_string();
            meta_.title_info.authors.push_back(std::move(ai));
        }

        for (auto t : ti.children("translator")) {
            AuthorInfo ai;
            ai.first_name  = t.child("first-name").text().as_string();
            ai.middle_name = t.child("middle-name").text().as_string();
            ai.last_name   = t.child("last-name").text().as_string();
            ai.nickname    = t.child("nickname").text().as_string();
            meta_.title_info.translators.push_back(std::move(ai));
        }

        auto cover_img = ti.child("coverpage").child("image");
        if (cover_img) {
            const char* href = cover_img.attribute("l:href").value();
            if (!href || !*href)
                href = cover_img.attribute("href").value();
            if (href && href[0] == '#')
                meta_.title_info.cover_image_id = href + 1;
            else if (href && *href)
                meta_.title_info.cover_image_id = href;
        }
    }

    if (di) {
        meta_.document_info.id = di.child("id").text().as_string();
        meta_.document_info.version = di.child("version").text().as_float();
        meta_.document_info.program_used = di.child("program-used").text().as_string();
        meta_.document_info.date_text = di.child("date").text().as_string();
        meta_.document_info.date_value = di.child("date").attribute("value").as_string();
        meta_.document_info.src_ocr = di.child("src-ocr").text().as_string();
    }

    if (pi) {
        meta_.publish_info.book_name = pi.child("book-name").text().as_string();
        meta_.publish_info.publisher = pi.child("publisher").text().as_string();
        meta_.publish_info.city = pi.child("city").text().as_string();
        meta_.publish_info.isbn = pi.child("isbn").text().as_string();
    }

    // ─── Images ─────────────────────────────────────────────
    for (auto bin : fb.children("binary")) {
        ImageEntry ie;
        ie.id = bin.attribute("id").as_string();
        ie.content_type = bin.attribute("content-type").as_string();
        ie.binary_offset = bin.offset_debug();
        auto elen = element_length(bin);
        if (elen > 0) {
            ie.binary_length = static_cast<FileLength>(elen);
        } else {
            ie.binary_length = static_cast<FileLength>(load_size_ - ie.binary_offset);
        }
        index_.images.push_back(std::move(ie));
    }

    // ─── Body index ─────────────────────────────────────────
    size_t block_counter = 0;

    auto main_body = fb.child("body");
    auto notes_body = main_body ? main_body.next_sibling("body") : pugi::xml_node{};

    if (main_body) {
        std::vector<pugi::xml_node> sections;
        for (auto s : main_body.children("section"))
            sections.push_back(s);

        for (size_t i = 0; i < sections.size(); ++i) {
            fb2_index_yield();
            FileOffset end;
            if (i + 1 < sections.size())
                end = static_cast<FileOffset>(sections[i + 1].offset_debug());
            else
                end = notes_body
                    ? static_cast<FileOffset>(notes_body.offset_debug())
                    : static_cast<FileOffset>(xml_size);

            TocEntry entry;
            entry.first_block = index_.blocks.size();
            entry.depth = 0;
            auto title_el = sections[i].child("title");
            if (title_el)
                entry.title = title_el.child("p").text().as_string();

            indexSection(sections[i], index_, 0, entry, block_counter, end);
            entry.last_block = index_.blocks.empty() ? 0 : index_.blocks.size() - 1;
            index_.toc_root.children.push_back(std::move(entry));
        }
    }

    if (notes_body) {
        const char* name_attr = notes_body.attribute("name").as_string();
        if (std::strcmp(name_attr, "notes") == 0) {
            index_.notes_body_start = index_.blocks.size();
            for (auto note : notes_body.children("section")) {
                FootnoteEntry fe;
                fe.id = note.attribute("id").as_string();
                fe.first_block = index_.blocks.size();

                auto next_note = note.next_sibling("section");
                FileOffset section_end;
                if (next_note)
                    section_end = next_note.offset_debug();
                else
                    section_end = static_cast<FileOffset>(xml_size);

                for (auto child : note.children()) {
                    const char* n = child.name();
                    BlockType bt = BlockType::Paragraph;
                    if (std::strcmp(n, "p") == 0) bt = BlockType::Paragraph;
                    else if (std::strcmp(n, "title") == 0) bt = BlockType::Title;
                    else if (std::strcmp(n, "empty-line") == 0) bt = BlockType::EmptyLine;
                    else continue;

                    FileOffset flen = element_length(child);
                    if (flen <= 0) {
                        flen = section_end - child.offset_debug();
                    }
                    index_.blocks.emplace_back(child.offset_debug(), static_cast<FileLength>(flen), bt);
                }
                fe.last_block = index_.blocks.empty() ? 0 : index_.blocks.size() - 1;
                index_.footnotes.push_back(std::move(fe));
            }
        }
    }

    return true;
}

} // namespace fb2

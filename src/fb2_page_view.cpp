#include "fb2_page_view.h"
#include "hyphen_ru.h"
#include "image_decoder.h"
#include "palette.h"
#include "fb_helpers.h"
#include "platform.h"
#include "clock.h"
#include "layout_cache.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif


// ─── Layout constants ──────────────────────────────────────────
namespace {
    constexpr int kMarginX           = 22;
    constexpr int kMarginY           = 6;
    constexpr int kHeaderHeight      = 44;
    constexpr int kFooterHeight      = 34;
    // Отступ текста от краёв футера. Один и тот же слева (индикатор страниц)
    // и справа (индикатор заряда), иначе симметрия держится на совпадении
    // магических чисел.
    constexpr int kFooterMargin      = 8;
    constexpr float kLineHeightRatio = 1.45f;
    constexpr int kParaSpacing       = 4;
    constexpr float kParaIndentRatio = 1.6f;
    constexpr int kMaxEntityNameLen  = 12;
    constexpr int kMaxUnicodeCp      = 0x10FFFF;
    constexpr int kMaxLinkSearchWin  = 200;
    constexpr int kEpigraphIndentF   = 2;  // font_size_ * kEpigraphIndentF
    constexpr int kEpigraphWidthF    = 4;  // shrink: font_size_ * kEpigraphWidthF

    // ─── Переносы и выключка ────────────────────────────────────
    // Максимальная растяжка пробела при выключке: +50% от естественной
    // ширины. Всё, что не поместилось в этот предел, дотягивается переносом
    // слова; остаток намеренно остаётся справа, чтобы строка не превратилась
    // в «решётку».
    constexpr int kJustifySpacePermille = 500;  // 0.5 * space_width
    // Строка из одного однобуквенного слова («в», «с») недопустима — слово
    // уводится на следующую строку, даже если та станет шире колонки.
    constexpr int kMinHyphenPart = 2;  // символов с каждой стороны разрыва

    // ─── Colors (palette indices) ─────────────────────────────
    constexpr uint8_t kTextColor     = kDark2;     // 0xFF222222 → index 2
    constexpr uint8_t kEpigraphColor = kMid;       // 0xFF555555 → index 5
    constexpr uint8_t kLinkColor     = kMidDark;   // 0xFF2222AA → index 4
    constexpr uint8_t kDashColor     = kBorder;    // 0xFFD0C8B8 → index ~12
    constexpr uint8_t kPanelBg       = kCream;     // 0xFFF5EFE6 → index 14
    constexpr uint8_t kBorderColor   = kBorder;    // 0xFFCCCCCC → index 12
    constexpr uint8_t kTocIconColor  = kBrown;     // 0xFF8B7355 → index 7
    constexpr uint8_t kPageInfoColor = kGray;      // 0xFF888888 → index 8
    constexpr uint8_t kTocTextColor  = kMidDark;   // 0xFF444444 → index 4
    constexpr uint8_t kPageNumColor  = kLight1;    // 0xFFAAAAAA → index 10

    // ─── TOC panel constants ────────────────────────────────────
    constexpr int kTocPanelPercent  = 40;  // percent of width
    constexpr int kTocPanelMinW     = 120;
    constexpr int kTocItemExtraH    = 12;
    constexpr int kTocStartY        = 24;
    constexpr int kTocBottomPad     = 6;
    constexpr int kTocDepthIndent   = 18;
    constexpr int kScrollBtnH       = 40;
    constexpr int kTocTitleRightPad = 20;

    // ─── Title rendering ────────────────────────────────────────
    constexpr float kTitleScale0 = 1.35f;
    constexpr float kTitleScale1 = 1.20f;
    constexpr float kTitleScaleN = 1.10f;
    constexpr float kSubtitleScale = 1.10f;
    constexpr int   kTextBaseOff   = 4;  // baseline Y adjustment

#ifdef FBR_WIN32
    // Win32-only: power-off button in the reader header (top-right).
constexpr int kPowerBtnW      = 56;
constexpr int kPowerBtnInsetY = 2;  // vertical inset inside the header
constexpr int kPowerBtnRight  = 6;  // distance from the right edge
#endif

    // Отступ часов от правого края: нужен обеим платформам, поэтому вынесен
    // из блока FBR_WIN32 — часы рисуются и на устройстве.
constexpr int kHeaderRightMargin = 8;
}

Fb2PageView::Fb2PageView(FontRenderer* font, FontRenderer* ui_font, const fb2::Fb2Document* doc)
    : font_(font), ui_font_(ui_font), doc_(doc) {}

void Fb2PageView::set_size(int w, int h) {
    width_ = w;
    height_ = h;
    margin_x_ = kMarginX;
    margin_y_ = kMarginY;
    header_h_ = kHeaderHeight;
    footer_h_ = kFooterHeight;
    content_x_ = margin_x_;
    content_w_ = width_ - margin_x_ * 2;
    content_y_ = margin_y_ + header_h_;
    content_h_ = height_ - margin_y_ * 2 - header_h_ - footer_h_;
    toc_inited_ = false;
}

void Fb2PageView::set_font_size(int pts) {
    font_size_ = pts;
    line_height_ = (int)(pts * line_spacing_ratio_);
    para_spacing_ = (int)(pts * (para_spacing_ratio_ - 1.0f));
    para_indent_ = para_indent_px_;
    space_w_ = 0;   // метрики шрифта изменились — переизмерить
}

void Fb2PageView::set_layout_params(float line_spacing, float para_spacing, int para_indent) {
    line_spacing_ratio_ = line_spacing;
    para_spacing_ratio_ = para_spacing;
    para_indent_px_ = para_indent;
    line_height_ = (int)(font_size_ * line_spacing_ratio_);
    para_spacing_ = (int)(font_size_ * (para_spacing_ratio_ - 1.0f));
    para_indent_ = para_indent_px_;
}

void Fb2PageView::set_night_mode(bool on) {
    night_mode_ = on;
}

int Fb2PageView::line_advance(int block_type, int depth) const {
    int h = line_height_;
    if (block_type == (int)fb2::BlockType::Title) {
        h = (int)(h * title_scale(depth));
    } else if (block_type == (int)fb2::BlockType::Subtitle) {
        h = (int)(h * kSubtitleScale);
    }
    return h;
}

int Fb2PageView::text_width(const std::string& s) const {
    return font_->text_width(s);
}

int Fb2PageView::ui_text_width(const std::string& s) const {
    if (!ui_font_) return 0;
    return ui_font_->text_width(s);
}

// Ширина пробела и знака переноса считаются один раз на раскладку —
// раньше text_width(" ") вызывался на каждое слово, зря грузя FreeType.
void Fb2PageView::measure_justify() {
    if (space_w_ > 0) return;
    space_w_ = text_width(" ");
    if (space_w_ <= 0) space_w_ = std::max(1, font_size_ / 4);

    // U+2010 (короткое тире) есть не во всех шрифтах; если глифа нет,
    // text_width вернёт 0 и расчёт ширины разъедется — откатываемся на дефис.
    int h = text_width(hyph::kHyphenUtf8);
    have_hyphen_glyph_ = (h > 0);
    hyphen_w_ = have_hyphen_glyph_ ? h : text_width(hyph::kHyphenFallbackUtf8);
    if (hyphen_w_ <= 0) hyphen_w_ = std::max(1, space_w_);
}

// ─── Decode XML/HTML entities ───────────────────────────────────
static std::string decode_entities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            auto semi = s.find(';', i);
            if (semi == std::string::npos || semi - i > kMaxEntityNameLen) {
                out += s[i]; continue;
            }
            std::string_view ent(s.data() + i + 1, semi - i - 1);
            if (ent == "amp")       out += '&';
            else if (ent == "lt")   out += '<';
            else if (ent == "gt")   out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (ent.size() > 1 && ent[0] == '#') {
                // Numeric entity &#NNNN; or &#xHHHH;
                unsigned long cp = 0;
                if (ent.size() > 1 && ent[1] == 'x')
                    cp = std::strtoul(ent.data() + 2, nullptr, 16);
                else
                    cp = std::strtoul(ent.data() + 1, nullptr, 10);
                if (cp > 0 && cp <= kMaxUnicodeCp) {
                    // Encode as UTF-8
                    if (cp < 0x80)       out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6));
                                            out += (char)(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12));
                                             out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                             out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xF0 | (cp >> 18));
                           out += (char)(0x80 | ((cp >> 12) & 0x3F));
                           out += (char)(0x80 | ((cp >> 6) & 0x3F));
                           out += (char)(0x80 | (cp & 0x3F)); }
                }
            } else {
                out += s[i]; continue;
            }
            i = semi;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ─── Block → lines ─────────────────────────────────────────────
void Fb2PageView::layout_blocks(ProgressCb cb) {
    all_lines_.clear();

    const auto& idx = doc_->index();
    const auto& blocks = idx.blocks;
    size_t total_blocks = blocks.size();

#ifdef ESP_PLATFORM
    printf("fbr: layout_blocks start blocks=%u heap=%u largest=%u\n",
           (unsigned)total_blocks,
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif

    // Build block→depth mapping from TOC tree
    block_depth_.assign(blocks.size(), 0);
    std::function<void(const fb2::TocEntry&, int)> set_depth;
    set_depth = [&](const fb2::TocEntry& e, int d) {
        for (const auto& c : e.children) {
            for (size_t b = c.first_block; b <= c.last_block && b < blocks.size(); ++b)
                block_depth_[b] = d;
            set_depth(c, d + 1);
        }
    };
    set_depth(idx.toc_root, 0);

    // Build block→first line index
    block_first_line_.assign(blocks.size(), -1);

    // ─── Cover page (from <description>/<title-info>/<coverpage>) ──
    {
        const auto& cover_id = doc_->meta().title_info.cover_image_id;
        if (!cover_id.empty()) {
            for (size_t ii = 0; ii < idx.images.size(); ++ii) {
                if (idx.images[ii].id == cover_id) {
                    all_lines_.push_back({"", false, (int)fb2::BlockType::Image,
                                          false, -1, 0, (int)ii, 0, {}});
                    break;
                }
            }
        }
    }

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        if (cb && total_blocks > 0)
            cb((int)(bi * 100 / total_blocks));
        std::string xml = doc_->read_block(bi);
        fb2::BlockType type = blocks[bi].type;

        // Prepend missing '<' if the block starts with tag name (PugiXML
        // offset_debug may point to tag name, not the opening '<')
        if (!xml.empty() && xml[0] != '<')
            xml.insert(0, 1, '<');

        // Truncate padding: find opening tag name, find </tag> at end
        {
            auto lt = xml.find('<');
            if (lt != std::string::npos) {
                auto space = xml.find_first_of(" >", lt + 1);
                if (space != std::string::npos && space > lt + 1) {
                    std::string tname = xml.substr(lt + 1, space - lt - 1);
                    std::string close = "</" + tname + ">";
                    auto cp = xml.rfind(close);
                    if (cp != std::string::npos)
                        xml.resize(cp + close.size());
                }
            }
        }

        // ─── Scan for footnote links in raw XML ──────────────────
        // Links: <a l:href="#id">text</a>
        struct RawLink { int raw_start; int raw_end; std::string id; };
        std::vector<RawLink> raw_links;
        {
            size_t ap = 0;
            while ((ap = xml.find("<a", ap)) != std::string::npos) {
                auto gt_a = xml.find(">", ap);
                if (gt_a == std::string::npos) break;

                // Find href attribute (try l:href first, then href)
                auto href_pos = xml.find("l:href=", ap);
                if (href_pos == std::string::npos || href_pos > gt_a)
                    href_pos = xml.find("href=", ap);
                if (href_pos == std::string::npos || href_pos > gt_a) {
                    ap = gt_a + 1; continue;
                }
                auto q1 = xml.find('"', href_pos);
                if (q1 == std::string::npos || q1 > gt_a) { ap = gt_a + 1; continue; }
                auto q2 = xml.find('"', q1 + 1);
                if (q2 == std::string::npos) { ap = gt_a + 1; continue; }
                std::string href = xml.substr(q1 + 1, q2 - q1 - 1);
                if (href.empty() || href[0] != '#') { ap = gt_a + 1; continue; }
                std::string note_id = href.substr(1);

                auto close_a = xml.find("</a>", gt_a);
                if (close_a == std::string::npos) { ap = gt_a + 1; continue; }

                raw_links.push_back({(int)(gt_a + 1), (int)close_a, note_id});
                ap = close_a + 1;
            }
        }

        // Strip XML tags with position tracking
        std::string text;
        std::vector<int> raw_to_stripped(xml.size(), -1);
        bool in_tag = false;
        int stripped_pos = 0;
        for (int raw_pos = 0; raw_pos < (int)xml.size(); ++raw_pos) {
            char c = xml[raw_pos];
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) {
                text += c;
                raw_to_stripped[raw_pos] = stripped_pos;
                stripped_pos++;
            }
        }

        // Decode entities
        text = decode_entities(text);

        // Map raw links to stripped text positions
        std::vector<std::pair<int, std::string>> pending_links; // {clean_pos, id}
        for (const auto& rl : raw_links) {
            int ss = -1, se = -1;
            for (int r = rl.raw_start; r < rl.raw_end; ++r) {
                if (raw_to_stripped[r] >= 0) {
                    if (ss < 0) ss = raw_to_stripped[r];
                    se = raw_to_stripped[r];
                }
            }
            if (ss >= 0 && se >= ss) {
                pending_links.push_back({ss, rl.id});
                // We'll use ss as the "search" key and store end separately
            }
        }

        // Empty or Image block
        if (type == fb2::BlockType::EmptyLine) {
            if (block_first_line_[bi] < 0)
                block_first_line_[bi] = (int)all_lines_.size();
            all_lines_.push_back({"", false, (int)type, false, (int)bi, block_depth_[bi], -1, 0, {}});
            continue;
        }

        if (type == fb2::BlockType::Image) {
            if (block_first_line_[bi] < 0)
                block_first_line_[bi] = (int)all_lines_.size();
            int img_idx = blocks[bi].image_index;
            all_lines_.push_back({"", false, (int)type, false, (int)bi, 0, img_idx, 0, {}});
            continue;
        }

        // Scan for <image> references BEFORE empty-text skip
        // (images are often nested inside <p>, <epigraph>, <cite>, etc.
        //  and may be the only content — we must not skip them)
        if (type != fb2::BlockType::Image) {
            size_t scan_pos = 0;
            while ((scan_pos = xml.find("<image", scan_pos)) != std::string::npos) {
                auto close = xml.find("/>", scan_pos);
                if (close == std::string::npos) close = xml.find(">", scan_pos);
                if (close == std::string::npos) break;
                std::string_view tag(xml.data() + scan_pos + 6, close - scan_pos - 6);

                auto extract_href = [&](const char* attr) -> std::string {
                    std::string search = attr;
                    search += "=\"";
                    auto hp = tag.find(search);
                    if (hp == std::string::npos) return {};
                    hp += search.size();
                    auto he = tag.find('"', hp);
                    if (he == std::string::npos) return {};
                    return std::string(tag.data() + hp, he - hp);
                };

                std::string id = extract_href("l:href");
                if (id.empty()) id = extract_href("href");
                if (!id.empty() && id[0] == '#') id.erase(0, 1);

                if (!id.empty()) {
                    int found = -1;
                    for (size_t ii = 0; ii < idx.images.size(); ++ii) {
                        if (idx.images[ii].id == id) { found = (int)ii; break; }
                    }
                    if (found >= 0) {
                        if (block_first_line_[bi] < 0)
                            block_first_line_[bi] = (int)all_lines_.size();
                        all_lines_.push_back({"", false, (int)fb2::BlockType::Image, false, (int)bi, 0, found, 0, {}});
                    }
                }
                scan_pos = close + 2;
            }
        }

        // Collapse whitespace with position tracking
        std::string clean;
        std::vector<int> stripped_to_clean(text.size(), -1);
        bool last_space = true;
        for (int si = 0; si < (int)text.size(); ++si) {
            char c = text[si];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!last_space) { clean += ' '; stripped_to_clean[si] = (int)clean.size() - 1; last_space = true; }
            } else {
                clean += c; stripped_to_clean[si] = (int)clean.size() - 1; last_space = false;
            }
        }
        // Trim leading space
        if (!clean.empty() && clean[0] == ' ') {
            clean.erase(clean.begin());
            for (int& ci : stripped_to_clean) if (ci >= 0) ci--;
        }
        text = clean;

        // Map pending_links to clean text positions
        std::vector<std::pair<int, int>> link_clean_pos; // {clean_start, clean_end} per link
        for (const auto& [ss, id] : pending_links) {
            int se = ss + 1; // will be set properly
            int cs = -1, ce = -1;
            for (int si = ss; si < (int)stripped_to_clean.size() && si < ss + 200; ++si) {
                if (stripped_to_clean[si] >= 0) {
                    if (cs < 0) cs = stripped_to_clean[si];
                    ce = stripped_to_clean[si];
                }
            }
            if (cs >= 0) link_clean_pos.push_back({cs, ce + 1});
            else link_clean_pos.push_back({-1, -1});
        }

        if (text.empty()) continue;

        measure_justify();

        // Split into words, remembering each word's offset in the clean text
        // so footnote links can still be attached after hyphenation moved a
        // part of a word to the next line.
        struct SrcWord {
            std::string s;
            int         src;   // offset in `text`
        };
        std::vector<SrcWord> words;
        {
            const char* p = text.c_str();
            while (*p) {
                while (*p == ' ') ++p;
                if (!*p) break;
                const char* ws = p;
                while (*p && *p != ' ') ++p;
                words.push_back({std::string(ws, p - ws), (int)(ws - text.c_str())});
            }
        }

        if (words.empty()) continue;

        // Record block start
        if (block_first_line_[bi] < 0)
            block_first_line_[bi] = (int)all_lines_.size();

        int max_w = content_w_;
        bool centered = (type == fb2::BlockType::Title ||
                         type == fb2::BlockType::Subtitle);
        int para_extra = (type == fb2::BlockType::Paragraph ||
                          type == fb2::BlockType::Cite ||
                          type == fb2::BlockType::Epigraph) ? 1 : 0;

        // Эпиграф и стихи рисуются с отступом (kEpigraphIndentF) в суженной
        // колонке (kEpigraphWidthF) — раскладка обязана учитывать и то, и
        // другое, иначе строки выходят за content_w_ и обрезаются.
        const bool narrow = (type == fb2::BlockType::Epigraph ||
                             type == fb2::BlockType::Verse);
        if (narrow)
            max_w = content_w_ - font_size_ * (kEpigraphIndentF + kEpigraphWidthF);
        if (max_w < font_size_) max_w = font_size_;

        // Выключка применяется к основному тексту; заголовки центрируются,
        // стихи и эпиграфы не выключаются (стихи не выключают), но переносятся.
        const bool justify_ok = (type == fb2::BlockType::Paragraph ||
                                 type == fb2::BlockType::Cite);
        const bool hyphen_ok = !centered;

        bool first_line = true;
        std::string cur_line;
        int cur_w = 0;
        int cur_words = 0;        // слов в накапливаемой строке
        int cur_cps = 0;          // символов в накапливаемой строке
        int line_src_begin = 0;   // смещение в тексте, с которого началась строка
        int line_src_end = 0;     // смещение в тексте сразу за последним символом строки

        const int space_cap = std::max(1, (space_w_ * kJustifySpacePermille) / 1000);
        const char* kHyphen = hyph::hyphen_char(have_hyphen_glyph_);

        // Appends a piece of source text to the current line, updating width
        // and the source range used for footnote attachment.
        auto append_piece = [&](const std::string& piece, int src, int src_bytes, int w) {
            const bool was_empty = cur_line.empty();
            if (was_empty)
                line_src_begin = src;
            else
                cur_line += ' ';
            cur_line += piece;
            cur_w += (was_empty ? 0 : space_w_) + w;
            line_src_end = src + src_bytes;
            ++cur_words;
            cur_cps += hyph::decode_codepoints(piece, nullptr, nullptr);
        };

        auto close_line = [&](bool justify) {
            if (cur_line.empty()) return;
            int dep = (type == fb2::BlockType::Title) ? block_depth_[bi] : 0;

            // Attach footnotes fully contained in this line's source range.
            std::vector<FootnoteLink> line_fns;
            for (size_t fi = 0; fi < link_clean_pos.size(); ++fi) {
                auto [ls, le] = link_clean_pos[fi];
                if (ls < 0) continue;
                if (ls >= line_src_begin && le <= line_src_end) {
                    line_fns.push_back({ls - line_src_begin, le - line_src_begin,
                                        pending_links[fi].second});
                    link_clean_pos[fi] = {-1, -1}; // consumed
                }
            }

            // Выключка: остаток делится поровну между пробелами, но не
            // больше +50% ширины пробела — иначе строка рассыпается.
            int16_t extra = 0;
            if (justify && cur_words > 1) {
                int gaps = cur_words - 1;
                int slack = max_w - cur_w;
                int add = slack / gaps;
                extra = (int16_t)std::min(add, space_cap);
            }

            all_lines_.push_back({cur_line,
                                  (first_line && type == fb2::BlockType::Paragraph),
                                  (int)type, false, (int)bi, dep, -1, extra, line_fns});
            cur_line.clear();
            cur_w = 0;
            cur_words = 0;
            cur_cps = 0;
            first_line = false;
        };

        // ── Основной цикл: жадное заполнение + перенос + выключка ──────
        //
        // Слово, не поместившееся целиком, сначала режется переносом (хвост
        // уходит в начало следующей строки). Если резать нельзя — строка
        // закрывается и раздвигается пробелами.
        size_t wi = 0;
        std::string pending;     // хвост перенесённого слова
        int pending_src = 0;
        bool has_pending = false;

        std::vector<uint32_t> cps_buf;
        std::vector<int> offs_buf;
        std::vector<int> pref_buf;

        while (wi < words.size() || has_pending) {
            std::string w;
            int src = 0;
            if (has_pending) {
                w = pending;
                src = pending_src;
                has_pending = false;
            } else {
                w = words[wi].s;
                src = words[wi].src;
                ++wi;
            }

            bool placed = false;

            while (!placed) {
                if (cur_line.empty()) {
                    cur_w = (first_line && type == fb2::BlockType::Paragraph)
                            ? para_indent_ : 0;
                }
                const int sw = cur_line.empty() ? 0 : space_w_;
                const int ww = text_width(w);

                // Строка из одного однобуквенного слова недопустима: лучше
                // выпустить её за колонку, чем оставить «в» сиротой.
                const bool lonely = (cur_words == 1 && cur_cps == 1);

                if (cur_w + sw + ww <= max_w || (lonely && hyphen_ok)) {
                    append_piece(w, src, (int)w.size(), ww);
                    placed = true;
                    break;
                }

                // ── Пробуем перенести слово ──
                if (hyphen_ok) {
                    const int avail = max_w - cur_w - sw;
                    if (avail > hyphen_w_) {
                        int n = hyph::decode_codepoints(w, nullptr, nullptr);
                        if (n >= 2 * kMinHyphenPart) {
                            cps_buf.resize(n);
                            offs_buf.resize(n + 1);
                            pref_buf.resize(n + 1);
                            hyph::decode_codepoints(w, cps_buf.data(), offs_buf.data());
                            // text_width суммирует advance'ы без кернинга, поэтому
                            // ширина префикса = сумма ширин его символов. Считаем
                            // по одному символу: O(n) вместо O(n²).
                            pref_buf[0] = 0;
                            for (int i = 1; i <= n; ++i) {
                                pref_buf[i] = pref_buf[i - 1] +
                                    font_->text_width(std::string_view(
                                        w.data() + offs_buf[i - 1],
                                        (size_t)(offs_buf[i] - offs_buf[i - 1])));
                            }

                            int k = hyph::find_break(cps_buf.data(), n, pref_buf.data(),
                                                     n - kMinHyphenPart,
                                                     avail - hyphen_w_, hyphen_w_);
                            if (k < 0 && cur_line.empty()) {
                                // Слово шире всей строки: режем по правой
                                // допустимой точке, даже если она шире.
                                k = hyph::find_break(cps_buf.data(), n, pref_buf.data(),
                                                     n - kMinHyphenPart,
                                                     0x7FFFFFF, hyphen_w_);
                            }
                            if (k >= 0) {
                                const std::string head = w.substr(0, offs_buf[k]);
                                append_piece(head, src, offs_buf[k], pref_buf[k]);
                                cur_line += kHyphen;
                                cur_w += hyphen_w_;
                                pending = w.substr(offs_buf[k]);
                                pending_src = src + offs_buf[k];
                                has_pending = !pending.empty();
                                close_line(justify_ok);
                                break;
                            }
                        }
                    }
                }

                if (cur_line.empty()) {
                    // Ни разорвать, ни раздвинуть — слово шире колонки и не
                    // имеет допустимых точек. Ставим как есть (обрежется краем).
                    append_piece(w, src, (int)w.size(), ww);
                    placed = true;
                    break;
                }

            // Строку заполнить не вышло — закрываем и раскладываем слово
            // заново на следующей строке. Закрываемая строка заведомо не
            // последняя (слово w уйдёт дальше), поэтому выключиваем её.
            // Последнюю строку абзаца закрывает финальный close_line(false).
            close_line(justify_ok);

            }
        }
        close_line(false);

        if (para_extra)
            all_lines_.push_back({"", false, (int)type, true, (int)bi, 0, -1, 0, {}});

        if (type == fb2::BlockType::Title ||
            type == fb2::BlockType::Subtitle) {
            all_lines_.push_back({"", false, (int)type, true, (int)bi, block_depth_[bi], -1, 0, {}});
            all_lines_.push_back({"", false, (int)type, false, (int)bi, block_depth_[bi], -1, 0, {}}); // blank visible line
        }
    }

    // Reserve the flat TOC array
    toc_entry_page_.clear();
    std::function<void(const fb2::TocEntry&)> count_toc;
    count_toc = [&](const fb2::TocEntry& e) {
        for (const auto& c : e.children) {
            toc_entry_page_.push_back(-1);
            count_toc(c);
        }
    };
    count_toc(doc_->index().toc_root);
    toc_inited_ = true;
}

// ─── Lines → pages ─────────────────────────────────────────────
void Fb2PageView::layout(ProgressCb cb) {
    // Метрики пробела и знака переноса нужны и вёрстке, и рендеру выключенных
    // строк — считаем один раз на весь проход.
    measure_justify();
    layout_blocks(cb);
    if (cb) cb(100);
#ifdef ESP_PLATFORM
    printf("fbr: after100 build_pages lines=%u heap=%u\n",
           (unsigned)all_lines_.size(), (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif
    pages_.clear();

    // Reserve space for glyph descenders past the baseline so the visual bottom
    // of the last line never reaches the footer band, at any font size.
    int desc_budget = std::max(2, std::min(8, line_height_ / 5));
    int bound_y = content_y_ + content_h_ - desc_budget;

    // Block→page mapping
    std::vector<int> block_to_page(doc_->index().blocks.size(), 0);

    Page pg;
    pg.first_line_idx = 0;
    pg.last_line_idx = -1;
    pg.first_block = -1;
    int y = content_y_;

    auto page_line = [&](int idx) -> const Line& { return all_lines_[idx]; };

    for (int i = 0; i < (int)all_lines_.size(); ++i) {
        const Line& ln = all_lines_[i];

        // Extra top for first title/subtitle line on a page
        int extra_top = 0;
        if (ln.block_type == (int)fb2::BlockType::Title &&
            (pg.empty() || page_line(pg.last_line_idx).block_type != (int)fb2::BlockType::Title ||
             page_line(pg.last_line_idx).block_index != ln.block_index)) {
            extra_top = (int)(line_height_ * title_scale(ln.depth));
        }
        if (ln.block_type == (int)fb2::BlockType::Epigraph && pg.empty())
            extra_top = line_height_;

        // Line height
        int para_h = 0;
        if (ln.para_break) {
            para_h = para_spacing_;
        } else if (ln.text.empty() && !ln.para_break) {
            para_h = line_height_;
        } else {
            para_h = line_advance(ln.block_type, ln.depth);
        }

        int total_h = extra_top + para_h;

        // Page break if line doesn't fit (at least 1 line per page)
        if (y + total_h > bound_y && !pg.empty()) {
            // Orphan prevention: if current page has few non-title lines
            // and next line is a Title, merge them instead of breaking
            bool merged = false;
            if (pg.line_count() <= kOrphanMinLines &&
                ln.block_type == (int)fb2::BlockType::Title &&
                page_line(pg.first_line_idx).block_type != (int)fb2::BlockType::Title) {
                // Only merge if the combined content fits on one page
                int merged_end = content_y_;
                for (int li = pg.first_line_idx; li <= pg.last_line_idx; ++li) {
                    const auto& l = page_line(li);
                    merged_end += l.para_break ? para_spacing_ :
                                  (l.text.empty() ? line_height_ : line_advance(l.block_type, l.depth));
                }
                merged_end += total_h;
                if (merged_end <= bound_y) {
                    y = content_y_;
                    for (int li = pg.first_line_idx; li <= pg.last_line_idx; ++li) {
                        const auto& l = page_line(li);
                        y += l.para_break ? para_spacing_ :
                             (l.text.empty() ? line_height_ : line_advance(l.block_type, l.depth));
                    }
                    merged = true;
                }
            }
            if (!merged) {
                pg.last_line_idx = i - 1;
                pages_.push_back(pg);
                // Map blocks to page
                for (int b = pg.first_block; b <= pg.last_block && b < (int)block_to_page.size(); ++b)
                    if (block_first_line_[b] >= pg.first_line_idx && block_first_line_[b] <= pg.last_line_idx)
                        block_to_page[b] = (int)pages_.size() - 1;

                pg = Page();
                pg.first_line_idx = i;
                pg.last_line_idx = i - 1;
                pg.first_block = -1;
                y = content_y_;
            }
        }

        // Image block forces its own page
        if (ln.image_index >= 0) {
            if (!pg.empty()) {
                pg.last_line_idx = i - 1;
                pages_.push_back(pg);
                for (int b = pg.first_block; b <= pg.last_block && b < (int)block_to_page.size(); ++b)
                    if (block_first_line_[b] >= pg.first_line_idx && block_first_line_[b] <= pg.last_line_idx)
                        block_to_page[b] = (int)pages_.size() - 1;
            }
            pg = Page();
            pg.first_line_idx = i;
            pg.last_line_idx = i;
            pg.first_block = ln.block_index;
            pg.last_block = ln.block_index;
            pg.is_image_page = true;
            pages_.push_back(pg);
            pg = Page();
            pg.first_line_idx = i + 1;
            pg.last_line_idx = i;
            y = content_y_;
            continue;
        }

        pg.last_line_idx = i;
        if (pg.first_block < 0 || ln.block_index < pg.first_block)
            pg.first_block = ln.block_index;
        if (pg.last_block < 0 || ln.block_index > pg.last_block)
            pg.last_block = ln.block_index;
        y += total_h;
    }

    if (!pg.empty() || pages_.empty()) {
        pg.last_line_idx = (int)all_lines_.size() - 1;
        pages_.push_back(pg);
        for (int b = pg.first_block; b <= pg.last_block && b < (int)block_to_page.size(); ++b)
            if (block_first_line_[b] >= pg.first_line_idx && block_first_line_[b] <= pg.last_line_idx)
                block_to_page[b] = (int)pages_.size() - 1;
    }

    // Map TOC entries to page numbers via block_to_page
    int ti = 0;
    std::function<void(const fb2::TocEntry&)> map_toc;
    map_toc = [&](const fb2::TocEntry& e) {
        for (const auto& c : e.children) {
            if (ti < (int)toc_entry_page_.size()) {
                int page = 0;
                size_t fb = c.first_block;
                if (fb < block_to_page.size())
                    page = block_to_page[fb];
                if (page >= (int)pages_.size())
                    page = (int)pages_.size() - 1;
                toc_entry_page_[ti] = page;
                ++ti;
            }
            map_toc(c);
        }
    };
    ti = 0;
    map_toc(doc_->index().toc_root);

    if (cur_page_ >= (int)pages_.size())
        cur_page_ = 0;

#ifdef ESP_PLATFORM
    printf("fbr: layout done pages=%u lines=%u heap=%u\n",
           (unsigned)pages_.size(), (unsigned)all_lines_.size(),
           (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif
}

// ─── Hit test ──────────────────────────────────────────────────
int Fb2PageView::hit_test(int x, int y) const {
#ifdef FBR_WIN32
    if (y >= margin_y_ && y < margin_y_ + header_h_) {
        int btn_x = width_ - kPowerBtnW - kPowerBtnRight;
        int btn_y = margin_y_ + kPowerBtnInsetY;
        int btn_h = header_h_ - kPowerBtnInsetY * 2;
        if (x >= btn_x && x < btn_x + kPowerBtnW &&
            y >= btn_y && y < btn_y + btn_h)
            return 3; // power-off button (win32)
        return 2; // toggle TOC
    }
#else
    if (y >= margin_y_ && y < margin_y_ + header_h_)
        return 2; // toggle TOC
#endif

    int third = content_w_ / 3;
    int cx = x - content_x_;
    if (cx >= 0 && cx < third)
        return -1; // prev
    if (cx >= third * 2 && cx < content_w_)
        return 1; // next
    return 0;
}

bool Fb2PageView::is_footnote_at(int x, int y) const {
    for (const auto& r : foot_hit_rects_) {
        if (x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2)
            return true;
    }
    return false;
}

bool Fb2PageView::has_footnote_click(int x, int y) const {
    for (const auto& r : foot_hit_rects_) {
        if (x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2) {
            const_cast<Fb2PageView*>(this)->follow_footnote(r.id);
            return true;
        }
    }
    return false;
}

int Fb2PageView::toc_hit_test(int x, int y) const {
    if (!toc_visible_) return -1;
    int pw = width_ * kTocPanelPercent / 100;
    if (pw < kTocPanelMinW) pw = kTocPanelMinW;
    if (x < 0 || x >= pw) return -1;

    int item_h = (ui_font_ ? ui_font_->line_height() : line_height_) + kTocItemExtraH;
    if (item_h < 50) item_h = 50;
    int sy = content_y_ + kTocStartY;
    int avail_h = height_ - sy - kTocBottomPad;

    std::vector<std::pair<std::string, int>> flat;
    std::function<void(const fb2::TocEntry&, int)> flatten;
    flatten = [&](const fb2::TocEntry& e, int d) {
        for (const auto& c : e.children) {
            flat.push_back({c.title, d});
            flatten(c, d + 1);
        }
    };
    flatten(doc_->index().toc_root, 0);
    int max_visible = flat.empty() ? 1 : (avail_h / item_h);
    int max_scroll = std::max(0, (int)flat.size() - max_visible);

    int cur_sy = sy;

    if (toc_scroll_ > 0) {
        if (y >= cur_sy && y < cur_sy + kScrollBtnH) return -2;
        cur_sy += kScrollBtnH;
    }

    int start_idx = toc_scroll_;
    for (int idx = start_idx; idx < (int)flat.size(); ++idx) {
        int ty = cur_sy + (idx - start_idx) * item_h;
        if (ty + item_h >= height_) break;
        if (y >= ty && y < ty + item_h) {
            int page = (idx < (int)toc_entry_page_.size()) ? toc_entry_page_[idx] : 0;
            return page;
        }
    }

    if (toc_scroll_ < max_scroll) {
        int by = height_ - kScrollBtnH;
        if (y >= by && y < height_) return -3;
    }

    return -1;
}

// ─── Navigation ────────────────────────────────────────────────
bool Fb2PageView::next_page() {
    if (cur_page_ + 1 < (int)pages_.size()) {
        ++cur_page_;
        // Stay in footnote mode until user exits via prev_page()
        return true;
    }
    return false;
}

bool Fb2PageView::prev_page() {
    if (fn_nav_.active) {
        exit_footnote();
        return true;
    }
    if (cur_page_ > 0) {
        --cur_page_;
        return true;
    }
    return false;
}

bool Fb2PageView::go_to_page(int page) {
    if (page >= 0 && page < (int)pages_.size()) {
        cur_page_ = page;
        // Exit footnote mode when going to pages outside the footnote block range
        if (fn_nav_.active) {
            if (pages_[page].first_block > fn_nav_.last_block ||
                pages_[page].last_block  < fn_nav_.first_block)
                fn_nav_.active = false;
        }
        return true;
    }
    return false;
}

int Fb2PageView::current_page_block() const {
    if (cur_page_ < 0 || cur_page_ >= (int)pages_.size()) return -1;
    return pages_[cur_page_].first_block;
}

int Fb2PageView::page_for_block(int block_idx) const {
    for (int i = 0; i < (int)pages_.size(); ++i) {
        const Page& pg = pages_[i];
        if (pg.first_block >= 0 && block_idx >= pg.first_block && block_idx <= pg.last_block)
            return i;
    }
    return 0;
}

// ─── Footnotes ──────────────────────────────────────────────────

int Fb2PageView::first_footnote_page() const {
    if (!fn_nav_.active) return -1;
    for (int p = 0; p < (int)pages_.size(); ++p) {
        // Overlap check: page block range intersects footnote block range
        if (pages_[p].first_block <= fn_nav_.last_block &&
            pages_[p].last_block  >= fn_nav_.first_block)
            return p;
    }
    return -1;
}

bool Fb2PageView::is_first_footnote_page() const {
    int ffp = first_footnote_page();
    return ffp >= 0 && cur_page_ == ffp;
}

void Fb2PageView::follow_footnote(const std::string& id) {
    if (fn_nav_.active) return; // already in a footnote
    const auto& idx = doc_->index();
    for (const auto& fn : idx.footnotes) {
        if (fn.id == id) {
            fn_nav_.active = true;
            fn_nav_.original_page = cur_page_;
            fn_nav_.first_block = fn.first_block;
            fn_nav_.last_block = fn.last_block;
            // Navigate to first page of this footnote
            int fp = first_footnote_page();
            if (fp >= 0) go_to_page(fp);
            return;
        }
    }
}

void Fb2PageView::exit_footnote() {
    if (!fn_nav_.active) return;
    int orig = fn_nav_.original_page;
    fn_nav_.active = false;
    if (orig >= 0 && orig < (int)pages_.size())
        go_to_page(orig);
    else
        go_to_page(0);
}

// ─── Render ────────────────────────────────────────────────────

// ─── Render ────────────────────────────────────────────────────
void Fb2PageView::render(uint8_t* fb, int fb_w, int fb_h) {
    if (pages_.empty()) return;

    for (int i = 0; i < fb_w * fb_h; ++i)
        fb[i] = kOffWhite;

    render_header(fb, fb_w, fb_h);
    render_footer(fb, fb_w, fb_h);

    // Ширина пробела нужна рендеру для раздвинутых строк. При загрузке книги
    // из кэша layout() не вызывается, поэтому меряем здесь — иначе слова
    // встали бы встык на выключенных строках.
    measure_justify();

    const Page& pg = pages_[cur_page_];

    // Image page: render image, skip text layout
    if (pg.is_image_page) {
        render_image(fb, fb_w, fb_h, pg);
        if (toc_visible_)
            render_toc(fb, fb_w, fb_h);
        return;
    }

    int y = content_y_;
    int max_w = content_w_;

    for (int li = pg.first_line_idx; li <= pg.last_line_idx && li < (int)all_lines_.size(); ++li) {
        const Line& ln = all_lines_[li];
        const int page_li = li - pg.first_line_idx;

        // Extra top for first title/subtitle line on the page
        bool title_first = (ln.block_type == (int)fb2::BlockType::Title &&
            (page_li == 0 ||
             all_lines_[li-1].block_type != (int)fb2::BlockType::Title ||
             all_lines_[li-1].block_index != ln.block_index));
        if (title_first) {
            float sc = 1.0f;
            if (ln.block_type == (int)fb2::BlockType::Title)
                sc = title_scale(ln.depth);
            y += (int)(line_height_ * sc);
        }
        if (ln.block_type == (int)fb2::BlockType::Epigraph && page_li == 0)
            y += line_height_;

        if (ln.para_break) {
            y += para_spacing_;
            continue;
        }

        if (ln.text.empty() && !ln.para_break) {
            y += line_height_;
            continue;
        }

        int x = content_x_;
        uint8_t color = kTextColor;

        if (ln.block_type == (int)fb2::BlockType::Epigraph ||
            ln.block_type == (int)fb2::BlockType::Verse) {
            x += font_size_ * kEpigraphIndentF;
            max_w = content_w_ - font_size_ * kEpigraphWidthF;
            color = kMid;
        } else {
            max_w = content_w_;
        }

        // Center titles and subtitles
        if (ln.block_type == (int)fb2::BlockType::Title ||
            ln.block_type == (int)fb2::BlockType::Subtitle) {
            int tw = text_width(ln.text);
            if (tw < content_w_)
                x = content_x_ + (content_w_ - tw) / 2;
        }

        if (ln.block_type == (int)fb2::BlockType::TextAuthor) {
            int tw = text_width(ln.text);
            x = content_x_ + content_w_ - tw;
        }

        if (ln.para_indent)
            x += para_indent_;

        // Title/subtitle lines are drawn with their full advance (scaled height),
        // so the last line of a page never dips below the reserved area.
        int advance = line_advance(ln.block_type, ln.depth);

        if (x < content_x_ + content_w_) {
            if (advance > line_height_) {
                // Larger font (title/subtitle): bold via 1px double-draw
                font_->draw_text(fb, fb_w, fb_h, x + 1, y + advance - 4, ln.text, color);
                font_->draw_text(fb, fb_w, fb_h, x,      y + advance - 4, ln.text, color);
            } else if (ln.space_extra > 0) {
                // Выключенная строка: рисуем по словам, раздвигая пробелы на
                // space_extra, иначе текст лёг бы по старой схеме и правый
                // край снова стал бы рваным.
                int wx = x;
                size_t pos = 0;
                while (pos < ln.text.size()) {
                    size_t sp = ln.text.find(' ', pos);
                    const size_t end = (sp == std::string::npos) ? ln.text.size() : sp;
                    const std::string word = ln.text.substr(pos, end - pos);
                    if (!word.empty())
                        font_->draw_text(fb, fb_w, fb_h, wx, y + line_height_ - 4, word, color);
                    wx += text_width(word) + space_w_ + ln.space_extra;
                    if (sp == std::string::npos) break;
                    pos = sp + 1;
                }
            } else {
                font_->draw_text(fb, fb_w, fb_h, x, y + line_height_ - 4, ln.text, color);
            }
        }
        y += advance;
    }

    // ─── Footnote underline + hit rectangles ──────────────────
    foot_hit_rects_.clear();
    {
        int fy = content_y_;
        for (int li = pg.first_line_idx; li <= pg.last_line_idx && li < (int)all_lines_.size(); ++li) {
            const Line& ln = all_lines_[li];
            const int page_li = li - pg.first_line_idx;
            // Recompute Y position to match text rendering
            if (ln.para_break) { fy += para_spacing_; continue; }
            if (ln.text.empty() && !ln.para_break) { fy += line_height_; continue; }
            if (ln.para_indent) { /* no extra spacing here */ }
            if (ln.block_type == (int)fb2::BlockType::Title &&
                (page_li == 0 || all_lines_[li-1].block_type != (int)fb2::BlockType::Title ||
                 all_lines_[li-1].block_index != ln.block_index))
                fy += (int)(line_height_ * title_scale(ln.depth));
            if (ln.block_type == (int)fb2::BlockType::Epigraph && page_li == 0)
                fy += line_height_;

            int fx = content_x_;
            if (ln.block_type == (int)fb2::BlockType::Epigraph ||
                ln.block_type == (int)fb2::BlockType::Verse)
                fx += font_size_ * kEpigraphIndentF;
            if (ln.block_type == (int)fb2::BlockType::TextAuthor) {
                int tw = text_width(ln.text);
                fx = content_x_ + content_w_ - tw;
            }
            if (ln.para_indent) fx += para_indent_;

            int fn_line_h = line_advance(ln.block_type, ln.depth);

            for (const auto& fn : ln.footnotes) {
                if (fn.start < 0 || fn.end > (int)ln.text.size() || fn.end <= fn.start)
                    continue;
                std::string link_text = ln.text.substr(fn.start, fn.end - fn.start);
                // У выключенной строки позиция считается по словам с учётом
                // растянутых пробелов, иначе подчёркивание уедет не туда.
                int lx = x_of_offset(ln, fx, fn.start);
                int lw = text_width(link_text);
                if (lw <= 0) continue;

                int underline_y = fy + fn_line_h - 2;
                uint8_t link_color = kMidDark;
                for (int px = lx; px < lx + lw && px < fb_w; ++px)
                    if (underline_y >= 0 && underline_y < fb_h)
                        fb[underline_y * fb_w + px] = link_color;

                foot_hit_rects_.push_back({lx, fy, lx + lw, fy + fn_line_h, fn.id});
            }
            fy += fn_line_h;
        }
    }

    // Dotted lines for page-flipping zones
    uint8_t dash = kBorder;
    int third = content_w_ / 3;
    for (int dy = content_y_; dy < content_y_ + content_h_; ++dy) {
        if ((dy - content_y_) % 6 < 3) {
            int dx1 = content_x_ + third;
            int dx2 = content_x_ + third * 2;
            if (dx1 >= 0 && dx1 < fb_w) fb[dy * fb_w + dx1] = dash;
            if (dx2 >= 0 && dx2 < fb_w) fb[dy * fb_w + dx2] = dash;
        }
    }

    if (toc_visible_)
        render_toc(fb, fb_w, fb_h);

    // Night mode: invert palette
    if (night_mode_) {
        for (int i = 0; i < fb_w * fb_h; ++i)
            fb[i] = 15 - fb[i];
    }
}

void Fb2PageView::predecode_cover() {
    if (!doc_ || !doc_->is_open()) return;
    const auto& cid = doc_->meta().title_info.cover_image_id;
    if (cid.empty()) return;

    const auto& imgs = doc_->index().images;
    for (size_t i = 0; i < imgs.size(); ++i) {
        if (imgs[i].id != cid) continue;
#ifdef ESP_PLATFORM
        printf("fbr: predecode_cover img=%u b64 heap=%u\n",
               (unsigned)i, (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
        std::string b64 = doc_->read_binary(i);
        if (b64.empty()) {
#ifdef ESP_PLATFORM
            printf("fbr: predecode_cover read_binary empty img=%u heap=%u\n",
                   (unsigned)i, (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
            return;
        }
        PaletteImage pal;
        if (!ImageDecoder::decode_fit_palette(b64, content_w_, content_h_, pal) ||
            pal.empty()) {
#ifdef ESP_PLATFORM
            printf("fbr: predecode_cover decode FAIL img=%u b64=%u heap=%u\n",
                   (unsigned)i, (unsigned)b64.size(),
                   (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
            return;
        }
        current_image_ = std::move(pal);
        current_image_idx_ = (int)i;

        // Миниатюра для книжной полки: та же картинка, ужатая до размера
        // обложки на полке. Держим её и после того, как current_image_
        // перезапишется при показе страницы с иллюстрацией.
        // 200×280 — бокс обложки на полке (kCoverMaxW/kCoverMaxH).
        if (ImageDecoder::downscale_palette(current_image_, 200, 280, cover_palette_)) {
#ifdef ESP_PLATFORM
            printf("fbr: cover thumb %dx%d heap=%u\n",
                   cover_palette_.width, cover_palette_.height,
                   (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
        }

#ifdef ESP_PLATFORM
        printf("fbr: predecode_cover ok %dx%d img=%u heap=%u\n",
               current_image_.width, current_image_.height, (unsigned)i,
               (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
        return;
    }
}

void Fb2PageView::render_image(uint8_t* fb, int fb_w, int fb_h, const Page& pg) {
    if (pg.empty() || pg.first_line_idx >= (int)all_lines_.size()) return;
    int img_idx = all_lines_[pg.first_line_idx].image_index;
    if (img_idx < 0) return;

#ifdef ESP_PLATFORM
    printf("fbr: render_image enter page=first_line=%d img=%d cur=%d empty=%d heap=%u\n",
           pg.first_line_idx, img_idx, current_image_idx_,
           current_image_.empty() ? 1 : 0, (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif

    // Decode if not cached — stream to 16-color palette (no truecolor MB).
    if (img_idx != current_image_idx_ || current_image_.empty()) {
        const auto& idx = doc_->index();
        if ((size_t)img_idx >= idx.images.size()) return;
        std::string b64 = doc_->read_binary(img_idx);
        if (b64.empty()) return;
        if (!ImageDecoder::decode_fit_palette(b64, content_w_, content_h_,
                                              current_image_)) {
#ifdef ESP_PLATFORM
            printf("fbr: render_image decode FAIL img=%d heap=%u\n",
                   img_idx, (unsigned)esp_get_free_heap_size());
            fflush(stdout);
#endif
            current_image_idx_ = img_idx;
            return;
        }
        current_image_idx_ = img_idx;
#ifdef ESP_PLATFORM
        printf("fbr: render_image ok %dx%d img=%d heap=%u\n",
               current_image_.width, current_image_.height, img_idx,
               (unsigned)esp_get_free_heap_size());
        fflush(stdout);
#endif
    }

    if (current_image_.empty()) return;

    // Center image in content area
    int img_w = current_image_.width;
    int img_h = current_image_.height;
    if (img_w > content_w_) img_w = content_w_;
    if (img_h > content_h_) img_h = content_h_;
    int ox = content_x_ + (content_w_ - img_w) / 2;
    int oy = content_y_ + (content_h_ - img_h) / 2;

    for (int y = 0; y < img_h && y + oy < fb_h; ++y) {
        const uint8_t* row =
            &current_image_.indices[(size_t)y * current_image_.width];
        uint8_t* dst = fb + (y + oy) * fb_w + ox;
        for (int x = 0; x < img_w && x + ox < fb_w; ++x)
            dst[x] = row[x]; // already palette index 0..15
    }
}

void Fb2PageView::render_header(uint8_t* fb, int fb_w, int fb_h) {
    FontRenderer* cf = ui_font_ ? ui_font_ : font_;

    for (int x = 0; x < fb_w; ++x)
        for (int y = 0; y < header_h_; ++y)
            fb[y * fb_w + x] = kPanelBg;

    for (int x = 0; x < fb_w; ++x)
        fb[(header_h_ - 1) * fb_w + x] = kBorderColor;

    std::string toc_icon = "\xE2\x89\xA1";
    cf->draw_text(fb, fb_w, fb_h, 8, header_h_ - 6, toc_icon, kTocIconColor);

    // Часы в правом верхнем углу. Внизу хедера проходит рамка, поэтому
    // базовая линия та же, что у значка оглавления. Справа на устройстве
    // пусто, а в симуляторе там кнопка выключения — под неё резервируем
    // место, чтобы «ЧЧ:ММ» ничего не задевало.
    {
        int hh = 0, mm = 0;
        if (clock_now(hh, mm)) {
            char clock[8];
            std::snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
            int right = width_ - kHeaderRightMargin;
#ifdef FBR_WIN32
            // Кнопка выключения рисуется от width_-66 (bx = fb_w - bw - 10),
            // поэтому под неё с запасом нужно 66 + 8, а не 56 + 6.
            right -= 66 + kHeaderRightMargin;
#endif
            const int cw = cf->text_width(clock);
            cf->draw_text(fb, fb_w, fb_h, right - cw, header_h_ - 6, clock, kTocIconColor);
        }
    }

#ifdef FBR_WIN32
    // Win32-only: power-off button (shows PowerOffScene from BookScene).
    int btn_x = fb_w - kPowerBtnW - kPowerBtnRight;
    int btn_y = margin_y_ + kPowerBtnInsetY;
    int btn_h = header_h_ - kPowerBtnInsetY * 2;
    fb_fill_rect(fb, fb_w, btn_x, btn_y, btn_x + kPowerBtnW - 1, btn_y + btn_h - 1, kPanelBg);
    for (int x = btn_x; x < btn_x + kPowerBtnW; ++x) {
        fb[btn_y * fb_w + x] = kBorderColor;
        fb[(btn_y + btn_h - 1) * fb_w + x] = kBorderColor;
    }
    for (int y = btn_y; y <= btn_y + btn_h - 1; ++y) {
        fb[y * fb_w + btn_x] = kBorderColor;
        fb[y * fb_w + btn_x + kPowerBtnW - 1] = kBorderColor;
    }
    int off_w = cf->text_width("OFF");
    cf->draw_text(fb, fb_w, fb_h, btn_x + (kPowerBtnW - off_w) / 2,
                  btn_y + btn_h - 6, "OFF", kDark1);
#endif
}

void Fb2PageView::render_footer(uint8_t* fb, int fb_w, int fb_h) {
    FontRenderer* cf = ui_font_ ? ui_font_ : font_;
    int fy = height_ - footer_h_;

    for (int x = 0; x < fb_w; ++x)
        for (int y = fy; y < height_; ++y)
            fb[y * fb_w + x] = kPanelBg;

    for (int x = 0; x < fb_w; ++x)
        fb[fy * fb_w + x] = kBorderColor;

    char buf[64];
    int total = (int)pages_.size();
    int pct = total > 1 ? (cur_page_ * 100) / (total - 1) : 100;
    snprintf(buf, sizeof(buf), "%d / %d  (%d%%)", cur_page_ + 1, total, pct);
    // Общая базовая линия для обоих индикаторов — по ней и выравнивается
    // полоса батареи.
    const int text_base = fy + footer_h_ - 6;
    cf->draw_text(fb, fb_w, fb_h, kFooterMargin, text_base, buf, kPageInfoColor);

    // Battery progress bar (bottom-right). Иконок в ассетах нет, поэтому
    // роль иконки играет сама полоса, а число рядом показывает уровень.
    int level = Platform::instance()->battery_level();
    if (level >= 0) {
        const int bar_w = 40;
        const int bar_h = 12;
        // Привязка справа по измеренной ширине текста: раньше x брался из
        // жёсткого fb_w-82, из-за чего «100%» (~67px) уезжал за правый
        // край экрана и обрезался, а правого поля не было вовсе.
        snprintf(buf, sizeof(buf), "%d%%", level);
        const int txt_x = fb_w - kFooterMargin - cf->text_width(buf);
        const int bar_x = txt_x - 4 - bar_w;
        // По центру полосы футера, а не прижато к верху: центр полосы
        // (fy+17) совпадает с оптическим центром цифр индикатора страниц.
        const int bar_y = fy + (footer_h_ - bar_h) / 2;

        // Background
        fb_fill_rect(fb, fb_w, bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1, kOffWhite);
        // Border
        for (int x = bar_x; x <= bar_x + bar_w - 1; ++x) {
            fb[bar_y * fb_w + x] = kDark2;
            fb[(bar_y + bar_h - 1) * fb_w + x] = kDark2;
        }
        for (int y = bar_y; y <= bar_y + bar_h - 1; ++y) {
            fb[y * fb_w + bar_x] = kDark2;
            fb[y * fb_w + bar_x + bar_w - 1] = kDark2;
        }
        // Fill
        int fill_w = (bar_w - 2) * level / 100;
        if (fill_w > 0) {
            uint8_t fill_c = level > 20 ? kMid : kBrown;
            fb_fill_rect(fb, fb_w, bar_x + 1, bar_y + 1,
                         bar_x + 1 + fill_w - 1, bar_y + bar_h - 2, fill_c);
        }
        // Text — та же базовая линия, что у индикатора страниц
        cf->draw_text(fb, fb_w, fb_h, txt_x, text_base, buf, kPageInfoColor);
    }
}

void Fb2PageView::render_toc(uint8_t* fb, int fb_w, int fb_h) {
    if (toc_entry_page_.empty()) return;

    FontRenderer* cf = ui_font_ ? ui_font_ : font_;
    int pw = width_ * kTocPanelPercent / 100;
    if (pw < kTocPanelMinW) pw = kTocPanelMinW;

    uint8_t text_c = kTocTextColor;
    int item_h = cf->line_height() + kTocItemExtraH;
    if (item_h < 50) item_h = 50;

    // Panel background
    for (int y = 0; y < fb_h; ++y)
        for (int x = 0; x < pw; ++x)
            fb[y * fb_w + x] = kPanelBg;

    for (int y = 0; y < fb_h; ++y)
        fb[y * fb_w + pw - 1] = kBorderColor;

    cf->draw_text(fb, fb_w, fb_h, 8, content_y_ - 6, "Оглавление", text_c);
    int sy = content_y_ + kTocStartY;
    int avail_h = fb_h - sy - kTocBottomPad;

    // Gather flat list. Alongside the title, keep the block the entry starts
    // at, so the current position can be resolved by block rather than by page:
    // a chapter and its <subtitle> often share a page, and highlighting both
    // looked like a bug.
    std::vector<std::pair<std::string, int>> flat;
    std::vector<int> flat_block;
    std::function<void(const fb2::TocEntry&, int)> flatten;
    flatten = [&](const fb2::TocEntry& e, int d) {
        for (const auto& c : e.children) {
            flat.push_back({c.title, d});
            flat_block.push_back(static_cast<int>(c.first_block));
            flatten(c, d + 1);
        }
    };
    flatten(doc_->index().toc_root, 0);

    int current_item = -1;
    {
        const int cb = current_page_block();
        int best = -1;
        for (int i = 0; i < (int)flat_block.size(); ++i) {
            if (cb >= 0 && flat_block[i] >= 0 && flat_block[i] <= cb &&
                flat_block[i] >= best) {
                best = flat_block[i];
                current_item = i;
            }
        }
    }

    int max_visible = flat.empty() ? 1 : (avail_h / item_h);
    int max_scroll = std::max(0, (int)flat.size() - max_visible);

    // ▲ scroll up button
    if (toc_scroll_ > 0) {
        for (int i = 0; i < 8 && sy + i < fb_h; ++i)
            for (int x = pw / 2 - 5; x < pw / 2 + 5 && x < pw; ++x)
                fb[(sy + i) * fb_w + x] = kGray;
        sy += kScrollBtnH;
        avail_h -= kScrollBtnH;
    }

    // Items
    int drawn = 0;
    int start_idx = toc_scroll_;
    for (int idx = start_idx; idx < (int)flat.size(); ++idx) {
        const auto& [title, depth] = flat[idx];
        int page = (idx < (int)toc_entry_page_.size()) ? toc_entry_page_[idx] : 0;
        int tx = 8 + depth * kTocDepthIndent;
        if (tx >= pw) continue;
        int ty = sy + drawn * item_h;
        if (ty + item_h >= fb_h) break;

        uint8_t c = (idx == current_item) ? kBlack : text_c;

        // Page number
        char pn[16];
        snprintf(pn, sizeof(pn), "%d", page + 1);
        std::string pn_s = pn;
        int pn_w = ui_text_width(pn_s);
        cf->draw_text(fb, fb_w, fb_h, pw - pn_w - 8, ty + item_h - 4,
                      pn_s, kPageNumColor);

        if (!title.empty()) {
            int max_tw = pw - tx - pn_w - kTocTitleRightPad;
            std::string display = title;
            while (ui_text_width(display) > max_tw && display.size() > 1)
                display.pop_back();
            if (display != title && display.size() > 1)
                display.pop_back();
            cf->draw_text(fb, fb_w, fb_h, tx, ty + item_h - 4, display, c);
        }
        ++drawn;
    }

    // ▼ scroll down button
    if (toc_scroll_ < max_scroll) {
        int by = fb_h - kScrollBtnH;
        for (int i = 0; i < 8 && by + i < fb_h; ++i)
            for (int x = pw / 2 - 5; x < pw / 2 + 5 && x < pw; ++x)
                fb[(by + i) * fb_w + x] = kGray;
    }

    for (int y = 0; y < fb_h; ++y)
        for (int x = pw; x < fb_w; ++x) {
            uint8_t p = fb[y * fb_w + x];
            fb[y * fb_w + x] = fb_dim(p, 12);
        }
}

int Fb2PageView::max_toc_scroll() const {
    if (toc_entry_page_.empty()) return 0;
    int pw = width_ * kTocPanelPercent / 100;
    if (pw < kTocPanelMinW) pw = kTocPanelMinW;
    int item_h = (ui_font_ ? ui_font_->line_height() : line_height_) + kTocItemExtraH;
    if (item_h < 50) item_h = 50;
    int sy = content_y_ + kTocStartY;
    int avail_h = height_ - sy - kTocBottomPad;
    int max_visible = avail_h / item_h;
    return std::max(0, (int)toc_entry_page_.size() - max_visible);
}

// X символа с заданным смещением внутри строки. У выключенной строки пробелы
// раздвинуты на space_extra, поэтому text_width(префикс) дал бы неверную
// позицию — идём по словам и добавляем растяжку за каждый пробел до offset.
int Fb2PageView::x_of_offset(const Line& ln, int start_x, int off) const {
    if (off <= 0) return start_x;
    if (ln.space_extra <= 0)
        return start_x + text_width(ln.text.substr(0, (size_t)std::min(off, (int)ln.text.size())));

    int x = start_x;
    size_t pos = 0;
    while (pos < ln.text.size() && (int)pos < off) {
        size_t sp = ln.text.find(' ', pos);
        const size_t end = (sp == std::string::npos) ? ln.text.size() : sp;
        if ((int)end > off) {
            // offset попал внутрь слова — возвращаем начало слова плюс ширину
            // его префикса.
            return x + text_width(ln.text.substr(pos, (size_t)off - pos));
        }
        x += text_width(ln.text.substr(pos, end - pos)) + space_w_ + ln.space_extra;
        if (sp == std::string::npos) break;
        pos = sp + 1;
    }
    return x;
}

// ─── Диагностика вёрстки ───────────────────────────────────────
namespace {

// Ширина строки так, как её увидит рендер: слова + растянутые пробелы.
int dump_line_width(const FontRenderer* font, const std::string& text,
                    int space_w, int space_extra, int* out_gaps) {
    int w = 0, gaps = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t sp = text.find(' ', pos);
        const size_t end = (sp == std::string::npos) ? text.size() : sp;
        w += font->text_width(text.substr(pos, end - pos));
        if (sp != std::string::npos) { w += space_w + space_extra; ++gaps; }
        pos = (sp == std::string::npos) ? text.size() : sp + 1;
    }
    if (out_gaps) *out_gaps = gaps;
    return w;
}

std::string first_token(const std::string& s) {
    size_t sp = s.find(' ');
    return s.substr(0, (sp == std::string::npos) ? s.size() : sp);
}

}  // namespace

void Fb2PageView::dump_layout(int max_pages, const char* tag) const {
    std::printf("--- dump %s: pages=%u lines=%u col_w=%d space=%d hyphen=%d ---\n",
                tag ? tag : "", (unsigned)pages_.size(), (unsigned)all_lines_.size(),
                content_w_, space_w_, hyphen_w_);
    const int limit = (max_pages > 0 && max_pages < (int)pages_.size())
                    ? max_pages : (int)pages_.size();
    int greedy_warn = 0;
    for (int p = 0; p < limit; ++p) {
        const Page& pg = pages_[p];
        std::printf("  == page %d (lines %d..%d) ==\n", p,
                    pg.first_line_idx, pg.last_line_idx);
        for (int li = pg.first_line_idx; li <= pg.last_line_idx; ++li) {
            if (li < 0 || li >= (int)all_lines_.size()) continue;
            const Line& ln = all_lines_[li];
            if (ln.para_break) { std::printf("    [para-break]      \n"); continue; }
            if (ln.text.empty()) { std::printf("    [blank]           \n"); continue; }

            int gaps = 0;
            int w = dump_line_width(font_, ln.text, space_w_, ln.space_extra, &gaps);
            const int indent = ln.para_indent ? para_indent_ : 0;
            const int slack = content_w_ - (indent + w);

            // Проверка жадности: если строка недобрана, а первое слово
            // следующей поместилось бы — перенос был неоптимальным.
            // Исключения: строка кончилась переносом (хвост слова назад
            // не вернуть) и следующая строка — другой абзац.
            const char* warn = nullptr;
            const bool ends_hyphen = ln.text.size() > 1 &&
                (unsigned char)ln.text.back() == 0x90 &&
                ln.text.size() > 2 && (unsigned char)ln.text[ln.text.size()-2] == 0x80 &&
                ln.text.size() > 3 && (unsigned char)ln.text[ln.text.size()-3] == 0xE2;
            if (slack > 0 && ln.space_extra == 0 && !ends_hyphen && li + 1 < (int)all_lines_.size()) {
                const Line& nx = all_lines_[li + 1];
                if (!nx.para_break && !nx.para_indent && !nx.text.empty()) {
                    const std::string tok = first_token(nx.text);
                    if (text_width(tok) + space_w_ <= slack) {
                        warn = "  <<GREEDY: next word would fit";
                        ++greedy_warn;
                    }
                }
            }

            // Глубина печатается только для заголовков: по ней выбирается
            // масштаб шрифта, и именно её затирает запись оглавления, если её
            // диапазон блоков шире собственного блока (см. set_depth).
            if (ln.block_type == (int)fb2::BlockType::Title)
                std::printf("    w=%4d gaps=%d extra=%2d bt=%2d d=%d slack=%4d | %s%s\n",
                            w, gaps, ln.space_extra, ln.block_type, ln.depth, slack,
                            ln.text.c_str(), warn ? warn : "");
            else
                std::printf("    w=%4d gaps=%d extra=%2d bt=%2d slack=%4d | %s%s\n",
                            w, gaps, ln.space_extra, ln.block_type, slack,
                            ln.text.c_str(), warn ? warn : "");
        }
    }
    std::printf("  (greedy shortfalls: %d)\n", greedy_warn);
    std::fflush(stdout);
}

uint32_t Fb2PageView::layout_signature() const {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        h ^= v; h *= 16777619u;
    };
    for (const Line& ln : all_lines_) {
        for (char c : ln.text) mix((unsigned char)c);
        mix(ln.space_extra & 0xFFFF);
        mix((uint32_t)ln.para_indent);
        mix((uint32_t)ln.para_break);
        mix((uint32_t)ln.block_type);
        mix((uint32_t)ln.image_index);
    }
    for (const Page& pg : pages_) {
        mix((uint32_t)pg.first_line_idx);
        mix((uint32_t)pg.last_line_idx);
        mix((uint32_t)pg.is_image_page);
    }
    return h;
}

// ─── Layout cache ─────────────────────────────────────────────
bool Fb2PageView::has_cache(const std::string& file_path) const {
    CachedLayout dummy;
    return LayoutCache::load(file_path, font_file_, font_size_, line_spacing_ratio_,
                             para_spacing_ratio_, para_indent_px_, width_, height_, dummy);
}

bool Fb2PageView::save_cache(const std::string& file_path) {
    CachedLayout data;
    data.file_path = file_path;
    data.font_size = font_size_;
    data.line_spacing = line_spacing_ratio_;
    data.para_spacing = para_spacing_ratio_;
    data.para_indent = para_indent_px_;
    data.width = width_;
    data.height = height_;

    data.pages.reserve(pages_.size());
    for (const auto& pg : pages_) {
        CachedLayout::CachedPage cp;
        cp.first_line_idx = pg.first_line_idx;
        cp.last_line_idx = pg.last_line_idx;
        cp.first_block = pg.first_block;
        cp.last_block = pg.last_block;
        cp.is_image_page = pg.is_image_page;
        cp.line_indices.reserve((size_t)pg.line_count());
        for (int i = pg.first_line_idx; i <= pg.last_line_idx; ++i)
            cp.line_indices.push_back(i);
        data.pages.push_back(std::move(cp));
    }

    data.block_depth = block_depth_;
    data.block_first_line = block_first_line_;
    data.toc_entry_page = toc_entry_page_;

#ifdef ESP_PLATFORM
    printf("fbr: save_cache start lines=%u pages=%u heap=%u\n",
           (unsigned)all_lines_.size(), (unsigned)pages_.size(),
           (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif

    // Stream all_lines_ → file (no CachedLayout.lines full-text copy → no OOM).
    std::vector<LayoutFnSpan> fn_buf;
    bool ok = LayoutCache::save_stream(
        file_path, font_file_, font_size_, line_spacing_ratio_,
        para_spacing_ratio_, para_indent_px_, width_, height_,
        (int)all_lines_.size(),
        [&](int i, LayoutLineView& lv) -> bool {
            if (i < 0 || i >= (int)all_lines_.size()) return false;
            const Line& ln = all_lines_[i];
            lv.text = &ln.text;
            lv.para_indent = ln.para_indent;
            lv.para_break = ln.para_break;
            lv.block_type = ln.block_type;
            lv.block_index = ln.block_index;
            lv.depth = ln.depth;
            lv.image_index = ln.image_index;
            lv.space_extra = ln.space_extra;
            if (ln.footnotes.empty()) {
                lv.footnotes = nullptr;
            } else {
                fn_buf.clear();
                fn_buf.reserve(ln.footnotes.size());
                for (const auto& fn : ln.footnotes)
                    fn_buf.push_back({fn.start, fn.end, fn.id});
                lv.footnotes = &fn_buf;
            }
            return true;
        },
        data.pages, data.block_depth, data.block_first_line, data.toc_entry_page);

#ifdef ESP_PLATFORM
    printf("fbr: save_cache done ok=%d heap=%u\n", (int)ok,
           (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif
    return ok;
}

bool Fb2PageView::load_cache(const std::string& file_path) {
    CachedLayout data;
    if (!LayoutCache::load(file_path, font_file_, font_size_, line_spacing_ratio_,
                           para_spacing_ratio_, para_indent_px_, width_, height_, data))
        return false;

    // Число блоков хранится в кэше. Если оно не совпадает с текущим индексом,
    // раскладка построена для другой версии книги — так бывает после правок
    // индексатора, и тогда кэш молча описывает не эту книгу: часть текста
    // просто пропадает, а номера страниц и сносок не сходятся. Пересчитываем.
    if (data.block_depth.size() != doc_->index().blocks.size()) {
        printf("fbr: cache stale blocks doc=%u cache=%u -> пересчёт\n",
               (unsigned)doc_->index().blocks.size(),
               (unsigned)data.block_depth.size());
        return false;
    }

    all_lines_.clear();
    all_lines_.reserve(data.lines.size());
    for (const auto& cl : data.lines) {
        Line ln;
        ln.text = cl.text;
        ln.para_indent = cl.para_indent;
        ln.block_type = cl.block_type;
        ln.para_break = cl.para_break;
        ln.block_index = cl.block_index;
        ln.depth = cl.depth;
        ln.image_index = cl.image_index;
        ln.space_extra = cl.space_extra;
        ln.footnotes.reserve(cl.footnotes.size());
        for (const auto& fn : cl.footnotes)
            ln.footnotes.push_back({fn.start, fn.end, fn.id});
        all_lines_.push_back(std::move(ln));
    }

    pages_.clear();
    pages_.reserve(data.pages.size());
    for (const auto& cp : data.pages) {
        Page pg;
        // Index range only — lines stay in all_lines_ (no text copies).
        pg.first_line_idx = cp.first_line_idx;
        pg.last_line_idx = cp.last_line_idx;
        pg.first_block = cp.first_block;
        pg.last_block = cp.last_block;
        pg.is_image_page = cp.is_image_page;
        pages_.push_back(pg);
    }

    block_depth_ = data.block_depth;
    block_first_line_ = data.block_first_line;
    toc_entry_page_ = data.toc_entry_page;
    toc_inited_ = true;

    if (cur_page_ >= (int)pages_.size())
        cur_page_ = 0;

#ifdef ESP_PLATFORM
    printf("fbr: load_cache done lines=%u pages=%u heap=%u\n",
           (unsigned)all_lines_.size(), (unsigned)pages_.size(),
           (unsigned)esp_get_free_heap_size());
    fflush(stdout);
#endif
    return true;
}

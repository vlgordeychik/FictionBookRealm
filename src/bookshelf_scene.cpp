#include "bookshelf_scene.h"
#include "scene_manager.h"
#include "browser_scene.h"
#include "settings_scene.h"
#include "book_scene.h"
#include "history_scene.h"
#include "cover_loader.h"
#include "font_renderer.h"
#include "config.h"
#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include "image_decoder.h"
#include "ui_overlay.h"
#include <cstdio>
#include <cstring>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_system.h"
#endif

static constexpr int kCoverMaxW = 200;
static constexpr int kCoverMaxH = 280;
static constexpr int kBtnW = 240;
static constexpr int kBtnH = 60;
static constexpr int kBtnGap = 24;
// Потолок строк аннотации. Реальное число считается от геометрии
// (между автором и блоком прогресса), это только предохранитель.
static constexpr int kMaxAnnotationLines = 12;

BookshelfScene::BookshelfScene(FontRenderer* font, fs::FileSystem* fs)
    : font_(font), fs_(fs) {}

void BookshelfScene::on_enter() {
    load_book_info();
}

void BookshelfScene::load_book_info() {
    last_file_ = cfg::get_str("last_file");
    cache_key_ = cfg::get_str("last_cache_key");
    title_ = cfg::get_str("last_title");
    author_ = cfg::get_str("last_author");
    total_pages_ = cfg::get_int("last_total_pages");
    current_page_ = cfg::get_int("last_page");
    has_book_ = !last_file_.empty();
    cover_loaded_ = false;
    cover_pal_ = PaletteImage{};
    annotation_.clear();

    if (!has_book_ || !fs_) return;

    // ── Книга всё ещё открыта под полкой (возврат свайпом вниз) ──
    // Книгу закрывать нельзя: под ней ~1 МБ раскладки, второй разбор .fb2
    // через pugi на этом не помещается. Поэтому обложку и аннотацию берём
    // из открытой книги:
    //   * аннотация уже разобрана в DOM — выделений не нужно вовсе;
    //   * обложка уже декодирована при открытии, когда куча была свободна.
    // Раньше здесь стоял ранний return, из-за которого аннотация терялась
    // именно при возврате из ридера, а повторный декод base64 (~1.6 МБ)
    // не влезал в оставшиеся ~386 КБ — обложка исчезала до перезагрузки.
    if (mgr_) {
        if (BookScene* book = mgr_->find_book(cache_key_)) {
            annotation_ = book->annotation();
            PaletteImage pal;
            if (book->cached_cover(kCoverMaxW, kCoverMaxH, pal)) {
                cover_pal_ = std::move(pal);
                cover_loaded_ = true;
            }
#ifdef ESP_PLATFORM
            ESP_LOGI("bookshelf", "from open book: cover=%d ann=%u heap=%u",
                     (int)cover_loaded_, (unsigned)annotation_.size(),
                     (unsigned)esp_get_free_heap_size());
#endif
            if (cover_loaded_ && !annotation_.empty()) return;
        }
    }

    // ── Холодный старт: книги в стеке нет, куча свободна ──
    CoverResult cr;
    bool cr_ok = load_cover_from_fb2_fs(fs_, last_file_, kCoverMaxW, kCoverMaxH, cr);
#ifdef ESP_PLATFORM
    ESP_LOGI("bookshelf", "load_cover ok=%d ann=%u id=%s px=%u heap=%u",
             (int)cr_ok, (unsigned)cr.annotation.size(),
             cr.cover_image_id.c_str(), (unsigned)cr.pixels.size(),
             (unsigned)esp_get_free_heap_size());
#endif
    if (cr_ok) {
        if (!cr.pixels.empty() && !cover_loaded_) {
            // Палитровый путь читает индексы напрямую; здесь обложка ещё
            // пришла в RGBA, поэтому приводим один раз.
            cover_pal_ = PaletteImage{};
            if (ImageDecoder::rgba_to_palette(cr.pixels, cr.width, cr.height,
                                              kCoverMaxW, kCoverMaxH, cover_pal_))
                cover_loaded_ = true;
        }
        if (annotation_.empty()) annotation_ = std::move(cr.annotation);
    }
#ifdef ESP_PLATFORM
    if (!cover_loaded_) {
        ESP_LOGI("bookshelf", "cover file path FAIL/empty heap=%u key=%s",
                 (unsigned)esp_get_free_heap_size(), cache_key_.c_str());
    }
#endif
}

int BookshelfScene::wrap_text(const std::string& text, int max_w, int max_lines,
                              std::vector<std::string>& lines) const {
    lines.clear();
    if (text.empty()) return 0;

    const char* p = text.c_str();
    int line_count = 0;

    while (*p && line_count < max_lines) {
        const char* para_start = p;
        while (*p && *p != '\n') ++p;
        std::string_view para(para_start, p - para_start);
        if (*p == '\n') ++p;

        std::string cur_line;
        const char* wp = para.data();
        const char* para_end = para.data() + para.size();

        while (wp < para_end && line_count < max_lines) {
            while (wp < para_end && *wp == ' ') ++wp;
            if (wp >= para_end) break;

            const char* word_start = wp;
            while (wp < para_end && *wp != ' ') ++wp;
            std::string_view word(word_start, wp - word_start);

            std::string test = cur_line.empty()
                ? std::string(word)
                : cur_line + " " + std::string(word);

            if (font_->text_width(test) > max_w && !cur_line.empty()) {
                lines.push_back(std::move(cur_line));
                cur_line.clear();
                ++line_count;
                wp = word_start;
            } else {
                cur_line = std::move(test);
            }
        }

        if (!cur_line.empty() && line_count < max_lines) {
            lines.push_back(std::move(cur_line));
            ++line_count;
        }
    }

    return line_count;
}

void BookshelfScene::draw_cover(uint8_t* fb, int fb_w, int fb_h,
                                 int x, int y, int w, int h) {
    if (cover_loaded_ && !cover_pal_.empty()) {
        int img_w = cover_pal_.width;
        int img_h = cover_pal_.height;
        if (img_w > w) img_w = w;
        if (img_h > h) img_h = h;
        int ox = x + (w - img_w) / 2;
        int oy = y + (h - img_h) / 2;

        // Индексы уже палитровые — копируем в буфер кадра напрямую, как
        // это делает Fb2PageView::render_image. Раньше здесь на каждый
        // пиксель вызывался argb_to_palette: 56000 конвертаций на кадр
        // ради картинки, которая уже палитровая.
        for (int iy = 0; iy < img_h && iy + oy < fb_h; ++iy) {
            if (oy + iy < 0) continue;
            const uint8_t* srow = &cover_pal_.indices[(size_t)iy * cover_pal_.width];
            uint8_t* dst = fb + (size_t)(iy + oy) * fb_w + ox;
            for (int ix = 0; ix < img_w && ix + ox < fb_w; ++ix)
                dst[ix] = srow[ix];
        }
        return;
    }

    fb_fill_rect(fb, fb_w, x, y, x + w - 1, y + h - 1, kButton);
    for (int px = x; px < x + w; ++px) {
        fb[px + y * fb_w] = kBorder;
        fb[px + (y + h - 1) * fb_w] = kBorder;
    }
    for (int py = y; py < y + h; ++py) {
        fb[x + py * fb_w] = kBorder;
        fb[(x + w - 1) + py * fb_w] = kBorder;
    }
    int spine_x = x + 12;
    for (int py = y + 4; py < y + h - 4; ++py)
        fb[spine_x + py * fb_w] = kBorder;

    if (!title_.empty()) {
        std::string letter;
        unsigned char c = (unsigned char)title_[0];
        if (c < 0x80) letter = title_.substr(0, 1);
        else if ((c & 0xE0) == 0xC0) letter = title_.substr(0, 2);
        else if ((c & 0xF0) == 0xE0) letter = title_.substr(0, 3);
        else if ((c & 0xF8) == 0xF0) letter = title_.substr(0, 4);
        if (!letter.empty()) {
            int tw = font_->text_width(letter);
            int lx = x + 20 + (w - 32 - tw) / 2;
            int ly = y + h / 2 + 8;
            font_->draw_text(fb, fb_w, fb_h, lx, ly, letter, kMidDark);
        }
    }

    std::string hint = "\xD0\x9E\xD0\xB1\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xBA\xD0\xb0";
    int tw = font_->text_width(hint);
    font_->draw_text(fb, fb_w, fb_h, x + (w - tw) / 2, y + h - 10, hint, kGray);
}

void BookshelfScene::render(uint8_t* fb, int fb_w, int fb_h) {
    fb_fill(fb, fb_w, fb_h, kOffWhite);

    // Кэпшн «Bookshelf» и разделительная линия убраны: они съедали 58 px
    // сверху, а аннотации не хватало места и она наезжала на кнопки.
    const int lh = font_->line_height();
    const int content_y = 14;
    const int info_x = 290;

    // Нижняя граница контента — верх кнопок. Считаем её до блока
    // информации, чтобы ею пользоваться как пределом для аннотации.
    const int btn_y = fb_h - 60 - kBtnH;

    // Тап-зона правой колонки. Границы берём оттуда же, откуда рисуем:
    // content_y сверху, info_x слева, край текста справа, а снизу — ровно
    // над кнопками, чтобы зоны не пересекались. Не зависит от длины
    // аннотации, поэтому пишется здесь, а не в конце отрисовки.
    info_x1_ = info_x;
    info_y1_ = content_y;
    info_x2_ = fb_w - 40 - 1;
    info_y2_ = btn_y - 1;

    int cover_x = 50;
    int cover_y = content_y;
    draw_cover(fb, fb_w, fb_h, cover_x, cover_y, kCoverMaxW, kCoverMaxH);
    cover_x1_ = cover_x;
    cover_y1_ = cover_y;
    cover_x2_ = cover_x + kCoverMaxW - 1;
    cover_y2_ = cover_y + kCoverMaxH - 1;

    // Блок прогресса прижат к кнопкам и не зависит от длины аннотации.
    // Все y ниже — базовые линии: FontRenderer::draw_text рисует глифы НАД
    // переданным значением, поэтому строка, начинающаяся с content_y, имеет
    // базовую линию content_y + lh - 4.
    const int prog_h = lh + 8 + 14;
    const int prog_top = btn_y - 12 - prog_h;
    // Последняя базовая линия аннотации: оставляем одну строку под саму
    // базовую прогресса плюс зазор, чтобы тексты не слипались.
    const int ann_last_baseline = prog_top - lh - 6;

    int info_y = content_y + lh - 4;

    if (has_book_) {
        std::string disp_title = title_.empty() ? last_file_ : title_;
        int max_tw = fb_w - info_x - 40;
        while (font_->text_width(disp_title) > max_tw && disp_title.size() > 1)
            disp_title.pop_back();
        font_->draw_text(fb, fb_w, fb_h, info_x, info_y, disp_title, kBlack);
        info_y += lh + 8;

        if (!author_.empty()) {
            font_->draw_text(fb, fb_w, fb_h, info_x, info_y, author_, kMid);
            info_y += lh + 12;
        } else {
            info_y += 8;
        }

        if (!annotation_.empty()) {
            const int max_ann_w = fb_w - info_x - 40;
            // Сколько строк влезает между автором и блоком прогресса —
            // считаем, а не берём константу: раньше предел стоял на
            // fb_h-100 = 440, тогда как кнопки начинались с 420, и текст
            // на них наезжал.
            int max_lines = (ann_last_baseline - info_y) / (lh + 2) + 1;
            if (max_lines > kMaxAnnotationLines) max_lines = kMaxAnnotationLines;
            if (max_lines < 0) max_lines = 0;

            if (max_lines > 0) {
                std::vector<std::string> ann_lines;
                wrap_text(annotation_, max_ann_w, max_lines, ann_lines);
                for (const auto& line : ann_lines) {
                    if (info_y > ann_last_baseline) break;
                    font_->draw_text(fb, fb_w, fb_h, info_x, info_y, line, kMid);
                    info_y += lh + 2;
                }
            }
        }

        if (total_pages_ > 0) {
            const int pct = current_page_ * 100 / total_pages_;
            char prog[64];
            std::snprintf(prog, sizeof(prog), "%d / %d  (%d%%)",
                         current_page_ + 1, total_pages_, pct);
            font_->draw_text(fb, fb_w, fb_h, info_x, prog_top, prog, kMidDark);
            int bar_x = info_x;
            int bar_y = prog_top + lh + 8;
            int bar_w = fb_w - info_x - 40;
            int bar_h = 14;
            fb_fill_rect(fb, fb_w, bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1, kButton);
            for (int px = bar_x; px < bar_x + bar_w; ++px) {
                fb[px + bar_y * fb_w] = kBorder;
                fb[px + (bar_y + bar_h - 1) * fb_w] = kBorder;
            }
            for (int py = bar_y; py < bar_y + bar_h; ++py) {
                fb[bar_x + py * fb_w] = kBorder;
                fb[(bar_x + bar_w - 1) + py * fb_w] = kBorder;
            }
            int fill = (bar_w - 2) * pct / 100;
            if (fill > 0)
                fb_fill_rect(fb, fb_w, bar_x + 1, bar_y + 1,
                             bar_x + 1 + fill - 1, bar_y + bar_h - 2, kMidDark);
        }
    } else {
        font_->draw_text(fb, fb_w, fb_h, info_x, info_y + 40,
                         "\xD0\x9A\xD0\xBD\xD0\xB8\xD0\xB3\xD0\xB0 \xD0\xBD\xD0\xB5 \xD0\xB2\xD1\x8B\xD0\xB1\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb0",
                         kGray);
    }

    // 3 buttons in a single centered row
    int btn_total_w = kBtnW * 3 + kBtnGap * 2;
    int btn_start_x = (fb_w - btn_total_w) / 2;

    auto draw_btn = [&](Btn& b, const char* label, int bx, int by) {
        b.x1 = bx;
        b.y1 = by;
        b.x2 = bx + kBtnW - 1;
        b.y2 = by + kBtnH - 1;
        b.label = label;

        fb_fill_rect(fb, fb_w, b.x1, b.y1, b.x2, b.y2, kButton);
        for (int px = b.x1; px <= b.x2; ++px) {
            fb[px + b.y1 * fb_w] = kBorder;
            fb[px + b.y2 * fb_w] = kBorder;
        }
        for (int py = b.y1; py <= b.y2; ++py) {
            fb[b.x1 + py * fb_w] = kBorder;
            fb[b.x2 + py * fb_w] = kBorder;
        }

        int tw = font_->text_width(label);
        int tx = b.x1 + (kBtnW - tw) / 2;
        int ty = b.y1 + (kBtnH + font_->line_height()) / 2;
        font_->draw_text(fb, fb_w, fb_h, tx, ty, label, kMidDark);
    };

    draw_btn(btn_browse_,
             "\xD0\x9E\xD0\xB1\xD0\xB7\xD0\xBE\xD1\x80 \xD0\xB1\xD0\xB8\xD0\xB1\xD0\xBB\xD0\xB8\xD0\xBE\xD1\x82\xD0\xB5\xD0\xBA\xD0\xB8",
             btn_start_x, btn_y);
    draw_btn(btn_history_,
             "\xD0\x98\xd1\x81\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f",
             btn_start_x + kBtnW + kBtnGap, btn_y);
    draw_btn(btn_settings_,
             "\xD0\x9D\xD0\xB0\xD1\x81\xd1\x82\xd1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8",
             btn_start_x + (kBtnW + kBtnGap) * 2, btn_y);
}

void BookshelfScene::on_mouse_down(int x, int y, int button) {
    if (button == 2) { mgr_->go_back(); return; }

    auto in_btn = [](const Btn& b, int mx, int my) {
        return mx >= b.x1 && mx <= b.x2 && my >= b.y1 && my <= b.y2;
    };

    if (in_btn(btn_browse_, x, y)) {
        mgr_->push(new BrowserScene(font_, fs_));
    } else if (in_btn(btn_history_, x, y)) {
        mgr_->push(new HistoryScene(font_, fs_));
    } else if (in_btn(btn_settings_, x, y)) {
        mgr_->push(new SettingsScene(font_, fs_));
    } else if (has_book_) {
        const bool on_cover = x >= cover_x1_ && x <= cover_x2_ &&
                              y >= cover_y1_ && y <= cover_y2_;
        // Правая колонка открывает книгу тоже: держа девайс в правой руке,
        // до обложки у левого края не дотянуться.
        const bool on_info = x >= info_x1_ && x <= info_x2_ &&
                             y >= info_y1_ && y <= info_y2_;
        if (on_cover || on_info)
            open_book(mgr_, fs_, last_file_, cache_key_);
    }
}

void BookshelfScene::on_key_down(int key) {
    if (key == 27) { mgr_->go_back(); return; }
    if ((key == 13 || key == 39) && has_book_) {
        open_book(mgr_, fs_, last_file_, cache_key_);
    }
}

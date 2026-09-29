#include "toc_scene.h"
#include "scene_manager.h"
#include "font_renderer.h"
#include "palette.h"
#include "fb_helpers.h"
#include "icons_data.h"
#include "config.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {
    constexpr int kTopMargin    = 40;
    constexpr int kLeftMargin   = 22;
    constexpr int kRightMargin  = 90;
    constexpr int kDepthIndent  = 22;
    constexpr int kItemPad      = 16;
    constexpr int kScrollBtnH   = 40;
    // Пиктограмма прокрутки. Раньше треугольник был 6 px высотой при полосе
    // нажатия 40 px: целиться приходилось в крошечную фигуру, а не в полосу.
    // Теперь 12 линий по 2 px = 24 px высотой, 144 px шириной у основания, и
    // подложка показывает, где вообще можно тыкать.
    constexpr int kArrowRows    = 12;
    constexpr int kArrowThick   = 2;
    constexpr int kArrowHalfMax = 72;      // полуразмах у основания
    constexpr int kScrollPillW  = 200;     // ширина подложки
}

std::vector<TocScene::Item>
TocScene::items_from_index(const fb2::TocEntry& root,
                           const std::vector<int>& toc_pages,
                           int cur_block, int cur_page) {
    std::vector<Item> items;
    std::vector<int> blocks;   // параллельно items: first_block записи
    int ti = 0;
    std::function<void(const fb2::TocEntry&, int)> flatten;
    flatten = [&](const fb2::TocEntry& e, int depth) {
        for (const auto& c : e.children) {
            int page = (ti < (int)toc_pages.size()) ? toc_pages[ti] : 0;
            items.push_back({c.title, depth, page, false});
            blocks.push_back(static_cast<int>(c.first_block));
            ++ti;
            flatten(c, depth + 1);
        }
    };
    flatten(root, 0);

    // Подсвечивается ровно одна запись — последняя, чей блок уже начался.
    // Сравнение «страница записи == текущая страница» подсвечивало две строки
    // подряд, когда глава и её <subtitle> попадали на одну страницу, и не
    // подсвечивало ничего на странице, не совпадающей ни с одной записью.
    int best = -1, best_at = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (cur_block >= 0 && blocks[i] >= 0 && blocks[i] <= cur_block &&
            blocks[i] >= best) {
            best = blocks[i];
            best_at = i;
        }
    }
    if (best_at >= 0) {
        items[best_at].is_current = true;
    } else {
        for (auto& it : items) {
            if (it.page == cur_page) { it.is_current = true; break; }
        }
    }
    return items;
}

TocScene::TocScene(FontRenderer* font,
                   std::vector<Item> items, int current_page, NavFn nav)
    : font_(font), nav_(std::move(nav)),
      items_(std::move(items)), current_page_(current_page) {}

int TocScene::item_h() const {
    int h = font_->line_height() + kItemPad;
    return h < 50 ? 50 : h;
}

int TocScene::max_visible(int fb_h) const {
    // Резервируются верхняя и нижняя полосы прокрутки. Раньше вместо нижней
    // резервировалась строка под индикатор — при том, что сам индикатор
    // рисовался поверх списка, и зарезервированное место не использовалось.
    int avail = fb_h - kTopMargin - 2 * kScrollBtnH;
    if (avail <= 0) return 1;
    return avail / item_h();
}

namespace {
// Треугольник в полосе прокрутки: up — вершина сверху. Раньше верхняя и
// нижняя кнопки рисовались одинаковым кодом, где полуразмах рос сверху вниз,
// поэтому обе смотрели вверх. Здесь направление задаётся знаком роста.
void draw_scroll_button(uint8_t* fb, int fb_w, int fb_h, int band_y, bool up,
                        uint8_t bg, uint8_t border, uint8_t arrow) {
    const int px = fb_w / 2 - kScrollPillW / 2;
    fb_fill_rect(fb, fb_w, px, band_y, px + kScrollPillW - 1,
                 band_y + kScrollBtnH - 1, bg);
    for (int x = px; x < px + kScrollPillW; ++x) {
        fb[band_y * fb_w + x] = border;
        fb[(band_y + kScrollBtnH - 1) * fb_w + x] = border;
    }
    for (int y = band_y; y < band_y + kScrollBtnH; ++y) {
        fb[y * fb_w + px] = border;
        fb[y * fb_w + px + kScrollPillW - 1] = border;
    }

    const int step = kArrowHalfMax / (kArrowRows - 1);
    const int top = band_y + (kScrollBtnH - kArrowRows * kArrowThick) / 2;
    for (int i = 0; i < kArrowRows; ++i) {
        const int grow = up ? i : (kArrowRows - 1 - i);
        const int half = grow * step;
        for (int t = 0; t < kArrowThick; ++t) {
            const int y = top + i * kArrowThick + t;
            if (y < band_y || y >= band_y + kScrollBtnH) continue;
            fb_hline(fb, fb_w, fb_w / 2 - half, fb_w / 2 + half, y, arrow);
        }
    }
}
}  // namespace

void TocScene::on_enter() {
    scroll_ = 0;
    sel_ = -1;
    window_h_ = 540; // updated in render()
    // Auto-scroll to current chapter
    for (int i = 0; i < (int)items_.size(); ++i) {
        if (items_[i].is_current) {
            sel_ = i;
            int maxv = max_visible(window_h_);
            int target = i - maxv / 2;
            if (target > 0) scroll_ = target;
            break;
        }
    }
}

void TocScene::render(uint8_t* fb, int fb_w, int fb_h) {
    bool nm = cfg::get_int("night_mode") != 0;
    uint8_t bg = nm ? kBlack : kOffWhite;
    uint8_t text_c = nm ? kOffWhite : kDark2;
    uint8_t btn_bg = nm ? kDark2 : kButton;
    uint8_t icon_c = nm ? kOffWhite : kDark1;

    fb_fill(fb, fb_w, fb_h, bg);

    window_h_ = fb_h;
    window_w_ = fb_w;

    font_->draw_text(fb, fb_w, fb_h, kLeftMargin, kTopMargin - 6, "\xd0\x9e\xd0\xb3\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5", text_c);

    // Положение в списке — в верхней строке, рядом с заголовком. Раньше
    // индикатор рисовался внизу: его базовая линия (494 при высоте 540)
    // попадала в глифы последнего пункта (476), и надпись становилась
    // нечитаемой. Внизу теперь только кнопка прокрутки.
    // Показывается диапазон видимого, а не «сколько показано»: величина
    // «сколько показано» постоянна (окно всегда заполнено), и на прокрутку
    // не реагировала вовсе.
    if (!items_.empty()) {
        char hint[48];
        const int first = scroll_ + 1;
        const int last = std::min(scroll_ + max_visible(fb_h), (int)items_.size());
        std::snprintf(hint, sizeof(hint), "%d\xe2\x80\x93%d \xd0\xb8\xd0\xb7 %d",
                      first, last, (int)items_.size());
        const int hw = font_->text_width(hint);
        font_->draw_text(fb, fb_w, fb_h, window_w_ - kRightMargin - hw,
                         kTopMargin - 6, hint, nm ? kGray : kLight2);
    }

    // "Назад" button (top-right) — turn-back icon
    {
        int bw = 56, bh = 44;
        int bx = fb_w - bw - 10, by = 5;
        fb_fill_rect(fb, fb_w, bx, by, bx + bw - 1, by + bh - 1, btn_bg);
        for (int xx = bx; xx < bx + bw; ++xx) { fb[by * fb_w + xx] = text_c; fb[(by + bh - 1) * fb_w + xx] = text_c; }
        for (int yy = by; yy < by + bh; ++yy) { fb[yy * fb_w + bx] = text_c; fb[yy * fb_w + bx + bw - 1] = text_c; }
        int ix = bx + (bw - kIconTurnBack.w) / 2;
        int iy = by + (bh - kIconTurnBack.h) / 2;
        fb_blit_icon(fb, fb_w, fb_h, ix, iy, kIconTurnBack, icon_c, nm);
    }

    if (items_.empty()) {
        font_->draw_text(fb, fb_w, fb_h, kLeftMargin, fb_h / 2, "(no sections)", nm ? kGray : kLight2);
        return;
    }

    int lh = item_h();
    int avail_h = fb_h - kTopMargin - kScrollBtnH;
    int max_vis = max_visible(fb_h);
    int max_sc = std::max(0, (int)items_.size() - max_vis);

    int y = kTopMargin + kScrollBtnH;

    // Scroll up
    if (scroll_ > 0)
        draw_scroll_button(fb, fb_w, fb_h, kTopMargin, true,
                           btn_bg, text_c, nm ? kGray : kBrown);

    // Items
    int end = std::min(scroll_ + max_vis, (int)items_.size());
    for (int idx = scroll_; idx < end; ++idx) {
        const auto& item = items_[idx];
        int ty = y + (idx - scroll_) * lh;

        // Current item highlight
        if (idx == sel_ || item.is_current) {
            fb_fill_rect(fb, fb_w, 0, ty, fb_w - 1, ty + lh - 1, nm ? kDark1 : kCream);
        }

        // Depth indent
        int tx = kLeftMargin + item.depth * kDepthIndent;
        if (tx >= fb_w) continue;

        // Title text
        uint8_t tc;
        if (nm)
            tc = (idx == sel_ || item.is_current) ? kOffWhite : kLight2;
        else
            tc = (idx == sel_ || item.is_current) ? kBlack : kMidDark;
        int max_tw = fb_w - tx - kRightMargin;
        std::string display = item.title;
        if (display.empty()) display = "(untitled)";
        while (font_->text_width(display) > max_tw && display.size() > 1)
            display.pop_back();

        font_->draw_text(fb, fb_w, fb_h, tx, ty + lh - 4, display, tc);

        // Page number
        char pnb[16];
        std::snprintf(pnb, sizeof(pnb), "%d", item.page + 1);
        font_->draw_text(fb, fb_w, fb_h, fb_w - kRightMargin + 10, ty + lh - 4, pnb, nm ? kGray : kLight1);
    }

    // Scroll down
    if (scroll_ < max_sc)
        draw_scroll_button(fb, fb_w, fb_h, fb_h - kScrollBtnH, false,
                           btn_bg, text_c, nm ? kGray : kBrown);
}

void TocScene::on_mouse_down(int x, int y, int button) {
    (void)button;

    // "Назад" button (top-right)
    if (x >= window_w_ - 66 && x < window_w_ - 10 && y >= 5 && y < 49) {
        go_back();
        return;
    }

    if (items_.empty()) {
        go_back();
        return;
    }

    int lh = item_h();
    int start_y = kTopMargin + kScrollBtnH;
    int max_vis = max_visible(window_h_);
    int max_sc = std::max(0, (int)items_.size() - max_vis);

    // Scroll up click
    if (scroll_ > 0 && y >= kTopMargin && y < kTopMargin + kScrollBtnH) {
        scroll_ = std::max(0, scroll_ - max_vis / 2);
        return;
    }

    // Scroll down click
    if (scroll_ < max_sc && y >= window_h_ - kScrollBtnH && y < window_h_) {
        scroll_ = std::min((int)items_.size() - max_vis, scroll_ + max_vis / 2);
        return;
    }

    // Item click
    int idx = scroll_ + (y - start_y) / lh;
    if (idx >= 0 && idx < (int)items_.size() && y >= start_y) {
        sel_ = idx;
        nav_(items_[idx].page);
        go_back();
    }
}

void TocScene::on_key_down(int key) {
    if (key == 38) { // UP
        if (sel_ > 0) --sel_;
        if (sel_ < scroll_) scroll_ = sel_;
    } else if (key == 40) { // DOWN
        if (sel_ < (int)items_.size() - 1) ++sel_;
        int maxv = max_visible(window_h_);
        if (sel_ >= scroll_ + maxv) scroll_ = sel_ - maxv + 1;
    } else if (key == 13 || key == 39) { // ENTER / RIGHT
        if (sel_ >= 0 && sel_ < (int)items_.size()) {
            nav_(items_[sel_].page);
            go_back();
        }
    } else if (key == 27) { // ESC
        go_back();
    }
}

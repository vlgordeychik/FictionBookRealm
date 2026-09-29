#include "history_scene.h"
#include "scene_manager.h"
#include "book_scene.h"
#include "font_renderer.h"
#include "config.h"
#include "palette.h"
#include "fb_helpers.h"
#include "icons_data.h"
#include "platform.h"
#include "ui_overlay.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {
    constexpr int kTopMargin    = 40;
    constexpr int kLeftMargin   = 22;
    constexpr int kScrollBtnH   = 40;
    constexpr int kItemPad      = 6;
    constexpr int kItemGap      = 10;
}

HistoryScene::HistoryScene(FontRenderer* font, fs::FileSystem* fs)
    : font_(font), fs_(fs) {}

void HistoryScene::on_enter() {
    History::set_fs(fs_);
    History::load(entries_);
}

int HistoryScene::item_h() const {
    // 3 lines per entry: title, author, progress + gap
    return font_->line_height() * 3 + kItemPad + kItemGap;
}

int HistoryScene::max_visible(int fb_h) const {
    return (fb_h - kTopMargin - kScrollBtnH - 30) / item_h();
}

void HistoryScene::render(uint8_t* fb, int fb_w, int fb_h) {
    fb_fill(fb, fb_w, fb_h, kOffWhite);
    bool nm = cfg::get_int("night_mode") != 0;

    uint8_t text_c = nm ? kOffWhite : kBlack;
    uint8_t sub_c  = nm ? kGray : kMid;
    uint8_t btn_bg = nm ? kDark2 : kButton;
    uint8_t icon_c = nm ? kOffWhite : kMidDark;

    window_w_ = fb_w;
    window_h_ = fb_h;

    // Title
    font_->draw_text(fb, fb_w, fb_h, kLeftMargin, kTopMargin - 4,
                     "\xd0\x98\xd1\x81\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f", text_c);

    // "Назад" button (top-right)
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

    if (entries_.empty()) {
        font_->draw_text(fb, fb_w, fb_h, kLeftMargin, fb_h / 2,
                         "\xd0\x9d\xd0\xb5\xd1\x82 \xd1\x8d\xd0\xbb\xd0\xb5\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x82\xd0\xbe\xd0\xb2 \xd0\xb2 \xd0\xb8\xd1\x81\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8",
                         nm ? kGray : kLight2);
        return;
    }

    int lh = item_h();
    int max_vis = max_visible(fb_h);
    int max_sc = std::max(0, (int)entries_.size() - max_vis);

    // Scroll up button
    if (scroll_ > 0) {
        int bw = fb_w - 2 * kLeftMargin;
        fb_fill_rect(fb, fb_w, kLeftMargin, kTopMargin, kLeftMargin + bw - 1, kTopMargin + kScrollBtnH - 1, btn_bg);
        for (int xx = kLeftMargin; xx < kLeftMargin + bw; ++xx) {
            fb[kTopMargin * fb_w + xx] = text_c;
            fb[(kTopMargin + kScrollBtnH - 1) * fb_w + xx] = text_c;
        }
        std::string up_arrow = "\xe2\x96\xb2"; // ▲
        int aw = font_->text_width(up_arrow);
        font_->draw_text(fb, fb_w, fb_h, fb_w / 2 - aw / 2, kTopMargin + kScrollBtnH - 3, up_arrow, text_c);
    }

    // Scroll down button
    if (scroll_ < max_sc) {
        int bw = fb_w - 2 * kLeftMargin;
        int sby = fb_h - kScrollBtnH;
        fb_fill_rect(fb, fb_w, kLeftMargin, sby, kLeftMargin + bw - 1, fb_h - 1, btn_bg);
        for (int xx = kLeftMargin; xx < kLeftMargin + bw; ++xx) {
            fb[sby * fb_w + xx] = text_c;
            fb[(fb_h - 1) * fb_w + xx] = text_c;
        }
        std::string dn_arrow = "\xe2\x96\xbc"; // ▼
        int aw = font_->text_width(dn_arrow);
        font_->draw_text(fb, fb_w, fb_h, fb_w / 2 - aw / 2, fb_h - 3, dn_arrow, text_c);
    }

    // List entries
    int start_y = kTopMargin + kScrollBtnH + 4;
    int start = scroll_;
    for (int i = start; i < (int)entries_.size() && (i - start) < max_vis; ++i) {
        const auto& e = entries_[i];
        int y = start_y + (i - start) * lh;

        // Highlight selected
        if (i == sel_) {
            int y2 = y + lh - kItemGap - 1;
            if (y2 >= fb_h) y2 = fb_h - 1;
            fb_fill_rect(fb, fb_w, 0, y, fb_w - 1, y2, nm ? kDark2 : kCream);
        }

        int ty = y + font_->line_height() - 2;

        // Title (bold-ish: darker color)
        std::string disp_title = e.title.empty() ? e.file_path : e.title;
        int max_tw = fb_w - kLeftMargin - 40;
        while (font_->text_width(disp_title) > max_tw && disp_title.size() > 1)
            disp_title.pop_back();
        font_->draw_text(fb, fb_w, fb_h, kLeftMargin, ty, disp_title, text_c);
        ty += font_->line_height();

        // Author
        if (!e.author.empty())
            font_->draw_text(fb, fb_w, fb_h, kLeftMargin, ty, e.author, sub_c);
        ty += font_->line_height();

        // Progress
        if (e.total_pages > 0) {
            int pct = e.current_page * 100 / e.total_pages;
            char prog[64];
            std::snprintf(prog, sizeof(prog), "\xd1\x81\xd1\x82\xd1\x80. %d / %d  (%d%%)",
                         e.current_page + 1, e.total_pages, pct);
            font_->draw_text(fb, fb_w, fb_h, kLeftMargin, ty, prog, sub_c);

            // Mini progress bar
            int bar_x = kLeftMargin;
            int bar_y = ty + 4;
            int bar_w = 120;
            int bar_h = 8;
            if (bar_y + bar_h < fb_h) {
                fb_fill_rect(fb, fb_w, bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1, nm ? kMid : kButton);
                int fill = (bar_w - 2) * pct / 100;
                if (fill > 0)
                    fb_fill_rect(fb, fb_w, bar_x + 1, bar_y + 1, bar_x + 1 + fill - 1, bar_y + bar_h - 2,
                                 nm ? kMidDark : kMidDark);
            }
        }
    }

    // Hint
    char hint[32];
    std::snprintf(hint, sizeof(hint), "%d/%d", (int)entries_.size(), (int)entries_.size());
    font_->draw_text(fb, fb_w, fb_h, kLeftMargin, fb_h - 2, hint, nm ? kGray : kLight2);
}

void HistoryScene::on_mouse_down(int x, int y, int button) {
    if (button == 2) { go_back(); return; }

    // "Назад" button (top-right)
    if (x >= window_w_ - 66 && x < window_w_ - 10 && y >= 5 && y < 49) {
        go_back();
        return;
    }

    if (entries_.empty()) { go_back(); return; }

    int lh = item_h();
    int max_vis = max_visible(window_h_);
    int max_sc = std::max(0, (int)entries_.size() - max_vis);
    int start_y = kTopMargin + kScrollBtnH + 4;

    // Scroll up
    if (scroll_ > 0 && y >= kTopMargin && y < kTopMargin + kScrollBtnH) {
        scroll_ = std::max(0, scroll_ - max_vis / 2);
        return;
    }

    // Scroll down
    if (scroll_ < max_sc && y >= window_h_ - kScrollBtnH && y < window_h_) {
        scroll_ = std::min((int)entries_.size() - max_vis, scroll_ + max_vis / 2);
        return;
    }

    // Item click
    int idx = scroll_ + (y - start_y) / lh;
    if (idx >= 0 && idx < (int)entries_.size() && y >= start_y) {
        sel_ = idx;
        const auto& e = entries_[idx];
        open_book(mgr_, fs_, e.file_path, e.cache_key, e.current_page);
    }
}

void HistoryScene::on_key_down(int key) {
    if (key == 27) { go_back(); return; }

    int max_vis = max_visible(window_h_);
    int max_sc = std::max(0, (int)entries_.size() - max_vis);

    if (key == 38) { // UP
        if (sel_ > 0) --sel_;
        if (sel_ < scroll_) scroll_ = sel_;
    } else if (key == 40) { // DOWN
        if (sel_ < (int)entries_.size() - 1) ++sel_;
        if (sel_ >= scroll_ + max_vis) scroll_ = sel_ - max_vis + 1;
    } else if (key == 13 || key == 39) { // ENTER / RIGHT
        if (sel_ >= 0 && sel_ < (int)entries_.size()) {
            const auto& e = entries_[sel_];
            open_book(mgr_, fs_, e.file_path, e.cache_key, e.current_page);
        }
    }
}

#include "browser_scene.h"
#include "scene_manager.h"
#include "book_scene.h"
#include "settings_scene.h"
#include "font_renderer.h"
#include "filesys/filesystem.h"
#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include "icons_data.h"
#include "config.h"
#include "fb2zip.h"
#include "ui_overlay.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

BrowserScene::BrowserScene(FontRenderer* font, fs::FileSystem* fs)
    : font_(font), fs_(fs), cur_dir_("/") {
#ifdef ESP_PLATFORM
    ESP_LOGI("browser", "ctor this=%p fs=%p", (void*)this, (void*)fs);
#endif
}

/* Touch rows must be at least ~50px so a finger can reliably select them. */
int BrowserScene::row_h() const {
    int lh = font_->line_height() + 4;
    return lh < 52 ? 52 : lh;
}

std::string BrowserScene::parent(const std::string& p) const {
    if (p.empty() || p == "/") return "/";
    std::string s = p;
    if (s.back() == '/') s.pop_back();
    auto pos = s.rfind('/');
    if (pos == std::string::npos) return "/";
    return s.substr(0, pos + 1);
}

void BrowserScene::refresh() {
    entries_.clear();
    scroll_ = 0;
    sel_ = 0;
    if (!fs_) return;

    if (cur_dir_ != "/")
        entries_.push_back({true, ".."});

    constexpr size_t kMaxEntries = 128;
    auto* fat_entries = new fs::DirEntry[kMaxEntries];
    size_t n = fs_->read_dir(cur_dir_.c_str(), fat_entries, kMaxEntries);

    std::vector<Entry> dirs, books;
    for (size_t i = 0; i < n; ++i) {
        if (fat_entries[i].is_dir)
            dirs.push_back({true, fat_entries[i].name});
        else {
            std::string name = fat_entries[i].name;
            bool is_zip = fb2zip::is_zip_path(name);
            bool is_fb2 = false;
            if (name.size() > 4) {
                std::string ext = name.substr(name.size() - 4);
                is_fb2 = ext == ".fb2" || ext == ".FB2";
            }
            if (is_fb2 || is_zip)
                books.push_back({false, name});
        }
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    std::sort(books.begin(), books.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    for (auto& e : dirs) entries_.push_back(e);
    for (auto& e : books) entries_.push_back(e);

    delete[] fat_entries;
}

int BrowserScene::max_visible(int fb_h) const {
    return (fb_h - 80) / row_h();
}

void BrowserScene::on_enter() {
#ifdef ESP_PLATFORM
    ESP_LOGI("browser", "on_enter this=%p", (void*)this);
#endif
    refresh();
#ifdef ESP_PLATFORM
    ESP_LOGI("browser", "refresh done, entries=%d", (int)entries_.size());
#endif
}

void BrowserScene::render(uint8_t* fb, int fb_w, int fb_h) {
    fb_fill(fb, fb_w, fb_h, kOffWhite);
    window_w_ = fb_w;
    window_h_ = fb_h;
    bool nm = cfg::get_int("night_mode") != 0;

    int lh = row_h();
    int y = 30;
    int max_vis = max_visible(fb_h);

    // Back button (top-right)
    {
        int bw = 56, bh = 44;
        int bx = fb_w - bw - 10, by = 5;
        uint8_t btn_bg = nm ? kDark2 : kButton;
        uint8_t text_c = nm ? kOffWhite : kBlack;
        uint8_t icon_c = nm ? kOffWhite : kMidDark;
        fb_fill_rect(fb, fb_w, bx, by, bx + bw - 1, by + bh - 1, btn_bg);
        for (int xx = bx; xx < bx + bw; ++xx) { fb[by * fb_w + xx] = text_c; fb[(by + bh - 1) * fb_w + xx] = text_c; }
        for (int yy = by; yy < by + bh; ++yy) { fb[yy * fb_w + bx] = text_c; fb[yy * fb_w + bx + bw - 1] = text_c; }
        int ix = bx + (bw - kIconTurnBack.w) / 2;
        int iy = by + (bh - kIconTurnBack.h) / 2;
        fb_blit_icon(fb, fb_w, fb_h, ix, iy, kIconTurnBack, icon_c, nm);
    }

    std::string header = cur_dir_;
    if (header.size() > 50) header = "..." + header.substr(header.size() - 47);
    font_->draw_text(fb, fb_w, fb_h, 10, y, header, kMidDark);
    y += lh + 10;

    const int kScrollBtnW = 66;
    const int col_x0 = fb_w - kScrollBtnW - 10;
    const int col_x1 = fb_w - 10;
    const int mid_y = (60 + fb_h - 30) / 2;
    const int max_sc = std::max(0, (int)entries_.size() - max_vis);

    // Right-edge scroll buttons (▲ up / ▼ down), drawn after rows
    uint8_t btn_bg = nm ? kDark2 : kButton;
    uint8_t text_c  = nm ? kOffWhite : kBlack;
    uint8_t icon_c  = nm ? kOffWhite : kMidDark;

    int start = scroll_;
    for (int i = start; i < (int)entries_.size() && y + lh < fb_h - 10; ++i) {
        auto& e = entries_[i];
        const char* kind = e.is_dir ? "DIR" : (fb2zip::is_zip_path(e.name) ? "ZIP" : "FB2");
        std::string line = std::string("[") + kind + "] " + e.name;
        uint8_t color = (i == sel_) ? kBlack : kMid;
        if (i == sel_) {
            int y1 = (y + lh - 1 < fb_h) ? y + lh - 1 : fb_h - 1;
            fb_fill_rect(fb, fb_w, 0, y, col_x0 - 1, y1, kCream);
        }
        int max_tw = col_x0 - 20;
        while (font_->text_width(line) > max_tw && line.size() > 1)
            line.pop_back();
        font_->draw_text(fb, fb_w, fb_h, 10, y + lh - 4, line, color);
        y += lh;
    }

    // Up button
    {
        int b0 = 60, b1 = mid_y - 7;
        if (b1 > b0 && scroll_ > 0) {
            fb_fill_rect(fb, fb_w, col_x0, b0, col_x1, b1, btn_bg);
            for (int xx = col_x0; xx <= col_x1; ++xx) { fb[b0 * fb_w + xx] = text_c; fb[b1 * fb_w + xx] = text_c; }
            for (int yy = b0; yy <= b1; ++yy) { fb[yy * fb_w + col_x0] = text_c; fb[yy * fb_w + col_x1] = text_c; }
            std::string up_arrow = "\xe2\x96\xb2"; // ▲
            int aw = font_->text_width(up_arrow);
            font_->draw_text(fb, fb_w, fb_h, (col_x0 + col_x1 - aw) / 2,
                             (b0 + b1) / 2 + 4, up_arrow, text_c);
        }
    }
    // Down button
    {
        int b0 = mid_y + 7, b1 = fb_h - 30;
        if (b1 > b0 && scroll_ < max_sc) {
            fb_fill_rect(fb, fb_w, col_x0, b0, col_x1, b1, btn_bg);
            for (int xx = col_x0; xx <= col_x1; ++xx) { fb[b0 * fb_w + xx] = text_c; fb[b1 * fb_w + xx] = text_c; }
            for (int yy = b0; yy <= b1; ++yy) { fb[yy * fb_w + col_x0] = text_c; fb[yy * fb_w + col_x1] = text_c; }
            std::string dn_arrow = "\xe2\x96\xbc"; // ▼
            int aw = font_->text_width(dn_arrow);
            font_->draw_text(fb, fb_w, fb_h, (col_x0 + col_x1 - aw) / 2,
                             (b0 + b1) / 2 - font_->line_height() + 3, dn_arrow, text_c);
        }
    }

    char hint[128];
    int total = (int)entries_.size();
    int shown = std::min(total - start, max_vis);
    std::snprintf(hint, sizeof(hint), "%d/%d  Enter=open  RClick=settings", shown, total);
    font_->draw_text(fb, fb_w, fb_h, 10, fb_h - 22, hint, kLight2);

    if (!fs_)
        font_->draw_text(fb, fb_w, fb_h, 10, fb_h / 2,
                         "No media. RClick to open from filesystem.", kLight2);
    else if (total == 0)
        font_->draw_text(fb, fb_w, fb_h, 10, fb_h / 2,
                         "(empty directory)", kLight2);
}

void BrowserScene::on_mouse_down(int x, int y, int button) {
    if (button == 2) {
        mgr_->push(new SettingsScene(font_, fs_));
        return;
    }

    // Back button hit-test (top-right, 56x44)
    if (x >= window_w_ - 66 && x < window_w_ - 10 && y >= 5 && y < 49) {
        go_back();
        return;
    }

    int fb_w = window_w_;
    const int kScrollBtnW = 66;
    const int col_x0 = fb_w - kScrollBtnW - 10;
    const int col_x1 = fb_w - 10;
    const int mid_y = (60 + window_h_ - 30) / 2;

    int lh = row_h();
    int max_vis = max_visible(window_h_);
    int max_sc = std::max(0, (int)entries_.size() - max_vis);

    // Scroll buttons (right edge, below back button)
    if (x >= col_x0 && x < col_x1 && y >= 60 && y < window_h_ - 30) {
        int step = max_vis / 2;
        if (y < mid_y) { // up
            if (scroll_ > 0) {
                scroll_ -= step;
                if (scroll_ < 0) scroll_ = 0;
                if (sel_ > scroll_ + max_vis - 1) sel_ = scroll_ + max_vis - 1;
            }
        } else { // down
            if (scroll_ < max_sc) {
                scroll_ += step;
                if (scroll_ > max_sc) scroll_ = max_sc;
                if (sel_ < scroll_) sel_ = scroll_;
            }
        }
        return;
    }

    int startY = 30 + lh + 10;
    int idx = scroll_ + (y - startY) / lh;

    if (idx >= 0 && idx < (int)entries_.size() && y >= startY) {
        auto& e = entries_[idx];
        if (e.is_dir) {
            cur_dir_ = (e.name == "..") ? parent(cur_dir_) : cur_dir_ + e.name + "/";
            refresh();
        } else {
            sel_ = idx;
            open_selected_file();
        }
    }
}

void BrowserScene::open_selected_file() {
    if (sel_ < 0 || sel_ >= (int)entries_.size()) return;
    auto& e = entries_[sel_];
    if (e.is_dir || !fs_) return;

    std::string full_path = cur_dir_ + e.name;

    // Full path is passed both as internal storage path and as the
    // user-visible "source_path" (used as layout-cache/history key).
    open_book(mgr_, fs_, full_path, full_path);
}

void BrowserScene::on_key_down(int key) {
    if (key == 38) { // UP
        if (sel_ > 0) --sel_;
        if (sel_ < scroll_) scroll_ = sel_;
    } else if (key == 40) { // DOWN
        if (sel_ < (int)entries_.size() - 1) ++sel_;
        int maxv = max_visible(Platform::instance()->height());
        if (sel_ >= scroll_ + maxv) scroll_ = sel_ - maxv + 1;
    } else if (key == 13 || key == 39) { // ENTER / RIGHT
        if (sel_ >= 0 && sel_ < (int)entries_.size()) {
            auto& e = entries_[sel_];
            if (e.is_dir) {
                cur_dir_ = (e.name == "..") ? parent(cur_dir_) : cur_dir_ + e.name + "/";
                refresh();
            } else {
                open_selected_file();
            }
        }
    } else if (key == 27) { // ESC
        go_back();
    }
}

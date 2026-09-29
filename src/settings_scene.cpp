#include "settings_scene.h"
#include "scene_manager.h"
#include "font_renderer.h"
#include "config.h"
#include "clock.h"
#include "filesys/filesystem.h"
#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include "icons_data.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(...) ((void)0)
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <new>
#include <string>

// Р“СЂР°РЅРёС†С‹ Р·РЅР°С‡РµРЅРёР№ Р±РµСЂСѓС‚СЃСЏ РёР· СЃС…РµРјС‹ РєРѕРЅС„РёРіСѓСЂР°С†РёРё (config.cpp): С‚Р°Рј Р¶Рµ РѕРЅРё
// РїРѕРїР°РґР°СЋС‚ РІ РїРѕСЏСЃРЅРµРЅРёСЏ Рє РїР°СЂР°РјРµС‚СЂР°Рј РІ settings.json, С‚Р°Рє С‡С‚Рѕ С„Р°Р№Р» Рё UI РЅРµ
// РјРѕРіСѓС‚ СЂР°Р·РѕР№С‚РёСЃСЊ.
static constexpr int kBtnW = 60;
static constexpr int kBtnH = 52;
static constexpr int kRowY[] = {60, 116, 172, 228, 284, 340, 396};
static constexpr int kLabelCol = 30;
static constexpr int kRightMargin = 40;
static constexpr int kBackRow = 480;
// РџРѕР»РѕСЃР° РІРєР»Р°РґРѕРє Р·Р°РЅРёРјР°РµС‚ РјРµСЃС‚Рѕ РїСЂРµР¶РЅРµРіРѕ Р·Р°РіРѕР»РѕРІРєР°, РїРѕСЌС‚РѕРјСѓ kRowY РЅРµ
// СЃРґРІРёРіР°СЋС‚СЃСЏ: 7 СЃС‚СЂРѕРє РїРѕ-РїСЂРµР¶РЅРµРјСѓ Р·Р°РЅРёРјР°СЋС‚ 60..448, В«РќР°Р·Р°РґВ» РЅР° 480.
static constexpr int kTabY = 12;
static constexpr int kTabH = 38;

// РџРѕРґРїРёСЃРё РІРєР»Р°РґРѕРє. Р СѓСЃСЃРєРёР№ С‚РµРєСЃС‚ вЂ” escape-РїРѕСЃР»РµРґРѕРІР°С‚РµР»СЊРЅРѕСЃС‚СЏРјРё UTF-8,
// РєР°Рє РїСЂРёРЅСЏС‚Рѕ РІ РѕСЃС‚Р°Р»СЊРЅС‹С… СЃС†РµРЅР°С….
static const char* const kTabName[] = {
    "\xD0\xA7\xD1\x82\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",                       // Р§С‚РµРЅРёРµ
    "\xD0\xA1\xD0\xB8\xD1\x81\xD1\x82\xD0\xB5\xD0\xBC\xD0\xB0",                // РЎРёСЃС‚РµРјР°
};
static constexpr int kTabCount = 2;
// Р—Р°РјРµС‚РєРё РЅР° РІРєР»Р°РґРєР°С….
static const char* const kTabEmptyNote =
    "\xD0\x97\xD0\xB4\xD0\xB5\xD1\x81\xD1\x8C \xD0\xB2\xD0\xBA\xD0\xBB\xD0\xB0\xD0\xB4\xD0\xBA\xD0\xB5 \xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8";
static const char* const kClockNowLabel =
    "\xD0\xA1\xD0\xB5\xD0\xB9\xD1\x87\xD0\xB0\xD1\x81:";   // РЎРµР№С‡Р°СЃ:
static const char* const kClockNoClock =
    "\xD1\x87\xD0\xB0\xD1\x81\xD1\x8B \xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE\xD1\x81\xD1\x82\xD1\x83\xD0\xBF\xD0\xBD\xD1\x8B";  // С‡Р°СЃС‹ РЅРµРґРѕСЃС‚СѓРїРЅС‹

SettingsScene::SettingsScene(FontRenderer* font, fs::FileSystem* fs)
    : font_(font), fs_(fs) {}

void SettingsScene::scan_fonts() {
    font_files_.clear();
    if (!fs_) return;
    fs::DirEntry entries[64];
    size_t n = fs_->read_dir(".fonts", entries, 64);
    for (size_t i = 0; i < n; ++i) {
        if (entries[i].is_dir) continue;
        const char* name = entries[i].name;
        size_t len = std::strlen(name);
        if (len > 4 && (std::strcmp(name + len - 4, ".ttf") == 0 ||
                        std::strcmp(name + len - 4, ".TTF") == 0))
            font_files_.push_back(name);
    }
    std::sort(font_files_.begin(), font_files_.end());
}

void SettingsScene::refresh_clock() {
    int hh = 0, mm = 0;
    if (clock_now(hh, mm)) {
        clock_hh_ = hh;
        clock_mm_ = mm;
    } else {
        clock_hh_ = -1;
        clock_mm_ = -1;
    }
}

void SettingsScene::bump_clock(bool hours, int delta) {
    // ������ ���� ������: ������ ������ ������� ���������� �����, � �� ��,
    // ��� ���� �������� ��� ����� �� �����.
    refresh_clock();
    if (clock_hh_ < 0) return;   // РІСЂРµРјСЏ РІР·СЏС‚СЊ РЅРµРѕС‚РєСѓРґР° вЂ” РєСЂСѓС‚РёС‚СЊ РЅРµС‡РµРіРѕ

    int hh = clock_hh_, mm = clock_mm_;
    if (hours) {
        hh = (hh + delta + 24) % 24;
    } else {
        // РњРёРЅСѓС‚С‹ 59 -> 00 РїРµСЂРµРЅРѕСЃСЏС‚ С‡Р°СЃ: РёРЅР°С‡Рµ В«14:59В» + 1 РґР°Р»Рѕ Р±С‹ 14:00.
        int total = hh * 60 + mm + delta;
        total %= 24 * 60;
        if (total < 0) total += 24 * 60;
        hh = total / 60;
        mm = total % 60;
    }

    clock_hh_ = hh;
    clock_mm_ = mm;
    // РџРёС€РµРј СЃСЂР°Р·Сѓ, Р·РґРµСЃСЊ Р¶Рµ: Р¶РґР°С‚СЊ РІРѕР·РІСЂР°С‚Р° РЅР° РїРѕР»РєСѓ РЅРµ РЅСѓР¶РЅРѕ.
    clock_set(hh, mm);
    dirty_ = true;
}

void SettingsScene::load() {
    std::string font_file = cfg::get_str("font_file");
    // cfg::get_* СѓР¶Рµ РѕРіСЂР°РЅРёС‡РёРІР°РµС‚ Р·РЅР°С‡РµРЅРёСЏ РґРёР°РїР°Р·РѕРЅР°РјРё РёР· СЃС…РµРјС‹.
    font_size_ = cfg::get_int("font_size");
    line_spacing_ = cfg::get_float("line_spacing");
    para_spacing_ = cfg::get_float("para_spacing");
    para_indent_ = cfg::get_int("para_indent");
    night_mode_ = cfg::get_int("night_mode") != 0;
    refresh_pages_ = cfg::get_int("refresh_pages");
    auto_off_min_ = cfg::get_int("auto_off_min");

    font_index_ = -1;
    for (size_t i = 0; i < font_files_.size(); ++i) {
        if (font_files_[i] == font_file) {
            font_index_ = (int)i;
            break;
        }
    }
    if (font_index_ < 0 && !font_files_.empty())
        font_index_ = 0;
}

void SettingsScene::save() {
    // Р—РЅР°С‡РµРЅРёСЏ С‚РѕР»СЊРєРѕ РјРµРЅСЏСЋС‚СЃСЏ РІ РїР°РјСЏС‚Рё: settings.json РїРµСЂРµР·Р°РїРёСЃС‹РІР°РµС‚СЃСЏ
    // Р±Р»РёР¶Р°Р№С€РёРј cfg::flush() Рё РЅРµ С‡Р°С‰Рµ СЂР°Р·Р° РЅР° РєР°РґСЂ. Р Р°РЅСЊС€Рµ Р·РґРµСЃСЊ Р±С‹Р»Рѕ СЃРµРјСЊ
    // РІС‹Р·РѕРІРѕРІ, РєР°Р¶РґС‹Р№ РїРµСЂРµРїРёСЃС‹РІР°Р» РІРµСЃСЊ С„Р°Р№Р» вЂ” РІСЃРїР»РµСЃРєРё С‚РѕРєР° РЅР° SD РїСЂРёРІРѕРґРёР»Рё
    // Рє brownout РїСЂРё РІС‹С…РѕРґРµ РёР· РЅР°СЃС‚СЂРѕРµРє.
    const char* font_name =
        (font_index_ >= 0 && font_index_ < (int)font_files_.size())
            ? font_files_[font_index_].c_str() : "";

    cfg::set_int("font_size", font_size_);
    cfg::set_str("font_file", font_name);
    cfg::set_float("line_spacing", line_spacing_);
    cfg::set_float("para_spacing", para_spacing_);
    cfg::set_int("para_indent", para_indent_);
    cfg::set_int("night_mode", night_mode_ ? 1 : 0);
    cfg::set_int("refresh_pages", refresh_pages_);
    cfg::set_int("auto_off_min", auto_off_min_);

    dirty_ = false;
    ESP_LOGI("fbr", "settings queued (flushed once per frame) size=%d file=%s",
             font_size_,
             (font_index_ >= 0 && font_index_ < (int)font_files_.size())
                 ? font_files_[font_index_].c_str() : "-");
}

void SettingsScene::apply(int row) {
    // row 0 = font file, 1 = size, 2..4 = spacing/indent.
    // Persist only in on_exit() вЂ” avoid SD rewrite on every button.
    dirty_ = true;
    if (row == 2 || row == 3 || row == 4 || row == 6) return;

    FontRenderer* rf = mgr_ ? mgr_->font() : nullptr;
    if (!rf) return;

    if (row == 1) {
        // Size only: rescale the already-loaded face вЂ” no SD read, no
        // memory_fonts_ growth (every full TTF reload risked OOM в†’ abort).
        ESP_LOGI("fbr", "settings size apply %d loaded=%d cur=%d",
                 font_size_, rf->is_loaded() ? 1 : 0, rf->pixel_size());
        if (rf->is_loaded())
            rf->set_pixel_size(font_size_);
        return;
    }

    // row 0 вЂ” font file change: safe full reload
    if (font_index_ < 0 || font_index_ >= (int)font_files_.size() || !fs_)
        return;

    std::string path = std::string(".fonts/") + font_files_[font_index_];
    fs::File* f = fs_->open(path.c_str());
    if (!f) return;

    size_t sz = f->size();
    if (sz == 0) {
        delete f;
        return;
    }
    std::unique_ptr<unsigned char[]> buf(new (std::nothrow) unsigned char[sz]);
    if (!buf) {
        delete f;
        return;
    }
    size_t n = f->read(buf.get(), sz);
    delete f;
    if (n != sz) return;

    rf->load_font_from_memory(buf.get(), sz,
                              font_files_[font_index_].c_str(), font_size_);
}

void SettingsScene::on_enter() {
    scan_fonts();
    load();
    refresh_clock();
    dirty_ = false;
}

void SettingsScene::on_exit() {
    if (dirty_) save();
}

static void draw_button(uint8_t* fb, int fb_w, int fb_h, int x, int y, int w, int h,
                         const char* text, uint8_t bg_color, uint8_t text_color, FontRenderer* font) {
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    if (x < fb_w && y < fb_h && x1 >= 0 && y1 >= 0)
        fb_fill_rect(fb, fb_w, x, y, x1, y1, bg_color);

    int tw = font->text_width(text);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - font->line_height()) / 2;
    font->draw_text(fb, fb_w, fb_h, tx, ty, text, text_color);
}

static void draw_icon_button(uint8_t* fb, int fb_w, int fb_h, int x, int y, int w, int h,
                              const Icon& icon, uint8_t bg_color, uint8_t icon_color,
                              bool invert = false) {
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    if (x < fb_w && y < fb_h && x1 >= 0 && y1 >= 0)
        fb_fill_rect(fb, fb_w, x, y, x1, y1, bg_color);
    int ix = x + (w - icon.w) / 2;
    int iy = y + (h - icon.h) / 2;
    fb_blit_icon(fb, fb_w, fb_h, ix, iy, icon, icon_color, invert);
}

void SettingsScene::render(uint8_t* fb, int fb_w, int fb_h) {
    fb_fill(fb, fb_w, fb_h, night_mode_ ? kBlack : kOffWhite);
    uint8_t label_color = night_mode_ ? kLight2 : kMidDark;
    uint8_t text_color = night_mode_ ? kOffWhite : kDark1;
    uint8_t btn_bg = night_mode_ ? kDark2 : kButton;

    // ������ ������� �� ����� �������� ���������: ����� ������� ��� ��, �
    // ������ ����� ����������.
    for (int t = 0; t < kTabCount; ++t) {
        const int tw = fb_w / kTabCount;
        const int tx0 = t * tw;
        const bool on = ((int)active_tab_ == t);
        fb_fill_rect(fb, fb_w, tx0, kTabY, tx0 + tw - 1, kTabY + kTabH,
                     on ? btn_bg : (night_mode_ ? kDark1 : kCream));
        const char* nm = kTabName[t];
        const int nw = font_->text_width(nm);
        font_->draw_text(fb, fb_w, fb_h, tx0 + (tw - nw) / 2,
                         font_->baseline_for_center(kTabY, kTabH), nm,
                         on ? (night_mode_ ? kBlack : kOffWhite) : label_color);
    }

    // Колонка кнопок: [-] ... значение ... [+]
    const int controls_col = fb_w - 300;
    const int plus_x = fb_w - kRightMargin - kBtnW;
    auto draw_row = [&](int row, const char* label, const char* value) {
        const int y = kRowY[row];
        // Та же базовая линия, что и у кнопок: иначе подпись и значок на
        // кнопке расходятся по вертикали примерно на половину заглавных.
        const int base = font_->baseline_for_center(y, kBtnH);
        font_->draw_text(fb, fb_w, fb_h, kLabelCol, base, label, label_color);
        if (value)
            font_->draw_text(fb, fb_w, fb_h, controls_col + kBtnW + 4, base,
                             value, text_color);
        draw_icon_button(fb, fb_w, fb_h, controls_col, y, kBtnW, kBtnH,
                         kIconMinus, btn_bg, text_color, night_mode_);
        draw_icon_button(fb, fb_w, fb_h, plus_x, y, kBtnW, kBtnH,
                         kIconPlus, btn_bg, text_color, night_mode_);
    };
    // ����� ���� �� ������ �����������: ����� ����� ���� ������ �����
    // �������, � ��� ����� ������� ������� �� �� ������� �����. �����
    // ������ �������� ���� ��� ��������, ������� refresh_clock() ������
    // �� ��������.
    // ������� �������: ������������ ������ � ���� �������. ��
    // �������� ���� ������ ����, � �������� ����� ������ �������
    // �������� ��� ��������, ����� ��� ����� �� ������ ������ �����.
    if (active_tab_ != Tab::Reading && active_tab_ != Tab::System) {
        font_->draw_text(fb, fb_w, fb_h, kLabelCol, kRowY[0] + kBtnH / 2,
                         kTabEmptyNote, label_color);
    } else if (active_tab_ == Tab::Reading) {

    // Row 0: Font
    {
        std::string name = (font_index_ >= 0 && font_index_ < (int)font_files_.size())
                           ? font_files_[font_index_] : "<none>";
        draw_row(0, "Font:", name.c_str());
    }

    // Row 1: Size
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", font_size_);
        draw_row(1, "Size:", buf);
    }

    // Row 2: Line Spacing
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", line_spacing_);
        draw_row(2, "Line Spc:", buf);
    }

    // Row 3: Paragraph Spacing
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f", para_spacing_);
        draw_row(3, "Para Spc:", buf);
    }

    // Row 4: Paragraph Indent
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", para_indent_);
        draw_row(4, "Indent:", buf);
    }

    // Row 5: Night Mode (toggle)
    {
        font_->draw_text(fb, fb_w, fb_h, kLabelCol,
                         font_->baseline_for_center(kRowY[5], kBtnH),
                         "Night:", label_color);
        const Icon& toggle_icon = night_mode_ ? kIconDay : kIconNight;
        uint8_t toggle_bg = night_mode_ ? kDark2 : kButton;
        draw_icon_button(fb, fb_w, fb_h, plus_x, kRowY[5], kBtnW, kBtnH,
                         toggle_icon, toggle_bg, text_color, night_mode_);
    }

    // Row 6: Refresh interval (pages between full-quality redraws)
    {
        char buf[16];
        if (refresh_pages_ <= 0)
            std::snprintf(buf, sizeof(buf), "off");
        else
            std::snprintf(buf, sizeof(buf), "%d", refresh_pages_);
        draw_row(6, "Refresh:", buf);
    }

    }  // РєРѕРЅРµС† РІРєР»Р°РґРєРё В«Р§С‚РµРЅРёРµВ»

    // в”Ђв”Ђ Р’РєР»Р°РґРєР° В«РЎРёСЃС‚РµРјР°В» в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ
    if (active_tab_ == Tab::System) {
        char buf[16];
        const bool have = clock_hh_ >= 0;
        if (have) std::snprintf(buf, sizeof(buf), "%02d", clock_hh_);
        else      std::snprintf(buf, sizeof(buf), "--:--");
        // Р§Р°СЃС‹ Рё РјРёРЅСѓС‚С‹ СЂР°Р·РґРµР»РµРЅС‹ РЅР° РґРІРµ СЃС‚СЂРѕРєРё: С‚Р°Рє С€Р°Рі РІ 1 С‡Р°СЃ РЅРµ С‚СЂРµР±СѓРµС‚
        // РґРІРµРЅР°РґС†Р°С‚Рё РЅР°Р¶Р°С‚РёР№, Р° Р·РЅР°С‡РµРЅРёРµ РІРёРґРЅРѕ С†РµР»РёРєРѕРј Рё РїРёС€РµС‚СЃСЏ РІ RTC СЃСЂР°Р·Сѓ.
        draw_row(0, "Hours:", have ? buf : "--");
        if (have) std::snprintf(buf, sizeof(buf), "%02d", clock_mm_);
        else      std::snprintf(buf, sizeof(buf), "--");
        draw_row(1, "Mins:", have ? buf : "--");
        const int now_base = font_->baseline_for_center(kRowY[2], kBtnH);
        font_->draw_text(fb, fb_w, fb_h, kLabelCol, now_base,
                         kClockNowLabel, label_color);
        if (have) {
            std::snprintf(buf, sizeof(buf), "%02d:%02d", clock_hh_, clock_mm_);
            font_->draw_text(fb, fb_w, fb_h, controls_col + kBtnW + 4,
                             now_base, buf, text_color);
        } else {
            font_->draw_text(fb, fb_w, fb_h, controls_col + kBtnW + 4,
                             now_base, kClockNoClock, label_color);
        }

        // Row 3: Auto-off timeout. 0 renders as "off" like refresh_pages.
        {
            if (auto_off_min_ > 0) std::snprintf(buf, sizeof(buf), "%d", auto_off_min_);
            else                  std::snprintf(buf, sizeof(buf), "off");
            draw_row(3, "Auto-off:", buf);
        }
    }

    // Back button
    int back_w = 200;
    int back_x = (fb_w - back_w) / 2;
    draw_icon_button(fb, fb_w, fb_h, back_x, kBackRow, back_w, kBtnH + 8,
                     kIconTurnBack, night_mode_ ? kDark2 : kRed, kWhite, night_mode_);
}

void SettingsScene::on_mouse_down(int x, int y, int) {
    int fb_w = Platform::instance()->width();
    int controls_col = fb_w - 300;
    int plus_x = fb_w - kRightMargin - kBtnW;

    // РџРѕР»РѕСЃР° РІРєР»Р°РґРѕРє: С‚Р°Рї РїРµСЂРµРєР»СЋС‡Р°РµС‚ СЃРѕРґРµСЂР¶РёРјРѕРµ, РЅРёС‡РµРіРѕ РЅРµ РїСЂРёРјРµРЅСЏСЏ.
    if (y >= kTabY && y < kTabY + kTabH) {
        const int tw = fb_w / kTabCount;
        if (x < tw * kTabCount) {
            const int t = x / tw;
            if (t != (int)active_tab_) {
                active_tab_ = (Tab)t;
                refresh_clock();
                dirty_ = true;
            }
        }
        return;
    }

    // Back button
    int back_w = 200;
    int back_x = (fb_w - back_w) / 2;
    if (y >= kBackRow && y < kBackRow + kBtnH + 8 && x >= back_x && x < back_x + back_w) {
        go_back();
        return;
    }

    // в”Ђв”Ђ Р’РєР»Р°РґРєР° В«РЎРёСЃС‚РµРјР°В»: С‡Р°СЃС‹ Рё РјРёРЅСѓС‚С‹ в”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђв”Ђ
    if (active_tab_ == Tab::System) {
        for (int row = 0; row < 2; ++row) {
            if (y < kRowY[row] || y >= kRowY[row] + kBtnH) continue;
            if (x >= controls_col && x < controls_col + kBtnW) {
                bump_clock(row == 0, -1);
                return;
            }
            if (x >= plus_x && x < plus_x + kBtnW) {
                bump_clock(row == 0, +1);
                return;
            }
        }
        // Row 3: auto-off timeout. Not a clock row, so it is a plain setting:
        // it does not touch the reader, no apply() needed.
        if (y >= kRowY[3] && y < kRowY[3] + kBtnH) {
            int lo = 0, hi = 0;
            if (!cfg::range_int("auto_off_min", &lo, &hi)) return;
            if (x >= controls_col && x < controls_col + kBtnW) {
                auto_off_min_ = std::max(lo, auto_off_min_ - 1);
                dirty_ = true;
                return;
            }
            if (x >= plus_x && x < plus_x + kBtnW) {
                auto_off_min_ = std::min(hi, auto_off_min_ + 1);
                dirty_ = true;
                return;
            }
        }
        return;
    }

    // Font/Size/LineSpacing/ParaSpacing/ParaIndent buttons (rows 0-4)
    for (int ri = 0; ri < 5; ++ri) {
        if (y < kRowY[ri] || y >= kRowY[ri] + kBtnH) continue;

        std::string val;
        if (ri == 0) {
            val = (font_index_ >= 0 && font_index_ < (int)font_files_.size())
                  ? font_files_[font_index_] : "<none>";
        } else if (ri == 1) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", font_size_);
            val = buf;
        } else if (ri == 2) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%.2f", line_spacing_);
            val = buf;
        } else if (ri == 3) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%.2f", para_spacing_);
            val = buf;
        } else if (ri == 4) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%d", para_indent_);
            val = buf;
        }
        int tw = (int)font_->text_width(val);

        // [<] button
        if (x >= controls_col && x < controls_col + kBtnW) {
            int ilo = 0, ihi = 0;
            float flo = 0, fhi = 0;
            if (ri == 0 && (int)font_files_.size() > 1)
                font_index_ = (font_index_ - 1 + (int)font_files_.size()) % (int)font_files_.size();
            else if (ri == 1 && cfg::range_int("font_size", &ilo, &ihi))
                font_size_ = std::max(ilo, font_size_ - 1);
            else if (ri == 2 && cfg::range_float("line_spacing", &flo, &fhi))
                line_spacing_ = std::max(flo, line_spacing_ - 0.05f);
            else if (ri == 3 && cfg::range_float("para_spacing", &flo, &fhi))
                para_spacing_ = std::max(flo, para_spacing_ - 0.25f);
            else if (ri == 4 && cfg::range_int("para_indent", &ilo, &ihi))
                para_indent_ = std::max(ilo, para_indent_ - 5);
            apply(ri);
            return;
        }

        // [>] button
        if (x >= plus_x && x < plus_x + kBtnW) {
            int ilo = 0, ihi = 0;
            float flo = 0, fhi = 0;
            if (ri == 0 && (int)font_files_.size() > 1)
                font_index_ = (font_index_ + 1) % (int)font_files_.size();
            else if (ri == 1 && cfg::range_int("font_size", &ilo, &ihi))
                font_size_ = std::min(ihi, font_size_ + 1);
            else if (ri == 2 && cfg::range_float("line_spacing", &flo, &fhi))
                line_spacing_ = std::min(fhi, line_spacing_ + 0.05f);
            else if (ri == 3 && cfg::range_float("para_spacing", &flo, &fhi))
                para_spacing_ = std::min(fhi, para_spacing_ + 0.25f);
            else if (ri == 4 && cfg::range_int("para_indent", &ilo, &ihi))
                para_indent_ = std::min(ihi, para_indent_ + 5);
            apply(ri);
            return;
        }
    }

    // Row 5: Night mode toggle
    if (y >= kRowY[5] && y < kRowY[5] + kBtnH) {
        if (x >= plus_x && x < plus_x + kBtnW) {
            night_mode_ = !night_mode_;
            dirty_ = true;
            return;
        }
    }

    // Row 6: Refresh interval (pages between full-quality redraws)
    if (y >= kRowY[6] && y < kRowY[6] + kBtnH) {
        int lo = 0, hi = 0;
        if (!cfg::range_int("refresh_pages", &lo, &hi)) return;
        if (x >= controls_col && x < controls_col + kBtnW) {
            refresh_pages_ = std::max(lo, refresh_pages_ - 1);
            apply(6);
            return;
        }
        if (x >= plus_x && x < plus_x + kBtnW) {
            refresh_pages_ = std::min(hi, refresh_pages_ + 1);
            apply(6);
            return;
        }
    }
}

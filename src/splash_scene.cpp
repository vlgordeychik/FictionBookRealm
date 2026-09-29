#include "splash_scene.h"
#include "scene_manager.h"
#include "font_renderer.h"
#include "config.h"
#include "layout_cache.h"
#include "history.h"
#include "filesys/filesystem.h"
#include "bookshelf_scene.h"
#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include <cstdio>
#include <cstring>

// ─── Экран загрузки ─────────────────────────────────────────────
// До цикла сцен: рисуется прямо в app_main, пока идёт тяжёлое декодирование
// логотипа выключения. Раньше на этом месте стоял забытый стресс-тест
// шрифта, рисовавший три строки «Обложка Библиотека Bookshelf Книга».
void draw_loading_screen(uint8_t* fb, int fb_w, int fb_h, FontRenderer* font) {
    fb_fill(fb, fb_w, fb_h, kWhite);

    if (!font) return;
    static const char kText[] = "\xD0\x97\xD0\x90\xD0\x93\xD0\xA0"
                                "\xD0\xA3\xD0\x97\xD0\x9A\xD0\x90";  // ЗАГРУЗКА
    // Базовая линия, а не верх строки: строка занимает lh, базовая линия
    // на Y + lh - 4 — то же соглашение, что в строках полки и оглавления.
    const int lh = font->line_height();
    const int x = fb_w / 2 - font->text_width(kText) / 2;
    const int y = fb_h / 2 + lh / 2 - 4;
    font->draw_text(fb, fb_w, fb_h, x, y, kText, kMidDark);
}

uint32_t loading_hold_ms(uint32_t drawn_at_ms, uint32_t now_ms) {
    // Счётчик 32-битный и перескакивает через ноль, поэтому разность
    // беззнаковая: «прошло» всегда корректно даже на переходе минуты.
    const uint32_t elapsed = now_ms - drawn_at_ms;
    return elapsed >= kLoadingMinMs ? 0u : (kLoadingMinMs - elapsed);
}

void SplashScene::render(uint8_t* fb, int fb_w, int fb_h) {
    draw_loading_screen(fb, fb_w, fb_h, font_);
}

void SplashScene::on_mouse_down(int, int, int) {
    mgr_->go_back();
}

void SplashScene::on_key_down(int) {
    mgr_->go_back();
}

// ─── NoFsScene: shown over the splash when no filesystem is available ──

void NoFsScene::render(uint8_t* fb, int fb_w, int fb_h) {
    fb_fill(fb, fb_w, fb_h, kDark1);

    int cx = fb_w / 2;
    int cy = fb_h / 2;

    const char* title = "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f "
                        "\xd1\x81\xd0\xb8\xd1\x81\xd1\x82\xd0\xb5\xd0\xbc\xd0\xb0 "
                        "\xd0\xbd\xd0\xb5\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xbd\xd0\xb0";
    int tw = font_->text_width(title);
    font_->draw_text(fb, fb_w, fb_h, cx - tw / 2, cy - 30, title, kOffWhite);

#ifdef _WIN32
    const char* sub = "\x46\x41\x54-\xd0\xbe\xd0\xb1\xd1\x80\xd0\xb0\xd0\xb7 "
                      "\xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd";
#else
    const char* sub = "\x53\x44-\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb0 "
                      "\xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb0";
#endif
    tw = font_->text_width(sub);
    font_->draw_text(fb, fb_w, fb_h, cx - tw / 2, cy, sub, kGray);

    const char* hint = last_failed_
        ? "\xd0\x9d\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c, "
          "\xd0\xbf\xd0\xbe\xd0\xbf\xd1\x80\xd0\xbe\xd0\xb1\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5 "
          "\xd0\xb5\xd1\x89\xd0\xb5 \xd1\x80\xd0\xb0\xd0\xb7"
        : "\xd0\x9d\xd0\xb0\xd0\xb6\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb5, \xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b "
          "\xd0\xbf\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd1\x8c "
          "\xd0\xbf\xd0\xbe\xd0\xb4\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5";
    tw = font_->text_width(hint);
    font_->draw_text(fb, fb_w, fb_h, cx - tw / 2, cy + 40, hint, kLight2);

    for (int i = 0; i < 8; ++i) {
        fb_fill_rect(fb, fb_w, cx - 160 + i * 40, cy + 100,
                     cx - 128 + i * 40, cy + 140, (uint8_t)(i * 2));
    }
}

void NoFsScene::on_mouse_down(int, int, int) {
    retry_mount();
}

void NoFsScene::on_key_down(int) {
    retry_mount();
}

void NoFsScene::retry_mount() {
    uint32_t now = Platform::instance()->tick_ms();
    if (now - last_click_ms_ < 500)
        return;
    last_click_ms_ = now;

    fs::FileSystem* new_fs = fs::FileSystem::mount_any();
    if (!new_fs) {
        last_failed_ = true;
        return;
    }

    cfg::init(new_fs);
    LayoutCache::set_fs(new_fs);
    History::set_fs(new_fs);
    mgr_->set_fs(new_fs);

    FontRenderer* ui_font = font_;
    mgr_->pop(); // NoFsScene
    mgr_->pop(); // SplashScene
    mgr_->push(new BookshelfScene(ui_font, new_fs));
}

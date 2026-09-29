#include "ui_overlay.h"

#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include "font_renderer.h"

namespace {

FontRenderer* g_overlay_ui_font = nullptr;

void draw_centered_text(uint8_t* fb, int fb_w, int fb_h,
                        FontRenderer* f, int y, const char* s, uint8_t color) {
    if (!f || !s || !*s) return;
    int w = f->text_width(s);
    int x = (fb_w - w) / 2;
    if (x < 8) x = 8;
    f->draw_text(fb, fb_w, fb_h, x, y, s, color);
}

} // namespace

void ui_overlay_set_font(FontRenderer* ui_font) {
    g_overlay_ui_font = ui_font;
}

void show_error_overlay(const char* title, const char* detail) {
    Platform* p = Platform::instance();
    if (!p) return;
    uint8_t* fb = p->framebuffer();
    if (!fb) return;
    int w = p->width();
    int h = p->height();

    FontRenderer* f = g_overlay_ui_font;
    const int line_h = f ? f->line_height() : 30;
    const int pad = 24;
    const int lines = (detail && *detail) ? 2 : 1;
    const int box_h = pad * 2 + line_h * (lines + 1); // + hint line
    const int box_w = w * 3 / 4;
    const int box_x = (w - box_w) / 2;
    const int box_y = (h - box_h) / 2;

    // Dim everything behind, then draw opaque box
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fb[y * w + x] = fb_dim(fb[y * w + x], 6);

    fb_fill_rect(fb, w, box_x, box_y, box_x + box_w - 1, box_y + box_h - 1, kCream);
    fb_fill_rect(fb, w, box_x, box_y, box_x + box_w - 1, box_y, kBlack);
    fb_fill_rect(fb, w, box_x, box_y + box_h - 1, box_x + box_w - 1, box_y + box_h - 1, kBlack);
    fb_fill_rect(fb, w, box_x, box_y, box_x, box_y + box_h - 1, kBlack);
    fb_fill_rect(fb, w, box_x + box_w - 1, box_y, box_x + box_w - 1, box_y + box_h - 1, kBlack);

    int ty = box_y + pad;
    draw_centered_text(fb, w, h, f, ty, title, kBlack);
    ty += line_h + 4;
    if (detail && *detail) {
        draw_centered_text(fb, w, h, f, ty, detail, kMidDark);
        ty += line_h + 4;
    }
    const char* hint = "\xd0\x9a\xd0\xbe\xd1\x81\xd0\xbd\xd0\xb8\xd1\x82\xd0\xb5\xd1\x81\xd1\x8c, \xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b \xd0\xb7\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c";
    draw_centered_text(fb, w, h, f, box_y + box_h - pad - line_h, hint, kGray);

    // Full quality present — also wipes residual ghosts
    p->force_quality_next_present();
    p->present();
    p->wait_display();

    // Wait for touch (device has no keyboard). Debounce: require release
    // then a fresh press so the tap that opened us doesn't instantly dismiss.
    bool saw_release = false;
    uint32_t start = p->tick_ms();
    while (true) {
        int tx, ty2;
        if (p->get_touch(tx, ty2)) {
            if (saw_release) break;
        } else {
            saw_release = true;
        }
        // Safety: auto-dismiss after 30s so the UI can never hard-lock
        if (p->tick_ms() - start > 30000) break;
        p->delay_ms(40);
    }

    // Drain any residual swipe from the dismiss tap
    int dx, dy, sx, sy;
    while (p->get_swipe(dx, dy, sx, sy)) {}

    p->wait_display();
}

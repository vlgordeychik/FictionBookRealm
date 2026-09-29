#include "book_scene.h"
#include "gestures.h"
#include "toc_scene.h"
#include "power_off_scene.h"
#include "scene_manager.h"
#include "font_renderer.h"
#include "config.h"
#include "cover_loader.h"
#include "history.h"
#include "fb2/fb2_document.h"
#include "filesys/filesystem.h"
#include "fb2zip.h"
#include "platform.h"
#include "palette.h"
#include "fb_helpers.h"
#include "ui_overlay.h"
#include "cover_loader.h"
#include "image_decoder.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(...) ((void)0)
#endif
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>

#ifndef _WIN32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#else
#include <thread>
#include <atomic>
#endif

namespace {
#ifndef _WIN32
// Context for the background parse task (file scope so the timeout path
// in open() can free it after vTaskDelete).
struct TaskCtx {
    fb2::Fb2Document* doc;
    fs::FileSystem* fs;
    std::string path;        // zip source (if extract_zip) or final parse path
    std::string temp_path;   // staging file for zip
    std::string parse_path;  // path passed to open_fs/open after extract
    bool use_fs;
    bool extract_zip;        // stream zip → temp_path before open
    std::atomic<bool>* done;
    bool* result;
    std::string* err_out;    // extract/open error detail (main owns)
    std::atomic<int>* stage; // 0=extract, 1=parse (UI hint)
    // Static-task storage: dynamic xTaskCreate fails under heap pressure
    // even with free SPIRAM; we own these and free after done/timeout.
    void* stack_buf = nullptr;
    StaticTask_t* tcb = nullptr;
};
#endif
} // namespace

BookScene::BookScene(FontRenderer* font, FontRenderer* ui_font, fs::FileSystem* fs)
    : font_(font), ui_font_(ui_font), fs_(fs), doc_(new fb2::Fb2Document), view_(font, ui_font, doc_) {}

BookScene::~BookScene() {
    delete doc_;
    delete bg_doc_;
}

void BookScene::go_to_page(int page) {
    if (view_.go_to_page(page)) {
        note_page_move();
        save_state();
        if (mgr_) mgr_->request_render();
    }
}

void BookScene::note_page_move() {
    if (refresh_pages_ <= 0) return;
    ++pages_since_refresh_;
    if (pages_since_refresh_ >= refresh_pages_) {
        pages_since_refresh_ = 0;
        Platform::instance()->force_quality_next_present();
        ESP_LOGI("fbr", "note_page_move: quality present %d pages", refresh_pages_);
    }
}

std::string BookScene::annotation() const {
    if (!doc_ || !doc_->is_open()) return {};
    const std::string& a = doc_->meta().title_info.annotation;
    if (a.size() <= kAnnCap) return a;
    return a.substr(0, kAnnCap);
}

bool BookScene::cached_cover(int max_w, int max_h, PaletteImage& out) const {
    if (!doc_ || !doc_->is_open()) return false;
    return ImageDecoder::downscale_palette(view_.cached_cover(), max_w, max_h, out);
}

bool open_book(SceneManager* mgr, fs::FileSystem* fs,
               const std::string& path, const std::string& cache_key,
               int restore_page) {
    if (!mgr) return false;

    if (BookScene* existing = mgr->find_book(cache_key)) {
        ESP_LOGI("fbr", "open_book resume key=%s page=%d", cache_key.c_str(), restore_page);
        mgr->pop_to(existing);
        if (restore_page >= 0)
            existing->go_to_page(restore_page);
        return true;
    }

    mgr->close_books();

    auto* book = new BookScene(mgr->font(), mgr->ui_font(), fs);
    if (book->open(path, cache_key, restore_page)) {
        mgr->push(book);
        return true;
    }

    std::string err = book->last_error();
    delete book;
    if (!err.empty())
        show_error_overlay("Не удалось открыть книгу", err.c_str());
    else
        show_error_overlay("Не удалось открыть книгу");
    return false;
}

bool BookScene::open(const std::string& path, const std::string& source_path, int restore_page) {
    file_path_ = path;
    cache_key_ = source_path.empty() ? path : source_path;
    last_error_.clear();

    // ─── UI first: «Открытие книги…» до любой работы с zip/файлом ───
    FontRenderer* cf = ui_font_ ? ui_font_ : font_;
    int bar_x = Platform::instance()->width() / 6;
    int bar_y = Platform::instance()->height() / 2 + 20;
    int bar_w = Platform::instance()->width() * 2 / 3;
    int bar_h = 20;
    const int label_y = bar_y - 38;
    const int lh = cf ? cf->line_height() : 32;
    const int region_x = bar_x;
    const int region_y = label_y - lh - 4;
    const int region_w = bar_w + 96;
    const int region_h = (bar_y + bar_h + 8) - region_y;

    auto draw_progress = [&](const char* label, int pct) {
        uint8_t* fb = Platform::instance()->framebuffer();
        int fb_w = Platform::instance()->width();
        int fb_h = Platform::instance()->height();

        fb_fill_rect(fb, fb_w, region_x, region_y,
                     region_x + region_w - 1, region_y + region_h - 1, kWhite);
        cf->draw_text(fb, fb_w, fb_h, bar_x, label_y, label, kBlack);
        fb_fill_rect(fb, fb_w, bar_x, bar_y,
                     bar_x + bar_w - 1, bar_y + bar_h - 1, kMid);

        if (pct >= 0) {
            int fill = bar_w * pct / 100;
            if (fill > 0)
                fb_fill_rect(fb, fb_w, bar_x + 2, bar_y + 2,
                             bar_x + 2 + fill - 1, bar_y + bar_h - 3, kMidLight);
            char pct_s[16];
            std::snprintf(pct_s, sizeof(pct_s), "%d%%", pct);
            int pct_w = cf->text_width(pct_s);
            int pct_x = bar_x + bar_w + 10;
            if (pct_x + pct_w > region_x + region_w)
                pct_x = region_x + region_w - pct_w;
            cf->draw_text(fb, fb_w, fb_h, pct_x, bar_y + bar_h - 3,
                          pct_s, kBlack);
        }

        Platform::instance()->present_area(region_x, region_y, region_w, region_h);
    };

    const char* kOpenLabel =
        "\xd0\x9e\xd1\x82\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd0\xb8\xd0\xb5 \xd0\xba\xd0\xbd\xd0\xb8\xd0\xb3\xd0\xb8...";
    const char* kZipLabel =
        "\xd0\xa0\xd0\xb0\xd1\x81\xd0\xbf\xd0\xb0\xd0\xba\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0 \xd0\xb0\xd1\x80\xd1\x85\xd0\xb8\xd0\xb2\xd0\xb0...";
    draw_progress(kOpenLabel, -1);

    // ─── Phase 0: Resolve parse path ───
    // .fb2.zip streams to temp on SD (small buffers); extract+open run on
    // the bg task so this UI stays visible the whole time.
    std::string parse_path;
    std::string temp_path;
    bool use_fs = false;
    bool extract_zip = false;

    if (fb2zip::is_zip_path(path)) {
        if (!fs_) {
            printf("fbr: zip fs=null path=%s\n", path.c_str());
            last_error_ = "Файловая система недоступна";
            return false;
        }
        const char* kTemp = "__temp_book.fb2";

        // Reuse __temp_book.fb2 staged by cover load / previous open.
        // Only when temp_source matches this zip (avoids stale temp).
        bool temp_ok = false;
        if (fs_->exists(kTemp)) {
            fs::File* probe = fs_->open(kTemp);
            if (probe && *probe) {
                size_t psz = probe->size();
                delete probe;
                if (psz > 0 && psz <= fb2zip::kMaxUncompressed &&
                    cfg::get_str("temp_source") == path) {
                    temp_ok = true;
                }
            } else {
                delete probe;
            }
        }

        if (temp_ok) {
            printf("fbr: use staged temp=%s path=%s heap=%u\n",
                   kTemp, path.c_str(),
#ifdef ESP_PLATFORM
                   (unsigned)esp_get_free_heap_size()
#else
                   0u
#endif
            );
            draw_progress(kOpenLabel, -1);
            parse_path = kTemp; // open staged fb2; do not re-extract zip
            temp_path  = kTemp;
            use_fs     = true;
            extract_zip = false;
        } else {
            draw_progress(kZipLabel, -1);
            printf("fbr: zip will extract path=%s heap=%u\n",
                   path.c_str(),
#ifdef ESP_PLATFORM
                   (unsigned)esp_get_free_heap_size()
#else
                   0u
#endif
            );
            parse_path = path; // source zip; bg does extract → temp
            temp_path  = kTemp;
            use_fs     = true;
            extract_zip = true;
        }
    } else {
        FILE* probe = std::fopen(path.c_str(), "rb");
        if (probe) {
            std::fclose(probe);
            parse_path = path;
            use_fs = false;
        } else {
            parse_path = path;
            use_fs = true;
        }
    }

    // ─── Phase 1: Background extract + XML parse ───
    bg_doc_ = new fb2::Fb2Document;
    bg_done_.store(false, std::memory_order_release);
    bg_result_ = false;
    bg_stage_.store(0, std::memory_order_release);
    std::string bg_err;

    auto run_parse = [&]() {
        if (extract_zip) {
            draw_progress(kZipLabel, -1);
            if (!fb2zip::extract_fb2_to_file(fs_, path, temp_path.c_str())) {
                const char* ze = fb2zip::last_error();
                bg_err = "Не удалось распаковать .fb2.zip";
                if (ze && *ze) {
                    bg_err += "\n";
                    bg_err += ze;
                }
                bg_result_ = false;
                return;
            }
            printf("fbr: zip staged temp=%s heap=%u\n", temp_path.c_str(),
#ifdef ESP_PLATFORM
                   (unsigned)esp_get_free_heap_size()
#else
                   0u
#endif
            );
            bg_stage_.store(1, std::memory_order_release);
            draw_progress(kOpenLabel, -1);
            // nullptr: keep __temp_book.fb2 after close (cover/reopen reuse).
            bg_result_ = bg_doc_->open_fs(fs_, temp_path.c_str(), nullptr);
        } else if (use_fs) {
            bg_stage_.store(1, std::memory_order_release);
            bg_result_ = bg_doc_->open_fs(fs_, parse_path.c_str(), nullptr);
        } else {
            bg_stage_.store(1, std::memory_order_release);
            bg_result_ = bg_doc_->open(parse_path.c_str());
        }
    };

    bool task_created = false;
#ifndef _WIN32
    auto* ctx = new TaskCtx{bg_doc_, fs_, path, temp_path, parse_path,
                            use_fs, extract_zip, &bg_done_, &bg_result_,
                            &bg_err, &bg_stage_, nullptr, nullptr};
    bg_ctx_ = ctx;

    auto parse_entry = [](void* arg) {
        auto* c = static_cast<TaskCtx*>(arg);
        if (c->extract_zip) {
            if (!fb2zip::extract_fb2_to_file(c->fs, c->path, c->temp_path.c_str())) {
                const char* ze = fb2zip::last_error();
                *c->err_out = "Не удалось распаковать .fb2.zip";
                if (ze && *ze) {
                    *c->err_out += "\n";
                    *c->err_out += ze;
                }
                *c->result = false;
                printf("fbr: zip extract FAIL path=%s err=%s\n",
                       c->path.c_str(), ze ? ze : "?");
                c->done->store(true, std::memory_order_release);
                vTaskDelete(nullptr);
                return;
            }
            printf("fbr: zip staged temp=%s heap=%u\n",
                   c->temp_path.c_str(), (unsigned)esp_get_free_heap_size());
            c->stage->store(1, std::memory_order_release);
            // nullptr: keep temp after close for shelf cover / reopen.
            *c->result = c->doc->open_fs(c->fs, c->temp_path.c_str(),
                                         nullptr);
        } else if (c->use_fs) {
            c->stage->store(1, std::memory_order_release);
            *c->result = c->doc->open_fs(c->fs, c->parse_path.c_str(), nullptr);
        } else {
            c->stage->store(1, std::memory_order_release);
            *c->result = c->doc->open(c->parse_path.c_str());
        }
        if (!*c->result && c->err_out && c->err_out->empty()) {
            const char* de = c->doc->last_error();
            if (de && *de) *c->err_out = de;
        }
        printf("fbr: fb2_parse done result=%d hwm=%u heap=%u\n",
               *c->result, (unsigned)uxTaskGetStackHighWaterMark(nullptr),
               (unsigned)esp_get_free_heap_size());
        c->done->store(true, std::memory_order_release);
        vTaskDelete(nullptr);
    };

    // Prefer SPIRAM for the stack: xTaskCreate needs a contiguous block and
    // often fails even when free heap looks large (internal fragmentation).
    constexpr uint32_t kParseStackBytes = 49152;
    void* stack = heap_caps_malloc(kParseStackBytes,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack)
        stack = heap_caps_malloc(kParseStackBytes, MALLOC_CAP_8BIT);
    StaticTask_t* tcb = static_cast<StaticTask_t*>(
        heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!tcb)
        tcb = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_8BIT));

    if (stack && tcb) {
        ctx->stack_buf = stack;
        ctx->tcb = tcb;
        bg_task_ = xTaskCreateStaticPinnedToCore(
            parse_entry, "fb2_parse", kParseStackBytes, ctx, 3,
            static_cast<StackType_t*>(stack), tcb, 1);
        task_created = (bg_task_ != nullptr);
        if (!task_created) {
            printf("fbr: fb2_parse static create FAIL stack=%p tcb=%p heap=%u\n",
                   stack, tcb, (unsigned)esp_get_free_heap_size());
        }
    } else {
        printf("fbr: fb2_parse stack/tcb alloc FAIL stack=%p tcb=%p heap=%u\n",
               stack, tcb, (unsigned)esp_get_free_heap_size());
    }

    if (!task_created) {
        // Last resort: dynamic create (may fail under pressure).
        BaseType_t rc = xTaskCreatePinnedToCore(
            parse_entry, "fb2_parse", kParseStackBytes, ctx, 3,
            reinterpret_cast<TaskHandle_t*>(&bg_task_), 1);
        task_created = (rc == pdPASS);
        if (task_created) {
            heap_caps_free(stack);
            heap_caps_free(tcb);
            ctx->stack_buf = nullptr;
            ctx->tcb = nullptr;
        } else {
            printf("fbr: fb2_parse task create FAILED rc=%d, sync fallback\n",
                   (int)rc);
            heap_caps_free(stack);
            heap_caps_free(tcb);
            ctx->stack_buf = nullptr;
            ctx->tcb = nullptr;
            delete ctx;
            bg_ctx_ = nullptr;
            bg_task_ = nullptr;
        }
    }
#else
    std::thread([this, path, parse_path, temp_path, use_fs, extract_zip, &bg_err]() {
        if (extract_zip) {
            if (!fb2zip::extract_fb2_to_file(fs_, path, temp_path.c_str())) {
                const char* ze = fb2zip::last_error();
                bg_err = "Не удалось распаковать .fb2.zip";
                if (ze && *ze) {
                    bg_err += "\n";
                    bg_err += ze;
                }
                bg_result_ = false;
                bg_done_.store(true, std::memory_order_release);
                return;
            }
            bg_stage_.store(1, std::memory_order_release);
            bg_result_ = bg_doc_->open_fs(fs_, temp_path.c_str(), nullptr);
        } else if (use_fs) {
            bg_stage_.store(1, std::memory_order_release);
            bg_result_ = bg_doc_->open_fs(fs_, parse_path.c_str(), nullptr);
        } else {
            bg_stage_.store(1, std::memory_order_release);
            bg_result_ = bg_doc_->open(parse_path.c_str());
        }
        if (!bg_result_ && bg_err.empty()) {
            const char* de = bg_doc_->last_error();
            if (de && *de) bg_err = de;
        }
        bg_done_.store(true, std::memory_order_release);
    }).detach();
    task_created = true;
#endif

    if (!task_created) {
        run_parse();
        bg_done_.store(true, std::memory_order_release);
    }

    if (extract_zip)
        draw_progress(kZipLabel, -1);
    else
        draw_progress(kOpenLabel, -1);

    constexpr uint32_t kProgThrottleMs = 2000;
    constexpr uint32_t kParseTimeoutMs = 120000;
    uint32_t wait_start_ms = Platform::instance()->tick_ms();
    int last_stage = -1;
    while (!bg_done_.load(std::memory_order_acquire)) {
        int st = bg_stage_.load(std::memory_order_acquire);
        if (st != last_stage) {
            last_stage = st;
            draw_progress(st == 0 && extract_zip ? kZipLabel : kOpenLabel, -1);
        }
        uint32_t now = Platform::instance()->tick_ms();
        if (now - wait_start_ms > kParseTimeoutMs) {
            last_error_ = "Таймаут открытия книги";
#ifndef _WIN32
            if (bg_task_) {
                vTaskDelete(static_cast<TaskHandle_t>(bg_task_));
                bg_task_ = nullptr;
            }
            if (bg_ctx_) {
                auto* c = static_cast<TaskCtx*>(bg_ctx_);
                if (c->stack_buf) heap_caps_free(c->stack_buf);
                if (c->tcb) heap_caps_free(c->tcb);
                delete c;
                bg_ctx_ = nullptr;
            }
#endif
            delete bg_doc_;
            bg_doc_ = nullptr;
            last_error_ = "Таймаут открытия книги";
            return false;
        }
        Platform::instance()->delay_ms(50);
    }

    if (bg_task_) {
#ifndef _WIN32
        bg_task_ = nullptr;
#endif
    }
#ifndef _WIN32
    // Единый владелец TaskCtx — main-сторона освобождает после done/timeout.
    if (bg_ctx_) {
        auto* c = static_cast<TaskCtx*>(bg_ctx_);
        if (c->stack_buf) heap_caps_free(c->stack_buf);
        if (c->tcb) heap_caps_free(c->tcb);
        delete c;
        bg_ctx_ = nullptr;
    }
#else
    bg_ctx_ = nullptr;
#endif

    if (!bg_result_) {
        if (bg_err.empty() && bg_doc_) {
            const char* de = bg_doc_->last_error();
            if (de && *de) bg_err = de;
        }
        delete bg_doc_;
        bg_doc_ = nullptr;
        last_error_ = bg_err.empty() ? "Файл повреждён или не является FB2" : bg_err;
        return false;
    }

    // Staged zip temp is valid for this source — record for reopen/cover.
    if (extract_zip)
        cfg::set_str("temp_source", path.c_str());

    // ─── Phase 2: Transfer ownership (main thread, noexcept) ───
    *doc_ = std::move(*bg_doc_);
    delete bg_doc_;
    bg_doc_ = nullptr;

    // ─── Phase 3: Layout ───
    view_.set_size(Platform::instance()->width(), Platform::instance()->height());
    SettingsSnapshot cur = snapshot_from_ini();
    apply_settings(cur);
    settings_at_open_ = cur;

    // Decode the cover BEFORE load_cache: the cache drops heap to ~0.5MB,
    // and read_binary of the ~1.66MB base64 cover then fails the OOM guard
    // → white page 0. Do it now while ~3.8MB is free.
    view_.predecode_cover();

    if (!view_.load_cache(cache_key_)) {
        const char* kLayoutLabel =
            "\xd0\xa0\xd0\xb0\xd0\xb7\xd0\xb1\xd0\xb8\xd0\xb2\xd0\xba\xd0\xb0 \xd0\xbd\xd0\xb0 \xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x86\xd1\x8b...";
        int last_pct = -20;
        uint32_t last_draw_ms = 0;
        view_.layout([&](int pct) {
            // ≥10% step AND ≥2s between partial refreshes (E-ink-friendly)
            if (pct - last_pct < 10 && pct < 100)
                return;
            uint32_t now = Platform::instance()->tick_ms();
            if (pct < 100 && last_draw_ms != 0 && now - last_draw_ms < kProgThrottleMs)
                return;
            last_pct = pct;
            last_draw_ms = now;
            draw_progress(kLayoutLabel, pct);
        });
        view_.save_cache(cache_key_);
    }

    int page = 0;
    if (restore_page >= 0) {
        page = restore_page;
    } else {
        HistoryEntry he;
        if (History::find(cache_key_, he))
            page = he.current_page; // книга открывалась ранее — с места остановки
        // иначе page = 0: книга открывается первый раз
    }
    view_.go_to_page(page);

    cfg::set_str("last_file", cache_key_.c_str());
    cfg::set_str("last_cache_key", cache_key_.c_str());
    cfg::set_int("last_page", page);

    // Save metadata for bookshelf
    const auto& ti = doc_->meta().title_info;
    if (!ti.book_title.empty())
        cfg::set_str("last_title", ti.book_title.c_str());
    if (!ti.authors.empty()) {
        const auto& a = ti.authors[0];
        std::string an;
        if (!a.first_name.empty() && !a.last_name.empty())
            an = a.first_name + " " + a.last_name;
        else if (!a.last_name.empty())
            an = a.last_name;
        else if (!a.nickname.empty())
            an = a.nickname;
        if (!an.empty())
            cfg::set_str("last_author", an.c_str());
    }
    cfg::set_int("last_total_pages", view_.total_pages());

    // Record in history
    HistoryEntry he;
    he.title = cfg::get_str("last_title");
    he.author = cfg::get_str("last_author");
    he.file_path = cache_key_;
    he.cache_key = cache_key_;
    he.current_page = page;
    he.total_pages = view_.total_pages();
    History::add(he);

    // Progress partials used epd_text — force the first full present of the
    // reader page into epd_quality so ghosts of browser/progress are wiped.
    Platform::instance()->wait_display();
    Platform::instance()->force_quality_next_present();
    pages_since_refresh_ = 0;

    return true;
}

void BookScene::update() {
    // Background task completion is handled in open() via polling loop.
    // No additional update logic needed here.
}

void BookScene::on_enter() {
    SettingsSnapshot cur = snapshot_from_ini();

    if (view_.total_pages() == 0 || cur != settings_at_open_) {
        apply_settings(cur);
        view_.layout();
        view_.save_cache(cache_key_);
        settings_at_open_ = cur;
    }

    cfg::set_str("last_file", cache_key_.c_str());
    cfg::set_int("last_page", view_.current_page());
    cfg::set_int("last_total_pages", view_.total_pages());
}

void BookScene::on_exit() {
    // Книга читается из резидентного буфера (Fb2Document::open_memory) —
    // временный файл не используется, очищать нечего.
}

void BookScene::open_user_menu() {
    save_state();
}

void BookScene::save_state() {
    if (cache_key_.empty()) return;
    cfg::set_int("last_page", view_.current_page());

    History::update_page(cache_key_, view_.current_page());

    int cur_block = view_.current_page_block();
    section_block_ = -1;
    if (cur_block >= 0) {
        const auto& flat = doc_->index().toc_flat;
        for (int i = (int)flat.size() - 1; i >= 0; --i) {
            if (flat[i]->first_block <= (size_t)cur_block) {
                section_block_ = (int)flat[i]->first_block;
                break;
            }
        }
    }
}

BookScene::SettingsSnapshot BookScene::snapshot_from_ini() const {
    SettingsSnapshot s;
    s.font_size = cfg::get_int("font_size");
    s.font_file = cfg::get_str("font_file");
    s.line_spacing = cfg::get_float("line_spacing");
    s.para_spacing = cfg::get_float("para_spacing");
    s.para_indent = cfg::get_int("para_indent");
    s.night_mode = cfg::get_int("night_mode") != 0;
    s.refresh_pages = cfg::get_int("refresh_pages");
    return s;
}

bool BookScene::reload_reader_font(int font_size, const std::string& font_file) {
    if (!fs_) return false;
    std::string name = font_file.empty() ? "arial.ttf" : font_file;
    std::string path = std::string(".fonts/") + name;
    fs::File* f = fs_->open(path.c_str());
    if (!f && name != "arial.ttf") {
        f = fs_->open(".fonts/arial.ttf");
        if (!f) f = fs_->open(".fonts/Arial.ttf");
    }
    if (!f) return false;

    size_t sz = f->size();
    if (sz == 0) { delete f; return false; }
    std::unique_ptr<unsigned char[]> buf(new (std::nothrow) unsigned char[sz]);
    if (!buf) { delete f; return false; }
    size_t nread = f->read(buf.get(), sz);
    delete f;
    if (nread != sz) return false;

    return font_->load_font_from_memory(buf.get(), sz, name.c_str(), font_size);
}

void BookScene::apply_settings(const SettingsSnapshot& s) {
    // Size-only: rescale via the already-loaded face (SettingsScene may
    // have done it). Full reload only when the font file itself changed.
    ESP_LOGI("fbr", "apply_settings size=%d file=%s raster_size=%d raster_file=%s pix=%d",
             s.font_size, s.font_file.c_str(),
             raster_font_size_, raster_font_file_.c_str(),
             font_ ? font_->pixel_size() : -1);
    if (s.font_file != raster_font_file_) {
        if (reload_reader_font(s.font_size, s.font_file)) {
            raster_font_size_ = s.font_size;
            raster_font_file_ = s.font_file;
        }
    } else if (font_ && font_->is_loaded() && s.font_size != font_->pixel_size()) {
        if (font_->set_pixel_size(s.font_size))
            raster_font_size_ = s.font_size;
    }

    view_.set_font_size(s.font_size);
    // Ключ кэша раскладки включает реально загруженный файл шрифта: метрики
    // определяют точки переноса и выключку, поэтому смена файла обязана
    // пересчитывать раскладку даже при прочих равных параметрах.
    view_.set_font_file(!raster_font_file_.empty() ? raster_font_file_
                        : (s.font_file.empty() ? std::string("arial.ttf") : s.font_file));
    view_.set_layout_params(s.line_spacing, s.para_spacing, s.para_indent);
    view_.set_night_mode(s.night_mode);
    if (refresh_pages_ != s.refresh_pages) {
        refresh_pages_ = s.refresh_pages;
        pages_since_refresh_ = 0;
    }
}

void BookScene::render(uint8_t* fb, int fb_w, int fb_h) {
    if (doc_->is_open())
        view_.render(fb, fb_w, fb_h);
}

void BookScene::on_mouse_down(int x, int y, int button) {
    if (button == 2) {
        save_state();
        go_back();
        return;
    }

    if (view_.has_footnote_click(x, y)) {
        note_page_move();
        save_state();
        // Middle-third taps don't get an automatic present (double-tap pause
        // window) — footnote navigation must request the redraw itself.
        if (mgr_) mgr_->request_render();
        return;
    }

    int action = view_.hit_test(x, y);
#ifdef FBR_WIN32
    if (action == 3) { mgr_->push(new PowerOffScene); }
    else if (action == -1) { if (view_.prev_page()) { note_page_move(); save_state(); } }
    else if (action == 1) { if (view_.next_page()) { note_page_move(); save_state(); } }
    else if (action == 2) { open_toc(); }
#else
    if (action == -1) { if (view_.prev_page()) { note_page_move(); save_state(); } }
    else if (action == 1) { if (view_.next_page()) { note_page_move(); save_state(); } }
    else if (action == 2) { open_toc(); }
#endif
}

void BookScene::on_key_down(int key) {
    if (key == 37 || key == 38) { // LEFT / UP
        if (view_.prev_page()) { note_page_move(); save_state(); }
    } else if (key == 39 || key == 40) { // RIGHT / DOWN
        if (view_.next_page()) { note_page_move(); save_state(); }
    } else if (key == 27) { // ESC
        save_state(); go_back();
    }
}

bool BookScene::on_swipe(int dx, int dy, int sx, int sy) {
    // Зона берётся по точке касания, а не по точке отпускания: зоны
    // ридера определены нажатием, и палец обычно уезжает в сторону.
    const BookSwipe act = book_swipe_action(dx, dy, view_.hit_test(sx, sy),
                                            view_.is_footnote_at(sx, sy));
    if (act == BookSwipe::OpenToc) {
        open_toc();
        return true;
    }
    // Горизонтальный жест в книге всегда поглощается: в крайних третьях
    // нажатие уже листает, и отправлять ещё и on_key_down значило бы
    // перелистнуть по две страницы одним касанием.
    return std::abs(dx) > std::abs(dy);
}

void BookScene::open_toc() {
    if (!doc_->is_open()) return;

    auto items = TocScene::items_from_index(doc_->index().toc_root,
                                            view_.get_toc_pages(),
                                            view_.current_page_block(),
                                            view_.current_page());

    auto nav = [this](int page) {
        save_state();
        view_.go_to_page(page);
    };

    mgr_->push(new TocScene(mgr_->ui_font(), std::move(items),
                             view_.current_page(), std::move(nav)));
}

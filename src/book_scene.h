#pragma once
#include "scene.h"
#include "fb2_page_view.h"
#include <string>
#include <atomic>

class FontRenderer;
struct CoverResult;
namespace fb2 { class Fb2Document; }
namespace fs { class FileSystem; }

class BookScene : public Scene {
public:
    BookScene(FontRenderer* font, FontRenderer* ui_font, fs::FileSystem* fs);
    ~BookScene();
    const char* name() const override { return "book"; }
    bool open(const std::string& path, const std::string& cache_key = "", int restore_page = -1);
    const std::string& last_error() const { return last_error_; }
    const std::string& cache_key() const { return cache_key_; }
    void go_to_page(int page);
    // Аннотация из уже разобранного DOM книги. Не требует выделений, в
    // отличие от повторного разбора .fb2, поэтому работает и когда книга
    // открыта и куча занята раскладкой. Обрезана до kAnnCap — как и в
    // пути через load_cover_from_fb2_fs, иначе текст на полке отличался бы
    // до и после перезагрузки.
    std::string annotation() const;
    // Обложка из кэша, заполненного predecode_cover() при открытии книги.
    // Повторный декод base64 (~1.6 МБ) не нужен, поэтому работает при
    // нехватке кучи. false, если книга не открыта или обложка не
    // декодировалась.
    bool cached_cover(int max_w, int max_h, PaletteImage& out) const;
    void on_enter() override;
    void on_exit() override;
    void update() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;
    void open_user_menu();
    // Свайп вправо из средней трети открывает оглавление; остальные
    // горизонтальные жесты поглощаются, чтобы одно касание не делало
    // два действия. true — жест обработан здесь.
    bool on_swipe(int dx, int dy, int sx, int sy);
    int  current_page() const { return view_.current_page(); }
private:
    FontRenderer* font_;
    FontRenderer* ui_font_;
    fs::FileSystem* fs_;
    fb2::Fb2Document* doc_;
    Fb2PageView view_;
    std::string file_path_;
    std::string cache_key_;
    struct SettingsSnapshot {
        int         font_size = 0;
        std::string font_file;
        float       line_spacing = 1.45f;
        float       para_spacing = 1.5f;
        int         para_indent = 30;
        bool        night_mode = false;
        int         refresh_pages = 0;
        bool operator!=(const SettingsSnapshot& o) const {
            return font_size != o.font_size || font_file != o.font_file ||
                   line_spacing != o.line_spacing || para_spacing != o.para_spacing ||
                   para_indent != o.para_indent || night_mode != o.night_mode ||
                   refresh_pages != o.refresh_pages;
        }
    };
    SettingsSnapshot settings_at_open_;
    int raster_font_size_ = -1;
    std::string raster_font_file_;
    int section_block_ = -1;
    int refresh_pages_ = 0;        // epd_quality full redraw every N page turns (0=off)
    int pages_since_refresh_ = 0;
    void note_page_move();         // called after a real page change
    void open_toc();
    void save_state();
    SettingsSnapshot snapshot_from_ini() const;
    bool reload_reader_font(int font_size, const std::string& font_file);
    void apply_settings(const SettingsSnapshot& s);

    // Background parsing state
    std::atomic<bool> bg_done_{false};
    std::atomic<int> bg_stage_{0}; // 0=zip extract, 1=xml parse
    bool bg_result_ = false;
    fb2::Fb2Document* bg_doc_ = nullptr;
    void* bg_task_ = nullptr;  // TaskHandle on ESP32, unused on Win32
    void* bg_ctx_ = nullptr;   // TaskCtx*, always freed by main (single owner)
    std::string last_error_;
};

// Open a book via SceneManager: resume if same cache_key is already in the
// stack, otherwise close any open BookScene (frees index+layout ~1MB) so
// pugi build_index has enough heap, then push a new BookScene.
// Returns true if the book is open (new or resumed).
bool open_book(SceneManager* mgr, fs::FileSystem* fs,
               const std::string& path, const std::string& cache_key,
               int restore_page = -1);

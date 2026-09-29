#pragma once
#include "scene.h"
#include "fb2/fb2_index.h"
#include <string>
#include <vector>
#include <functional>

class FontRenderer;

class TocScene : public Scene {
public:
    struct Item {
        std::string title;
        int depth;
        int page;
        bool is_current;
    };
    using NavFn = std::function<void(int page)>;

    // Собирает плоский список записей в порядке обхода дерева — тем же, по
    // которому считаются страницы (count_toc/map_toc в раскладке), поэтому
    // индекс записи совпадает с её страницей. Общая точка для BookScene и
    // для проверки экрана: копия этого обхода расходилась с оригиналом.
    // cur_block — блок текущей страницы; подсветкой отмечается последняя
    // запись, чей блок уже начался, и ровно одна. cur_page — запасной путь,
    // когда блок неизвестен.
    static std::vector<Item> items_from_index(const fb2::TocEntry& root,
                                              const std::vector<int>& toc_pages,
                                              int cur_block, int cur_page);

    TocScene(FontRenderer* font,
             std::vector<Item> items, int current_page, NavFn nav);
    const char* name() const override { return "toc"; }
    void on_enter() override;
    void render(uint8_t* fb, int fb_w, int fb_h) override;
    void on_mouse_down(int x, int y, int button) override;
    void on_key_down(int key) override;

private:
    int item_h() const;
    int max_visible(int fb_h) const;

    FontRenderer* font_;
    NavFn nav_;
    std::vector<Item> items_;
    int current_page_;
    int window_h_ = 540;
    int window_w_ = 960;
    int scroll_ = 0;
    int sel_ = -1;
};

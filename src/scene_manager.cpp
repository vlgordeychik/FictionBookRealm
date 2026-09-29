#include "scene_manager.h"
#include "book_scene.h"
#include "lock_scene.h"
#include "font_renderer.h"
#include "platform.h"
#include <cstdlib>
#include <cstring>

// Окно дабл-клика pause/resume. 500ms было слишком мало: полный e-ink
// present после первого тапа занимает 500ms+ и второй тап не успевал.
// Ещё важнее: первый тап в middle-third книги вообще НЕ должен вызывать
// present — иначе refresh блокирует цикл и второй тап физически теряется.
static constexpr uint32_t kDoubleTapMs = 700;

SceneManager::~SceneManager() {
    for (auto* s : stack_) delete s;
    stack_.clear();
}

void SceneManager::start(Scene* root) {
    stack_.push_back(root);
    root->mgr_ = this;
    dirty_ = true;
    root->on_enter();
}

void SceneManager::replace(Scene* s) {
    if (stack_.empty()) { start(s); return; }
    Scene* old = stack_.back();
    stack_.back() = s;
    s->mgr_ = this;
    if (stack_.size() > 1)
        s->back_ = stack_[stack_.size() - 2];
    old->on_exit();
    delete old;
    s->on_enter();
    dirty_ = true;
    last_click_time_ = 0;
}

void SceneManager::push(Scene* s) {
    stack_.push_back(s);
    s->mgr_ = this;
    if (stack_.size() > 1)
        s->back_ = stack_[stack_.size() - 2];
    dirty_ = true;
    last_click_time_ = 0;
    s->on_enter();
}

void SceneManager::pop() {
    if (stack_.size() <= 1) {
        request_quit();
        return;
    }
    Scene* s = stack_.back();
    stack_.pop_back();
    s->on_exit();
    delete s;
    dirty_ = true;
    last_click_time_ = 0;
    if (!stack_.empty())
        stack_.back()->on_enter();
}

void SceneManager::go_back() {
    pop();
}

Scene* SceneManager::top() {
    return stack_.empty() ? nullptr : stack_.back();
}

BookScene* SceneManager::find_book(const std::string& cache_key) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (std::strcmp((*it)->name(), "book") == 0) {
            auto* b = static_cast<BookScene*>(*it);
            if (b->cache_key() == cache_key)
                return b;
        }
    }
    return nullptr;
}

void SceneManager::close_books() {
    for (size_t i = 0; i < stack_.size(); ) {
        if (std::strcmp(stack_[i]->name(), "book") == 0) {
            Scene* s = stack_[i];
            stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(i));
            for (size_t j = i; j < stack_.size(); ++j)
                stack_[j]->back_ = j > 0 ? stack_[j - 1] : nullptr;
            s->on_exit();
            delete s;
            dirty_ = true;
            last_click_time_ = 0;
        } else {
            ++i;
        }
    }
}

void SceneManager::pop_to(Scene* target) {
    if (!target) return;
    while (!stack_.empty() && stack_.back() != target) {
        Scene* s = stack_.back();
        stack_.pop_back();
        s->on_exit();
        delete s;
    }
    dirty_ = true;
    last_click_time_ = 0;
    if (!stack_.empty())
        stack_.back()->on_enter();
}

void SceneManager::update() {
    if (!stack_.empty()) stack_.back()->update();
}

void SceneManager::render(uint8_t* fb, int fb_w, int fb_h) {
    dirty_ = false;
    if (!stack_.empty()) stack_.back()->render(fb, fb_w, fb_h);
}

void SceneManager::on_mouse_down(int x, int y, int button) {
    if (stack_.empty()) return;
    Scene* top_scene = stack_.back();

    // LockScene: resume — её собственный дабл-клик; pop() выставит dirty_.
    // Один клик по паузе ничего не меняет на экране → present не нужен
    // (иначе e-ink refresh съедает второй тап окна resume).
    if (top_scene->is_lock_scene()) {
        top_scene->on_mouse_down(x, y, button);
        return;
    }

    if (button == 1) {
        uint32_t now = Platform::instance()->tick_ms();
        int w = Platform::instance()->width();
        bool middle = (x >= w / 3 && x < w * 2 / 3);
        bool book = std::strcmp(top_scene->name(), "book") == 0;

        // Дабл-клик по средней трети → pause (LockScene).
        if (last_click_time_ != 0 && now - last_click_time_ < kDoubleTapMs &&
            std::abs(x - last_click_x_) < 20 && std::abs(y - last_click_y_) < 20 &&
            middle) {
            last_click_time_ = 0;
            push(new LockScene(ui_font_));
            return;
        }

        last_click_time_ = now;
        last_click_x_ = x;
        last_click_y_ = y;

        top_scene->on_mouse_down(x, y, button);

        // Middle-third книги: не present'им по умолчанию. Одиночный тап без
        // действия (hit_test == 0) не меняет кадр; сноска/TOC сама вызовут
        // request_render(). Без этого полный refresh после 1-го тапа блокировал
        // main loop на 500ms+ и 2-й тап дабл-клика не опрашивался.
        if (book && middle)
            return;

        dirty_ = true;
        return;
    }

    dirty_ = true;
    top_scene->on_mouse_down(x, y, button);
}

void SceneManager::on_key_down(int key) {
    dirty_ = true;
    if (!stack_.empty()) stack_.back()->on_key_down(key);
}

void SceneManager::on_mouse_move(int x, int y) {
    if (!stack_.empty()) stack_.back()->on_mouse_move(x, y);
}

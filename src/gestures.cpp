#include "gestures.h"
#include "scene_manager.h"
#include "book_scene.h"
#include "bookshelf_scene.h"
#include <cstring>
#include <cstdlib>

BookSwipe book_swipe_action(int dx, int dy, int press_zone, bool on_footnote) {
    // Вертикальный жест разбирает полка, горизонтальный — ридер.
    if (std::abs(dx) <= std::abs(dy)) return BookSwipe::Nothing;
    // Влево ничего не делает: раньше свайк влево листал назад, но листать
    // свайком неудобно (то же касание уже листает в крайних третьях), и
    // одно действие на жест важнее симметрии.
    if (dx <= 0) return BookSwipe::Nothing;
    // Нажатие в крайней трети или в шапке уже что-то сделало — свайп
    // лишь поглощается. Иначе один жест листал бы по две страницы:
    // нажатие давало страницу, а свайп слал ещё on_key_down со страницей.
    if (press_zone != 0) return BookSwipe::Nothing;
    if (on_footnote) return BookSwipe::Nothing;
    return BookSwipe::OpenToc;
}

void dispatch_swipe(SceneManager& mgr, int dx, int dy, int sx, int sy,
                    FontRenderer* ui_font, fs::FileSystem* fs) {
    (void)ui_font;
    (void)fs;

    Scene* top = mgr.top();
    // RTTI в тулчейне выключен (-fno-rtti), поэтому сцена опознаётся по
    // имени — так же, как это уже сделано для свайпа вниз.
    if (top && std::strcmp(top->name(), "book") == 0) {
        BookScene* book = static_cast<BookScene*>(top);
        if (book->on_swipe(dx, dy, sx, sy)) return;
    }

    if (std::abs(dx) > std::abs(dy)) {
        // Горизонтальный свайп → перелистывание. В книге до сюда не доходит:
        // on_swipe там поглощает горизонтальный жест целиком.
        mgr.on_key_down(dx > 0 ? 39 : 37);
    } else if (dy > 0) {
        // Свайп вниз → полка. Зона намеренно не проверяется: это единственный
        // жест, которым реально пользуются, и терять его из-за попадания не
        // в треть нельзя.
        if (top && std::strcmp(top->name(), "book") == 0)
            static_cast<BookScene*>(top)->open_user_menu();
        mgr.push(new BookshelfScene(ui_font, fs));
    }
}

#pragma once
#include <string>

class FontRenderer;
class SceneManager;
namespace fs { class FileSystem; }

// Что свайп делает в книге. Решение вынесено в чистую функцию, чтобы его
// можно было проверить без платформы, сцен и документа.
enum class BookSwipe {
    OpenToc,   // свайп вправо из средней зоны → оглавление
    Nothing,   // жест поглощается, ничего не делая
};

// press_zone — результат hit_test в точке касания: 0 — средняя треть
// (нажатие там ничего не делает), ненулевое — нажатие уже совершило
// действие, и свайп не должен его дублировать. on_footnote — попал ли
// палец на сноску: переход по ней уже произошёл, открывать поверх
// оглавление нельзя.
BookSwipe book_swipe_action(int dx, int dy, int press_zone, bool on_footnote);

// Разбор жеста. sx, sy — точка, с которой он начался. Сцена, умеющая
// обработать свайп сама (сейчас это книга), забирает жест; всё остальное
// достаётся прежней логике: горизонталь → перелистывание, вниз → полка.
void dispatch_swipe(SceneManager& mgr, int dx, int dy, int sx, int sy,
                    FontRenderer* ui_font, fs::FileSystem* fs);

#include "config.h"
#include "filesys/filesystem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ───────────────────────────────────────────────────────────────────────────
// Схема. Единственный источник истины: значения по умолчанию, диапазоны и
// пояснения, которые попадают в settings.json.
// ───────────────────────────────────────────────────────────────────────────
namespace cfg {
namespace {

enum class File_ { Settings, State };
enum class Type { Str, Int, Float, Bool };

struct Item {
    File_      file;
    const char* key;
    Type       type;
    const char* def;
    float      min_v, max_v;   // для Int/Float; для Str/Bool не используются
    const char* comment;       // '\n' — новая строка комментария
};

const Item kItems[] = {
    // ── settings.json ───────────────────────────────────────────────────
    {File_::Settings, "font_file", Type::Str, "arial.ttf", 0, 0,
     "Файл шрифта из папки .fonts (например \"verdana.ttf\").\n"
     "Если такого файла нет, берётся первый найденный .ttf."},

    {File_::Settings, "font_size", Type::Int, "18", 4, 28,
     "Размер шрифта в пикселях. 4..28.\n"
     "Ниже 12 читается мелко, выше 24 на страницу влезает мало текста."},

    {File_::Settings, "line_spacing", Type::Float, "1.45", 1.0f, 2.5f,
     "Межстрочный интервал, кратный размеру шрифта. 1.0..2.5.\n"
     "1.0 — тесно, 1.45 — по умолчанию, 2.0 — свободно."},

    {File_::Settings, "para_spacing", Type::Float, "1.5", 1.0f, 4.0f,
     "Интервал между абзацами, кратный размеру шрифта. 1.0..4.0."},

    {File_::Settings, "para_indent", Type::Int, "30", 0, 100,
     "Отступ первой строки абзаца («красная строка») в пикселях. 0..100.\n"
     "0 — абзацы без отступа."},

    {File_::Settings, "night_mode", Type::Bool, "false", 0, 0,
     "Ночной режим: чёрный фон, светлый текст."},

    {File_::Settings, "refresh_pages", Type::Int, "0", 0, 50,
     "Полное обновление экрана (сбрасывает накопленные «призраки»)\n"
     "каждые N перелистываний. 0 — выключено. 5..50.\n"
     "Для долгого чтения разумно 5..15."},

    {File_::Settings, "auto_off_min", Type::Int, "10", 0, 60,
     "Автовыключение после паузы в ридере, в минутах. 0 — выключено.\n"
     "Отсчёт идёт с момента постановки на паузу и сбрасывается,\n"
     "когда чтение возобновить. Значение 0 отключает выключение."},

    // ── state.json (перезаписывается приложением, руками не править) ────
    {File_::State, "last_file", Type::Str, "", 0, 0,
     "Путь к последней открытой книге."},
    {File_::State, "last_cache_key", Type::Str, "", 0, 0,
     "Ключ файла кэша раскладки в папке .cache."},
    {File_::State, "last_page", Type::Int, "0", 0, 0,
     "Страница, на которой остановились."},
    {File_::State, "last_total_pages", Type::Int, "0", 0, 0,
     "Всего страниц в книге."},
    {File_::State, "last_title", Type::Str, "", 0, 0,
     "Название книги для полки."},
    {File_::State, "last_author", Type::Str, "", 0, 0,
     "Автор книги для полки."},
    {File_::State, "temp_source", Type::Str, "", 0, 0,
     "Исходник книги, распакованный во временный __temp_book.fb2."},
    {File_::State, "clock_minutes", Type::Int, "-1", 0, 0,
     "Запасное время: минуты от полуночи. -1 — не задано.\n"
     "Используется, только если аппаратные часы (BM8563) недоступны."},
    {File_::State, "version", Type::Str, "1.0", 0, 0,
     "Версия сборки, показывается на заставке."},
};

constexpr int kCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

const char* const kSettingsFile = "settings.json";
const char* const kStateFile    = "state.json";
const char* const kLegacyFile   = "sims3.ini";

// Пути книг достигают ~110 символов; 256 — трёхкратный запас.
constexpr int kValMax = 256;
constexpr int kBufMax = 6144;   // буфер чтения/записи

struct Slot {
    char val[kValMax];
    bool present;   // есть ли значение из файла, иначе берётся def
};

Slot  g_slots[kCount];
fs::FileSystem* g_fs = nullptr;
bool g_dirty[2] = {false, false};   // [0]=settings [1]=state
char g_buf[kBufMax];

int index_of(const char* key) {
    for (int i = 0; i < kCount; ++i)
        if (std::strcmp(kItems[i].key, key) == 0) return i;
    return -1;
}

int file_index(File_ f) { return f == File_::Settings ? 0 : 1; }

// ── Хранилище значений ────────────────────────────────────────────────────

void slot_set(int i, const char* v) {
    Slot& s = g_slots[i];
    std::strncpy(s.val, v, kValMax - 1);
    s.val[kValMax - 1] = '\0';
    if (std::strlen(v) >= (size_t)kValMax) {
        // Молча обрезать путь опасно: книга перестанет открываться.
        std::printf("cfg: ВНИМАНИЕ значение '%s' длиннее %d — обрезано\n",
                    kItems[i].key, kValMax - 1);
    }
    s.present = true;
}

const char* slot_get(int i) {
    return g_slots[i].present ? g_slots[i].val : kItems[i].def;
}

int clamp_int(int i, int v) {
    if (kItems[i].type != Type::Int) return v;
    if (kItems[i].min_v == kItems[i].max_v) return v;   // диапазон не задан
    int lo = (int)kItems[i].min_v, hi = (int)kItems[i].max_v;
    return v < lo ? lo : (v > hi ? hi : v);
}

float clamp_float(int i, float v) {
    if (kItems[i].type != Type::Float) return v;
    if (kItems[i].min_v == kItems[i].max_v) return v;
    return v < kItems[i].min_v ? kItems[i].min_v
                               : (v > kItems[i].max_v ? kItems[i].max_v : v);
}

// ── Чтение файла в буфер ─────────────────────────────────────────────────

long slurp(const char* path, char* buf, int cap) {
    if (!g_fs) return -1;
    fs::File* f = g_fs->open(path);
    if (!f) return -1;
    long size = (long)f->size();
    if (size <= 0 || size >= cap) {
        // Файл больше буфера — читаем сколько влезет, разбор переживёт обрыв.
        if (size >= cap) {
            std::printf("cfg: '%s' больше %d байт, читаю усечённо\n", path, cap - 1);
            size = cap - 1;
        } else { delete f; return -1; }
    }
    size_t got = f->read(buf, (size_t)size);
    delete f;
    buf[got] = '\0';
    return (long)got;
}

// ── Мини-разбор JSONC ────────────────────────────────────────────────────
//
// Разбирает плоский объект: "key": значение, где значение — строка, число,
// true/false или null. Комментарии // и /* */ пропускаются, висячая запятая
// допускается. Любая ошибка → разбор прерывается, недостающие ключи остаются
// со значениями по умолчанию. Функция ничего не выделяет и не падает.

struct Reader {
    const char* p;
    const char* end;
    bool        failed;

    Reader(const char* text, long len)
        : p(text), end(text + (len < 0 ? 0 : len)), failed(false) {}

    void skip(void) {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
                p = (p + 1 < end) ? p + 2 : end;
                continue;
            }
            return;
        }
    }

    bool eat(char c) {
        skip();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }

    // Читает строковое значение в out (без кавычек). Поддерживает \" \\ \/ \n \r \t \uXXXX.
    bool read_string(char* out, int cap) {
        skip();
        if (p >= end || *p != '"') return false;
        ++p;
        int n = 0;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        // \uXXXX → UTF-8. Спецсимволы не разворачиваем.
                        unsigned cp = 0;
                        for (int k = 0; k < 4 && p < end; ++k) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else { failed = true; }
                        }
                        if (cp < 0x80) { if (n < cap - 1) out[n++] = (char)cp; }
                        else if (n < cap - 1) out[n++] = '?';
                        continue;
                    }
                    default: c = e; break;   // \" \\ \/ и прочее — как есть
                }
            }
            if (n < cap - 1) out[n++] = c;
        }
        if (p >= end) return false;     // строка не закрыта
        ++p;                            // закрывающая кавычка
        out[n] = '\0';
        return true;
    }

    // Значение без ключа: строка / число / true / false / null.
    bool read_scalar(char* out, int cap) {
        skip();
        if (p >= end) return false;
        if (*p == '"') return read_string(out, cap);

        int n = 0;
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
            p += 4; if (n < cap - 1) out[n++] = '1'; out[n] = '\0'; return true;
        }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
            p += 5; if (n < cap - 1) out[n++] = '0'; out[n] = '\0'; return true;
        }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
            p += 4; out[0] = '\0'; return true;
        }
        while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n') {
            if (n < cap - 1) out[n++] = *p;
            ++p;
        }
        out[n] = '\0';
        return n > 0;
    }
};

// Принимаются только ключи того файла, который разбираем: перенести
// last_page в settings.json руками нельзя, иначе получится два источника
// одного значения.
void parse_into_slots(const char* text, long len, File_ which) {
    Reader r(text, len);
    if (!r.eat('{')) {
        std::printf("cfg: не начинается с '{' — беру значения по умолчанию\n");
        return;
    }
    char key[64];
    char val[kValMax];
    for (;;) {
        if (r.eat('}')) return;               // конец объекта
        if (!r.read_string(key, (int)sizeof(key))) {
            if (r.failed) std::printf("cfg: битый ключ — разбор прерван\n");
            return;
        }
        if (!r.eat(':')) { std::printf("cfg: нет ':' после '%s'\n", key); return; }
        if (!r.read_scalar(val, kValMax)) {
            if (!r.failed) { std::printf("cfg: нет значения у '%s'\n", key); return; }
            return;
        }
        int i = index_of(key);
        if (i >= 0 && kItems[i].file == which) {
            // Приводим к каноническому виду типа: true/false → 1/0.
            slot_set(i, val);
        }
        // Неизвестные ключи и ключи чужого файла игнорируем: файл может
        // быть дописан вручную.
        if (r.eat(',')) continue;
        if (r.eat('}')) return;
        std::printf("cfg: пропущен разделитель после '%s'\n", key);
        return;
    }
}

bool read_file_into_slots(const char* path, File_ which) {
    long n = slurp(path, g_buf, kBufMax);
    if (n < 0) return false;
    parse_into_slots(g_buf, n, which);
    return true;
}

// ── Формирование файлов ──────────────────────────────────────────────────

struct Writer {
    char* buf;
    int   cap;
    int   len;
    bool  overflow;

    Writer(char* b, int c) : buf(b), cap(c), len(0), overflow(false) {}

    void put(const char* s) {
        int l = (int)std::strlen(s);
        if (len + l >= cap) { overflow = true; return; }
        std::memcpy(buf + len, s, (size_t)l);
        len += l;
    }
    void putn(const char* s, int l) {
        if (len + l >= cap) { overflow = true; return; }
        std::memcpy(buf + len, s, (size_t)l);
        len += l;
    }
    void putc_(char c) {
        if (len + 1 >= cap) { overflow = true; return; }
        buf[len++] = c;
    }
    void indent(int n) { for (int i = 0; i < n; ++i) put("  "); }
};

// Пояснение к параметру. levels — отступ в «двойных пробелах», как у indent().
void emit_comment(Writer& w, const char* comment, int levels) {
    const char* s = comment;
    while (*s) {
        const char* nl = std::strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)std::strlen(s);
        w.indent(levels);
        w.put("// ");
        w.putn(s, len);
        w.putc_('\n');
        if (!nl) break;
        s = nl + 1;
    }
}

// Значение в виде JSON-токена: строки в кавычках с экранированием,
// числа — по типу из схемы, булевы — true/false.
void emit_value(Writer& w, int i) {
    const char* v = slot_get(i);
    switch (kItems[i].type) {
        case Type::Bool:
            w.put((std::strcmp(v, "1") == 0) ? "true" : "false");
            break;
        case Type::Int:
            w.put(std::to_string(clamp_int(i, std::atoi(v))).c_str());
            break;
        case Type::Float: {
            char b[32];
            std::snprintf(b, sizeof(b), "%.2f", (double)clamp_float(i, std::strtof(v, nullptr)));
            w.put(b);
            break;
        }
        case Type::Str: {
            w.putc_('"');
            for (const char* s = v; *s; ++s) {
                if (*s == '"' || *s == '\\') { w.putc_('\\'); w.putc_(*s); }
                else if (*s == '\n') w.put("\\n");
                else w.putc_(*s);
            }
            w.putc_('"');
            break;
        }
    }
}

// settings.json — с пояснениями. Всё, что не задано, берётся по умолчанию,
// поэтому файл всегда самодостаточен.
bool write_settings(void) {
    Writer w(g_buf, kBufMax);
    w.put("{\n");
    for (int i = 0; i < kCount; ++i) {
        if (kItems[i].file != File_::Settings) continue;
        if (kItems[i].comment) { emit_comment(w, kItems[i].comment, 2); }
        w.indent(2);
        w.put("\"");
        w.put(kItems[i].key);
        w.put("\": ");
        emit_value(w, i);
        w.put(",\n");
    }
    w.indent(2);
    w.put("// Файл создан приложением. Правьте значения выше; при выходе\n");
    w.indent(2);
    w.put("// из меню настроек файл будет перезаписан с теми же комментариями.\n");
    w.put("}\n");
    if (w.overflow) std::printf("cfg: settings.json не поместился в буфер\n");

    fs::File* f = g_fs->create(kSettingsFile);
    if (!f) return false;
    f->write(w.buf, (size_t)w.len);
    f->sync();
    delete f;
    return true;
}

// state.json — компактно, без отступов и комментариев: этот файл
// перезаписывается на каждом перелистывании, лишние байты — это всплеск
// тока на SD.
bool write_state(void) {
    Writer w(g_buf, kBufMax);
    w.put("{");
    bool first = true;
    for (int i = 0; i < kCount; ++i) {
        if (kItems[i].file != File_::State) continue;
        if (!first) w.put(",");
        first = false;
        w.put("\"");
        w.put(kItems[i].key);
        w.put("\":");
        emit_value(w, i);
    }
    w.put("}");
    if (w.overflow) std::printf("cfg: state.json не поместился в буфер\n");

    fs::File* f = g_fs->create(kStateFile);
    if (!f) return false;
    f->write(w.buf, (size_t)w.len);
    f->sync();
    delete f;
    return true;
}

// ── Разовая миграция со старого sims3.ini ────────────────────────────────

// Формат прежний — плоские строки "ключ=значение" без секций, так что
// перенос прямой. Сам sims3.ini не удаляем: если придётся откатывать
// прошивку, настройки останутся на месте.
void migrate_legacy(void) {
    long n = slurp(kLegacyFile, g_buf, kBufMax);
    if (n < 0) return;

    int moved = 0;
    const char* p = g_buf;
    const char* end = g_buf + n;
    while (p < end) {
        const char* nl = (const char*)std::memchr(p, '\n', (size_t)(end - p));
        const char* line_end = nl ? nl : end;
        const char* eq = (const char*)std::memchr(p, '=', (size_t)(line_end - p));
        if (eq) {
            int klen = (int)(eq - p);
            char key[64];
            if (klen > 0 && klen < (int)sizeof(key)) {
                std::memcpy(key, p, (size_t)klen);
                key[klen] = '\0';
                int vlen = (int)(line_end - eq - 1);
                char val[kValMax];
                if (vlen >= 0) {
                    int copy = vlen < kValMax - 1 ? vlen : kValMax - 1;
                    std::memcpy(val, eq + 1, (size_t)copy);
                    val[copy] = '\0';
                int i = index_of(key);
                if (i >= 0) {
                    // Старый файл мог быть записан с CRLF — хвостовой \r
                    // обязан попасть внутрь строки, иначе порвутся все
                    // текстовые значения.
                    if (copy > 0 && val[copy - 1] == '\r') val[--copy] = '\0';
                    slot_set(i, val);
                    ++moved;
                }

                }
            }
        }
        p = nl ? nl + 1 : end;
    }
    if (moved) {
        std::printf("cfg: перенесено %d значений из %s\n", moved, kLegacyFile);
        g_dirty[0] = true;
        g_dirty[1] = true;
    }
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
// Публичный интерфейс
// ───────────────────────────────────────────────────────────────────────────

void init(fs::FileSystem* fs) {
    g_fs = fs;
    for (int i = 0; i < kCount; ++i) g_slots[i].present = false;
    g_dirty[0] = g_dirty[1] = false;

    if (!g_fs) return;   // без SD работаем на значениях по умолчанию

    bool had_settings = read_file_into_slots(kSettingsFile, File_::Settings);
    read_file_into_slots(kStateFile, File_::State);

    if (!had_settings) {
        // Либо файлов нет вовсе, либо settings.json ещё не создавался.
        migrate_legacy();
        // Создаём оба файла сразу: чтобы на карте сразу лежал
        // документированный settings.json, а не пустота.
        if (!g_dirty[0] && !g_dirty[1]) {
            g_dirty[0] = true;
            g_dirty[1] = true;
        }
    }
    flush();
}

std::string get_str(const char* key) {
    int i = index_of(key);
    return i < 0 ? std::string() : std::string(slot_get(i));
}

int get_int(const char* key) {
    int i = index_of(key);
    if (i < 0) return 0;
    return clamp_int(i, std::atoi(slot_get(i)));
}

float get_float(const char* key) {
    int i = index_of(key);
    if (i < 0) return 0.0f;
    return clamp_float(i, std::strtof(slot_get(i), nullptr));
}

void set_str(const char* key, const char* value) {
    int i = index_of(key);
    if (i < 0) { std::printf("cfg: неизвестный ключ '%s'\n", key); return; }
    if (std::strcmp(slot_get(i), value ? value : "") == 0 && g_slots[i].present) return;
    slot_set(i, value ? value : "");
    g_dirty[file_index(kItems[i].file)] = true;
}

void set_int(const char* key, int value) {
    int i = index_of(key);
    if (i < 0) { std::printf("cfg: неизвестный ключ '%s'\n", key); return; }
    value = clamp_int(i, value);
    char b[24];
    std::snprintf(b, sizeof(b), "%d", value);
    set_str(key, b);
}

void set_float(const char* key, float value) {
    int i = index_of(key);
    if (i < 0) { std::printf("cfg: неизвестный ключ '%s'\n", key); return; }
    value = clamp_float(i, value);
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", (double)value);
    set_str(key, b);
}

void flush(void) {
    if (!g_fs) { g_dirty[0] = g_dirty[1] = false; return; }
    if (g_dirty[0]) { write_settings(); g_dirty[0] = false; }
    if (g_dirty[1]) { write_state();    g_dirty[1] = false; }
}

bool range_int(const char* key, int* lo, int* hi) {
    int i = index_of(key);
    if (i < 0 || kItems[i].type != Type::Int) return false;
    if (kItems[i].min_v == kItems[i].max_v) return false;
    if (lo) *lo = (int)kItems[i].min_v;
    if (hi) *hi = (int)kItems[i].max_v;
    return true;
}

bool range_float(const char* key, float* lo, float* hi) {
    int i = index_of(key);
    if (i < 0 || kItems[i].type != Type::Float) return false;
    if (kItems[i].min_v == kItems[i].max_v) return false;
    if (lo) *lo = kItems[i].min_v;
    if (hi) *hi = kItems[i].max_v;
    return true;
}

const char* description(const char* key) {
    int i = index_of(key);
    return i < 0 ? nullptr : kItems[i].comment;
}

}  // namespace cfg

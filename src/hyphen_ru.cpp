#include "hyphen_ru.h"

namespace hyph {

namespace {

// Кириллица в нижнем регистре: 0x0430..0x044F, ё — 0x0451.
inline uint32_t lower_cp(uint32_t cp) {
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;  // А-Я → а-я
    if (cp == 0x0401) return 0x0451;                    // Ё → ё
    return cp;
}

inline bool is_vowel_cp(uint32_t cp) {
    switch (lower_cp(cp)) {
        case 0x0430:  // а
        case 0x0435:  // е
        case 0x0451:  // ё
        case 0x0438:  // и
        case 0x043E:  // о
        case 0x0443:  // у
        case 0x044B:  // ы
        case 0x044D:  // э
        case 0x044E:  // ю
        case 0x044F:  // я
            return true;
        default:
            return false;
    }
}

inline bool is_soft_sign_cp(uint32_t cp) {
    uint32_t c = lower_cp(cp);
    return c == 0x044A   // ъ
        || c == 0x044C   // ь
        || c == 0x0439;  // й
}

// Правило 3: разрыв между двумя согласными.
// Условия:
//   (а) в левой части есть гласная — иначе на строке остаётся слог из одной
//       согласной («с-трана», «ст-рана»);
//   (б) ни слева, ни справа не образуется трёх согласных подряд;
//   (в) правило 1 (ъ/ь/й) и правило 2 (Г-Г) приоритетнее — здесь они
//       обрабатываются отдельно.
// Среди подходящих точек берётся та, у которой короче «хвост» слева, —
// это даёт канонические «сес-тра», «извес-тный», «Мос-ква».
bool rule_consonant_pair(const uint32_t* cps, int count, int k) {
    if (k < 2 || k > count - 2) return false;
    if (classify(cps[k - 1]) != Kind::Consonant) return false;
    if (classify(cps[k]) != Kind::Consonant) return false;

    // (а) слева обязан остаться слог, то есть хотя бы одна гласная
    bool vowel_left = false;
    for (int i = 0; i < k; ++i) {
        if (classify(cps[i]) == Kind::Vowel) { vowel_left = true; break; }
    }
    if (!vowel_left) return false;

    // (б) не более двух согласных подряд с каждой стороны разрыва
    int left = 1, right = 1;
    for (int i = k - 2; i >= 0 && classify(cps[i]) == Kind::Consonant; --i) ++left;
    for (int i = k + 1; i < count && classify(cps[i]) == Kind::Consonant; ++i) ++right;
    return !(left >= 3 || right >= 3);
}

// Длина «хвоста» — сколько согласных подряд стоят непосредственно перед
// точкой разрыва. Меньше значит лучше: «сес|тра» предпочтительнее, чем
// «сест|ра».
int consonant_tail(const uint32_t* cps, int k) {
    int left = 1;
    for (int i = k - 2; i >= 0 && classify(cps[i]) == Kind::Consonant; --i) ++left;
    return left;
}

// Правило 1: строго после ъ, ь, й.
bool rule_sign(const uint32_t* cps, int k) { return is_soft_sign_cp(cps[k - 1]); }

// Правило 2: Г-Г, не первая и не последняя буква слова.
bool rule_vowel_pair(const uint32_t* cps, int k) {
    return k >= 2 && classify(cps[k - 1]) == Kind::Vowel &&
           classify(cps[k]) == Kind::Vowel;
}

}  // namespace

Kind classify(uint32_t cp) {
    uint32_t c = lower_cp(cp);
    // Кириллические буквы: а-я и ё. Всё остальное (латиница, цифры, знаки
    // препинания) переноса не получает.
    bool cyrillic = (c >= 0x0430 && c <= 0x044F) || c == 0x0451;
    if (!cyrillic) return Kind::Other;
    return is_vowel_cp(c) ? Kind::Vowel : Kind::Consonant;
}

int decode_codepoints(const std::string& text, uint32_t* out_cps, int* out_offsets) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char* end = p + text.size();
    int n = 0;
    while (p < end) {
        if (out_offsets) out_offsets[n] = (int)(p - reinterpret_cast<const unsigned char*>(text.data()));
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
            cp = ((uint32_t)(*p & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
            cp = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                 (uint32_t)(p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
            cp = ((uint32_t)(*p & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            p += 4;
        } else {
            cp = 0xFFFD;
            p += 1;
        }
        if (out_cps) out_cps[n] = cp;
        ++n;
    }
    if (out_offsets) out_offsets[n] = (int)text.size();
    return n;
}

bool break_allowed(const uint32_t* cps, int count, int k) {
    if (k < 2 || k > count - 2) return false;  // ≥2 символов с каждой стороны
    if (rule_sign(cps, k)) return true;
    if (rule_vowel_pair(cps, k)) return true;
    return rule_consonant_pair(cps, count, k);
}

int find_break(const uint32_t* cps, int count, const int* prefix_w,
               int max_k, int avail_px, int hyphen_px) {
    if (count < 4) return -1;
    if (max_k > count - 2) max_k = count - 2;
    if (max_k < 2) return -1;

    // Три прохода по правилам в порядке приоритета; внутри правил 1 и 2 берётся
    // самая правая точка, помещающаяся по ширине.
    for (int rule = 0; rule < 2; ++rule) {
        for (int k = max_k; k >= 2; --k) {
            const bool ok = (rule == 0) ? rule_sign(cps, k) : rule_vowel_pair(cps, k);
            if (!ok) continue;
            if (prefix_w[k] + hyphen_px <= avail_px) return k;
        }
    }

    // Правило 3: среди помещающихся точек берём самую правую, у которой
    // короче хвост из согласных («сес-тра» вместо «сест-ра»); при равном
    // хвосте — самую правую.
    int best_k = -1;
    int best_tail = 0;
    for (int k = 2; k <= max_k; ++k) {
        if (!rule_consonant_pair(cps, count, k)) continue;
        if (prefix_w[k] + hyphen_px > avail_px) continue;
        const int tail = consonant_tail(cps, k);
        // Обход k по возрастанию, поэтому при равном хвосте (<=) побеждает
        // более правая точка — она заполняет строку плотнее.
        if (best_k < 0 || tail <= best_tail) {
            best_k = k;
            best_tail = tail;
        }
    }
    return best_k;
}

}  // namespace hyph

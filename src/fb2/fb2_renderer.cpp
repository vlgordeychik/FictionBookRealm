#include "fb2/fb2_renderer.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace fb2 {

// ─── InlineFormatter ───────────────────────────────────────────

std::vector<TextRun> InlineFormatter::parse(const std::string& xml_fragment) {
    // Заглушка: извлекает текст между > и <, игнорирует теги.
    // В реальном проекте здесь полноценный разбор inline-разметки
    // с построением дерева TextRun с учётом вложенных strong/emphasis/a.
    std::vector<TextRun> runs;
    TextRun current;
    current.style = StyleFlags::None;

    bool in_tag = false;
    for (size_t i = 0; i < xml_fragment.size(); ++i) {
        char c = xml_fragment[i];
        if (c == '<') {
            if (!current.text.empty()) {
                runs.push_back(std::move(current));
                current.text.clear();
                current.style = StyleFlags::None;
            }
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
        } else if (!in_tag) {
            current.text += c;
        }
    }
    if (!current.text.empty())
        runs.push_back(std::move(current));

    return runs;
}

std::vector<InlineFormatter::WrappedLine> InlineFormatter::word_wrap(
    const std::vector<TextRun>& runs, int max_width)
{
    std::vector<WrappedLine> result;
    if (max_width <= 0) max_width = 80;

    // Склеиваем все TextRun в одну строку с учётом стилей
    // Затем разбиваем по словам, перенося на новую строку при превышении max_width

    std::string full_text;
    for (const auto& run : runs)
        full_text += run.text;

    // Простейший перенос по словам
    size_t pos = 0;
    while (pos < full_text.size()) {
        // Определяем длину строки
        int line_len = 0;
        size_t line_end = pos;

        while (line_end < full_text.size() && line_len <= max_width) {
            // Найти следующее слово
            auto space_pos = full_text.find(' ', line_end);
            if (space_pos == std::string::npos) space_pos = full_text.size();

            size_t word_len = space_pos - line_end;
            if (line_len > 0 && line_len + 1 + static_cast<int>(word_len) > max_width)
                break; // слово не влезает — перенос

            if (line_len > 0) {
                line_len++; // пробел
            }
            line_len += static_cast<int>(word_len);
            line_end = space_pos;

            // Пропускаем пробелы
            while (line_end < full_text.size() && full_text[line_end] == ' ')
                line_end++;
        }

        if (line_end == pos) {
            // Ничего не поместилось — принудительно берём max_width символов
            line_end = std::min(pos + max_width, full_text.size());
        }

        result.push_back({full_text.substr(pos, line_end - pos), StyleFlags::None});
        pos = line_end;
    }

    return result;
}

// ─── AnsiRenderer ──────────────────────────────────────────────

AnsiRenderer::AnsiRenderer(FILE* out) : out_(out), style_depth_(0) {
    if (!out_) out_ = stdout;
}

AnsiRenderer::~AnsiRenderer() {
    // Сброс ANSI-стилей
    if (style_depth_ > 0)
        std::fprintf(out_, "\033[0m");
}

void AnsiRenderer::write(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), out_);
}

void AnsiRenderer::write_line(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), out_);
    std::fputc('\n', out_);
}

void AnsiRenderer::newline() {
    std::fputc('\n', out_);
}

void AnsiRenderer::push_style(StyleFlags style) {
    if (style_depth_ == 0)
        std::fprintf(out_, "\033[0m");
    style_depth_++;
    apply_ansi();
}

void AnsiRenderer::pop_style() {
    style_depth_ = std::max(0, style_depth_ - 1);
    apply_ansi();
}

void AnsiRenderer::apply_ansi() {
    // Заглушка: в реальном проекте — маппинг StyleFlags в ANSI escape codes
    // \033[1m = bold, \033[3m = italic, \033[9m = strikethrough, etc.
}

bool AnsiRenderer::show_image_placeholder(std::string_view image_id) {
    std::string msg = "[Image: ";
    msg += image_id;
    msg += ']';
    write_line(msg);
    return true;
}

} // namespace fb2

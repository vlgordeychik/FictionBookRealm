#pragma once
#include "fb2_types.h"
#include <string>
#include <string_view>
#include <vector>

namespace fb2 {

// ─── Интерфейс рендерера ──────────────────────────────────────
// Абстракция над способом вывода текста.
// Позволяет один и тот же движок форматирования использовать
// с разными интерфейсами (консоль ANSI, curses, GUI, HTML).
class RenderOutput {
public:
    virtual ~RenderOutput() = default;

    virtual void write(std::string_view text) = 0;
    virtual void write_line(std::string_view text) = 0;
    virtual void newline() = 0;

    // Управление стилями (опционально)
    virtual void push_style(StyleFlags style) = 0;
    virtual void pop_style() = 0;

    // Изображение: вернуть true если поддерживается
    virtual bool show_image_placeholder(std::string_view image_id) { return false; }
};

// ─── Утилиты рендеринга ───────────────────────────────────────
// Форматирование inline-разметки FB2 в последовательность
// стилизованных текстовых фрагментов.

struct TextRun {
    std::string text;
    StyleFlags  style;
};

class InlineFormatter {
public:
    // Разобрать XML-фрагмент блока в последовательность TextRun
    static std::vector<TextRun> parse(const std::string& xml_fragment);

    // Разбить TextRun на строки заданной ширины (word-wrap)
    struct WrappedLine {
        std::string text;
        StyleFlags  style;
    };
    static std::vector<WrappedLine> word_wrap(
        const std::vector<TextRun>& runs,
        int max_width);
};

// ─── Рендерер для ANSI-терминала ──────────────────────────────
class AnsiRenderer : public RenderOutput {
public:
    explicit AnsiRenderer(FILE* out = nullptr);
    ~AnsiRenderer();

    void write(std::string_view text) override;
    void write_line(std::string_view text) override;
    void newline() override;
    void push_style(StyleFlags style) override;
    void pop_style() override;
    bool show_image_placeholder(std::string_view image_id) override;

private:
    FILE* out_;
    int   style_depth_ = 0;
    void  apply_ansi();
};

} // namespace fb2

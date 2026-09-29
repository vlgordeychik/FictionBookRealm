#pragma once
#include <string>
#include <vector>

namespace fb2 {

// ─── Автор ─────────────────────────────────────────────────────
// См. doc/fb2-parsing-guide.md: authorType
struct AuthorInfo {
    std::string first_name;   // может быть пустым (тогда обязателен nickname)
    std::string middle_name;
    std::string last_name;
    std::string nickname;
    std::vector<std::string> home_pages;
    std::vector<std::string> emails;

    bool HasFullName() const noexcept {
        return !first_name.empty() && !last_name.empty();
    }
    std::string DisplayName() const;
};

// ─── Серия книг ────────────────────────────────────────────────
struct SequenceInfo {
    std::string name;
    int number = 0;
    bool has_number = false;
};

// ─── title-info ─────────────────────────────────────────────────
struct TitleInfo {
    std::vector<std::string> genres;          // список жанровых токенов
    std::vector<AuthorInfo>  authors;         // авторы книги
    std::string              book_title;
    std::string              annotation;      // plain-text аннотации
    std::string              keywords;
    std::string              date_text;       // человекочитаемая дата
    std::string              date_value;      // машинная дата (атрибут value)
    std::string              lang;
    std::string              src_lang;
    std::vector<AuthorInfo>  translators;
    std::vector<SequenceInfo> sequences;

    std::string cover_image_id;  // id изображения обложки (без #)
};

// ─── document-info ──────────────────────────────────────────────
struct DocumentInfo {
    std::vector<AuthorInfo> authors;
    std::string  program_used;
    std::string  date_text;
    std::string  date_value;
    std::vector<std::string> src_urls;
    std::string  src_ocr;
    std::string  id;           // уникальный идентификатор документа
    float        version = 0;
    std::string  history;
};

// ─── publish-info ───────────────────────────────────────────────
struct PublishInfo {
    std::string book_name;
    std::string publisher;
    std::string city;
    int         year = 0;
    bool        has_year = false;
    std::string isbn;
};

// ─── Метаданные книги ─────────────────────────────────────────
// Собираются при первом проходе, хранятся в Fb2Document.
struct BookMeta {
    TitleInfo    title_info;
    TitleInfo    src_title_info;    // заполнено только для переводов
    DocumentInfo document_info;
    PublishInfo  publish_info;
};

} // namespace fb2

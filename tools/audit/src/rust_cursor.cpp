#include "munch/tools/audit/rust_cursor.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief What opens a character literal.
 */
constexpr std::string_view character_opener{"'"};

/**
 * @brief What opens a byte literal.
 */
constexpr std::string_view byte_opener{"b'"};

/**
 * @brief What a raw identifier opens with, `r#match`.
 */
constexpr std::string_view raw_identifier_prefix{"r#"};

/**
 * @brief The most hex digits a `\u{...}` escape takes.
 */
constexpr std::size_t most_unicode_digits{6};

/**
 * @brief Returns the delimiter that closes a group an opening one opens.
 * @param open The opening delimiter, `(`, `[` or `{`.
 * @return `)`, `]` or `}`.
 */
[[nodiscard]] char closer_of(const char open) noexcept
{
    switch (open)
    {
    case '(':
        return ')';
    case '[':
        return ']';
    default:
        return '}';
    }
}

/**
 * @brief Returns where a character literal opening at an offset ends: after one scalar or one escape and the closing
 *        quote; a lifetime or a label, which does not close, ends just past its quote.
 * @param text The text.
 * @param open The offset just past the opening quote.
 * @param end The offset the text is read up to.
 * @return The offset just past the literal, or the given one where no quote closes it.
 */
[[nodiscard]] std::size_t character_end(const std::string_view text, const std::size_t open, const std::size_t end)
{
    auto close{open};

    if (close < end && text[close] == '\\')
    {
        const auto quote{text.find('\'', close + 2)};

        close = std::min(quote, end);
    }
    else if (close < end)
    {
        const auto lead{static_cast<unsigned char>(text[close])};

        close += sequence_length(lead);
    }

    return close < end && text[close] == '\'' ? close + 1 : open;
}

} // namespace

Rust_cursor::Rust_cursor(const std::string_view text, const std::size_t begin, const std::size_t end)
    : Cursor{text, begin, end}
{}

void Rust_cursor::skip_trivia()
{
    // Rust nests block comments.
    const auto skip_block_comment{[this] {
        const auto opened_line{line()};

        std::size_t depth{0};

        do
        {
            if (at_ >= end_)
            {
                throw Spec_error{"a block comment is never closed", opened_line};
            }

            if (at(comment_opener))
            {
                ++depth;

                at_ += comment_opener.size();
            }
            else if (at(comment_closer))
            {
                --depth;

                at_ += comment_closer.size();
            }
            else
            {
                ++at_;
            }
        } while (depth > 0);
    }};

    for (;;)
    {
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        if (at(line_comment_opener))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }

            continue;
        }

        if (!at(comment_opener))
        {
            return;
        }

        skip_block_comment();
    }
}

void Rust_cursor::skip_token()
{
    if (at(line_comment_opener) || at(comment_opener))
    {
        skip_trivia();

        return;
    }

    if (const auto end{string_end()})
    {
        at_ = *end;

        return;
    }

    if (at(character_opener) || at(byte_opener))
    {
        const auto opener{at(byte_opener) ? byte_opener : character_opener};

        const auto open{at_ + opener.size()};

        at_ = character_end(text_, open, end_);

        return;
    }

    if (is_opening(peek()))
    {
        skip_group();

        return;
    }

    if (word().empty())
    {
        ++at_;
    }
}

void Rust_cursor::skip_group()
{
    const auto open{next("a group")};

    const auto close{closer_of(open)};

    const auto opened_line{line()};

    for (skip_trivia(); !accept(close); skip_trivia())
    {
        if (done())
        {
            throw Spec_error{std::format("the '{}' is never closed", open), opened_line};
        }

        if (is_closing(peek()))
        {
            fail(std::format("'{}' closes nothing, '{}' was expected", *peek(), close));
        }

        skip_token();
    }
}

std::string_view Rust_cursor::word()
{
    const auto begin{at_};

    const auto after_prefix{at_ + raw_identifier_prefix.size()};

    if (at(raw_identifier_prefix) && after_prefix < end_ && is_word_byte(text_[after_prefix]))
    {
        at_ = after_prefix;
    }

    while (peek() && is_word_byte(*peek()))
    {
        ++at_;
    }

    return text_.substr(begin, at_ - begin);
}

String_literal Rust_cursor::literal()
{
    const auto begin{at_};

    const auto end{string_end()};

    if (!end)
    {
        fail("expected a string literal");
    }

    if (peek() == 'c')
    {
        fail("a C string is no pattern; logos takes a &str or a &[u8] literal");
    }

    const auto byte_string{accept('b')};

    const auto raw{accept('r')};

    std::size_t hashes{0};

    while (accept('#'))
    {
        ++hashes;
    }

    expect('"', R"('"')");

    const auto content_end{*end - hashes - 1};

    String_literal literal{.written = std::string{slice(begin, *end)}, .bytes = {}, .byte_string = byte_string};

    if (raw)
    {
        literal.bytes = slice(at_, content_end);

        at_ = *end;

        return literal;
    }

    while (at_ < content_end)
    {
        const auto byte{next("the string's content")};

        if (byte != '\\')
        {
            literal.bytes.push_back(byte);

            continue;
        }

        escape(literal.bytes, byte_string);
    }

    at_ = *end;

    return literal;
}

Rust_cursor Rust_cursor::inside(const std::size_t begin, const std::size_t end) const noexcept
{
    return {text_, begin, end};
}

std::string_view Rust_cursor::slice(const std::size_t begin, const std::size_t end) const noexcept
{
    return text_.substr(begin, end - begin);
}

Rust_cursor Rust_cursor::group_inside(const std::size_t open) const noexcept
{
    return inside(open + 1, offset() - 1);
}

std::string_view Rust_cursor::group_text(const std::size_t open) const noexcept
{
    return slice(open + 1, offset() - 1);
}

bool Rust_cursor::at_string() const
{
    return string_end().has_value();
}

bool Rust_cursor::at_literal() const
{
    return at_string() || at(character_opener) || at(byte_opener);
}

std::optional<std::size_t> Rust_cursor::string_end() const
{
    auto scan{at_};

    // The prefixes: b for a byte string, r for a raw one, br for both; c and cr, the C strings, have no pattern to give
    // but are skipped like the rest.
    if (scan < end_ && (text_[scan] == 'b' || text_[scan] == 'c'))
    {
        ++scan;
    }

    const auto raw{scan < end_ && text_[scan] == 'r'};

    if (raw)
    {
        ++scan;
    }

    std::size_t hashes{0};

    for (; raw && scan < end_ && text_[scan] == '#'; ++scan)
    {
        ++hashes;
    }

    if (scan >= end_ || text_[scan] != '"')
    {
        return std::nullopt;
    }

    const auto opened_line{line()};

    const auto closing_hashes{std::string(hashes, '#')};

    for (++scan; scan < end_; ++scan)
    {
        if (text_[scan] == '\\' && !raw)
        {
            ++scan;

            continue;
        }

        if (text_[scan] != '"' || scan + hashes >= end_)
        {
            continue;
        }

        if (text_.substr(scan + 1, hashes) == closing_hashes)
        {
            return scan + 1 + hashes;
        }
    }

    throw Spec_error{"a string literal is never closed", opened_line};
}

void Rust_cursor::escape(std::string& bytes, const bool byte_string)
{
    switch (const auto escaped{next("the escape")}; escaped)
    {
    case 'n':
        bytes.push_back('\n');
        break;
    case 'r':
        bytes.push_back('\r');
        break;
    case 't':
        bytes.push_back('\t');
        break;
    case '0':
        bytes.push_back('\0');
        break;
    case '\\':
    case '\'':
    case '"':
        bytes.push_back(escaped);
        break;
    case 'x':
        bytes.push_back(byte_escape(byte_string));
        break;
    case 'u':
    {
        if (byte_string)
        {
            fail(R"(a byte string has no \u escape)");
        }

        const auto scalar{unicode_escape()};

        bytes += regex::utf8::encode(scalar);
        break;
    }
    case '\n':
    case '\r':
        // A backslash ending the line continues the string on the next, its leading blanks dropped.
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        break;
    default:
        fail(std::format(R"('\{}' is not an escape Rust knows)", escaped));
    }
}

char Rust_cursor::byte_escape(const bool byte_string)
{
    const auto high{next("a hex digit")};

    const auto low{next("a hex digit")};

    if (!is_hex_digit(high) || !is_hex_digit(low))
    {
        fail(R"(\x needs two hex digits)");
    }

    const auto value{hex_value(high) * 16 + hex_value(low)};

    if (value > last_ascii && !byte_string)
    {
        fail(R"(\x in a string reaches only \x7f; a higher scalar is written \u{...})");
    }

    return static_cast<char>(value);
}

char32_t Rust_cursor::unicode_escape()
{
    expect('{', R"('{' after \u)");

    char32_t value{0};

    std::size_t digits{0};

    // Rust writes an underscore between the digits of a numeric escape as it writes one in a number, `\u{1F_600}` being
    // `\u{1F600}`, and the underscores are no digits of the escape's own.
    while (peek() && (is_hex_digit(*peek()) || *peek() == '_'))
    {
        if (*peek() == '_')
        {
            std::ignore = next("an underscore");

            continue;
        }

        const auto digit{next("a hex digit")};

        value = value * 16 + hex_value(digit);

        ++digits;
    }

    expect('}', R"('}' to close the \u escape)");

    if (digits == 0 || digits > most_unicode_digits || value > last_scalar || is_surrogate(value))
    {
        fail(R"(\u{...} needs one to six hex digits naming a scalar)");
    }

    return value;
}

bool at_attribute(const Rust_cursor& cursor)
{
    auto look{cursor};

    if (!look.accept('#'))
    {
        return false;
    }

    look.skip_trivia();

    return look.at("[");
}

std::string compacted(const std::string_view text)
{
    std::string compact{};

    Rust_cursor cursor{text, 0, text.size()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        const auto begin{cursor.offset()};

        // A literal is copied whole, trivia inside it being none; a group's delimiters are single bytes here, so that
        // the trivia inside the group is dropped as well.
        if (cursor.at_literal())
        {
            cursor.skip_token();
        }
        else if (cursor.word().empty())
        {
            std::ignore = cursor.next("a token");
        }

        compact += text.substr(begin, cursor.offset() - begin);
    }

    return compact;
}

void skip_generics(Rust_cursor& cursor)
{
    std::size_t depth{0};

    do
    {
        const auto byte{cursor.next("'>' to close the generics")};

        if (byte == '<')
        {
            ++depth;
        }
        else if (byte == '>')
        {
            --depth;
        }
        else if (byte == '-' && cursor.peek() == '>')
        {
            // The arrow of a function type among the bounds, `F: Fn() -> u8`.
            std::ignore = cursor.next("'>'");
        }
    } while (depth > 0);
}

void skip_until(Rust_cursor& cursor, const std::string_view stops)
{
    while (!cursor.done() && !stops.contains(*cursor.peek()))
    {
        cursor.skip_token();
    }
}

void skip_list_item(Rust_cursor& cursor)
{
    while (!cursor.done() && cursor.peek() != ',')
    {
        if (cursor.peek() == '<')
        {
            skip_generics(cursor);
        }
        else
        {
            cursor.skip_token();
        }
    }
}

void expect_spelled(Rust_cursor& cursor, const std::string_view token)
{
    for (const auto byte : token)
    {
        cursor.expect(byte, std::format("'{}'", byte));
    }
}

} // namespace munch::tools::audit

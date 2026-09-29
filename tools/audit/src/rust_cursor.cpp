#include "munch/tools/audit/rust_cursor.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
Rust_cursor::Rust_cursor(const std::string_view text, const std::size_t begin, const std::size_t end)
    : Cursor{text, begin, end}
{}

void Rust_cursor::skip_trivia()
{
    for (;;)
    {
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        if (at("//"))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }
        }
        else if (at("/*"))
        {
            const auto line{this->line()};

            // Rust nests block comments.
            std::size_t depth{0};

            do
            {
                if (at_ >= end_)
                {
                    throw Spec_error{"a block comment is never closed", line};
                }

                if (at("/*"))
                {
                    ++depth;

                    at_ += 2;
                }
                else if (at("*/"))
                {
                    --depth;

                    at_ += 2;
                }
                else
                {
                    ++at_;
                }
            } while (depth > 0);
        }
        else
        {
            return;
        }
    }
}

void Rust_cursor::skip_token()
{
    if (at("//") || at("/*"))
    {
        skip_trivia();

        return;
    }

    if (const auto end{string_end()})
    {
        at_ = *end;

        return;
    }

    if (at("'") || at("b'"))
    {
        // A character literal closes after one scalar or one escape; a lifetime or label does not close at all.
        const auto open{at_ + (at("b'") ? 2 : 1)};

        auto close{open};

        if (close < end_ && text_[close] == '\\')
        {
            close = std::min(text_.find('\'', close + 2), end_);
        }
        else if (close < end_)
        {
            const auto lead{static_cast<unsigned char>(text_[close])};

            close += sequence_length(lead);
        }

        at_ = close < end_ && text_[close] == '\'' ? close + 1 : open;

        return;
    }

    if (peek() == '(' || peek() == '[' || peek() == '{')
    {
        skip_group();

        return;
    }

    if (word().empty())
    {
        ++at_;
    }
}

std::optional<std::size_t> Rust_cursor::string_end() const
{
    auto at{at_};

    // The prefixes: b for a byte string, r for a raw one, br for both; c and cr, the C strings, have no pattern to give
    // but are skipped like the rest.
    if (at < end_ && (text_[at] == 'b' || text_[at] == 'c'))
    {
        ++at;
    }

    const auto raw{at < end_ && text_[at] == 'r'};

    if (raw)
    {
        ++at;
    }

    std::size_t hashes{0};

    for (; raw && at < end_ && text_[at] == '#'; ++at)
    {
        ++hashes;
    }

    if (at >= end_ || text_[at] != '"')
    {
        return std::nullopt;
    }

    const auto line{this->line()};

    for (++at; at < end_; ++at)
    {
        if (text_[at] == '\\' && !raw)
        {
            ++at;
        }
        else if (text_[at] == '"' && at + hashes < end_ && text_.substr(at + 1, hashes) == std::string(hashes, '#'))
        {
            return at + 1 + hashes;
        }
    }

    throw Spec_error{"a string literal is never closed", line};
}

void Rust_cursor::skip_group()
{
    const auto open{next("a group")};

    const auto close{open == '(' ? ')' : open == '[' ? ']' : '}'};

    const auto line{this->line()};

    for (skip_trivia(); !accept(close); skip_trivia())
    {
        if (done())
        {
            throw Spec_error{std::string{"the '"} + open + "' is never closed", line};
        }

        if (peek() == ')' || peek() == ']' || peek() == '}')
        {
            fail(std::string{"'"} + *peek() + "' closes nothing, '" + close + "' was expected");
        }

        skip_token();
    }
}

std::string_view Rust_cursor::word()
{
    const auto begin{at_};

    if (at("r#") && at_ + 2 < end_ && is_word_byte(text_[at_ + 2]))
    {
        at_ += 2;
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
        if (byte_string)
        {
            fail(R"(a byte string has no \u escape)");
        }

        bytes += encoded(unicode_escape());
        break;
    case '\n':
    case '\r':
        // A backslash ending the line continues the string on the next, its leading blanks dropped.
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        break;
    default:
        fail(std::string{R"('\)"} + escaped + "' is not an escape Rust knows");
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

    if (value > 0x7F && !byte_string)
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
    for (; peek() && (is_hex_digit(*peek()) || *peek() == '_');)
    {
        if (*peek() == '_')
        {
            std::ignore = next("an underscore");

            continue;
        }

        value = value * 16 + hex_value(next("a hex digit"));

        ++digits;
    }

    expect('}', R"('}' to close the \u escape)");

    if (digits == 0 || digits > 6 || value > last_scalar || is_surrogate(value))
    {
        fail(R"(\u{...} needs one to six hex digits naming a scalar)");
    }

    return value;
}

Rust_cursor Rust_cursor::inside(const std::size_t begin, const std::size_t end) const noexcept
{
    return {text_, begin, end};
}

std::string_view Rust_cursor::slice(const std::size_t begin, const std::size_t end) const noexcept
{
    return text_.substr(begin, end - begin);
}

bool Rust_cursor::at_string() const
{
    return string_end().has_value();
}

bool at_attribute(const Rust_cursor& cursor)
{
    auto look{cursor};

    if (!look.at("#"))
    {
        return false;
    }

    std::ignore = look.accept('#');

    look.skip_trivia();

    return look.at("[");
}

std::string compacted(const std::string_view text)
{
    std::string compact;

    Rust_cursor cursor{text, 0, text.size()};

    for (cursor.skip_trivia(); !cursor.done(); cursor.skip_trivia())
    {
        const auto begin{cursor.offset()};

        // A literal is copied whole, trivia inside it being none; a group's delimiters are single bytes here, so that
        // the trivia inside the group is dropped as well.
        if (cursor.at_string() || cursor.at("'") || cursor.at("b'"))
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

} // namespace munch::tools::audit

#include "munch/tools/audit/antlr_cursor.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements antlr_cursor.hpp: the code points a surrogate pair spends and the UTF-16 measure of the grammar's text are
// private to this unit.

/**
 * @brief The last high surrogate, the first of a pair.
 */
constexpr char32_t last_high_surrogate{0xDBFF};

/**
 * @brief The first low surrogate, the second of a pair.
 */
constexpr char32_t first_low_surrogate{0xDC00};

/**
 * @brief The UTF-16 code units a span of UTF-8 text holds, which is how ANTLR's lexer, reading the grammar into Java
 *        strings, measures it: one per character up to U+FFFF, two per character beyond.
 * @param text The text.
 * @param begin The span's first offset.
 * @param end The offset past its last.
 * @return The count.
 */
[[nodiscard]] std::size_t units(const std::string_view text, const std::size_t begin, const std::size_t end)
{
    std::size_t count{0};

    for (const auto byte : text.substr(begin, end - begin))
    {
        const auto value{static_cast<unsigned char>(byte)};

        count += is_continuation(value) ? 0 : value >= 0xF0 ? 2 : 1;
    }

    return count;
}

} // namespace

Antlr_cursor::Antlr_cursor(const std::string_view source) : Cursor{source, Line_comment_end::newline_or_return}
{}

Antlr_cursor::Antlr_cursor(const std::string_view text, const std::size_t begin)
    : Cursor{text, begin, text.size(), Line_comment_end::newline_or_return}
{}

void Antlr_cursor::element_options()
{
    skip_blanks();

    if (accept('>'))
    {
        return;
    }

    // A name, dotted or not, each dot a token of its own to ANTLR's lexer with blanks and comments allowed on either
    // side of it; a value follows `=` after an undotted name alone, a name dotted the same way, a number, a quoted
    // string or a brace block.
    const auto qualified{[this](std::string name) {
        skip_blanks();

        while (!name.empty() && accept('.'))
        {
            skip_blanks();

            const auto part{identifier()};

            name = part.empty() ? std::string{} : name + '.' + part;

            skip_blanks();
        }

        return name;
    }};

    for (;;)
    {
        const auto first{identifier()};

        const auto name{qualified(first)};

        if (name.empty())
        {
            fail("an element option needs a name");
        }

        if (name == first && accept('='))
        {
            skip_blanks();

            if (peek() == '\'')
            {
                ++at_;

                std::ignore = literal();
            }
            else if (peek() == '{')
            {
                skip_block();
            }
            else if (const auto value{identifier()}; !value.empty())
            {
                if (qualified(value).empty())
                {
                    fail("an element option's value needs a name after its dot");
                }
            }
            else
            {
                const auto begin{at_};

                while (peek() && is_digit(*peek()))
                {
                    ++at_;
                }

                if (at_ == begin)
                {
                    fail("an element option needs a value");
                }
            }

            skip_blanks();
        }

        if (accept(','))
        {
            skip_blanks();

            continue;
        }

        expect('>', "'>' to close the element options");

        return;
    }
}

void Antlr_cursor::skip_blanks()
{
    for (Cursor::skip_blanks(); at("\xEF\xBB\xBF"); Cursor::skip_blanks())
    {
        at_ += 3;
    }
}

std::string Antlr_cursor::identifier()
{
    std::string name;

    if (!peek() || !is_letter(static_cast<unsigned char>(*peek())))
    {
        return name;
    }

    while (peek() && is_name_byte(*peek()))
    {
        name.push_back(next("a name"));
    }

    return name;
}

Literal Antlr_cursor::literal()
{
    const auto quote{at_ - 1};

    std::string bytes;

    std::optional<char32_t> high;

    auto pieces{0UZ};

    auto wide{false};

    const auto lone{[this](const char32_t scalar) {
        fail(std::format(
                R"(the literal holds a lone surrogate, \u{:04X}, which no UTF-8 input decodes to, so ANTLR's lexer )"
                R"(never matches it; a high surrogate and a low one after it are the character the pair encodes)",
                static_cast<std::uint32_t>(scalar)));
    }};

    for (;;)
    {
        const auto written{peek() != '\\'};

        const auto braced{at("\\u{")};

        const auto scalar{character('\'')};

        if (!scalar)
        {
            break;
        }

        if (braced && units(text_, quote, at_ - 1) >= 12)
        {
            const std::string sequence{text_.substr(quote, at_ - quote)};

            at_ = quote;

            fail("invalid escape sequence " + sequence);
        }

        ++pieces;

        wide = wide || (written && *scalar > 0xFFFF);

        if (high && *scalar >= first_low_surrogate && *scalar <= last_surrogate)
        {
            bytes += encoded(0x10000 + ((*high - first_surrogate) << 10U) + (*scalar - first_low_surrogate));

            high = std::nullopt;

            continue;
        }

        if (high)
        {
            lone(*high);
        }

        if (*scalar >= first_surrogate && *scalar <= last_high_surrogate)
        {
            high = *scalar;

            continue;
        }

        if (is_surrogate(*scalar))
        {
            lone(*scalar);
        }

        bytes += encoded(*scalar);
    }

    if (high)
    {
        lone(*high);
    }

    return {.bytes = std::move(bytes), .single = pieces == 1 && !wide};
}

std::optional<char32_t> Antlr_cursor::character(const char closing)
{
    const auto byte{next(std::format("'{}'", closing))};

    if (byte == closing)
    {
        return std::nullopt;
    }

    // ANTLR's lexer takes no raw line break inside a literal or a set (ANTLRLexer.g's STRING_LITERAL and
    // LEXER_CHAR_SET): a literal holding one ends unterminated at the break, its error 152, and a set holding one is
    // its error 50 at the break, each in its words at the line the literal or set opened on.
    if (byte == '\n' || byte == '\r')
    {
        --at_;

        fail(closing == '\'' ? std::string{"unterminated string literal"} :
                               std::format(
                                       "syntax error: mismatched character '{}' expecting ']'",
                                       byte == '\n' ? R"(\n)" : R"(\r)"));
    }

    if (byte != '\\')
    {
        // A scalar typed directly in UTF-8: its lead byte says how many follow.
        const auto value{static_cast<unsigned char>(byte)};

        if (value < 0x80)
        {
            return value;
        }

        auto scalar{lead_bits(value)};

        for (auto count{1UZ}; count < sequence_length(value); ++count)
        {
            scalar = (scalar << 6U) | (static_cast<unsigned char>(next("a continuation byte")) & 0x3FU);
        }

        return scalar;
    }

    const auto escaped{next("the escaped character")};

    // The escapes ANTLR takes: in a literal, its lexer's ESC_SEQ, \b \t \n \f \r \' \\ and the two Unicode forms; in a
    // set, EscapeSequenceParsing's, \b \t \n \f \r \\ \] \- \p{...} \P{...} and the two Unicode forms. Any other is its
    // error 156, in its words: `'\q'`, `'\]'` and `[\']` among them.
    switch (escaped)
    {
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case '\\':
        return '\\';
    case '\'':
        if (closing == '\'')
        {
            return '\'';
        }

        break;
    case ']':
    case '-':
        if (closing == ']')
        {
            return static_cast<unsigned char>(escaped);
        }

        break;
    case 'p':
    case 'P':
        if (closing == ']')
        {
            --at_;

            fail("a Unicode property class needs tables the byte reading has not got");
        }

        break;
    default:
        break;
    }

    if (escaped != 'u')
    {
        // The sequence as written, the escaped character whole where it is more than one byte.
        const auto begin{at_ - 2};

        auto end{at_};

        while (end < end_ && is_continuation(static_cast<unsigned char>(text_[end])))
        {
            ++end;
        }

        const std::string sequence{text_.substr(begin, end - begin)};

        at_ = begin;

        fail("invalid escape sequence " + sequence);
    }

    // \uXXXX, or \u{X...} of one to six digits.
    const auto braced{peek() == '{'};

    if (braced)
    {
        ++at_;
    }

    char32_t scalar{0};

    std::size_t digits{0};

    while (digits < (braced ? 6U : 4U) && peek() && is_hex_digit(*peek()))
    {
        const auto digit{next("a hex digit")};

        scalar = scalar * 16 + hex_value(digit);

        ++digits;
    }

    if (digits == 0 || (!braced && digits < 4) || (braced && next("'}'") != '}') || scalar > last_scalar)
    {
        fail(R"(a Unicode escape is \uXXXX or \u{X...} up to U+10FFFF)");
    }

    return scalar;
}

void Antlr_cursor::skip_block()
{
    const auto opened{at_};

    std::size_t depth{0};

    do
    {
        // A comment's quotes are prose, the apostrophe of "the parser's" among them, so comments are skipped whole.
        skip_blanks();

        if (!peek())
        {
            at_ = opened;

            fail("a brace block never closes");
        }

        const auto byte{next("'}'")};

        if (byte == '\'' || byte == '"')
        {
            skip_quoted(byte);
        }
        else if (byte == '{')
        {
            ++depth;
        }
        else if (byte == '}')
        {
            --depth;
        }
    } while (depth > 0);
}

void Antlr_cursor::skip_quoted(const char quote)
{
    const auto opened{at_ - 1};

    for (;;)
    {
        if (!peek())
        {
            at_ = opened;

            fail("a quoted literal never closes");
        }

        const auto byte{next("the closing quote")};

        if (byte == quote)
        {
            return;
        }

        if (byte == '\\' && peek())
        {
            ++at_;
        }
    }
}

} // namespace munch::tools::audit

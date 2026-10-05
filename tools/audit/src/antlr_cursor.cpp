#include "munch/tools/audit/antlr_cursor.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "munch/regex/utf8.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The last high surrogate, the first of a pair.
 */
constexpr char32_t last_high_surrogate{0xDBFF};

/**
 * @brief The first low surrogate, the second of a pair.
 */
constexpr char32_t first_low_surrogate{0xDC00};

/**
 * @brief The byte order mark, which ANTLR's lexer reads as a blank wherever it stands.
 */
constexpr std::string_view byte_order_mark{"\xEF\xBB\xBF"};

/**
 * @brief The UTF-16 code units from a literal's opening quote through a braced Unicode escape at which ANTLR refuses
 *        the escape as an invalid escape sequence.
 */
constexpr std::size_t braced_escape_units{12};

/**
 * @brief The hex digits of a Unicode escape without braces, `\uXXXX`.
 */
constexpr std::size_t unbraced_escape_digits{4};

/**
 * @brief The most hex digits a braced Unicode escape holds, `\u{X...}`.
 */
constexpr std::size_t braced_escape_digits{6};

/**
 * @brief The last scalar of the basic multilingual plane, the last that UTF-16 holds in one unit.
 */
constexpr char32_t last_basic_scalar{0xFFFF};

/**
 * @brief The bits of a scalar each surrogate of a pair carries, the low surrogate's below the high one's.
 */
constexpr unsigned surrogate_payload_bits{10U};

/**
 * @brief Returns the scalar a UTF-16 surrogate pair encodes.
 * @param high The high surrogate, the first of the pair.
 * @param low The low surrogate, the second.
 * @return The scalar, beyond U+FFFF.
 */
[[nodiscard]] constexpr char32_t paired_scalar(const char32_t high, const char32_t low) noexcept
{
    return last_basic_scalar + 1 + ((high - first_surrogate) << surrogate_payload_bits) + (low - first_low_surrogate);
}

/**
 * @brief Returns the UTF-16 code units a span of UTF-8 text holds, which is how ANTLR's lexer, reading the grammar into
 *        Java strings, measures it: one per character up to U+FFFF, two per character beyond.
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

        if (is_continuation(value))
        {
            continue;
        }

        count += sequence_length(value) == 4 ? 2 : 1;
    }

    return count;
}

} // namespace

Antlr_cursor::Antlr_cursor(const std::string_view source) : Cursor{source, Line_comment_end::newline_or_return}
{}

Antlr_cursor::Antlr_cursor(const std::string_view text, const std::size_t begin)
    : Cursor{text, begin, text.size(), Line_comment_end::newline_or_return}
{}

void Antlr_cursor::skip_blanks()
{
    for (Cursor::skip_blanks(); at(byte_order_mark); Cursor::skip_blanks())
    {
        at_ += byte_order_mark.size();
    }
}

std::string Antlr_cursor::identifier()
{
    std::string name{};

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

    std::string bytes{};

    std::optional<char32_t> high{};

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

        const auto braced{at(R"(\u{)")};

        const auto scalar{character('\'')};

        if (!scalar)
        {
            break;
        }

        if (braced && units(text_, quote, at_ - 1) >= braced_escape_units)
        {
            const std::string sequence{text_.substr(quote, at_ - quote)};

            at_ = quote;

            fail(std::format("invalid escape sequence {}", sequence));
        }

        ++pieces;

        wide = wide || (written && *scalar > last_basic_scalar);

        if (high && *scalar >= first_low_surrogate && *scalar <= last_surrogate)
        {
            const auto paired{paired_scalar(*high, *scalar)};

            bytes += regex::utf8::encode(paired);

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

        bytes += regex::utf8::encode(*scalar);
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
        const auto shown{byte == '\n' ? R"(\n)" : R"(\r)"};

        const auto message{
                closing == '\'' ? std::string{"unterminated string literal"} :
                                  std::format("syntax error: mismatched character '{}' expecting ']'", shown)};

        --at_;

        fail(message);
    }

    if (byte != '\\')
    {
        return typed_scalar(static_cast<unsigned char>(byte));
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

        fail(std::format("invalid escape sequence {}", sequence));
    }

    return unicode_escape();
}

char32_t Antlr_cursor::typed_scalar(const unsigned char lead)
{
    if (lead <= last_ascii)
    {
        return lead;
    }

    auto scalar{lead_bits(lead)};

    for (auto count{1UZ}; count < sequence_length(lead); ++count)
    {
        const auto continuation{static_cast<unsigned char>(next("a continuation byte"))};

        scalar = continued(scalar, continuation);
    }

    return scalar;
}

char32_t Antlr_cursor::unicode_escape()
{
    // \uXXXX, or \u{X...} of one to six digits.
    const auto braced{peek() == '{'};

    if (braced)
    {
        ++at_;
    }

    const auto most_digits{braced ? braced_escape_digits : unbraced_escape_digits};

    char32_t scalar{0};

    std::size_t digits{0};

    while (digits < most_digits && peek() && is_hex_digit(*peek()))
    {
        const auto digit{next("a hex digit")};

        scalar = scalar * 16 + hex_value(digit);

        ++digits;
    }

    const std::string malformed{R"(a Unicode escape is \uXXXX or \u{X...} up to U+10FFFF)"};

    if (digits == 0 || (!braced && digits < most_digits))
    {
        fail(malformed);
    }

    if (braced)
    {
        const auto close{next("'}'")};

        if (close != '}')
        {
            fail(malformed);
        }
    }

    if (scalar > last_scalar)
    {
        fail(malformed);
    }

    return scalar;
}

void Antlr_cursor::element_options()
{
    skip_blanks();

    if (accept('>'))
    {
        return;
    }

    // Each dot is a token of its own to ANTLR's lexer, with blanks and comments allowed on either side.
    const auto qualified{[this](std::string name) {
        skip_blanks();

        while (!name.empty() && accept('.'))
        {
            skip_blanks();

            const auto part{identifier()};

            name = part.empty() ? std::string{} : std::format("{}.{}", name, part);

            skip_blanks();
        }

        return name;
    }};

    const auto skip_value{[&] {
        if (peek() == '\'')
        {
            ++at_;

            std::ignore = literal();

            return;
        }

        if (peek() == '{')
        {
            skip_block();

            return;
        }

        if (const auto first{identifier()}; !first.empty())
        {
            const auto name{qualified(first)};

            if (name.empty())
            {
                fail("an element option's value needs a name after its dot");
            }

            return;
        }

        const auto begin{at_};

        while (peek() && is_digit(*peek()))
        {
            ++at_;
        }

        if (at_ == begin)
        {
            fail("an element option needs a value");
        }
    }};

    // A value follows `=` after an undotted name alone.
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

            skip_value();

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

void Antlr_cursor::skip_block()
{
    // A comment's quotes are prose, the apostrophe of "the parser's" among them, so comments are skipped whole.
    skip_bracketed('{', '}', true, "a brace block never closes");
}

void Antlr_cursor::skip_bracketed(
        const char opener, const char closer, const bool comments, const std::string& unclosed)
{
    const auto opened{at_};

    const auto expected{std::format("'{}'", closer)};

    std::size_t depth{0};

    do
    {
        if (comments)
        {
            skip_blanks();
        }

        if (!peek())
        {
            at_ = opened;

            fail(unclosed);
        }

        const auto byte{next(expected)};

        if (byte == '\'' || byte == '"')
        {
            skip_quoted(byte);
        }
        else if (byte == opener)
        {
            ++depth;
        }
        else if (byte == closer)
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

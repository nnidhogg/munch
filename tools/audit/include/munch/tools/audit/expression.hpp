#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "munch/regex/set.hpp"

/**
 * @brief What every reader writes for the pattern parser, a quoted literal, quoted(), a definition's reference,
 *        reference(), a bracket, bracket(), a run of its members, bracket_run(), a run of code points,
 *        code_point_member(), and one member, bracket_member(); a text without its trailing blanks,
 *        without_trailing_blanks(), or without its two delimiters, without_delimiters(); the line an offset stands on,
 *        lines_before(); and what the readers share to read bytes with, the byte tests, each made directly so that no
 *        locale is consulted, the byte and scalar bounds, the case bit, the hex base, the comment markers,
 *        appended_digit() and the UTF-8 measures.
 */
namespace munch::tools::audit
{
/**
 * @brief The last ASCII scalar, which every byte at or below is ASCII.
 */
constexpr char32_t last_ascii{0x7F};

/**
 * @brief The last byte.
 */
constexpr char32_t last_byte{0xFF};

/**
 * @brief How many values a byte takes.
 */
constexpr std::size_t byte_values{256};

/**
 * @brief The bit that tells an ASCII letter's two cases apart: set in the small letter, clear in the capital.
 */
constexpr unsigned case_bit{0x20U};

/**
 * @brief The base a hex number is written in, whose digits hex_value() reads.
 */
constexpr int hex_base{16};

/**
 * @brief The last scalar there is, the ceiling of a hex escape and of every class over scalars.
 */
constexpr char32_t last_scalar{0x10FFFF};

/**
 * @brief The first surrogate: the code points UTF-16 spends on pairs, which no character is and no UTF-8 input decodes
 *        to, run from it to last_surrogate.
 */
constexpr char32_t first_surrogate{0xD800};

/**
 * @brief The last surrogate.
 */
constexpr char32_t last_surrogate{0xDFFF};

/**
 * @brief What opens a line comment in C, C++ and Rust.
 */
constexpr std::string_view line_comment_opener{"//"};

/**
 * @brief What opens a block comment in C, C++ and Rust.
 */
constexpr std::string_view comment_opener{"/*"};

/**
 * @brief What closes a block comment in C, C++ and Rust.
 */
constexpr std::string_view comment_closer{"*/"};

/**
 * @brief Returns a run of bytes as the parser's quoted literal, the quote and the backslash escaped and every byte that
 *        does not print written as the bracket member would be.
 * @param bytes The run.
 * @return The text, quotes included.
 */
[[nodiscard]] std::string quoted(std::string_view bytes);

/**
 * @brief Returns the parser's reference to a definition, its name in braces.
 * @param name The definition's name.
 * @return The reference.
 */
[[nodiscard]] std::string reference(std::string_view name);

/**
 * @brief Returns a set of bytes as the parser's bracket expression, its runs written as ranges.
 * @param set The set, not empty.
 * @return The bracket.
 */
[[nodiscard]] std::string bracket(const regex::Set& set);

/**
 * @brief Returns a run of bytes as members of the parser's bracket expression: the one byte alone, two bytes side by
 *        side, and a longer run as a range.
 * @param first The run's first byte.
 * @param last Its last, at least the first.
 * @return The members' text.
 */
[[nodiscard]] std::string bracket_run(unsigned char first, unsigned char last);

/**
 * @brief Returns a run of code points as a member of the parser's bracket expression, each end a code point escape: the
 *        one escape alone, or the two joined by `-`.
 * @param first The run's first code point.
 * @param last Its last, at least the first.
 * @return The member's text.
 */
[[nodiscard]] std::string code_point_member(char32_t first, char32_t last);

/**
 * @brief Returns a byte as a member of the parser's bracket expression: escaped where the bracket syntax would read it
 *        otherwise, a named escape for the newline, tab and return, hex where it does not print.
 * @param byte The byte.
 * @return The member's text.
 */
[[nodiscard]] std::string bracket_member(unsigned char byte);

/**
 * @brief Returns a text with the blanks that end it dropped, what an ANTLR rule's pattern and clause and a logos
 *        argument are kept as.
 * @param text The text.
 * @return The text through its last byte that is no blank.
 */
[[nodiscard]] std::string without_trailing_blanks(std::string text);

/**
 * @brief Returns a delimited text without its two delimiters, its first byte and its last.
 * @param text The text, two bytes or more.
 * @return The bytes between.
 */
[[nodiscard]] std::string_view without_delimiters(std::string_view text) noexcept;

/**
 * @brief Returns how many lines of a text stand before an offset, the zero-based line the offset is on.
 * @param text The text.
 * @param offset The offset, within the text.
 * @return The newlines before the offset.
 */
[[nodiscard]] std::size_t lines_before(std::string_view text, std::size_t offset) noexcept;

/**
 * @brief Returns whether a byte is a blank.
 * @param byte The byte.
 * @return True for a space, a tab, a newline or a carriage return.
 */
[[nodiscard]] constexpr bool is_blank(const char byte) noexcept
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/**
 * @brief Returns whether a byte prints as a mark of its own: ASCII from `!` to `~`, the space left out.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] constexpr bool is_graphic(const unsigned char byte) noexcept
{
    return byte > ' ' && byte < last_ascii;
}

/**
 * @brief Returns whether a byte is printable ASCII, the space among them.
 * @param byte The byte.
 * @return True for the space through `~`.
 */
[[nodiscard]] constexpr bool is_printable(const unsigned char byte) noexcept
{
    return byte >= ' ' && byte < last_ascii;
}

/**
 * @brief Returns whether a byte is a hexadecimal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 9, a to f and A to F.
 */
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

/**
 * @brief Returns whether a byte can begin or continue a name in every generator's file: a letter, a digit or an
 *        underscore.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_name_byte(const char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_';
}

/**
 * @brief Returns whether a scalar is an ASCII letter, the only letters whose case the readers fold.
 * @param scalar The scalar.
 * @return True for a to z and A to Z.
 */
[[nodiscard]] constexpr bool is_letter(const char32_t scalar) noexcept
{
    return (scalar >= 'a' && scalar <= 'z') || (scalar >= 'A' && scalar <= 'Z');
}

/**
 * @brief Returns whether a byte is a decimal digit.
 * @param byte The byte.
 * @return True for 0 to 9.
 */
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Returns whether a byte is an octal digit.
 * @param byte The byte.
 * @return True for 0 to 7.
 */
[[nodiscard]] constexpr bool is_octal_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '7';
}

/**
 * @brief Returns whether a byte can begin a name in every generator's file: a letter or an underscore.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_name_start(const char byte) noexcept
{
    return is_letter(static_cast<unsigned char>(byte)) || byte == '_';
}

/**
 * @brief Returns whether a word begins as a name does, with a letter or an underscore.
 * @param word The word.
 * @return True when it does, false for the empty word.
 */
[[nodiscard]] constexpr bool starts_name(const std::string_view word) noexcept
{
    return !word.empty() && is_name_start(word.front());
}

/**
 * @brief Returns whether a byte can begin or continue a word of Rust, C or C++: a letter, a digit, an underscore or a
 *        byte beyond ASCII, which is part of a UTF-8 identifier.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_word_byte(const char byte) noexcept
{
    return is_digit(byte) || is_letter(static_cast<unsigned char>(byte)) || byte == '_' ||
           static_cast<unsigned char>(byte) > last_ascii;
}

/**
 * @brief Returns the value of a hexadecimal digit.
 * @param byte The digit.
 * @return Its value.
 */
[[nodiscard]] constexpr unsigned hex_value(const char byte) noexcept
{
    return static_cast<unsigned>(is_digit(byte) ? byte - '0' : (byte | case_bit) - 'a' + 10);
}

/**
 * @brief Returns a decimal with one more digit written after it, when that fits a std::size_t.
 * @param value The decimal so far.
 * @param digit The digit's value.
 * @return Ten times the value plus the digit, or std::nullopt when that would not fit.
 */
[[nodiscard]] constexpr std::optional<std::size_t> appended_digit(
        const std::size_t value, const std::size_t digit) noexcept
{
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
    {
        return std::nullopt;
    }

    return value * 10 + digit;
}

/**
 * @brief Returns whether a scalar is a surrogate.
 * @param scalar The scalar.
 * @return True when it is.
 */
[[nodiscard]] constexpr bool is_surrogate(const char32_t scalar) noexcept
{
    return scalar >= first_surrogate && scalar <= last_surrogate;
}

/**
 * @brief Returns whether a byte continues a UTF-8 sequence rather than beginning one.
 * @param byte The byte.
 * @return True for a continuation byte, `10xxxxxx`.
 */
[[nodiscard]] constexpr bool is_continuation(const unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}

/**
 * @brief Returns how many bytes the UTF-8 sequence a lead byte begins holds, as its high bits say; a continuation byte
 *        read as a lead is taken to begin two.
 * @param lead The lead byte.
 * @return One to four.
 */
[[nodiscard]] constexpr std::size_t sequence_length(const unsigned char lead) noexcept
{
    return lead < 0x80U ? 1 : lead >= 0xF0U ? 4 : lead >= 0xE0U ? 3 : 2;
}

/**
 * @brief Returns the bits of a scalar a lead byte carries, the length marker dropped.
 * @param lead The lead byte.
 * @return The bits, which the continuation bytes' six each follow.
 */
[[nodiscard]] constexpr char32_t lead_bits(const unsigned char lead) noexcept
{
    const auto length{sequence_length(lead)};

    return length == 1 ? lead : lead & (0xFFU >> (length + 1));
}

/**
 * @brief Returns the bits of a scalar read so far with the six a continuation byte carries appended below them.
 * @param scalar The bits so far, lead_bits() of the lead byte and those of the continuation bytes before this one.
 * @param continuation The continuation byte.
 * @return The bits with the continuation's appended.
 */
[[nodiscard]] constexpr char32_t continued(const char32_t scalar, const unsigned char continuation) noexcept
{
    return (scalar << 6U) | (continuation & 0x3FU);
}

/**
 * @brief Returns the length of the well-formed UTF-8 sequence at an index, zero when none stands there: an ASCII byte,
 *        or a lead byte of the shape 110xxxxx, 1110xxxx or 11110xxx followed by as many continuation bytes as its shape
 *        says, the overlong, surrogate and beyond-U+10FFFF encodings excluded as the standard and Rust's
 *        `std::str::from_utf8` exclude them, which the second byte's narrower range after a lead of E0, ED, F0 or F4
 *        rules out.
 * @param text The text.
 * @param at The index, within the text.
 * @return One to four, or zero.
 */
[[nodiscard]] constexpr std::size_t well_formed_length(const std::string_view text, const std::size_t at) noexcept
{
    const auto lead{static_cast<unsigned char>(text[at])};

    const auto length{
            lead < 0x80U                  ? 1UZ :
            lead >= 0xC2U && lead < 0xE0U ? 2UZ :
            lead >= 0xE0U && lead < 0xF0U ? 3UZ :
            lead >= 0xF0U && lead < 0xF5U ? 4UZ :
                                            0UZ};

    if (length == 0 || at + length > text.size())
    {
        return 0;
    }

    for (std::size_t index{1}; index < length; ++index)
    {
        if (!is_continuation(static_cast<unsigned char>(text[at + index])))
        {
            return 0;
        }
    }

    const auto second{static_cast<unsigned char>(text[at + (length > 1 ? 1 : 0)])};

    const auto malformed{
            (lead == 0xE0U && second < 0xA0U) || (lead == 0xEDU && second >= 0xA0U) ||
            (lead == 0xF0U && second < 0x90U) || (lead == 0xF4U && second >= 0x90U)};

    return malformed ? 0 : length;
}

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

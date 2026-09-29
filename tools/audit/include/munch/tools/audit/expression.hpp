#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "munch/regex/set.hpp"

/**
 * @brief What every reader writes for the pattern parser, a scalar's UTF-8, encoded(), a quoted literal, quoted(), a
 *        bracket, bracket(), and one member of it, bracket_member(); a text without its trailing blanks,
 *        without_trailing_blanks(); and what the readers share to read bytes with, the byte tests, each made directly
 *        so that no locale is consulted, the scalar bounds and the UTF-8 measures.
 */
namespace munch::tools::audit
{
/**
 * @brief A scalar's UTF-8 encoding, which is what a character-level generator's literal stands for over bytes.
 * @param scalar The scalar, at most U+10FFFF.
 * @return Its bytes, one to four.
 */
[[nodiscard]] std::string encoded(char32_t scalar);

/**
 * @brief A run of bytes as the parser's quoted literal, the quote and the backslash escaped and every byte that does
 *        not print written as the bracket member would be.
 * @param bytes The run.
 * @return The text, quotes included.
 */
[[nodiscard]] std::string quoted(std::string_view bytes);

/**
 * @brief A set of bytes as the parser's bracket expression, its runs written as ranges.
 * @param set The set, not empty.
 * @return The bracket.
 */
[[nodiscard]] std::string bracket(const regex::Set& set);

/**
 * @brief A byte as a member of the parser's bracket expression: escaped where the bracket syntax would read it
 *        otherwise, a named escape for the newline, tab and return, hex where it does not print.
 * @param byte The byte.
 * @return The member's text.
 */
[[nodiscard]] std::string bracket_member(unsigned char byte);

/**
 * @brief A text with the blanks that end it dropped, what an ANTLR rule's pattern and clause and a logos argument are
 *        kept as.
 * @param text The text.
 * @return The text through its last byte that is no blank.
 */
[[nodiscard]] std::string without_trailing_blanks(std::string text);

/**
 * @brief Whether a byte is a blank.
 * @param byte The byte.
 * @return True for a space, a tab, a newline or a carriage return.
 */
[[nodiscard]] constexpr bool is_blank(const char byte) noexcept
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/**
 * @brief Whether a byte is a hexadecimal digit, tested directly so no locale is consulted.
 * @param byte The byte.
 * @return True for 0 to 9, a to f and A to F.
 */
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept
{
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

/**
 * @brief Whether a byte can begin or continue a name in every generator's file: a letter, a digit or an underscore.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_name_byte(const char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_';
}

/**
 * @brief Whether a scalar is an ASCII letter, the only letters whose case the readers fold.
 * @param scalar The scalar.
 * @return True for a to z and A to Z.
 */
[[nodiscard]] constexpr bool is_letter(const char32_t scalar) noexcept
{
    return (scalar >= 'a' && scalar <= 'z') || (scalar >= 'A' && scalar <= 'Z');
}

/**
 * @brief Whether a byte is a decimal digit.
 * @param byte The byte.
 * @return True for 0 to 9.
 */
[[nodiscard]] constexpr bool is_digit(const char byte) noexcept
{
    return byte >= '0' && byte <= '9';
}

/**
 * @brief Whether a byte can begin a name in every generator's file: a letter or an underscore.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_name_start(const char byte) noexcept
{
    return is_letter(static_cast<unsigned char>(byte)) || byte == '_';
}

/**
 * @brief Whether a word begins as a name does, with a letter or an underscore.
 * @param word The word.
 * @return True when it does, false for the empty word.
 */
[[nodiscard]] constexpr bool starts_name(const std::string_view word) noexcept
{
    return !word.empty() && is_name_start(word.front());
}

/**
 * @brief Whether a byte can begin or continue a word of Rust, C or C++: a letter, a digit, an underscore or a byte
 *        beyond ASCII, which is part of a UTF-8 identifier.
 * @param byte The byte.
 * @return True when it can.
 */
[[nodiscard]] constexpr bool is_word_byte(const char byte) noexcept
{
    return is_digit(byte) || is_letter(static_cast<unsigned char>(byte)) || byte == '_' ||
           static_cast<unsigned char>(byte) >= 0x80;
}

/**
 * @brief The value of a hexadecimal digit.
 * @param byte The digit.
 * @return Its value.
 */
[[nodiscard]] constexpr unsigned hex_value(const char byte) noexcept
{
    return static_cast<unsigned>(is_digit(byte) ? byte - '0' : (byte | 0x20) - 'a' + 10);
}

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
 * @brief Whether a scalar is a surrogate.
 * @param scalar The scalar.
 * @return True when it is.
 */
[[nodiscard]] constexpr bool is_surrogate(const char32_t scalar) noexcept
{
    return scalar >= first_surrogate && scalar <= last_surrogate;
}

/**
 * @brief Whether a byte continues a UTF-8 sequence rather than beginning one.
 * @param byte The byte.
 * @return True for a continuation byte, `10xxxxxx`.
 */
[[nodiscard]] constexpr bool is_continuation(const unsigned char byte) noexcept
{
    return (byte & 0xC0U) == 0x80U;
}

/**
 * @brief How many bytes the UTF-8 sequence a lead byte begins holds, as its high bits say; a continuation byte read as
 *        a lead is taken to begin two.
 * @param lead The lead byte.
 * @return One to four.
 */
[[nodiscard]] constexpr std::size_t sequence_length(const unsigned char lead) noexcept
{
    return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
}

/**
 * @brief The bits of a scalar a lead byte carries, the length marker dropped.
 * @param lead The lead byte.
 * @return The bits, which the continuation bytes' six each follow.
 */
[[nodiscard]] constexpr char32_t lead_bits(const unsigned char lead) noexcept
{
    const auto length{sequence_length(lead)};

    return length == 1 ? lead : lead & (0xFFU >> (length + 1));
}

/**
 * @brief The length of the well-formed UTF-8 sequence at an index, zero when none stands there: an ASCII byte, or a
 *        lead byte of the shape 110xxxxx, 1110xxxx or 11110xxx followed by as many continuation bytes as its shape
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
            lead < 0x80                 ? 1UZ :
            lead >= 0xC2 && lead < 0xE0 ? 2UZ :
            lead >= 0xE0 && lead < 0xF0 ? 3UZ :
            lead >= 0xF0 && lead < 0xF5 ? 4UZ :
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
            (lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) || (lead == 0xF0 && second < 0x90) ||
            (lead == 0xF4 && second >= 0x90)};

    return malformed ? 0 : length;
}

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

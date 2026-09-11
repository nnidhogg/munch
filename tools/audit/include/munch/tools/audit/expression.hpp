#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

#include <string>
#include <string_view>

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
 * @brief A byte as a member of the parser's bracket expression: escaped where the bracket syntax would read it
 *        otherwise, a named escape for the newline, tab and return, hex where it does not print.
 * @param byte The byte.
 * @return The member's text.
 */
[[nodiscard]] std::string bracket_member(unsigned char byte);

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

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_EXPRESSION_HPP

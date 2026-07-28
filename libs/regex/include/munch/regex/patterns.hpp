#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PATTERNS_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PATTERNS_HPP

#include "munch/regex/regex.hpp"

namespace munch::regex::patterns
{
/**
 * @brief Creates a regex matching a C-family identifier.
 *
 * A letter or underscore, followed by any number of letters, digits, or underscores.
 * @return The created regex.
 */
[[nodiscard]] Regex identifier();

/**
 * @brief Creates a regex matching a decimal integer literal.
 *
 * One or more digits. No sign: unary `+`/`-` is a parser-level concern, not a lexeme.
 * @return The created regex.
 */
[[nodiscard]] Regex decimal_integer();

/**
 * @brief Creates a regex matching a decimal floating-point literal.
 *
 * One or more digits, a decimal point, then one or more digits. No sign or exponent part.
 * @return The created regex.
 */
[[nodiscard]] Regex decimal_float();

} // namespace munch::regex::patterns

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_PATTERNS_HPP

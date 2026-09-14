#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP

#include <span>
#include <string_view>

#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"

namespace munch::regex::unicode
{
/**
 * @brief Creates a regex matching one code point holding the XID_Start property, encoded as UTF-8.
 *
 * XID_Start is the Unicode property defining the first character of an identifier; see UAX #31. Note that
 * underscore does not hold it, so a C-style identifier head is spelled choice(text('_'), unicode::xid_start()).
 * The property tables are generated from the Unicode Character Database pinned at version(); the properties are
 * applied as-is, with no normalization, so input in a different normalization form is the caller's to normalize.
 * @return The created regex.
 */
[[nodiscard]] Regex xid_start();

/**
 * @brief Creates a regex matching one code point holding the XID_Continue property, encoded as UTF-8.
 *
 * XID_Continue is the Unicode property defining every character of an identifier after the first: it holds
 * XID_Start entirely, plus underscore, digits, combining marks, and the other continuation characters of UAX #31.
 * @return The created regex.
 */
[[nodiscard]] Regex xid_continue();

/**
 * @brief Creates a regex matching one decimal digit, a code point of general category Nd, encoded as UTF-8.
 *
 * This is the class the regex crate gives \d in Unicode mode. The ASCII digits are its first row, so a scanner
 * that took [0-9] before takes the same bytes from this class plus the other scripts' digits.
 * @return The created regex.
 */
[[nodiscard]] Regex decimal_digit();

/**
 * @brief Creates a regex matching one code point holding the White_Space property, encoded as UTF-8.
 *
 * This is the class the regex crate gives \s in Unicode mode: the ASCII whitespace [\t-\r ], the next line
 * character U+0085, the no-break spaces and the Unicode line and paragraph separators among the rest.
 * @return The created regex.
 */
[[nodiscard]] Regex white_space();

/**
 * @brief Creates a regex matching one word character, encoded as UTF-8.
 *
 * This is the class the regex crate gives \w in Unicode mode, the union of Alphabetic, the mark categories M, the
 * decimal digits Nd, the connector punctuation Pc and Join_Control; [0-9A-Za-z_] is its ASCII row.
 * @return The created regex.
 */
[[nodiscard]] Regex word();

/**
 * @brief The generated classes, for a caller that needs their code point ranges rather than a regex over them.
 */
enum class Property
{
    xid_start,
    xid_continue,
    decimal_digit,
    white_space,
    word
};

/**
 * @brief The code point ranges of a property table, sorted ascending, pairwise disjoint and non-adjacent.
 *
 * The same tables the regex builders above expand; a reader that assembles its own classes over code points, and
 * then rewrites them over the UTF-8 bytes by its own machinery, takes the ranges from here so that the two never
 * disagree about what a Unicode escape matches.
 * @param property The table.
 * @return A view of the ranges, valid for the life of the program.
 */
[[nodiscard]] std::span<const utf8::Code_point_range> ranges(Property property) noexcept;

/**
 * @brief The Unicode version the property tables were generated from, such as "17.0.0".
 */
[[nodiscard]] std::string_view version() noexcept;

} // namespace munch::regex::unicode

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP

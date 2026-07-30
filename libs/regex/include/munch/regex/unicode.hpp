#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP

#include <string_view>

#include "munch/regex/regex.hpp"

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
 * @brief The Unicode version the XID property tables were generated from, such as "17.0.0".
 */
[[nodiscard]] std::string_view version() noexcept;

} // namespace munch::regex::unicode

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UNICODE_HPP

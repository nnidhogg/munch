#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_UTF8_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_UTF8_HPP

#include "lexer/regex/regex.hpp"

namespace lexer::regex::utf8
{
/**
 * @brief Creates a regex matching one code point from an inclusive range, encoded as UTF-8.
 *
 * The range is expanded into the byte sequences of its code points, so the automaton stays byte-oriented and no part
 * of the engine needs to know about wide characters. Surrogates (U+D800 to U+DFFF) are excluded from the range, and
 * ill-formed input such as overlong encodings, stray continuation bytes, or code points beyond U+10FFFF is rejected
 * by construction.
 * @param first The first code point of the range.
 * @param last The last code point of the range, at most U+10FFFF.
 * @return The created regex.
 * @throws std::invalid_argument If the range is empty, exceeds U+10FFFF, or holds only surrogates.
 */
[[nodiscard]] Regex range(char32_t first, char32_t last);

} // namespace lexer::regex::utf8

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_UTF8_HPP

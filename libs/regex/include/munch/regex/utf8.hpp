#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UTF8_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UTF8_HPP

#include <span>

#include "munch/regex/regex.hpp"

namespace munch::regex::utf8
{
/**
 * @brief One inclusive code point range of a larger class, for matching through ranges().
 */
struct Code_point_range
{
    char32_t first;

    char32_t last;
};

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

/**
 * @brief Creates a regex matching one code point from any of the given ranges, each encoded as UTF-8.
 *
 * The single construction seam for generated classes such as the XID properties: the ranges are validated as
 * sorted and disjoint, adjacent ranges are merged, and each surviving range expands through range(), so future
 * expansion improvements apply to every generated class in one place.
 * @param ranges The ranges to match, sorted ascending and pairwise disjoint.
 * @return The created regex.
 * @throws std::invalid_argument If the ranges are empty, unsorted, overlapping, or invalid for range().
 */
[[nodiscard]] Regex ranges(std::span<const Code_point_range> ranges);

} // namespace munch::regex::utf8

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_UTF8_HPP

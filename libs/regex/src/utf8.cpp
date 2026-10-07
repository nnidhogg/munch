#include "munch/regex/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/regex/set.hpp"

namespace munch::regex::utf8
{
namespace
{
/**
 * @brief A UTF-8 encoded code point, as up to four bytes.
 */
using Bytes_t = std::array<unsigned char, 4>;

/**
 * @brief A run of code points whose encodings share one length, i.e. one row of the UTF-8 table.
 */
struct Block
{
    /**
     * @brief The first code point of the block.
     */
    char32_t first{};

    /**
     * @brief The last code point of the block.
     */
    char32_t last{};
};

/**
 * @brief The largest Unicode scalar value.
 */
constexpr char32_t max_scalar{0x10FFFF};

/**
 * @brief The first code point of the surrogate gap, which no scalar value occupies.
 */
constexpr char32_t surrogate_first{0xD800};

/**
 * @brief The last code point of the surrogate gap.
 */
constexpr char32_t surrogate_last{0xDFFF};

/**
 * @brief The smallest UTF-8 continuation byte.
 */
constexpr unsigned char continuation_first{0x80};

/**
 * @brief The largest UTF-8 continuation byte.
 */
constexpr unsigned char continuation_last{0xBF};

/**
 * @brief The encodable code point space, split by encoding length and around the surrogate gap.
 */
constexpr std::array<Block, 5> blocks{{
        {.first = 0x0, .last = 0x7F},
        {.first = 0x80, .last = 0x7FF},
        {.first = 0x800, .last = surrogate_first - 1},
        {.first = surrogate_last + 1, .last = 0xFFFF},
        {.first = 0x10000, .last = max_scalar},
}};

/**
 * @brief Returns the UTF-8 encoding length of a code point in bytes.
 * @param code_point The code point to measure.
 * @return The number of bytes its UTF-8 encoding occupies, from 1 to 4.
 */
[[nodiscard]] std::size_t length(const char32_t code_point)
{
    if (code_point <= 0x7F)
    {
        return 1;
    }

    if (code_point <= 0x7FF)
    {
        return 2;
    }

    if (code_point <= 0xFFFF)
    {
        return 3;
    }

    return 4;
}

/**
 * @brief Returns the bytes of a code point's encoding as a fixed array.
 * @param encoding The UTF-8 encoding of a code point, one to four bytes.
 * @return Its bytes; entries past the encoding length are zero.
 */
[[nodiscard]] Bytes_t padded(const std::string_view encoding)
{
    Bytes_t bytes{};

    std::ranges::copy(encoding, bytes.begin());

    return bytes;
}

/**
 * @brief Creates a regex matching the encodings from `first` to `last`, both of the given length, from `index` on.
 *
 * Bytes equal in both bounds are matched literally. At the first byte where the bounds diverge, the range splits into
 * three: encodings keeping the lower bound's byte, encodings keeping the upper bound's byte, and the bytes between them
 * followed by unconstrained continuation bytes.
 * @param first The bytes of the encoding at the start of the range.
 * @param last The bytes of the encoding at the end of the range.
 * @param index The byte position, from 0, to start matching from.
 * @param length The shared encoding length of `first` and `last`, in bytes.
 * @return The created regex.
 */
[[nodiscard]] Regex sequence(
        const Bytes_t& first, const Bytes_t& last, const std::size_t index, const std::size_t length)
{
    if (index + 1 == length)
    {
        return any_of(Set::range(first[index], last[index]));
    }

    if (first[index] == last[index])
    {
        auto rest{sequence(first, last, index + 1, length)};

        auto lead{any_of(Set::from(static_cast<char>(first[index])))};

        return concat(std::move(lead), std::move(rest));
    }

    Bytes_t highest{};

    highest.fill(continuation_last);

    std::vector<Regex> parts{};

    auto above_first{sequence(first, highest, index + 1, length)};

    auto first_lead{any_of(Set::from(static_cast<char>(first[index])))};

    auto keeps_first{concat(std::move(first_lead), std::move(above_first))};

    parts.push_back(std::move(keeps_first));

    if (first[index] + 1U <= last[index] - 1U)
    {
        std::vector<Regex> middle{};

        const auto between{Set::range(static_cast<char>(first[index] + 1U), static_cast<char>(last[index] - 1U))};

        middle.push_back(any_of(between));

        const auto continuation{
                Set::range(static_cast<char>(continuation_first), static_cast<char>(continuation_last))};

        for (auto position{index + 1}; position < length; ++position)
        {
            middle.push_back(any_of(continuation));
        }

        parts.push_back({.node = Concat{.regexes = std::move(middle)}});
    }

    Bytes_t lowest{};

    lowest.fill(continuation_first);

    auto below_last{sequence(lowest, last, index + 1, length)};

    auto last_lead{any_of(Set::from(static_cast<char>(last[index])))};

    auto keeps_last{concat(std::move(last_lead), std::move(below_last))};

    parts.push_back(std::move(keeps_last));

    return {.node = Choice{.regexes = std::move(parts)}};
}

/**
 * @brief Returns the one regex of a list, or the choice between them when there are several.
 * @param parts The regexes, at least one.
 * @return The regex.
 */
[[nodiscard]] Regex one_or_choice(std::vector<Regex> parts)
{
    if (parts.size() == 1)
    {
        return std::move(parts.front());
    }

    return {.node = Choice{.regexes = std::move(parts)}};
}

} // namespace

Regex range(const char32_t first, const char32_t last)
{
    if (first > last || last > max_scalar)
    {
        throw std::invalid_argument{"Invalid UTF-8 code point range"};
    }

    std::vector<Regex> parts{};

    for (const auto& [block_first, block_last] : blocks)
    {
        const auto low{std::max(first, block_first)};

        const auto high{std::min(last, block_last)};

        if (low > high)
        {
            continue;
        }

        const auto low_bytes{padded(encode(low))};

        const auto high_bytes{padded(encode(high))};

        const auto encoding_length{length(low)};

        parts.push_back(sequence(low_bytes, high_bytes, 0, encoding_length));
    }

    if (parts.empty())
    {
        throw std::invalid_argument{"UTF-8 code point range holds only surrogates"};
    }

    return one_or_choice(std::move(parts));
}

Regex ranges(const std::span<const Code_point_range> ranges)
{
    if (ranges.empty())
    {
        throw std::invalid_argument{"UTF-8 code point ranges are empty"};
    }

    // Each range is validated before adjacency merging: a surrogate-only range must be rejected, not silently absorbed
    // into a neighbor whose expansion then excises the surrogate gap.
    for (const auto& [first, last] : ranges)
    {
        if (first > last || last > max_scalar || (first >= surrogate_first && last <= surrogate_last))
        {
            throw std::invalid_argument{"Invalid UTF-8 code point range"};
        }
    }

    std::vector<Regex> parts{};

    parts.reserve(ranges.size());

    auto current{ranges.front()};

    const auto flush{[&parts, &current] { parts.push_back(range(current.first, current.last)); }};

    for (const auto& [first, last] : ranges.subspan(1))
    {
        if (first <= current.last)
        {
            throw std::invalid_argument{"UTF-8 code point ranges are unsorted or overlapping"};
        }

        if (first == current.last + 1)
        {
            current.last = last;

            continue;
        }

        flush();

        current = {.first = first, .last = last};
    }

    flush();

    return one_or_choice(std::move(parts));
}

std::string encode(const char32_t code_point)
{
    switch (std::string bytes{}; length(code_point))
    {
    case 1:
        bytes.push_back(static_cast<char>(code_point));

        return bytes;
    case 2:
        bytes.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));

        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));

        return bytes;
    case 3:
        bytes.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));

        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));

        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));

        return bytes;
    default:
        bytes.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));

        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));

        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));

        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));

        return bytes;
    }
}

} // namespace munch::regex::utf8

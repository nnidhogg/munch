#include "munch/regex/utf8.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

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
    char32_t first;

    /**
     * @brief The last code point of the block.
     */
    char32_t last;
};

/**
 * @brief The encodable code point space, split by encoding length and around the surrogate gap.
 */
constexpr std::array<Block, 5> blocks{{
        {.first = 0x0, .last = 0x7F},
        {.first = 0x80, .last = 0x7FF},
        {.first = 0x800, .last = 0xD7FF},
        {.first = 0xE000, .last = 0xFFFF},
        {.first = 0x10000, .last = 0x10FFFF},
}};

/**
 * @brief Returns the UTF-8 encoding length of a code point in bytes.
 * @param code_point The code point to measure.
 * @return The number of bytes its UTF-8 encoding occupies, from 1 to 4.
 */
std::size_t length(const char32_t code_point)
{
    return code_point <= 0x7F ? 1 : code_point <= 0x7FF ? 2 : code_point <= 0xFFFF ? 3 : 4;
}

/**
 * @brief Encodes a code point as UTF-8.
 * @param code_point The code point to encode.
 * @return The encoded bytes; entries past the encoding length are zero.
 */
Bytes_t encode(const char32_t code_point)
{
    switch (Bytes_t bytes{}; length(code_point))
    {
    case 1:
        bytes[0] = static_cast<unsigned char>(code_point);

        return bytes;
    case 2:
        bytes[0] = static_cast<unsigned char>(0xC0 | (code_point >> 6U));
        bytes[1] = static_cast<unsigned char>(0x80 | (code_point & 0x3FU));

        return bytes;
    case 3:
        bytes[0] = static_cast<unsigned char>(0xE0 | (code_point >> 12U));
        bytes[1] = static_cast<unsigned char>(0x80 | ((code_point >> 6U) & 0x3FU));
        bytes[2] = static_cast<unsigned char>(0x80 | (code_point & 0x3FU));

        return bytes;
    default:
        bytes[0] = static_cast<unsigned char>(0xF0 | (code_point >> 18U));
        bytes[1] = static_cast<unsigned char>(0x80 | ((code_point >> 12U) & 0x3FU));
        bytes[2] = static_cast<unsigned char>(0x80 | ((code_point >> 6U) & 0x3FU));
        bytes[3] = static_cast<unsigned char>(0x80 | (code_point & 0x3FU));

        return bytes;
    }
}

/**
 * @brief Creates a regex matching the encodings from `first` to `last`, both of the given length, from `index` on.
 *
 * Bytes equal in both bounds are matched literally. At the first byte where the bounds diverge, the range splits
 * into three: encodings keeping the lower bound's byte, encodings keeping the upper bound's byte, and the bytes
 * between them followed by unconstrained continuation bytes.
 * @param first The bytes of the encoding at the start of the range.
 * @param last The bytes of the encoding at the end of the range.
 * @param index The byte position, from 0, to start matching from.
 * @param length The shared encoding length of `first` and `last`, in bytes.
 * @return The created regex.
 */
Regex sequence(const Bytes_t& first, const Bytes_t& last, const std::size_t index, const std::size_t length)
{
    if (index + 1 == length)
    {
        return any_of(Set::range(first[index], last[index]));
    }

    if (first[index] == last[index])
    {
        return concat(any_of(Set::range(first[index], first[index])), sequence(first, last, index + 1, length));
    }

    constexpr unsigned char continuation_first{0x80};
    constexpr unsigned char continuation_last{0xBF};

    Bytes_t lowest;
    lowest.fill(continuation_first);

    Bytes_t highest;
    highest.fill(continuation_last);

    std::vector<Regex> parts;

    parts.push_back(
            concat(any_of(Set::range(first[index], first[index])), sequence(first, highest, index + 1, length)));

    if (first[index] + 1U <= last[index] - 1U)
    {
        std::vector<Regex> middle;

        middle.push_back(any_of(Set::range(first[index] + 1U, last[index] - 1U)));

        for (auto position{index + 1}; position < length; ++position)
        {
            middle.push_back(any_of(Set::range(continuation_first, continuation_last)));
        }

        parts.push_back({.node = Concat{.regexes = std::move(middle)}});
    }

    parts.push_back(concat(any_of(Set::range(last[index], last[index])), sequence(lowest, last, index + 1, length)));

    return {.node = Choice{.regexes = std::move(parts)}};
}

} // namespace

Regex range(const char32_t first, const char32_t last)
{
    if (first > last || last > 0x10FFFF)
    {
        throw std::invalid_argument("Invalid UTF-8 code point range");
    }

    std::vector<Regex> parts;

    for (const auto& block : blocks)
    {
        const auto low{std::max(first, block.first)};
        const auto high{std::min(last, block.last)};

        if (low <= high)
        {
            parts.push_back(sequence(encode(low), encode(high), 0, length(low)));
        }
    }

    if (parts.empty())
    {
        throw std::invalid_argument("UTF-8 code point range holds only surrogates");
    }

    return parts.size() == 1 ? std::move(parts.front()) : Regex{.node = Choice{.regexes = std::move(parts)}};
}

Regex ranges(const std::span<const Code_point_range> ranges)
{
    if (ranges.empty())
    {
        throw std::invalid_argument("UTF-8 code point ranges are empty");
    }

    std::vector<Regex> parts;

    parts.reserve(ranges.size());

    auto current{ranges.front()};

    const auto flush{[&parts, &current] { parts.push_back(range(current.first, current.last)); }};

    for (const auto& next : ranges.subspan(1))
    {
        if (next.first <= current.last)
        {
            throw std::invalid_argument("UTF-8 code point ranges are unsorted or overlapping");
        }

        if (next.first == current.last + 1)
        {
            current.last = next.last;
        }
        else
        {
            flush();

            current = next;
        }
    }

    flush();

    return parts.size() == 1 ? std::move(parts.front()) : Regex{.node = Choice{.regexes = std::move(parts)}};
}

} // namespace munch::regex::utf8

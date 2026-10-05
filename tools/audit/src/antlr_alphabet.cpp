#include "munch/tools/audit/antlr_alphabet.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Returns the runs of consecutive bytes a set of ASCII bytes holds, in order.
 * @param ascii The bytes.
 * @return Each run's first and last byte.
 */
[[nodiscard]] std::vector<Scalar_range> runs(const Ascii_t& ascii)
{
    std::vector<Scalar_range> found{};

    for (std::size_t byte{0}; byte < ascii.size();)
    {
        if (!ascii.test(byte))
        {
            ++byte;

            continue;
        }

        auto end{byte};

        while (end + 1 < ascii.size() && ascii.test(end + 1))
        {
            ++end;
        }

        found.push_back({.first = static_cast<char32_t>(byte), .last = static_cast<char32_t>(end)});

        byte = end + 1;
    }

    return found;
}

} // namespace

void Beginning::join(const Beginning& other) noexcept
{
    ascii |= other.ascii;

    beyond = beyond || other.beyond;
}

bool Beginning::overlaps(const Beginning& other) const noexcept
{
    return (ascii & other.ascii).any() || (beyond && other.beyond);
}

bool holds_beyond_ascii(const std::string_view bytes)
{
    const auto beyond_ascii{[](const char one) { return static_cast<unsigned char>(one) > last_ascii; }};

    return std::ranges::any_of(bytes, beyond_ascii);
}

std::optional<char32_t> decoded(const std::string_view bytes)
{
    if (bytes.empty())
    {
        return std::nullopt;
    }

    const auto lead{static_cast<unsigned char>(bytes.front())};

    if (bytes.size() != sequence_length(lead))
    {
        return std::nullopt;
    }

    auto scalar{lead_bits(lead)};

    for (const auto byte : bytes.substr(1))
    {
        scalar = continued(scalar, static_cast<unsigned char>(byte));
    }

    return scalar;
}

std::string step(const Alphabet& alphabet)
{
    if (alphabet.beyond.empty())
    {
        return bracket(alphabet.ascii);
    }

    std::string out{'['};

    for (const auto& [first, last] : runs(alphabet.ascii))
    {
        out += code_point_member(first, last);
    }

    for (const auto& [first, last] : alphabet.beyond)
    {
        out += code_point_member(first, last);
    }

    return out + ']';
}

std::string bracket(const Ascii_t& ascii)
{
    std::string out{'['};

    for (const auto& [first, last] : runs(ascii))
    {
        out += bracket_run(static_cast<unsigned char>(first), static_cast<unsigned char>(last));
    }

    return out + ']';
}

Alphabet complement(const Alphabet& alphabet)
{
    auto beyond{alphabet.beyond};

    std::ranges::sort(beyond, {}, &Scalar_range::first);

    std::vector<Scalar_range> rest{};

    char32_t from{last_ascii + 1};

    for (const auto& [first, last] : beyond)
    {
        if (first > from)
        {
            rest.push_back({.first = from, .last = first - 1});
        }

        from = std::max(from, static_cast<char32_t>(last + 1));
    }

    if (from <= last_scalar)
    {
        rest.push_back({.first = from, .last = last_scalar});
    }

    return {.ascii = ~alphabet.ascii, .beyond = std::move(rest)};
}

std::string caseless(const std::string_view bytes)
{
    std::string out{};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (is_letter(value))
        {
            const auto lower{static_cast<char>(byte | case_bit)};

            const auto upper{static_cast<char>(byte & ~case_bit)};

            out += std::format("[{}{}]", lower, upper);
        }
        else
        {
            out += std::format("[{}]", bracket_member(value));
        }
    }

    return out;
}

Alphabet spanning(const char32_t low, const char32_t high, const bool case_insensitive)
{
    Alphabet alphabet{};

    admit(alphabet, low, high, case_insensitive);

    return alphabet;
}

void admit(Alphabet& alphabet, const char32_t first, const char32_t last, const bool case_insensitive)
{
    const auto add{[&alphabet](const char32_t low, const char32_t high) {
        const auto ascii_high{std::min(high, last_ascii)};

        for (auto value{low}; value <= ascii_high; ++value)
        {
            alphabet.ascii.set(value);
        }

        if (high > last_ascii)
        {
            const auto beyond_low{std::max(low, char32_t{last_ascii + 1})};

            alphabet.beyond.push_back({.first = beyond_low, .last = high});
        }
    }};

    const auto lower{[](const char32_t value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char32_t>(value | case_bit) : value;
    }};

    const auto upper{[](const char32_t value) {
        return value >= 'a' && value <= 'z' ? static_cast<char32_t>(value & ~case_bit) : value;
    }};

    const auto lower_first{lower(first)};

    const auto upper_first{upper(first)};

    const auto lower_last{lower(last)};

    const auto upper_last{upper(last)};

    const auto mixed{(lower_first == first) != (lower_last == last)};

    const auto single{
            (lower_first == upper_first && lower_last == upper_last) || mixed ||
            lower_last + upper_first != upper_last + lower_first};

    if (!case_insensitive || single)
    {
        add(first, last);

        return;
    }

    add(lower_first, lower_last);

    add(upper_first, upper_last);
}

} // namespace munch::tools::audit

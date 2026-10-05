#include "munch/tools/audit/supply.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
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
 * @brief Returns the gaps between consecutive anchors, each percentile the order statistic at floor(q n) capped at the
 *        last.
 * @param anchors The anchor positions, ascending.
 * @return The gaps, std::nullopt when fewer than two anchors leave none.
 */
[[nodiscard]] std::optional<Gaps> gaps_of(const std::vector<std::size_t>& anchors)
{
    if (anchors.size() < 2)
    {
        return std::nullopt;
    }

    const auto gap{[](const std::size_t before, const std::size_t after) { return after - before; }};

    std::vector<std::size_t> gaps{};

    gaps.reserve(anchors.size() - 1);

    std::ranges::copy(anchors | std::views::pairwise_transform(gap), std::back_inserter(gaps));

    std::ranges::sort(gaps);

    const auto at_percentile{[&gaps](const std::size_t percent) {
        const auto rank{std::min(gaps.size() - 1, gaps.size() * percent / 100)};

        return gaps[rank];
    }};

    return Gaps{
            .median = at_percentile(50),
            .ninetieth = at_percentile(90),
            .ninety_ninth = at_percentile(99),
            .longest = gaps.back()};
}

/**
 * @brief Returns the interior positions of the input that stand before one of the certified bytes.
 * @param certified The bytes, ascending.
 * @param input The input.
 * @return Whether each position is anchored.
 */
[[nodiscard]] std::vector<bool> byte_anchors(const std::vector<unsigned char>& certified, const std::string_view input)
{
    std::array<bool, byte_values> is_certified{};

    for (const auto byte : certified)
    {
        is_certified[byte] = true;
    }

    std::vector<bool> anchored(input.size(), false);

    for (std::size_t at{1}; at < input.size(); ++at)
    {
        anchored[at] = is_certified[static_cast<unsigned char>(input[at])];
    }

    return anchored;
}

/**
 * @brief Returns the interior positions of the input that are the origin of an occurrence of a certified window, the
 *        input and the windows matched over the report's byte classes.
 * @param report The report.
 * @param input The input.
 * @return Whether each position is anchored.
 */
[[nodiscard]] std::vector<bool> window_anchors(const Report& report, const std::string_view input)
{
    // Every byte stands for the lowest byte of its class, on both sides of the match.
    std::array<char, byte_values> lowest_of_class{};

    std::ranges::iota(lowest_of_class, '\0');

    for (const auto& members : report.classes)
    {
        for (const auto member : members)
        {
            lowest_of_class[member] = static_cast<char>(members.front());
        }
    }

    const auto lowest{
            [&lowest_of_class](const char byte) { return lowest_of_class[static_cast<unsigned char>(byte)]; }};

    const auto canonicalized{[&lowest](const std::string_view bytes) {
        std::string out{};

        out.reserve(bytes.size());

        std::ranges::transform(bytes, std::back_inserter(out), lowest);

        return out;
    }};

    std::map<std::string, std::size_t, std::less<>> origin_of{};

    std::set<std::size_t> widths{};

    for (const auto& [window, origin] : report.windows)
    {
        origin_of.emplace(canonicalized(window), origin);

        widths.insert(window.size());
    }

    const auto text{canonicalized(input)};

    std::vector<bool> anchored(input.size(), false);

    for (std::size_t at{0}; at < text.size(); ++at)
    {
        for (const auto width : widths)
        {
            if (at + width > text.size())
            {
                break;
            }

            const auto occurrence{std::string_view{text}.substr(at, width)};

            const auto found{origin_of.find(occurrence)};

            if (found == origin_of.end())
            {
                continue;
            }

            const auto& [window, origin]{*found};

            if (at + origin > 0)
            {
                anchored[at + origin] = true;
            }
        }
    }

    return anchored;
}

/**
 * @brief Returns what a set of anchored positions supplies on an input.
 * @param anchored Whether each position of the input is an anchor, the ends never.
 * @return The count, the density and the gaps.
 */
[[nodiscard]] Anchors anchors_of(const std::vector<bool>& anchored)
{
    if (anchored.empty())
    {
        return Anchors{.count = 0, .per_kibibyte = 0.0, .gaps = std::nullopt};
    }

    std::vector<std::size_t> positions{};

    for (std::size_t at{0}; at < anchored.size(); ++at)
    {
        if (anchored[at])
        {
            positions.push_back(at);
        }
    }

    const auto density{1024.0 * static_cast<double>(positions.size()) / static_cast<double>(anchored.size())};

    auto gaps{gaps_of(positions)};

    return Anchors{.count = positions.size(), .per_kibibyte = density, .gaps = std::move(gaps)};
}

/**
 * @brief Returns the figures of one inventory as its JSON object.
 * @param anchors The inventory's supply.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_anchors(const Anchors& anchors)
{
    const auto& [count, per_kibibyte, gaps]{anchors};

    const auto [p50, p90, p99, longest]{gaps.value_or(Gaps{})};

    const auto figure{[&gaps](const std::size_t value) { return gaps ? std::to_string(value) : "null"; }};

    return std::format(
            R"({{"anchors": {}, "per_kibibyte": {:.1f}, "gap_p50": {}, "gap_p90": {}, "gap_p99": {}, "gap_max": {}}})",
            count, per_kibibyte, figure(p50), figure(p90), figure(p99), figure(longest));
}

/**
 * @brief Returns the figures of one inventory as the text row prints them.
 * @param anchors The inventory's supply.
 * @return The row's value.
 */
[[nodiscard]] std::string shown(const Anchors& anchors)
{
    const auto& [count, per_kibibyte, gaps]{anchors};

    const auto [median, ninetieth, ninety_ninth, longest]{gaps.value_or(Gaps{})};

    const auto spread{
            gaps ? std::format("gaps p50 {}, p90 {}, p99 {}, max {}", median, ninetieth, ninety_ninth, longest) :
                   std::string{"no gaps, fewer than two anchors"}};

    return std::format("{} anchor{}, {:.1f} per KiB, {}", count, plural(count), per_kibibyte, spread);
}

} // namespace

Supply supply(const Report& report, const std::string_view input)
{
    const auto exact{byte_anchors(report.exact, input)};

    std::optional<Anchors> windows{};

    if (!report.windows.empty())
    {
        auto with_windows{window_anchors(report, input)};

        for (std::size_t at{0}; at < with_windows.size(); ++at)
        {
            with_windows[at] = with_windows[at] || exact[at];
        }

        windows = anchors_of(with_windows);
    }

    const auto modulo{byte_anchors(report.modulo, input)};

    auto exact_supply{anchors_of(exact)};

    auto modulo_supply{anchors_of(modulo)};

    return Supply{
            .bytes = input.size(),
            .tokenized = std::nullopt,
            .exact = std::move(exact_supply),
            .modulo = std::move(modulo_supply),
            .windows = std::move(windows)};
}

Supply supply(const Report& report, const core::Lexer& lexer, const std::string_view input)
{
    auto measured{supply(report, input)};

    // The supply asks how far the scan gets alone, so it keeps no token.
    const auto ignore_token{[](std::size_t, std::size_t) {}};

    measured.tokenized = lexer.tokenize_all<std::size_t>(input, ignore_token);

    return measured;
}

std::string supply_json(const Supply& supply, const std::string_view path)
{
    const auto& [bytes, tokenized, exact, modulo, windows]{supply};

    const auto tokenized_json{tokenized ? std::to_string(*tokenized) : "null"};

    const auto windows_json{windows ? json_anchors(*windows) : "null"};

    return std::format(
            R"({{"input": {}, "bytes": {}, "tokenized": {}, "exact": {}, "modulo": {}, "windows": {}}})",
            json_string(path), bytes, tokenized_json, json_anchors(exact), json_anchors(modulo), windows_json);
}

std::string supply_section(const Supply& supply, const std::string_view path)
{
    const auto& [bytes, tokenized, exact, modulo, windows]{supply};

    auto out{std::format("\ncertified-anchor supply on {}, {} byte{}\n", path, bytes, plural(bytes))};

    // The condition every certificate's promise carries, stated before the rows that count on it.
    if (tokenized)
    {
        const auto serial{[&tokenized, bytes]() -> std::string {
            if (*tokenized < bytes)
            {
                return std::format(
                        "stops at offset {}, so the rows count occurrences and promise no boundary, the exact byte row "
                        "alone keeping tokenize_all_parallel()'s serial-prefix relation, which the window rows have "
                        "not got",
                        *tokenized);
            }

            return "tokenizes the input completely, so every anchor is a token boundary, the modulo row's once "
                   "discarded tokens are deleted";
        }()};

        out += std::format("  {:<26} {}\n", "serial scan", serial);
    }

    out += std::format("  {:<26} {}\n", "exact bytes", shown(exact));

    out += std::format("  {:<26} {}\n", "modulo discarded bytes", shown(modulo));

    if (windows)
    {
        out += std::format("  {:<26} {}\n", "exact bytes and windows", shown(*windows));
    }

    return out;
}

} // namespace munch::tools::audit

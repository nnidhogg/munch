#include "munch/tools/audit/supply.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The gaps between consecutive anchors, each percentile the order statistic at floor(q n) capped at the last.
 * @param anchors The anchor positions, ascending.
 * @return The gaps, std::nullopt when fewer than two anchors leave none.
 */
[[nodiscard]] std::optional<Gaps> gaps_of(const std::vector<std::size_t>& anchors)
{
    if (anchors.size() < 2)
    {
        return std::nullopt;
    }

    std::vector<std::size_t> gaps;

    gaps.reserve(anchors.size() - 1);

    for (std::size_t index{1}; index < anchors.size(); ++index)
    {
        gaps.push_back(anchors[index] - anchors[index - 1]);
    }

    std::ranges::sort(gaps);

    const auto at{[&gaps](const std::size_t percent) {
        return gaps[std::min(gaps.size() - 1, gaps.size() * percent / 100)];
    }};

    return Gaps{.median = at(50), .ninetieth = at(90), .ninety_ninth = at(99), .longest = gaps.back()};
}

/**
 * @brief What a set of anchored positions supplies on an input.
 * @param anchored Whether each position of the input is an anchor, the ends never.
 * @return The count, the density and the gaps.
 */
[[nodiscard]] Anchors anchors_of(const std::vector<bool>& anchored)
{
    std::vector<std::size_t> positions;

    for (std::size_t at{0}; at < anchored.size(); ++at)
    {
        if (anchored[at])
        {
            positions.push_back(at);
        }
    }

    const auto density{
            anchored.empty() ? 0.0 :
                               1024.0 * static_cast<double>(positions.size()) / static_cast<double>(anchored.size())};

    return Anchors{.count = positions.size(), .per_kibibyte = density, .gaps = gaps_of(positions)};
}

/**
 * @brief The interior positions of the input that stand before one of the certified bytes.
 * @param certified The bytes, ascending.
 * @param input The input.
 * @return Whether each position is anchored.
 */
[[nodiscard]] std::vector<bool> byte_anchors(const std::vector<unsigned char>& certified, const std::string_view input)
{
    std::array<bool, 256> is_certified{};

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
 * @brief The interior positions of the input that are the origin of an occurrence of a certified window, the input
 *        and the windows matched over the report's byte classes.
 * @param report The report.
 * @param input The input.
 * @return Whether each position is anchored.
 */
[[nodiscard]] std::vector<bool> window_anchors(const Report& report, const std::string_view input)
{
    // Every byte stands for the lowest byte of its class, on both sides of the match.
    std::array<char, 256> canonical{};

    for (std::size_t value{0}; value < canonical.size(); ++value)
    {
        canonical[value] = static_cast<char>(value);
    }

    for (const auto& members : report.classes)
    {
        for (const auto member : members)
        {
            canonical[member] = static_cast<char>(members.front());
        }
    }

    const auto canonicalized{[&canonical](const std::string_view bytes) {
        std::string out;

        out.reserve(bytes.size());

        for (const auto byte : bytes)
        {
            out.push_back(canonical[static_cast<unsigned char>(byte)]);
        }

        return out;
    }};

    std::map<std::string, std::size_t, std::less<>> origin_of;

    std::set<std::size_t> widths;

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

            const auto found{origin_of.find(std::string_view{text}.substr(at, width))};

            if (found != origin_of.end() && at + found->second > 0)
            {
                anchored[at + found->second] = true;
            }
        }
    }

    return anchored;
}

/**
 * @brief The figures of one inventory as the text row prints them.
 * @param anchors The inventory's supply.
 * @return The row's value.
 */
[[nodiscard]] std::string shown(const Anchors& anchors)
{
    const auto& [count, per_kibibyte, gaps]{anchors};

    return std::format(
            "{} anchor{}, {:.1f} per KiB, {}", count, count == 1 ? "" : "s", per_kibibyte,
            gaps ? std::format(
                           "gaps p50 {}, p90 {}, p99 {}, max {}", gaps->median, gaps->ninetieth, gaps->ninety_ninth,
                           gaps->longest) :
                   "no gaps, fewer than two anchors");
}

/**
 * @brief The figures of one inventory as its JSON object.
 * @param anchors The inventory's supply.
 * @return The JSON text.
 */
[[nodiscard]] std::string json_anchors(const Anchors& anchors)
{
    const auto& [count, per_kibibyte, gaps]{anchors};

    // Each gap figure is its number, or null where the anchors leave no gap.
    const auto [p50, p90, p99, longest]{gaps.value_or(Gaps{})};

    const auto figure{[&gaps](const std::size_t value) { return gaps ? std::to_string(value) : "null"; }};

    return std::format(
            R"({{"anchors": {}, "per_kibibyte": {:.1f}, "gap_p50": {}, "gap_p90": {}, "gap_p99": {}, "gap_max": {}}})",
            count, per_kibibyte, figure(p50), figure(p90), figure(p99), figure(longest));
}

} // namespace

Supply supply(const Report& report, const std::string_view input)
{
    const auto exact{byte_anchors(report.exact, input)};

    std::optional<Anchors> windows;

    if (!report.windows.empty())
    {
        auto with_windows{window_anchors(report, input)};

        for (std::size_t at{0}; at < with_windows.size(); ++at)
        {
            with_windows[at] = with_windows[at] || exact[at];
        }

        windows = anchors_of(with_windows);
    }

    return Supply{
            .bytes = input.size(),
            .tokenized = std::nullopt,
            .exact = anchors_of(exact),
            .modulo = anchors_of(byte_anchors(report.modulo, input)),
            .windows = windows};
}

Supply supply(const Report& report, const core::Lexer& lexer, const std::string_view input)
{
    auto measured{supply(report, input)};

    measured.tokenized = lexer.tokenize_all<std::size_t>(input, [](std::size_t, std::size_t) {});

    return measured;
}

std::string supply_json(const Supply& supply, const std::string_view path)
{
    const auto& [bytes, tokenized, exact, modulo, windows]{supply};

    return std::format(
            R"({{"input": {}, "bytes": {}, "tokenized": {}, "exact": {}, "modulo": {}, "windows": {}}})",
            json_string(path), bytes, tokenized ? std::to_string(*tokenized) : "null", json_anchors(exact),
            json_anchors(modulo), windows ? json_anchors(*windows) : "null");
}

std::string supply_section(const Supply& supply, const std::string_view path)
{
    const auto& [bytes, tokenized, exact, modulo, windows]{supply};

    auto out{std::format("\ncertified-anchor supply on {}, {} byte{}\n", path, bytes, bytes == 1 ? "" : "s")};

    // The condition every certificate's promise carries, stated before the rows that count on it.
    if (tokenized)
    {
        out += std::format(
                "  {:<26} {}\n", "serial scan",
                *tokenized < bytes ?
                        std::format(
                                "stops at offset {}, so the rows count occurrences and promise no boundary, the "
                                "exact byte row alone keeping tokenize_all_parallel()'s serial-prefix relation, "
                                "which the window rows have not got",
                                *tokenized) :
                        "tokenizes the input completely, so every anchor is a token boundary, the modulo row's once "
                        "discarded tokens are deleted");
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

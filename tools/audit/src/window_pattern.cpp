#include "munch/tools/audit/window_pattern.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief A cloud of hypotheses, each a state and the offset its token began at, kept ascending and without duplicates
 *        between positions.
 */
using Cloud_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief The origin that marks a hypothesis whose token began before the window, a value no in-window offset can take.
 */
constexpr std::size_t before_window{std::numeric_limits<std::size_t>::max()};

/**
 * @brief Returns the bytes a position is read as: the byte itself, which decides for its class, or one byte of every
 *        class for a wildcard.
 * @param position The position.
 * @param classes The byte classes.
 * @return The bytes.
 */
[[nodiscard]] std::vector<unsigned char> admitted(
        const std::optional<unsigned char> position, const std::vector<std::vector<unsigned char>>& classes)
{
    if (position)
    {
        return {*position};
    }

    std::vector<unsigned char> bytes{};

    std::ranges::transform(classes, std::back_inserter(bytes), [](const std::vector<unsigned char>& members) {
        return members.front();
    });

    return bytes;
}

/**
 * @brief Returns the hypotheses one byte leads a cloud to, as is_split_window() steps its cloud.
 *
 * Every hypothesis advances or is dropped, its origin renamed to this offset where it reads from an initial state that
 * nothing re-enters, and one fresh hypothesis begins here wherever some hypothesis had just accepted.
 * @param simulator The compiled token set.
 * @param cloud The cloud advanced from.
 * @param byte The byte read.
 * @param at The byte's offset in the window.
 * @return The hypotheses, unsorted and possibly repeated, since a position's clouds are merged before they are sorted.
 */
[[nodiscard]] Cloud_t step(
        const dfa::Simulator& simulator, const Cloud_t& cloud, const unsigned char byte, const std::size_t at)
{
    const auto is_accepting{[&simulator](const std::size_t state) { return simulator.is_accepting(state); }};

    const auto accepting{std::ranges::any_of(cloud | std::views::keys, is_accepting)};

    Cloud_t next{};

    for (const auto& [state, origin] : cloud)
    {
        const auto to{simulator.step(state, byte)};

        if (!to || !simulator.is_live(*to))
        {
            continue;
        }

        const auto begins{state == simulator.init_state() && !simulator.init_reentrant()};

        const auto stepped_origin{begins ? at : origin};

        next.emplace_back(*to, stepped_origin);
    }

    const auto fresh{accepting ? simulator.step(simulator.init_state(), byte) : std::nullopt};

    if (fresh && simulator.is_live(*fresh))
    {
        next.emplace_back(*fresh, at);
    }

    return next;
}

/**
 * @brief Returns the verdict a cloud past a pattern's last position gives: certified at an origin when the cloud is
 *        nonempty and every hypothesis carries that in-window origin.
 * @param pattern The pattern.
 * @param cloud The cloud past its last position, empty when no member's hypotheses survive.
 * @return The verdict.
 */
[[nodiscard]] Pattern_verdict verdict(const Window_pattern_t& pattern, const Cloud_t& cloud)
{
    std::vector<std::size_t> origins{};

    std::ranges::copy(cloud | std::views::values, std::back_inserter(origins));

    std::ranges::sort(origins);

    const auto duplicates{std::ranges::unique(origins)};

    origins.erase(duplicates.begin(), duplicates.end());

    // The marker is the largest value, so it sorts last.
    const auto before{!origins.empty() && origins.back() == before_window};

    if (before)
    {
        origins.pop_back();
    }

    const auto unanimous{!before && origins.size() == 1};

    const auto origin{unanimous ? std::optional{origins.front()} : std::nullopt};

    return Pattern_verdict{.pattern = pattern, .origin = origin, .origins = std::move(origins), .before = before};
}

/**
 * @brief Returns what one window pattern certifies, by one walk that reads at each position every byte it admits.
 * @param simulator The compiled token set.
 * @param classes The byte classes.
 * @param pattern The pattern.
 * @return The verdict.
 */
[[nodiscard]] Pattern_verdict decide(
        const dfa::Simulator& simulator, const std::vector<std::vector<unsigned char>>& classes,
        const Window_pattern_t& pattern)
{
    Cloud_t cloud{};

    const auto is_live{[&simulator](const std::size_t state) { return simulator.is_live(state); }};

    const auto states{std::views::iota(std::size_t{0}, simulator.state_count())};

    for (const auto state : states | std::views::filter(is_live))
    {
        cloud.emplace_back(state, before_window);
    }

    for (const auto [at, position] : std::views::enumerate(pattern))
    {
        const auto offset{static_cast<std::size_t>(at)};

        Cloud_t next{};

        for (const auto byte : admitted(position, classes))
        {
            const auto stepped{step(simulator, cloud, byte, offset)};

            next.insert(next.end(), stepped.begin(), stepped.end());
        }

        std::ranges::sort(next);

        const auto duplicates{std::ranges::unique(next)};

        next.erase(duplicates.begin(), duplicates.end());

        cloud = std::move(next);

        // The empty cloud is absorbing: no member's history crosses the window.
        if (cloud.empty())
        {
            break;
        }
    }

    return verdict(pattern, cloud);
}

/**
 * @brief Returns a byte as the report prints it: the character when it is printable, an escape when it is a common
 *        control, the backslash or the quote, hex otherwise.
 * @param byte The byte.
 * @return The rendering, quoted.
 */
[[nodiscard]] std::string shown(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return R"('\n')";
    case '\t':
        return R"('\t')";
    case '\r':
        return R"('\r')";
    case '\\':
        return R"('\\')";
    case '\'':
        return R"('\'')";
    default:
        break;
    }

    if (is_printable(byte))
    {
        return std::format("'{}'", static_cast<char>(byte));
    }

    return std::format("0x{:02X}", byte);
}

/**
 * @brief Returns a pattern as the report prints it, each byte as shown() writes it and `*` for every byte.
 * @param pattern The pattern.
 * @return The rendering, the positions separated by spaces.
 */
[[nodiscard]] std::string shown(const Window_pattern_t& pattern)
{
    const auto shown_position{
            [](const std::optional<unsigned char> position) { return position ? shown(*position) : std::string{"*"}; }};

    std::string text{};

    auto joined{pattern | std::views::transform(shown_position) | std::views::join_with(' ')};

    std::ranges::copy(joined, std::back_inserter(text));

    return text;
}

} // namespace

std::vector<Pattern_verdict> window_patterns(
        const Report& report, const core::Lexer& lexer, const std::vector<Window_pattern_t>& patterns)
{
    const auto& simulator{lexer.simulator()};

    std::vector<Pattern_verdict> verdicts{};

    for (const auto& pattern : patterns)
    {
        auto decided{decide(simulator, report.classes, pattern)};

        verdicts.push_back(std::move(decided));
    }

    return verdicts;
}

std::string window_pattern_json(const std::vector<Pattern_verdict>& verdicts)
{
    const auto position_json{[](const std::optional<unsigned char> position) {
        return position ? std::format("{}", *position) : std::string{R"("*")"};
    }};

    const auto value{[](const std::size_t number) { return std::format("{}", number); }};

    const auto comma_joined{[]<typename Texts>(Texts&& texts) {
        std::string joined{};

        std::ranges::copy(
                std::forward<Texts>(texts) | std::views::join_with(std::string_view{", "}), std::back_inserter(joined));

        return joined;
    }};

    const auto verdict_entry{[&](const Pattern_verdict& decided) {
        const auto& [pattern, origin, origins, before]{decided};

        const auto positions{comma_joined(pattern | std::views::transform(position_json))};

        const auto origin_json{origin ? value(*origin) : std::string{"null"}};

        const auto carried{comma_joined(origins | std::views::transform(value))};

        return std::format(
                R"({{"pattern": [{}], "origin": {}, "origins": [{}], "before": {}}})", positions, origin_json, carried,
                before);
    }};

    const auto entries{comma_joined(verdicts | std::views::transform(verdict_entry))};

    return std::format("[{}]", entries);
}

std::string window_pattern_section(const std::vector<Pattern_verdict>& verdicts)
{
    std::string out{"\nwindow patterns asked\n"};

    const auto number{[](const std::size_t at) { return std::format("{}", at); }};

    for (const auto& [pattern, origin, origins, before] : verdicts)
    {
        const auto label{shown(pattern)};

        if (origin)
        {
            out += std::format("  {:<26} certified at {}\n", label, *origin);

            continue;
        }

        if (origins.empty() && !before)
        {
            out += std::format("  {:<26} not certified; no member occurs in a completely tokenizable input\n", label);

            continue;
        }

        std::vector<std::string> carried{};

        if (before)
        {
            carried.emplace_back("before the window");
        }

        std::ranges::transform(origins, std::back_inserter(carried), number);

        std::string listed{};

        auto joined{carried | std::views::join_with(std::string_view{", "})};

        std::ranges::copy(joined, std::back_inserter(listed));

        out += std::format("  {:<26} not certified; origins carried: {}\n", label, listed);
    }

    return out;
}

} // namespace munch::tools::audit

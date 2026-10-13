#include "munch/tools/audit/window_modulo.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief How many example windows the section shows before it counts the rest.
 */
constexpr std::size_t shown_examples{6};

/**
 * @brief Returns the byte a class is shown by: its first graphic member when it has one, since a reader recognises that
 *        one, else its lowest; every member decides alike, so the choice is a matter of display alone.
 * @param members The class, ascending.
 * @return The representative.
 */
[[nodiscard]] unsigned char representative(const std::vector<unsigned char>& members) noexcept
{
    const auto graphic{std::ranges::find_if(members, is_graphic)};

    return graphic == members.end() ? members.front() : *graphic;
}

/**
 * @brief Returns the DFA of a compiled token set read over one byte of every class: its live states with the tokens
 *        they accept, and the step each takes on each class's representative into a live state.
 *
 * Every member of a class moves every state alike, so a decision over the representatives holds of every byte string
 * they stand for.
 * @param simulator The compiled token set.
 * @param symbols The representative of every class, in the order of the classes.
 * @return The DFA, its initial state the set's.
 */
[[nodiscard]] dfa::Dfa represented_dfa(const dfa::Simulator& simulator, const std::vector<unsigned char>& symbols)
{
    dfa::Dfa::Transitions_t transitions{};

    dfa::Dfa::Accept_states_t accepting{};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_live(state))
        {
            continue;
        }

        if (const auto token{simulator.accepted(state)})
        {
            accepting.emplace(state, *token);
        }

        for (const auto symbol : symbols)
        {
            const auto to{simulator.step(state, symbol)};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            const dfa::Dfa::Key_t key{state, dfa::Label{static_cast<char>(symbol)}};

            transitions.emplace(key, *to);
        }
    }

    return {simulator.init_state(), std::move(transitions), std::move(accepting)};
}

/**
 * @brief Returns how many byte strings the windows stand for once every representative expands to its class, counted
 *        checked, since the count can outgrow what holds it.
 * @param windows The windows over class representatives.
 * @param classes The byte classes.
 * @return The count, or std::nullopt when it is more than a std::size_t can hold.
 */
[[nodiscard]] std::optional<std::size_t> expansion_count(
        const std::vector<Window_cut>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, std::size_t> class_size{};

    for (const auto& members : classes)
    {
        class_size[representative(members)] = members.size();
    }

    std::size_t total{0};

    static constexpr auto most{std::numeric_limits<std::size_t>::max()};

    for (const auto& [window, cut] : windows)
    {
        std::size_t count{1};

        for (const auto byte : window)
        {
            const auto size{class_size.at(static_cast<unsigned char>(byte))};

            if (count > most / size)
            {
                return std::nullopt;
            }

            count *= size;
        }

        if (total > most - count)
        {
            return std::nullopt;
        }

        total += count;
    }

    return total;
}

/**
 * @brief Returns a window count as the report prints it: the number, or the bound the count passed when none holds it.
 * @param count The count, std::nullopt when it is more than a std::size_t can hold.
 * @return The rendering.
 */
[[nodiscard]] std::string counted(const std::optional<std::size_t>& count)
{
    return count ? std::to_string(*count) : std::format("more than {}", std::numeric_limits<std::size_t>::max());
}

/**
 * @brief Returns the escape a byte is written as inside the quotes a window is shown in, when it has one: a common
 *        control, the backslash, and the quote.
 * @param byte The byte.
 * @return The escape, or std::nullopt for a byte written as itself or in hex.
 */
[[nodiscard]] std::optional<std::string_view> quoted_escape(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return R"(\n)";
    case '\t':
        return R"(\t)";
    case '\r':
        return R"(\r)";
    case '\\':
        return R"(\\)";
    case '"':
        return R"(\")";
    default:
        break;
    }

    return std::nullopt;
}

/**
 * @brief Returns a byte string as the report prints it, each byte as its escape, as itself when it is printable, and in
 *        hex as the escape `\xHH` otherwise.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (const auto escape{quoted_escape(value)})
        {
            out += *escape;
        }
        else if (is_printable(value))
        {
            out += byte;
        }
        else
        {
            out += std::format(R"(\x{:02X})", value);
        }
    }

    return out + '"';
}

/**
 * @brief Returns a byte string as a JSON string, each byte the code point of its value.
 * @param bytes The bytes.
 * @return The JSON text, quotes included.
 */
[[nodiscard]] std::string json_byte_string(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (byte == '"' || byte == '\\')
        {
            out += std::format(R"(\{})", byte);

            continue;
        }

        if (!is_printable(value))
        {
            out += std::format(R"(\u{:04x})", value);

            continue;
        }

        out += byte;
    }

    return out + '"';
}

} // namespace

Windows_modulo windows_modulo(const Report& report, const core::Lexer& lexer)
{
    if (report.discarded.empty())
    {
        return {.discards = false, .class_count = report.classes.size(), .windows = {}, .window_count = 0};
    }

    std::vector<unsigned char> symbols{};

    std::ranges::transform(report.classes, std::back_inserter(symbols), representative);

    const auto represented{represented_dfa(lexer.simulator(), symbols)};

    const auto armed{verifier::armed_run(represented)};

    const auto cuts{verifier::modulo_cuts(armed, symbols, report.window_limit, report.discarded)};

    std::vector<Window_cut> windows{};

    for (const auto& [window, offset] : cuts)
    {
        // Each window's offsets come ascending, so its first is its least.
        const auto listed{!windows.empty() && windows.back().window == window};

        // A window is two bytes at least, and one the exact certificate certifies is the report's own.
        if (listed || window.size() < 2 || lexer.is_split_window(window))
        {
            continue;
        }

        windows.push_back({.window = window, .cut = offset});
    }

    const auto window_count{expansion_count(windows, report.classes)};

    return {.discards = true,
            .class_count = report.classes.size(),
            .windows = std::move(windows),
            .window_count = window_count};
}

std::string windows_modulo_json(const Windows_modulo& found)
{
    const auto& [discards, class_count, windows, window_count]{found};

    if (!discards)
    {
        return "null";
    }

    const auto window_json{[](const Window_cut& certified) {
        const auto& [window, cut]{certified};

        return std::format(R"({{"window": {}, "cut": {}}})", json_byte_string(window), cut);
    }};

    std::string entries{};

    auto joined{windows | std::views::transform(window_json) | std::views::join_with(std::string_view{", "})};

    std::ranges::copy(joined, std::back_inserter(entries));

    // A count past what a std::size_t holds prints as the bound it passed, a string.
    const auto count{window_count ? std::to_string(*window_count) : json_byte_string(counted(window_count))};

    return std::format(R"({{"windows": [{}], "window_count": {}}})", entries, count);
}

std::string windows_modulo_section(const Windows_modulo& found)
{
    const auto& [discards, class_count, windows, window_count]{found};

    std::string out{"\nwhat deleting the discarded tokens adds to the windows\n"};

    const auto row{[&out](const std::string_view label, const std::string_view value) {
        out += std::format("  {:<26} {}\n", label, value);
    }};

    const std::string_view label{"windows modulo discarded"};

    if (!discards)
    {
        row(label, "none, since no token is discarded");

        return out;
    }

    if (windows.empty())
    {
        row(label, "none");

        return out;
    }

    std::map<std::size_t, std::size_t> per_width{};

    for (const auto& [window, cut] : windows)
    {
        ++per_width[window.size()];
    }

    const auto at_width{[]<typename Entry>(const Entry& entry) {
        const auto& [width, count]{entry};

        return std::format("{} at width {}", count, width);
    }};

    std::string summary{};

    auto joined{per_width | std::views::transform(at_width) | std::views::join_with(std::string_view{", "})};

    std::ranges::copy(joined, std::back_inserter(summary));

    const auto expanded{counted(window_count)};

    const auto counts{std::format(
            "{} over {} byte classes, {} once classes expand, the exact ones left out", summary, class_count,
            expanded)};

    row(label, counts);

    // A window of printable ASCII makes the better example.
    const auto printable_window{[](const Window_cut& certified) {
        const auto& [window, cut]{certified};

        const auto printable{[](const char byte) { return is_printable(static_cast<unsigned char>(byte)); }};

        return std::ranges::all_of(window, printable);
    }};

    auto examples{windows};

    std::ranges::stable_partition(examples, printable_window);

    for (const auto& [window, cut] : examples | std::views::take(shown_examples))
    {
        row("", std::format("{} cut at {}", shown(window), cut));
    }

    if (windows.size() > shown_examples)
    {
        row("", std::format("... {} more", windows.size() - shown_examples));
    }

    return out;
}

} // namespace munch::tools::audit

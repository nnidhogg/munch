#include "munch/tools/audit/report.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The most certified windows the span is decided over once each class is expanded to its bytes; beyond it
 *        the report says so rather than run a walk whose node count grows with every window.
 */
constexpr std::size_t span_window_cap{4096};

/**
 * @brief What the blame section says per token: how many candidate bytes it consumes mid-token, and one of them
 *        with the shortest input after which it does.
 */
struct Consumed
{
    /**
     * @brief How many candidate bytes the token consumes mid-token.
     */
    std::size_t bytes{0};

    /**
     * @brief The byte shown as the example.
     */
    unsigned char example{0};

    /**
     * @brief The shortest input after which the token consumes the example.
     */
    std::string after;
};

/**
 * @brief The byte classes of the tables: two bytes are one class when every state moves on both to the same state,
 *        so any decision over transitions gives one answer for the whole class.
 * @param simulator The tables.
 * @return One representative per class, and every class's members.
 */
[[nodiscard]] std::vector<std::vector<unsigned char>> byte_classes(const dfa::Simulator& simulator)
{
    std::map<std::vector<std::optional<std::size_t>>, std::vector<unsigned char>> by_signature;

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        std::vector<std::optional<std::size_t>> signature;

        signature.reserve(simulator.state_count());

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, static_cast<unsigned char>(value)));
        }

        by_signature[std::move(signature)].push_back(static_cast<unsigned char>(value));
    }

    std::vector<std::vector<unsigned char>> classes;

    for (auto& members : by_signature | std::views::values)
    {
        classes.push_back(std::move(members));
    }

    // Ordered by representative, so the enumeration and the report are deterministic and read in byte order.
    std::ranges::sort(classes, {}, [](const std::vector<unsigned char>& members) { return members.front(); });

    return classes;
}

/**
 * @brief Every certified window over class representatives up to a width, widths ascending.
 * @param lexer The token set.
 * @param classes The byte classes.
 * @param limit The longest width tried.
 * @return The windows.
 */
[[nodiscard]] std::vector<Certified_window> certified_windows(
        const core::Lexer& lexer, const std::vector<std::vector<unsigned char>>& classes, const std::size_t limit)
{
    std::vector<Certified_window> windows;

    std::vector<std::string> frontier{""};

    for (std::size_t width{1}; width <= limit; ++width)
    {
        std::vector<std::string> next;

        for (const auto& prefix : frontier)
        {
            for (const auto& members : classes)
            {
                auto window{prefix};

                window.push_back(static_cast<char>(members.front()));

                if (width >= 2)
                {
                    if (const auto origin{lexer.is_split_window(window)})
                    {
                        windows.push_back({.window = window, .origin = *origin});
                    }
                }

                next.push_back(std::move(window));
            }
        }

        frontier = std::move(next);
    }

    return windows;
}

/**
 * @brief The certified windows with every class expanded to its member bytes, the inventory the span is decided
 *        over, or std::nullopt when there would be more than the cap.
 * @param windows The windows over representatives.
 * @param classes The byte classes.
 * @return The expanded inventory.
 */
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::size_t>>> expanded(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, const std::vector<unsigned char>*> members_of;

    for (const auto& members : classes)
    {
        members_of[members.front()] = &members;
    }

    std::vector<std::pair<std::string, std::size_t>> inventory;

    for (const auto& [window, origin] : windows)
    {
        std::vector<std::string> partial{""};

        for (const auto byte : window)
        {
            std::vector<std::string> longer;

            for (const auto& head : partial)
            {
                for (const auto member : *members_of.at(static_cast<unsigned char>(byte)))
                {
                    longer.push_back(head + static_cast<char>(member));

                    if (inventory.size() + longer.size() > span_window_cap)
                    {
                        return std::nullopt;
                    }
                }
            }

            partial = std::move(longer);
        }

        for (auto& expansion : partial)
        {
            inventory.emplace_back(std::move(expansion), origin);
        }
    }

    return inventory;
}

/**
 * @brief A shortest input reaching every reachable state, found breadth first from the initial state.
 * @param simulator The tables.
 * @return The input per state, absent for a state no input reaches.
 */
[[nodiscard]] std::vector<std::optional<std::string>> shortest_inputs(const dfa::Simulator& simulator)
{
    std::vector<std::optional<std::string>> input(simulator.state_count());

    input[simulator.init_state()] = std::string{};

    std::deque<std::size_t> pending{simulator.init_state()};

    while (!pending.empty())
    {
        const auto from{pending.front()};

        pending.pop_front();

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            const auto to{simulator.step(from, static_cast<unsigned char>(value))};

            if (to && !input[*to])
            {
                input[*to] = *input[from] + static_cast<char>(value);

                pending.push_back(*to);
            }
        }
    }

    return input;
}

/**
 * @brief The token the nearest accepting state from a state accepts: the token a match path through the state is
 *        on its way to.
 * @param simulator The tables.
 * @param from The state.
 * @return The token id, or std::nullopt when no accepting state is reachable.
 */
[[nodiscard]] std::optional<std::size_t> token_ahead(const dfa::Simulator& simulator, const std::size_t from)
{
    std::vector<bool> seen(simulator.state_count(), false);

    std::deque<std::size_t> pending{from};

    seen[from] = true;

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        if (const auto token{simulator.accepted(at)})
        {
            return token->id();
        }

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            if (const auto to{simulator.step(at, static_cast<unsigned char>(value))}; to && !seen[*to])
            {
                seen[*to] = true;

                pending.push_back(*to);
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief A byte as the report prints it: the character when it is printable, an escape when it is a common
 *        control, hex otherwise.
 * @param byte The byte.
 * @return The rendering, quoted.
 */
[[nodiscard]] std::string shown(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return "'\\n'";
    case '\t':
        return "'\\t'";
    case '\r':
        return "'\\r'";
    case ' ':
        return "' '";
    case '\'':
        return "'\\''";
    default:
        break;
    }

    if (byte > 0x20 && byte < 0x7F)
    {
        return std::string{'\''} + static_cast<char>(byte) + '\'';
    }

    return std::format("0x{:02X}", byte);
}

/**
 * @brief A byte string as the report prints it, each byte shown as above but without its own quotes.
 * @param bytes The bytes.
 * @return The rendering, quoted once.
 */
[[nodiscard]] std::string shown(const std::string_view bytes)
{
    std::string out{'"'};

    for (const auto byte : bytes)
    {
        auto one{shown(static_cast<unsigned char>(byte))};

        if (one.front() == '\'')
        {
            one = one.substr(1, one.size() - 2);
        }

        if (one == "\\'")
        {
            one = "'";
        }

        out += one.starts_with("0x") ? "\\x" + one.substr(2) : one;
    }

    return out + '"';
}

/**
 * @brief A list of bytes as the report prints it.
 * @param bytes The bytes.
 * @return The rendering, or "none".
 */
[[nodiscard]] std::string shown(const std::vector<unsigned char>& bytes)
{
    if (bytes.empty())
    {
        return "none";
    }

    std::string out;

    for (const auto byte : bytes)
    {
        out += (out.empty() ? "" : " ") + shown(byte);
    }

    return out;
}

/**
 * @brief A span as the report prints it.
 * @param span The span.
 * @return The number, or "unbounded".
 */
[[nodiscard]] std::string shown(const std::optional<std::size_t>& span)
{
    return span ? std::to_string(*span) : "unbounded";
}

} // namespace

std::vector<Blame> blame(const core::Lexer& lexer)
{
    const auto& simulator{lexer.simulator()};

    const auto inputs{shortest_inputs(simulator)};

    std::vector<Blame> blame;

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        const auto from_start{simulator.step(simulator.init_state(), byte)};

        // Only a byte the initial state consumes live could have certified; the rest never begin a token.
        if (!from_start || !simulator.is_live(*from_start) || lexer.is_split_point(static_cast<char>(byte)))
        {
            continue;
        }

        std::map<std::size_t, std::string> shortest_by_token;

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            if (state == simulator.init_state() || !simulator.is_live(state) || !inputs[state])
            {
                continue;
            }

            const auto to{simulator.step(state, byte)};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            const auto token{token_ahead(simulator, *to)};

            if (!token)
            {
                continue;
            }

            const auto found{shortest_by_token.find(*token)};

            if (found == shortest_by_token.end() || inputs[state]->size() < found->second.size())
            {
                shortest_by_token.insert_or_assign(*token, *inputs[state]);
            }
        }

        for (const auto& [token, after] : shortest_by_token)
        {
            blame.push_back({.byte = byte, .token = token, .after = after});
        }
    }

    return blame;
}

Report audit(const Token_set& set, const std::size_t window_limit)
{
    auto report{audit(compile(set), window_limit)};

    for (const auto& rule : set.rules)
    {
        if (rule.discarded)
        {
            report.discarded.push_back(rule.id);
        }
    }

    // The newline first, since it is the byte every designer asks about, then the near misses.
    std::vector<unsigned char> asked{'\n'};

    for (const auto byte : report.modulo)
    {
        if (byte != '\n' && !std::ranges::binary_search(report.exact, byte))
        {
            asked.push_back(byte);
        }
    }

    for (const auto byte : asked)
    {
        if (!std::ranges::binary_search(report.exact, byte))
        {
            report.prices.push_back(price(set, byte));
        }
    }

    return report;
}

Report audit(const core::Lexer& lexer, const std::size_t window_limit)
{
    const auto& simulator{lexer.simulator()};

    Report report{
            .nullable = simulator.nullable(),
            .exact = {},
            .modulo = {},
            .discarded = {},
            .classes = 0,
            .window_limit = window_limit,
            .windows = {},
            .mandatory_core = std::string{lexer.mandatory_core()},
            .byte_span = lexer.anchor_free_span(),
            .window_span = std::nullopt,
            .lag = lexer.lag(),
            .rescue_free = lexer.rescue_free(),
            .blame = {},
            .prices = {}};

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (lexer.is_split_point(byte))
        {
            report.exact.push_back(static_cast<unsigned char>(value));
        }

        if (lexer.is_split_point_ignoring(byte))
        {
            report.modulo.push_back(static_cast<unsigned char>(value));
        }
    }

    const auto classes{byte_classes(simulator)};

    report.classes = classes.size();

    report.windows = certified_windows(lexer, classes, window_limit);

    if (!report.windows.empty())
    {
        if (const auto inventory{expanded(report.windows, classes)})
        {
            std::vector<std::pair<std::string_view, std::size_t>> views;

            views.reserve(inventory->size());

            for (const auto& [window, origin] : *inventory)
            {
                views.emplace_back(window, origin);
            }

            report.window_span = lexer.anchor_free_span(views);
        }
    }

    report.blame = blame(lexer);

    return report;
}

std::string render(const Report& report, const std::function<std::string(std::size_t)>& name)
{
    std::string out;

    const auto line{[&out](const std::string_view label, const std::string& value) {
        out += std::format("{:<28}{}\n", label, value);
    }};

    if (report.nullable)
    {
        out += "a token matches the empty string: decided through the positive-width equivalent\n\n";
    }

    line("certified bytes", shown(report.exact));

    line("certified modulo discarded", shown(report.modulo));

    // The discarded tokens, so that what the modulo row deleted is on the page.
    if (!report.discarded.empty())
    {
        std::string names;

        for (const auto id : report.discarded | std::views::take(6))
        {
            names += (names.empty() ? "" : ", ") + name(id);
        }

        if (report.discarded.size() > 6)
        {
            names += std::format(", ... {} more", report.discarded.size() - 6);
        }

        line("discarded tokens", std::format("{}: {}", report.discarded.size(), names));
    }

    // Windows: the count over class representatives, widths ascending, and the first few as examples.
    {
        std::map<std::size_t, std::size_t> per_width;

        for (const auto& window : report.windows)
        {
            ++per_width[window.window.size()];
        }

        std::string summary;

        for (const auto& [width, count] : per_width)
        {
            summary += std::format("{}{} at width {}", summary.empty() ? "" : ", ", count, width);
        }

        line(std::format("certified windows (<= {})", report.window_limit),
             report.windows.empty() ? "none" : std::format("{} over {} byte classes", summary, report.classes));

        for (const auto& [window, origin] : report.windows | std::views::take(6))
        {
            line("", std::format("{} at {}", shown(std::string_view{window}), origin));
        }

        if (report.windows.size() > 6)
        {
            line("", std::format("... {} more", report.windows.size() - 6));
        }
    }

    line("mandatory core", report.mandatory_core.empty() ? "none" : shown(std::string_view{report.mandatory_core}));

    line("anchor-free span, bytes", shown(report.byte_span));

    if (!report.windows.empty())
    {
        line("anchor-free span, windows",
             report.window_span ?
                     shown(*report.window_span) :
                     std::format("not decided: more than {} windows once classes expand", span_window_cap));
    }

    line("lag", shown(report.lag));

    line("rescue-free", report.rescue_free ? "yes" : "not established");

    // Blame: grouped by token, the bytes it de-certifies and the shortest input after which it consumes one.
    if (!report.blame.empty())
    {
        out += "\nwhy candidate bytes do not certify\n";

        std::map<std::size_t, Consumed> by_token;

        for (const auto& [byte, token, after] : report.blame)
        {
            auto& [bytes, example, shortest]{by_token[token]};

            ++bytes;

            if (bytes == 1 || after.size() < shortest.size())
            {
                example = byte;

                shortest = after;
            }
        }

        for (const auto& [token, consumed] : by_token)
        {
            out += std::format(
                    "  {:<26} consumes {} candidate byte{} mid-token, e.g. {} after {}\n", name(token), consumed.bytes,
                    consumed.bytes == 1 ? "" : "s", shown(consumed.example), shown(std::string_view{consumed.after}));
        }
    }

    // Prices: per byte, the edits in order with the certificate after each, and what cannot move.
    for (const auto& pricing : report.prices)
    {
        out += std::format("\nwhat it would cost to certify {}\n", shown(pricing.byte));

        if (pricing.modulo_before)
        {
            out += "  certifies already once discarded tokens are deleted; the steps below make it exact\n";
        }

        for (std::size_t index{0}; index < pricing.steps.size(); ++index)
        {
            const auto& step{pricing.steps[index]};

            out += std::format("  {}. {:<24} no longer admits {}", index + 1, name(step.token), shown(pricing.byte));

            if (step.separated)
            {
                out += std::format(
                        ", and {} becomes a token of its own{}", shown(pricing.byte),
                        step.separated_discarded ? ", discarded" : "");
            }

            out += std::format(
                    "\n     {:<24} {}\n", "",
                    step.exact  ? "certifies exactly" :
                    step.modulo ? "certifies once discarded tokens are deleted" :
                                  "still does not certify");
        }

        for (const auto token : pricing.immovable)
        {
            out += std::format(
                    "  {:<26} spells {} out and cannot lose it; the byte cannot certify while it stays\n", name(token),
                    shown(pricing.byte));
        }

        if (!pricing.gained.empty())
        {
            out += std::format("  {:<26} {}\n", "also certified after", shown(pricing.gained));
        }
    }

    return out;
}

} // namespace munch::tools::audit

#include "munch/tools/audit/report.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <limits>
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
// Implements report.hpp's decisions: the byte classes, the window enumeration and its expansion, and the shortest
// inputs the blame is given with, are private to this unit; report_render.cpp renders what they decide.

/**
 * @brief The byte a class is shown by: its first printable member when it has one, since a reader recognises that
 *        one, else its lowest; every member decides alike, so the choice is a matter of display alone.
 * @param members The class, ascending.
 * @return The representative.
 */
[[nodiscard]] unsigned char representative(const std::vector<unsigned char>& members) noexcept
{
    const auto printable{
            std::ranges::find_if(members, [](const unsigned char byte) { return byte > 0x20 && byte < 0x7F; })};

    return printable == members.end() ? members.front() : *printable;
}

/**
 * @brief The byte classes of the tables: two bytes are one class when every state moves on both to the same state,
 *        so any decision over transitions gives one answer for the whole class.
 * @param simulator The tables.
 * @return Every class's members, ascending, the classes in order of their lowest byte.
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

    // Ordered by lowest byte, so the enumeration and the report are deterministic and read in byte order.
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

                window.push_back(static_cast<char>(representative(members)));

                // A window is two bytes at least; one byte is the byte certificate's to decide.
                if (const auto origin{width >= 2 ? lexer.is_split_window(window) : std::nullopt})
                {
                    windows.push_back({.window = window, .origin = *origin});
                }

                next.push_back(std::move(window));
            }
        }

        frontier = std::move(next);
    }

    return windows;
}

/**
 * @brief How many byte strings the certified windows stand for once every representative expands to its class.
 *
 * A window stands for the product of its bytes' class sizes, and the windows an input can show are the sum over the
 * windows. Both are counted checked, since the count outgrows what holds it: eight bytes of one class of 256, the
 * widest window the command accepts, already stand for 2^64 strings, and a wrapped sum would be printed as a fact. A
 * byte class always has a member, so the products below divide by no zero.
 * @param windows The certified windows over class representatives.
 * @param classes The byte classes.
 * @return The count, or std::nullopt when it is more than a std::size_t can hold.
 */
[[nodiscard]] std::optional<std::size_t> expansion_count(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    constexpr auto most{std::numeric_limits<std::size_t>::max()};

    std::map<unsigned char, std::size_t> class_size;

    for (const auto& members : classes)
    {
        class_size[representative(members)] = members.size();
    }

    std::size_t total{0};

    for (const auto& [window, origin] : windows)
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
 * @brief The certified windows with every class expanded to its member bytes, the inventory the span is decided
 *        over, or std::nullopt when there would be more than the cap.
 * @param windows The windows over representatives.
 * @param classes The byte classes.
 * @return The expanded inventory.
 */
[[nodiscard]] std::optional<std::vector<std::pair<std::string, std::size_t>>> expanded(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, std::size_t> class_of;

    for (std::size_t index{0}; index < classes.size(); ++index)
    {
        class_of[representative(classes[index])] = index;
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
                for (const auto member : classes[class_of.at(static_cast<unsigned char>(byte))])
                {
                    longer.push_back(head + static_cast<char>(member));
                }

                if (inventory.size() + longer.size() > span_window_cap)
                {
                    return std::nullopt;
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
 * @brief The tokens reachable from each state: those the accepting states reachable from it accept, which are the
 *        tokens a match path through it can still be on its way to.
 *
 * Every reachable accepting state counts, not the nearest one, because an accepting state lies on the way to longer
 * tokens' accepting states: with the rules a[\nx] and a[\nx]b, the state accepting the shorter one is where the scan of
 * the longer one stands after the same bytes, so both tokens consume those bytes mid-token. Once per state rather than
 * once per state and byte, since the blame asks the same question of a state for every byte that reaches it.
 * @param simulator The tables.
 * @return The token ids per state, ascending, empty for a state no accepting state is reachable from.
 */
[[nodiscard]] std::vector<std::set<std::size_t>> tokens_ahead(const dfa::Simulator& simulator)
{
    std::vector<std::set<std::size_t>> ahead(simulator.state_count());

    for (std::size_t from{0}; from < simulator.state_count(); ++from)
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
                ahead[from].insert(token->id());
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
    }

    return ahead;
}

/**
 * @brief A shortest nonempty input after which the scan stands in the initial state again.
 *
 * A shortest such input is a shortest input reaching a state that steps into the initial state, one byte longer, so the
 * inputs already found decide it without a walk of its own. It exists exactly where a reachable state steps back into
 * the initial state, which is what Simulator::init_reentrant() says of the tables.
 * @param simulator The tables.
 * @param inputs A shortest input per state, empty for the initial state.
 * @return The input, std::nullopt when no input returns to the initial state.
 */
[[nodiscard]] std::optional<std::string> shortest_re_entry(
        const dfa::Simulator& simulator, const std::vector<std::optional<std::string>>& inputs)
{
    std::optional<std::string> shortest;

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!inputs[state])
        {
            continue;
        }

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            if (simulator.step(state, static_cast<unsigned char>(value)) != simulator.init_state())
            {
                continue;
            }

            auto returning{*inputs[state] + static_cast<char>(value)};

            if (!shortest || returning.size() < shortest->size())
            {
                shortest = std::move(returning);
            }
        }
    }

    return shortest;
}

} // namespace

Report audit(const Token_set& set, const std::size_t window_limit)
{
    auto report{audit(compile(set), window_limit)};

    for (const auto& [regex, id, priority, discarded] : set.rules)
    {
        if (discarded)
        {
            report.discarded.push_back(id);
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
            .classes = {},
            .window_limit = window_limit,
            .windows = {},
            .window_count = std::nullopt,
            .mandatory_core = std::string{lexer.mandatory_core()},
            .byte_span = lexer.anchor_free_span(),
            .window_span = std::nullopt,
            .lag = lexer.lag(),
            .rescue = lexer.rescue(),
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

    report.classes = byte_classes(simulator);

    report.windows = certified_windows(lexer, report.classes, window_limit);

    report.window_count = expansion_count(report.windows, report.classes);

    if (!report.windows.empty())
    {
        if (const auto inventory{expanded(report.windows, report.classes)})
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

std::vector<Blame> blame(const core::Lexer& lexer)
{
    const auto& simulator{lexer.simulator()};

    const auto inputs{shortest_inputs(simulator)};

    const auto ahead{tokens_ahead(simulator)};

    // The initial state is exempt on the entry before any input alone, where a byte may begin a token. Where a nonempty
    // input returns to it, the scan stands in it mid-token, so it blames like any other state, after that input rather
    // than after none.
    const auto re_entry{shortest_re_entry(simulator, inputs)};

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
            const auto& reached{state == simulator.init_state() ? re_entry : inputs[state]};

            if (!reached || !simulator.is_live(state))
            {
                continue;
            }

            const auto to{simulator.step(state, byte)};

            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            for (const auto token : ahead[*to])
            {
                const auto found{shortest_by_token.find(token)};

                if (found == shortest_by_token.end() || reached->size() < found->second.size())
                {
                    shortest_by_token.insert_or_assign(token, *reached);
                }
            }
        }

        for (const auto& [token, after] : shortest_by_token)
        {
            blame.push_back({.byte = byte, .token = token, .after = after});
        }
    }

    return blame;
}

} // namespace munch::tools::audit

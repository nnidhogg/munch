#include "munch/tools/audit/report.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <iterator>
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
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
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
 * @brief Returns every certified window over class representatives up to a width, widths ascending.
 * @param lexer The token set.
 * @param classes The byte classes.
 * @param limit The longest width tried.
 * @return The windows.
 */
[[nodiscard]] std::vector<Certified_window> certified_windows(
        const core::Lexer& lexer, const std::vector<std::vector<unsigned char>>& classes, const std::size_t limit)
{
    std::vector<Certified_window> windows{};

    std::vector<std::string> frontier{""};

    const auto extend{[&](const std::string& prefix, const std::size_t width, std::vector<std::string>& next) {
        for (const auto& members : classes)
        {
            const auto byte{representative(members)};

            auto window{prefix};

            window.push_back(static_cast<char>(byte));

            // A window is two bytes at least; one byte is the byte certificate's to decide.
            if (const auto origin{width >= 2 ? lexer.is_split_window(window) : std::nullopt})
            {
                windows.push_back({.window = window, .origin = *origin});
            }

            next.push_back(std::move(window));
        }
    }};

    for (std::size_t width{1}; width <= limit; ++width)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : frontier)
        {
            extend(prefix, width, next);
        }

        frontier = std::move(next);
    }

    return windows;
}

/**
 * @brief Returns where each byte class stands among the classes, by its representative.
 * @param classes The byte classes.
 * @return The index of each class, keyed by the byte it is shown by.
 */
[[nodiscard]] std::map<unsigned char, std::size_t> class_indices(const std::vector<std::vector<unsigned char>>& classes)
{
    std::map<unsigned char, std::size_t> class_of{};

    for (const auto [index, members] : std::views::enumerate(classes))
    {
        class_of[representative(members)] = static_cast<std::size_t>(index);
    }

    return class_of;
}

/**
 * @brief Returns how many byte strings the certified windows stand for once every representative expands to its class.
 *
 * A window stands for the product of its bytes' class sizes, and the windows an input can show are the sum over the
 * windows. Both are counted checked, since the count outgrows what holds it: eight bytes of one class of 256, the
 * widest window the command accepts, already stand for 2^64 strings, and a wrapped sum would be printed as a fact. A
 * byte class always has a member, so the products below divide by no zero.
 * @param windows The certified windows over class representatives.
 * @param classes The byte classes.
 * @param class_of The index of each class, keyed by its representative, as class_indices() gives it.
 * @return The count, or std::nullopt when it is more than a std::size_t can hold.
 */
[[nodiscard]] std::optional<std::size_t> expansion_count(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes,
        const std::map<unsigned char, std::size_t>& class_of)
{
    std::size_t total{0};

    static constexpr auto most{std::numeric_limits<std::size_t>::max()};

    for (const auto& [window, origin] : windows)
    {
        std::size_t count{1};

        for (const auto byte : window)
        {
            const auto size{classes[class_of.at(static_cast<unsigned char>(byte))].size()};

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
 * @brief Returns the certified windows with every class expanded to its member bytes, the inventory the span is decided
 *        over, or std::nullopt when there would be more than the cap.
 * @param windows The windows over representatives.
 * @param classes The byte classes.
 * @param class_of The index of each class, keyed by its representative.
 * @return The expanded inventory.
 */
[[nodiscard]] std::optional<std::vector<Certified_window>> expanded(
        const std::vector<Certified_window>& windows, const std::vector<std::vector<unsigned char>>& classes,
        const std::map<unsigned char, std::size_t>& class_of)
{
    std::vector<Certified_window> inventory{};

    // Nothing once the expansions and the inventory pass the cap.
    const auto expand{
            [&](const std::vector<std::string>& partial,
                const std::vector<unsigned char>& members) -> std::optional<std::vector<std::string>> {
                std::vector<std::string> longer{};

                for (const auto& head : partial)
                {
                    for (const auto member : members)
                    {
                        longer.push_back(head + static_cast<char>(member));
                    }

                    if (inventory.size() + longer.size() > span_window_cap)
                    {
                        return std::nullopt;
                    }
                }

                return longer;
            }};

    for (const auto& [window, origin] : windows)
    {
        std::vector<std::string> partial{""};

        for (const auto byte : window)
        {
            const auto& members{classes[class_of.at(static_cast<unsigned char>(byte))]};

            auto longer{expand(partial, members)};

            if (!longer)
            {
                return std::nullopt;
            }

            partial = std::move(*longer);
        }

        for (auto& expansion : partial)
        {
            inventory.push_back({.window = std::move(expansion), .origin = origin});
        }
    }

    return inventory;
}

/**
 * @brief Returns a shortest input reaching every reachable state, found breadth first from the initial state.
 * @param simulator The tables.
 * @return The input per state, absent for a state no input reaches.
 */
[[nodiscard]] std::vector<std::optional<std::string>> shortest_inputs(const dfa::Simulator& simulator)
{
    std::vector<std::optional<std::string>> input(simulator.state_count());

    input[simulator.init_state()] = std::string{};

    std::deque pending{simulator.init_state()};

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
 * @brief Returns the tokens reachable from each state: those the accepting states reachable from it accept, which are
 *        the tokens a match path through it can still be on its way to.
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
    const auto reachable_tokens{[&](const std::size_t from) {
        std::set<std::size_t> tokens{};

        std::vector<bool> seen(simulator.state_count(), false);

        std::deque pending{from};

        seen[from] = true;

        while (!pending.empty())
        {
            const auto at{pending.front()};

            pending.pop_front();

            if (const auto token{simulator.accepted(at)})
            {
                tokens.insert(token->id());
            }

            for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
            {
                const auto to{simulator.step(at, static_cast<unsigned char>(value))};

                if (!to || seen[*to])
                {
                    continue;
                }

                seen[*to] = true;

                pending.push_back(*to);
            }
        }

        return tokens;
    }};

    std::vector<std::set<std::size_t>> ahead(simulator.state_count());

    for (std::size_t from{0}; from < simulator.state_count(); ++from)
    {
        ahead[from] = reachable_tokens(from);
    }

    return ahead;
}

/**
 * @brief Returns a shortest nonempty input after which the scan stands in the initial state again.
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
    std::optional<std::string> shortest{};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!inputs[state])
        {
            continue;
        }

        for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
        {
            const auto to{simulator.step(state, static_cast<unsigned char>(value))};

            if (to != simulator.init_state())
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

/**
 * @brief Returns the anchor-free span decided over the certified windows expanded to their member bytes.
 * @param lexer The token set.
 * @param windows The certified windows over class representatives.
 * @param classes The byte classes.
 * @param class_of The index of each class, keyed by its representative.
 * @return The span the lexer decides, or std::nullopt when no window is certified or the inventory passes its cap.
 */
[[nodiscard]] std::optional<std::optional<std::size_t>> window_span(
        const core::Lexer& lexer, const std::vector<Certified_window>& windows,
        const std::vector<std::vector<unsigned char>>& classes, const std::map<unsigned char, std::size_t>& class_of)
{
    if (windows.empty())
    {
        return std::nullopt;
    }

    const auto inventory{expanded(windows, classes, class_of)};

    if (!inventory)
    {
        return std::nullopt;
    }

    const auto viewed{[](const Certified_window& entry) {
        const auto& [window, origin]{entry};

        return std::pair<std::string_view, std::size_t>{window, origin};
    }};

    std::vector<std::pair<std::string_view, std::size_t>> views{};

    views.reserve(inventory->size());

    std::ranges::transform(*inventory, std::back_inserter(views), viewed);

    return std::optional<std::optional<std::size_t>>{std::in_place, lexer.anchor_free_span(views)};
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
        if (std::ranges::binary_search(report.exact, byte))
        {
            continue;
        }

        auto pricing{price(set, byte)};

        report.prices.push_back(std::move(pricing));
    }

    return report;
}

Report audit(const core::Lexer& lexer, const std::size_t window_limit)
{
    const auto& simulator{lexer.simulator()};

    const auto nullable{simulator.nullable()};

    std::string mandatory_core{lexer.mandatory_core()};

    const auto byte_span{lexer.anchor_free_span()};

    const auto lag{lexer.lag()};

    const auto rescue{lexer.rescue()};

    std::vector<unsigned char> exact{};

    std::vector<unsigned char> modulo{};

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (lexer.is_split_point(byte))
        {
            exact.push_back(static_cast<unsigned char>(value));
        }

        if (lexer.is_split_point_ignoring(byte))
        {
            modulo.push_back(static_cast<unsigned char>(value));
        }
    }

    auto classes{simulator.symbol_classes()};

    const auto class_of{class_indices(classes)};

    auto windows{certified_windows(lexer, classes, window_limit)};

    const auto window_count{expansion_count(windows, classes, class_of)};

    const auto spanned{window_span(lexer, windows, classes, class_of)};

    auto blamed{blame(lexer)};

    return {.nullable = nullable,
            .exact = std::move(exact),
            .modulo = std::move(modulo),
            .discarded = {},
            .classes = std::move(classes),
            .window_limit = window_limit,
            .windows = std::move(windows),
            .window_count = window_count,
            .mandatory_core = std::move(mandatory_core),
            .byte_span = byte_span,
            .window_span = spanned,
            .lag = lag,
            .rescue = rescue,
            .blame = std::move(blamed),
            .prices = {}};
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

    std::vector<Blame> entries{};

    const auto keep_shortest{[&](std::map<std::size_t, std::string>& shortest_by_token, const std::size_t to,
                                 const std::string& reached) {
        for (const auto token : ahead[to])
        {
            const auto [found, inserted]{shortest_by_token.try_emplace(token, reached)};

            auto& [kept_token, kept]{*found};

            if (!inserted && reached.size() < kept.size())
            {
                kept = reached;
            }
        }
    }};

    for (std::size_t value{0}; value < dfa::Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        const auto from_start{simulator.step(simulator.init_state(), byte)};

        // Only a byte the initial state consumes live could have certified; the rest never begin a token.
        if (!from_start || !simulator.is_live(*from_start) || lexer.is_split_point(static_cast<char>(byte)))
        {
            continue;
        }

        std::map<std::size_t, std::string> shortest_by_token{};

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

            keep_shortest(shortest_by_token, *to, *reached);
        }

        for (const auto& [token, after] : shortest_by_token)
        {
            entries.push_back({.byte = byte, .token = token, .after = after});
        }
    }

    return entries;
}

} // namespace munch::tools::audit

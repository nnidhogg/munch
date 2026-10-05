#include "munch/tools/probes/wall_table.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

#include "munch/dfa/dfa.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief An automaton's states numbered in first-reached order with their dense rows.
 */
struct Numbered
{
    /**
     * @brief The automaton's states, by index.
     */
    std::vector<dfa::Dfa::State_t> order{};

    /**
     * @brief Per state index, its targets by byte, each a state index or dead.
     */
    std::vector<Row_t> rows{};
};

/**
 * @brief Numbers an automaton's states in the order a breadth-first walk from the initial state, bytes ascending, first
 *        meets them, and reads each state's row.
 * @param dfa The compiled automaton.
 * @return The states in index order and their rows.
 */
Numbered number_states(const dfa::Dfa& dfa)
{
    std::map<dfa::Dfa::State_t, std::size_t> index{};

    Numbered numbered{};

    auto& [order, raw]{numbered};

    const auto intern{[&](const dfa::Dfa::State_t state) {
        if (const auto found{index.find(state)}; found != index.end())
        {
            const auto& [found_state, at]{*found};

            return at;
        }

        index.emplace(state, order.size());

        order.push_back(state);

        return order.size() - 1;
    }};

    intern(dfa.init_state());

    for (std::size_t at{0}; at < order.size(); ++at)
    {
        auto& row{raw.emplace_back()};

        row.fill(dead);

        for (std::size_t byte{0}; byte < row.size(); ++byte)
        {
            if (const auto to{dfa.advance(order[at], static_cast<char>(byte))})
            {
                row[byte] = static_cast<int>(intern(*to));
            }
        }
    }

    return numbered;
}

/**
 * @brief Returns which of the numbered states accept.
 * @param dfa The compiled automaton.
 * @param order The states, by index.
 * @return Per state index, Flag::on when the state accepts.
 */
std::vector<Flag> acceptance_of(const dfa::Dfa& dfa, const std::vector<dfa::Dfa::State_t>& order)
{
    const auto acceptance{
            [&dfa](const dfa::Dfa::State_t state) { return dfa.has_accept_token(state) ? Flag::on : Flag::off; }};

    std::vector accept(order.size(), Flag::off);

    std::ranges::transform(order, accept.begin(), acceptance);

    return accept;
}

/**
 * @brief Finds the states from which an accepting state is reachable, by growing the accepting set to a fixpoint.
 * @param rows The states' rows.
 * @param accept Per state, Flag::on when the state accepts.
 * @return Per state, Flag::on when an accepting state is reachable from it.
 */
std::vector<Flag> coaccessible_states(const std::vector<Row_t>& rows, const std::vector<Flag>& accept)
{
    std::vector<Flag> coaccessible{accept};

    const auto is_coaccessible{[&coaccessible](const int to) {
        return to != dead && coaccessible[static_cast<std::size_t>(to)] == Flag::on;
    }};

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (std::size_t at{0}; at < rows.size(); ++at)
        {
            if (coaccessible[at] == Flag::on || !std::ranges::any_of(rows[at], is_coaccessible))
            {
                continue;
            }

            coaccessible[at] = Flag::on;

            grew = true;
        }
    }

    return coaccessible;
}

/**
 * @brief Redirects every transition into a state from which no accepting state is reachable to dead.
 * @param rows The rows redirected.
 * @param coaccessible Per state, Flag::on when an accepting state is reachable from it.
 */
void prune(std::vector<Row_t>& rows, const std::vector<Flag>& coaccessible)
{
    const auto is_pruned{[&coaccessible](const int to) {
        return to != dead && coaccessible[static_cast<std::size_t>(to)] == Flag::off;
    }};

    for (auto& row : rows)
    {
        std::ranges::replace_if(row, is_pruned, dead);
    }
}

} // namespace

Table extract(const dfa::Dfa& dfa)
{
    auto [order, raw]{number_states(dfa)};

    auto accept{acceptance_of(dfa, order)};

    const auto coaccessible{coaccessible_states(raw, accept)};

    Table table{.states = raw.size(), .init = 0, .next = std::move(raw), .accept = std::move(accept)};

    prune(table.next, coaccessible);

    return table;
}

std::vector<std::size_t> live_states(const Table& table)
{
    std::vector<std::size_t> states{};

    const auto is_transition{[](const int to) { return to != dead; }};

    for (std::size_t state{0}; state < table.states; ++state)
    {
        const auto live{table.accept[state] == Flag::on || std::ranges::any_of(table.next[state], is_transition)};

        if (live)
        {
            states.push_back(state);
        }
    }

    return states;
}

bool is_init_reentrant(const Table& table)
{
    const auto init{static_cast<int>(table.init)};

    const auto enters_init{[init](const int to) { return to == init; }};

    return std::ranges::any_of(table.next | std::views::join, enters_init);
}

std::optional<std::size_t> scan_step(const Table& table, const std::size_t state, const unsigned char byte)
{
    if (const auto to{table.next[state][byte]}; to != dead)
    {
        return static_cast<std::size_t>(to);
    }

    if (table.accept[state] == Flag::off)
    {
        return std::nullopt;
    }

    if (const auto to{table.next[table.init][byte]}; to != dead)
    {
        return static_cast<std::size_t>(to);
    }

    return std::nullopt;
}

std::optional<std::size_t> scan_word(const Table& table, std::size_t state, const std::string_view word)
{
    for (const auto letter : word)
    {
        const auto next{scan_step(table, state, static_cast<unsigned char>(letter))};

        if (!next)
        {
            return std::nullopt;
        }

        state = *next;
    }

    return state;
}

std::optional<std::vector<std::size_t>> serial_boundaries(const Table& table, const std::string_view input)
{
    std::vector<std::size_t> starts{0};

    std::size_t state{table.init};

    for (std::size_t at{0}; at < input.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(input[at])};

        if (const auto to{table.next[state][byte]}; to != dead)
        {
            state = static_cast<std::size_t>(to);

            continue;
        }

        if (table.accept[state] == Flag::off)
        {
            return std::nullopt;
        }

        starts.push_back(at);

        const auto restart{table.next[table.init][byte]};

        if (restart == dead)
        {
            return std::nullopt;
        }

        state = static_cast<std::size_t>(restart);
    }

    if (!input.empty() && table.accept[state] == Flag::off)
    {
        return std::nullopt;
    }

    starts.push_back(input.size());

    return starts;
}

} // namespace munch::tools::probes

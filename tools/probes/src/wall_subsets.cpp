#include "munch/tools/probes/wall_subsets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief Returns one byte's step of the subset walk: every member advancing through its transition, and the initial
 *        state's transition on the byte when a dying member accepts.
 * @param table The table.
 * @param subset The node's member states.
 * @param byte The byte.
 * @return The successor's member states, ascending.
 */
std::vector<std::size_t> stepped_subset(const Table& table, const std::vector<std::size_t>& subset, const int byte)
{
    std::set<std::size_t> stepped{};

    auto restart{false};

    for (const auto state : subset)
    {
        if (const auto to{table.next[state][static_cast<std::size_t>(byte)]}; to != dead)
        {
            stepped.insert(static_cast<std::size_t>(to));
        }
        else if (table.accept[state] == Flag::on)
        {
            restart = true;
        }
    }

    if (const auto to{table.next[table.init][static_cast<std::size_t>(byte)]}; restart && to != dead)
    {
        stepped.insert(static_cast<std::size_t>(to));
    }

    return {stepped.begin(), stepped.end()};
}

/**
 * @brief Returns the projection from a node of a subset graph to its width, the size of its subset.
 * @param graph The subset graph, which the projection refers to.
 * @return The projection, taking a node and returning its width.
 */
auto width_in(const Subset_graph& graph)
{
    return [&graph](const std::size_t node) { return node_width(graph, node); };
}

/**
 * @brief Returns which nodes each node reaches by a path of one or more edges.
 * @param graph The subset graph.
 * @return Per node, Flag::on at every node it reaches.
 */
std::vector<std::vector<Flag>> reachability(const Subset_graph& graph)
{
    const auto count{graph.subsets.size()};

    std::vector<std::vector<Flag>> reaches(count, std::vector<Flag>(count, Flag::off));

    const auto walk_from{[&graph, &reaches](const std::size_t from) {
        auto& reached{reaches[from]};

        std::deque<std::size_t> pending{from};

        while (!pending.empty())
        {
            const auto node{pending.front()};

            pending.pop_front();

            for (const auto to : graph.successor[node])
            {
                if (reached[to] == Flag::on)
                {
                    continue;
                }

                reached[to] = Flag::on;

                pending.push_back(to);
            }
        }
    }};

    for (std::size_t from{0}; from < count; ++from)
    {
        walk_from(from);
    }

    return reaches;
}

} // namespace

Premise zero_lag(const Table& table)
{
    std::vector<std::array<Flag, 2>> visited(table.states, {Flag::off, Flag::off});

    std::deque<std::pair<std::size_t, bool>> pending{{table.init, false}};

    visited[table.init][0] = Flag::on;

    while (!pending.empty())
    {
        const auto [state, seen]{pending.front()};

        pending.pop_front();

        if (seen && table.accept[state] == Flag::off)
        {
            return {.holds = false, .witness_state = state, .witness_byte = end_of_input_byte};
        }

        for (int byte{0}; byte < byte_count; ++byte)
        {
            const auto to{table.next[state][static_cast<std::size_t>(byte)]};

            if (to == dead)
            {
                continue;
            }

            const auto target{static_cast<std::size_t>(to)};

            const auto next_seen{seen || table.accept[target] == Flag::on};

            const auto seen_index{static_cast<std::size_t>(next_seen)};

            if (visited[target][seen_index] == Flag::off)
            {
                visited[target][seen_index] = Flag::on;

                pending.emplace_back(target, next_seen);
            }
        }
    }

    return {.holds = true};
}

std::optional<Subset_graph> subset_graph(const Table& table, const std::size_t budget)
{
    std::map<std::vector<std::size_t>, std::size_t> ids{};

    Subset_graph graph{};

    const auto intern{[&](std::vector<std::size_t> subset) {
        if (const auto found{ids.find(subset)}; found != ids.end())
        {
            const auto& [member_states, id]{*found};

            return id;
        }

        ids.emplace(subset, graph.subsets.size());

        graph.subsets.push_back(std::move(subset));

        graph.successor.emplace_back();

        return graph.subsets.size() - 1;
    }};

    intern(live_states(table));

    for (std::size_t at{0}; at < graph.subsets.size(); ++at)
    {
        for (int byte{0}; byte < byte_count; ++byte)
        {
            // Interning may grow the vectors, so the target is taken before the source's row is addressed.
            auto stepped{stepped_subset(table, graph.subsets[at], byte)};

            const auto target{intern(std::move(stepped))};

            graph.successor[at][static_cast<std::size_t>(byte)] = target;

            if (graph.subsets.size() > budget)
            {
                return std::nullopt;
            }
        }
    }

    return graph;
}

std::size_t node_width(const Subset_graph& graph, const std::size_t node)
{
    return graph.subsets[node].size();
}

std::vector<std::size_t> floors(const Subset_graph& graph)
{
    const auto count{graph.subsets.size()};

    std::vector<std::vector<std::size_t>> predecessors(count);

    for (std::size_t node{0}; node < count; ++node)
    {
        for (const auto to : graph.successor[node])
        {
            predecessors[to].push_back(node);
        }
    }

    constexpr auto untouched{std::numeric_limits<std::size_t>::max()};

    std::vector<std::size_t> floor(count, untouched);

    std::vector<std::size_t> order(count);

    std::ranges::iota(order, std::size_t{0});

    const auto width_of{width_in(graph)};

    std::ranges::sort(order, {}, width_of);

    const auto spread_from{[&](const std::size_t origin) {
        floor[origin] = width_of(origin);

        std::vector<std::size_t> pending{origin};

        while (!pending.empty())
        {
            const auto node{pending.back()};

            pending.pop_back();

            for (const auto from : predecessors[node])
            {
                if (floor[from] != untouched)
                {
                    continue;
                }

                floor[from] = floor[origin];

                pending.push_back(from);
            }
        }
    }};

    for (const auto origin : order)
    {
        if (floor[origin] == untouched)
        {
            spread_from(origin);
        }
    }

    return floor;
}

Verdict decide(const Table& table)
{
    // The sustained-width phase is quadratic in the node count, so the decider's budget is far tighter than the
    // synthesizer's.
    const auto graph{subset_graph(table, decider_budget)};

    if (!graph)
    {
        return {.over_budget = true};
    }

    const auto count{graph->subsets.size()};

    const auto reaches{reachability(*graph)};

    const auto nodes{std::views::iota(std::size_t{0}, count)};

    const auto on_cycle{[&reaches](const std::size_t node) { return reaches[node][node] == Flag::on; }};

    const auto width_of{width_in(*graph)};

    auto cycle_widths{nodes | std::views::filter(on_cycle) | std::views::transform(width_of)};

    const auto sustained{std::ranges::fold_left(cycle_widths, std::size_t{0}, std::ranges::max)};

    const auto floor{floors(*graph)};

    constexpr std::size_t origin{0};

    const auto from_origin{
            [&reaches](const std::size_t node) { return reaches[origin][node] == Flag::on || node == origin; }};

    const auto floor_of{[&floor](const std::size_t node) { return floor[node]; }};

    auto reached_floors{nodes | std::views::filter(from_origin) | std::views::transform(floor_of)};

    const auto wall{std::ranges::fold_left(reached_floors, std::size_t{0}, std::ranges::max)};

    return {.nodes = count, .sustained = sustained, .floor_start = floor[origin], .wall_floor = wall};
}

} // namespace munch::tools::probes

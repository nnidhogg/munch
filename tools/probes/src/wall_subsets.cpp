#include "munch/tools/probes/wall_subsets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements wall_subsets.hpp: the subset walk's step and the reachability the decider reads are private to this unit.

/**
 * @brief One byte's step of the subset walk: every member advancing through its transition, and the initial state's
 *        transition on the byte when a dying member accepts.
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
        if (const auto to{table.next[state][static_cast<std::size_t>(byte)]}; to != kDead)
        {
            stepped.insert(static_cast<std::size_t>(to));
        }
        else if (table.accept[state] != 0)
        {
            restart = true;
        }
    }

    if (restart)
    {
        if (const auto to{table.next[table.init][static_cast<std::size_t>(byte)]}; to != kDead)
        {
            stepped.insert(static_cast<std::size_t>(to));
        }
    }

    return {stepped.begin(), stepped.end()};
}

/**
 * @brief Which nodes each node reaches by a path of one or more edges.
 * @param graph The subset graph.
 * @return Per node, 1 at every node it reaches and 0 elsewhere.
 */
std::vector<std::vector<char>> reachability(const Subset_graph& graph)
{
    const auto count{graph.subsets.size()};

    std::vector<std::vector<char>> reaches(count, std::vector<char>(count, 0));

    for (std::size_t from{0}; from < count; ++from)
    {
        std::deque<std::size_t> pending{from};

        while (!pending.empty())
        {
            const auto node{pending.front()};

            pending.pop_front();

            for (const auto to : graph.successor[node])
            {
                if (reaches[from][to] == 0)
                {
                    reaches[from][to] = 1;

                    pending.push_back(to);
                }
            }
        }
    }

    return reaches;
}

} // namespace

Premise zero_lag(const Table& table)
{
    std::vector<std::array<char, 2>> visited(table.states, {0, 0});

    std::deque<std::pair<std::size_t, int>> pending{{table.init, 0}};

    visited[table.init][0] = 1;

    while (!pending.empty())
    {
        const auto [state, seen]{pending.front()};

        pending.pop_front();

        if (seen != 0 && table.accept[state] == 0)
        {
            return {.holds = false, .witness_state = state, .witness_byte = 256};
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto to{table.next[state][static_cast<std::size_t>(byte)]};

            if (to == kDead)
            {
                continue;
            }

            const auto target{static_cast<std::size_t>(to)};

            const auto next_seen{seen != 0 || table.accept[target] != 0 ? 1 : 0};

            if (visited[target][static_cast<std::size_t>(next_seen)] == 0)
            {
                visited[target][static_cast<std::size_t>(next_seen)] = 1;

                pending.emplace_back(target, next_seen);
            }
        }
    }

    return {};
}

std::optional<Subset_graph> subset_graph(const Table& table, const std::size_t budget)
{
    std::map<std::vector<std::size_t>, std::size_t> ids{};

    Subset_graph graph{};

    const auto intern{[&](std::vector<std::size_t> subset) {
        if (const auto found{ids.find(subset)}; found != ids.end())
        {
            return found->second;
        }

        ids.emplace(subset, graph.subsets.size());

        graph.subsets.push_back(std::move(subset));

        graph.successor.emplace_back();

        return graph.subsets.size() - 1;
    }};

    intern(live_states(table));

    for (std::size_t at{0}; at < graph.subsets.size(); ++at)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            // Interning may grow the vectors, so the target is taken before the source's row is addressed.
            const auto target{intern(stepped_subset(table, graph.subsets[at], byte))};

            graph.successor[at][static_cast<std::size_t>(byte)] = target;

            if (graph.subsets.size() > budget)
            {
                return std::nullopt;
            }
        }
    }

    return graph;
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

    for (std::size_t node{0}; node < count; ++node)
    {
        order[node] = node;
    }

    std::ranges::sort(order, [&](const auto left, const auto right) {
        return graph.subsets[left].size() < graph.subsets[right].size();
    });

    for (const auto origin : order)
    {
        if (floor[origin] != untouched)
        {
            continue;
        }

        floor[origin] = graph.subsets[origin].size();

        std::vector<std::size_t> pending{origin};

        while (!pending.empty())
        {
            const auto node{pending.back()};

            pending.pop_back();

            for (const auto from : predecessors[node])
            {
                if (floor[from] == untouched)
                {
                    floor[from] = floor[origin];

                    pending.push_back(from);
                }
            }
        }
    }

    return floor;
}

Verdict decide(const Table& table)
{
    // The sustained-width phase is quadratic in the node count, so the decider's budget is far tighter than the
    // synthesizer's.
    const auto graph{subset_graph(table, 128)};

    if (!graph)
    {
        return {.bounded = true};
    }

    constexpr std::size_t origin{0};

    const auto count{graph->subsets.size()};

    const auto reaches{reachability(*graph)};

    std::size_t sustained{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (reaches[node][node] != 0)
        {
            sustained = std::max(sustained, graph->subsets[node].size());
        }
    }

    const auto floor{floors(*graph)};

    std::size_t wall{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (reaches[origin][node] != 0 || node == origin)
        {
            wall = std::max(wall, floor[node]);
        }
    }

    return {.nodes = count, .sustained = sustained, .floor_start = floor[origin], .wall_floor = wall};
}

} // namespace munch::tools::probes

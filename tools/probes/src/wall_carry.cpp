#include "munch/tools/probes/wall_carry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "munch/tools/probes/wall_subsets.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements wall_carry.hpp: the kernel, its edges, the transfer semigroup, the labeling search, the state flavors, the
// seed and the group are private to this unit.

/**
 * @brief Where a byte sends each member of a kernel node: position i of the source subset maps to the position of its
 *        successor in the target subset, a dying accepting member continuing through the initial state's transition.
 * @param table The table.
 * @param graph The subset graph.
 * @param node The source node.
 * @param byte The byte.
 * @return Per source position, its target position, std::nullopt when the byte eliminates or merges a member.
 */
std::optional<std::vector<std::size_t>> member_map(
        const Table& table, const Subset_graph& graph, const std::size_t node, const int byte)
{
    const auto& source{graph.subsets[node]};

    const auto& target{graph.subsets[graph.successor[node][static_cast<std::size_t>(byte)]]};

    std::vector<std::size_t> mapped(source.size());

    std::set<std::size_t> hit{};

    for (std::size_t at{0}; at < source.size(); ++at)
    {
        auto to{table.next[source[at]][static_cast<std::size_t>(byte)]};

        if (to == kDead)
        {
            if (table.accept[source[at]] == 0)
            {
                return std::nullopt;
            }

            to = table.next[table.init][static_cast<std::size_t>(byte)];

            if (to == kDead)
            {
                return std::nullopt;
            }
        }

        const auto found{std::ranges::lower_bound(target, static_cast<std::size_t>(to))};

        mapped[at] = static_cast<std::size_t>(found - target.begin());

        if (!hit.insert(mapped[at]).second)
        {
            return std::nullopt;
        }
    }

    return mapped;
}

/**
 * @brief One edge of the kernel, its nodes numbered by their rank in the kernel.
 */
struct Kernel_edge
{
    /**
     * @brief The source node's rank.
     */
    std::size_t source{};

    /**
     * @brief The byte.
     */
    int byte{};

    /**
     * @brief The target node's rank.
     */
    std::size_t target{};

    /**
     * @brief Per source position, its target position.
     */
    std::vector<std::size_t> map{};
};

/**
 * @brief An element of the kernel's transfer semigroup: per kernel node, its target node and the member bijection.
 */
using Summary_t = std::vector<std::pair<std::size_t, std::vector<std::size_t>>>;

/**
 * @brief The composition of two transfers, the first applied before the second.
 * @param first The first transfer.
 * @param second The second transfer.
 * @param wall The kernel subsets' width.
 * @return The composed transfer.
 */
Summary_t composed(const Summary_t& first, const Summary_t& second, const std::size_t wall)
{
    Summary_t result(first.size());

    for (std::size_t at{0}; at < first.size(); ++at)
    {
        const auto& [middle, before]{first[at]};

        const auto& [end, after]{second[middle]};

        result[at].first = end;

        result[at].second.resize(wall);

        for (std::size_t member{0}; member < wall; ++member)
        {
            result[at].second[member] = after[before[member]];
        }
    }

    return result;
}

/**
 * @brief The identity permutation of a number of flavors.
 * @param wall The number of flavors.
 * @return 0, 1, ..., wall - 1.
 */
std::vector<int> identity(const std::size_t wall)
{
    std::vector<int> result(wall);

    for (std::size_t at{0}; at < wall; ++at)
    {
        result[at] = static_cast<int>(at);
    }

    return result;
}

/**
 * @brief The permutation each byte acts by under a labeling, when every edge on the byte derives the same one.
 * @param edges The kernel's edges.
 * @param flavor_of Per kernel node's rank, the flavor of each member position.
 * @param wall The number of flavors.
 * @return Per byte, its permutation, empty for a byte no edge carries; std::nullopt when two edges on one byte derive
 *         different permutations.
 */
std::optional<std::array<std::vector<int>, 256>> byte_actions(
        const std::vector<Kernel_edge>& edges, const std::vector<std::vector<int>>& flavor_of, const std::size_t wall)
{
    std::array<std::vector<int>, 256> trial{};

    for (const auto& edge : edges)
    {
        std::vector<int> derived(wall, -1);

        for (std::size_t member{0}; member < wall; ++member)
        {
            derived[static_cast<std::size_t>(flavor_of[edge.source][member])] =
                    flavor_of[edge.target][edge.map[member]];
        }

        auto& slot{trial[static_cast<std::size_t>(edge.byte)]};

        if (slot.empty())
        {
            slot = derived;
        }
        else if (slot != derived)
        {
            return std::nullopt;
        }
    }

    return trial;
}

/**
 * @brief A flavor labeling of the kernel and the byte actions it explains.
 */
struct Labeling
{
    /**
     * @brief Per kernel node's rank, the flavor of each member position.
     */
    std::vector<std::vector<int>> flavor_of{};

    /**
     * @brief Per byte, the permutation of flavors it acts by.
     */
    std::array<std::vector<int>, 256> sigma{};
};

/**
 * @brief The kernel: the nodes whose floor and width both equal the wall floor, which must be successor-closed. A
 *        kernel that is not is refused with `refused: the kernel is not successor-closed`.
 * @param graph The subset graph.
 * @param floor Per node, its floor.
 * @param wall The wall floor.
 * @return The kernel's nodes, ascending, std::nullopt on the refusal.
 */
std::optional<std::vector<std::size_t>> kernel_of(
        const Subset_graph& graph, const std::vector<std::size_t>& floor, const std::size_t wall)
{
    const auto count{graph.subsets.size()};

    std::vector<char> kernel(count, 0);

    for (std::size_t node{0}; node < count; ++node)
    {
        kernel[node] = floor[node] == wall && graph.subsets[node].size() == wall ? 1 : 0;
    }

    for (std::size_t node{0}; node < count; ++node)
    {
        if (kernel[node] == 0)
        {
            continue;
        }

        for (const auto to : graph.successor[node])
        {
            if (kernel[to] == 0)
            {
                std::cout << "refused: the kernel is not successor-closed\n";

                return std::nullopt;
            }
        }
    }

    std::vector<std::size_t> nodes{};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (kernel[node] != 0)
        {
            nodes.push_back(node);
        }
    }

    return nodes;
}

/**
 * @brief The kernel's edges, every kernel node's in ascending byte order, the nodes in ascending order. A byte that
 *        eliminates or merges a member is refused with `refused: a kernel byte action eliminates or merges`.
 * @param table The table.
 * @param graph The subset graph.
 * @param nodes The kernel's nodes, ascending.
 * @return The edges, std::nullopt on the refusal.
 */
std::optional<std::vector<Kernel_edge>> kernel_edges(
        const Table& table, const Subset_graph& graph, const std::vector<std::size_t>& nodes)
{
    std::map<std::size_t, std::size_t> dense{};

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        dense.emplace(nodes[at], at);
    }

    std::vector<Kernel_edge> edges{};

    for (const auto node : nodes)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            const auto mapped{member_map(table, graph, node, byte)};

            if (!mapped)
            {
                std::cout << "refused: a kernel byte action eliminates or merges\n";

                return std::nullopt;
            }

            edges.push_back(
                    {.source = dense.at(node),
                     .byte = byte,
                     .target = dense.at(graph.successor[node][static_cast<std::size_t>(byte)]),
                     .map = *mapped});
        }
    }

    return edges;
}

/**
 * @brief Counts the kernel's transfer semigroup over nonempty factors, the closure of the bytes' transfers under
 *        composition; the empty word's identity is not counted. A closure past 4,096 elements is refused with `refused:
 *        the transfer closure exceeds the budget`.
 * @param edges The kernel's edges.
 * @param kernel_size The number of kernel nodes.
 * @param wall The kernel subsets' width.
 * @return The number of elements, std::nullopt on the refusal.
 */
std::optional<std::size_t> transfer_semigroup(
        const std::vector<Kernel_edge>& edges, const std::size_t kernel_size, const std::size_t wall)
{
    // Planning composes nonempty chunks only, so the empty word's identity is outside the count; a model admitting
    // empty chunks would add it and could round the price up one bit.
    std::map<int, Summary_t> letters{};

    for (const auto& edge : edges)
    {
        auto& letter{letters[edge.byte]};

        letter.resize(kernel_size);

        letter[edge.source] = {edge.target, edge.map};
    }

    std::set<Summary_t> semigroup{};

    std::deque<Summary_t> queue{};

    for (const auto& [byte, letter] : letters)
    {
        if (semigroup.insert(letter).second)
        {
            queue.push_back(letter);
        }
    }

    while (!queue.empty())
    {
        const auto element{queue.front()};

        queue.pop_front();

        for (const auto& [byte, letter] : letters)
        {
            const auto product{composed(element, letter, wall)};

            if (semigroup.insert(product).second)
            {
                queue.push_back(product);
            }

            if (semigroup.size() > 4096)
            {
                std::cout << "refused: the transfer closure exceeds the budget\n";

                return std::nullopt;
            }
        }
    }

    return semigroup.size();
}

/**
 * @brief Whether the labeling search's assignments, the wall's factorial to the power of the non-base kernel nodes,
 *        stay within 100,000. The division test is exact: the floor of the budget over the factorial is the largest
 *        count whose product stays within the budget, so no intermediate can wrap.
 * @param wall The kernel subsets' width.
 * @param kernel_size The number of kernel nodes.
 * @return True when the search stays within the budget.
 */
bool is_labeling_within_budget(const std::size_t wall, const std::size_t kernel_size)
{
    // Dimensional caps cannot bound the product: a table with a tiny closure can still demand ten to the fourteenth
    // assignments, so the budget refuses before any permutation is materialized. A retained value is at most the budget
    // times the largest admitted factorial, far inside the type even after one further multiply.
    std::size_t factorial{1};

    for (std::size_t at{2}; at <= wall; ++at)
    {
        factorial *= at;
    }

    std::size_t assignments{1};

    auto overflowed{false};

    for (std::size_t at{1}; at < kernel_size && !overflowed; ++at)
    {
        overflowed = assignments > 100000 / factorial;

        assignments *= factorial;
    }

    return !overflowed && assignments <= 100000;
}

/**
 * @brief Searches the flavor labelings exactly: the first kernel node keeps the identity labeling, every other tries
 *        each permutation of flavors in lexicographic order, the choices advancing as a mixed-radix counter from the
 *        second node, and the first labeling under which every byte acts by one permutation wins.
 * @param edges The kernel's edges.
 * @param kernel_size The number of kernel nodes.
 * @param wall The number of flavors.
 * @return The winning labeling, std::nullopt when none makes the byte actions source-independent.
 */
std::optional<Labeling> search_labeling(
        const std::vector<Kernel_edge>& edges, const std::size_t kernel_size, const std::size_t wall)
{
    constexpr std::size_t base{0};

    std::vector<std::vector<int>> permutations{};

    auto current{identity(wall)};

    do
    {
        permutations.push_back(current);
    } while (std::ranges::next_permutation(current).found);

    std::vector<std::vector<int>> flavor_of(kernel_size);

    std::vector<std::size_t> choice(kernel_size, 0);

    for (auto searching{true}; searching;)
    {
        for (std::size_t at{0}; at < kernel_size; ++at)
        {
            flavor_of[at] = permutations[at == base ? 0 : choice[at]];
        }

        if (const auto sigma{byte_actions(edges, flavor_of, wall)})
        {
            return Labeling{.flavor_of = flavor_of, .sigma = *sigma};
        }

        searching = false;

        for (std::size_t at{0}; at < kernel_size; ++at)
        {
            if (at == base)
            {
                continue;
            }

            if (++choice[at] < permutations.size())
            {
                searching = true;

                break;
            }

            choice[at] = 0;
        }
    }

    return std::nullopt;
}

/**
 * @brief Every state's flavor under a labeling, which must agree wherever a state appears across kernel subsets. A
 *        state with two flavors is refused with `refused: a state carries two flavors across kernel subsets`.
 * @param table The table.
 * @param graph The subset graph.
 * @param nodes The kernel's nodes, ascending.
 * @param flavor_of Per kernel node's rank, the flavor of each member position.
 * @param wall The kernel subsets' width.
 * @return Per state, its flavor, -1 for a state no kernel subset holds; std::nullopt on the refusal.
 */
std::optional<std::vector<int>> state_flavors(
        const Table& table, const Subset_graph& graph, const std::vector<std::size_t>& nodes,
        const std::vector<std::vector<int>>& flavor_of, const std::size_t wall)
{
    std::vector<int> state_flavor(table.states, -1);

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        for (std::size_t member{0}; member < wall; ++member)
        {
            const auto state{graph.subsets[nodes[at]][member]};

            if (state_flavor[state] == -1)
            {
                state_flavor[state] = flavor_of[at][member];
            }
            else if (state_flavor[state] != flavor_of[at][member])
            {
                std::cout << "refused: a state carries two flavors across kernel subsets\n";

                return std::nullopt;
            }
        }
    }

    return state_flavor;
}

/**
 * @brief The boundary seed: every live image of the initial state, its byte's permutation undone, must name one flavor.
 *        It is refused with `refused: a live initial-state image is unflavored` at a live image no kernel subset holds,
 *        `refused: the boundary seed is byte-dependent` at an image naming another flavor, and `refused: no restart
 *        entry reaches a flavored state` when the initial state has no live image.
 * @param table The table.
 * @param state_flavor Per state, its flavor.
 * @param sigma Per byte, the permutation of flavors it acts by.
 * @param wall The number of flavors.
 * @return The seed, std::nullopt on a refusal.
 */
std::optional<int> boundary_seed(
        const Table& table, const std::vector<int>& state_flavor, const std::array<std::vector<int>, 256>& sigma,
        const std::size_t wall)
{
    int seed{-1};

    for (int byte{0}; byte < 256; ++byte)
    {
        const auto to{table.next[table.init][static_cast<std::size_t>(byte)]};

        if (to == kDead)
        {
            continue;
        }

        // A live image the labeling does not cover is a refusal, never a skip: the true scan can occupy it while the
        // carry claims a flavor, and the conditioned cloud would exclude the true reading.
        if (state_flavor[static_cast<std::size_t>(to)] == -1)
        {
            std::cout << "refused: a live initial-state image is unflavored\n";

            return std::nullopt;
        }

        const auto& perm{sigma[static_cast<std::size_t>(byte)]};

        int undone{-1};

        for (std::size_t at{0}; at < wall; ++at)
        {
            if (perm[at] == state_flavor[static_cast<std::size_t>(to)])
            {
                undone = static_cast<int>(at);
            }
        }

        if (seed == -1)
        {
            seed = undone;
        }
        else if (seed != undone)
        {
            std::cout << "refused: the boundary seed is byte-dependent\n";

            return std::nullopt;
        }
    }

    if (seed == -1)
    {
        std::cout << "refused: no restart entry reaches a flavored state\n";

        return std::nullopt;
    }

    return seed;
}

/**
 * @brief The carry group: the identity and the bytes' permutations closed under composition.
 * @param sigma Per byte, the permutation of flavors it acts by.
 * @param wall The number of flavors.
 * @return The group's elements.
 */
std::set<std::vector<int>> carry_group(const std::array<std::vector<int>, 256>& sigma, const std::size_t wall)
{
    std::set<std::vector<int>> group{identity(wall)};

    std::deque<std::vector<int>> pending{};

    for (int byte{0}; byte < 256; ++byte)
    {
        if (group.insert(sigma[static_cast<std::size_t>(byte)]).second)
        {
            pending.push_back(sigma[static_cast<std::size_t>(byte)]);
        }
    }

    const std::set<std::vector<int>> generators{group};

    while (!pending.empty())
    {
        const auto element{pending.front()};

        pending.pop_front();

        for (const auto& generator : generators)
        {
            std::vector<int> product(wall);

            for (std::size_t at{0}; at < wall; ++at)
            {
                product[at] = generator[static_cast<std::size_t>(element[at])];
            }

            if (group.insert(product).second)
            {
                pending.push_back(product);
            }
        }
    }

    return group;
}

} // namespace

std::optional<Carry> synthesize(const Table& table)
{
    // The shipped window certificate refuses nullable token sets wholesale; the synthesis mirrors that scope rather
    // than answering beyond it.
    if (table.accept[table.init] != 0)
    {
        std::cout << "refused: the token set is nullable\n";

        return std::nullopt;
    }

    if (!zero_lag(table).holds)
    {
        std::cout << "refused: the zero-lag premise fails\n";

        return std::nullopt;
    }

    const auto graph{subset_graph(table, 4096)};

    if (!graph)
    {
        std::cout << "refused: the subset graph exceeds its budget\n";

        return std::nullopt;
    }

    const auto floor{floors(*graph)};

    const auto wall{std::ranges::max(floor)};

    if (wall < 2)
    {
        std::cout << "refused: state-granular wall floor " << wall
                  << " is below two; the origin-level verdict is unavailable\n";

        return std::nullopt;
    }

    const auto nodes{kernel_of(*graph, floor, wall)};

    if (!nodes)
    {
        return std::nullopt;
    }

    // Resource caps run before any factorial or closure work, and the closure itself carries a hard element budget:
    // dimensional caps alone cannot bound it, since six width-six subsets admit wreath-product closures beyond any
    // enumeration. The labeling search is exhaustive over wall-factorial permutations and the summary closure grows
    // with the kernel, so both are bounded here, refused fail-closed beyond.
    if (nodes->size() > 6)
    {
        std::cout << "refused: the kernel has too many subsets for the labeling search\n";

        return std::nullopt;
    }

    if (wall > 6)
    {
        std::cout << "refused: the wall is too wide for the labeling search\n";

        return std::nullopt;
    }

    const auto edges{kernel_edges(table, *graph, *nodes)};

    if (!edges)
    {
        return std::nullopt;
    }

    const auto semigroup{transfer_semigroup(*edges, nodes->size(), wall)};

    if (!semigroup)
    {
        return std::nullopt;
    }

    if (!is_labeling_within_budget(wall, nodes->size()))
    {
        std::cout << "refused: the labeling search exceeds its budget\n";

        return std::nullopt;
    }

    const auto labeling{search_labeling(*edges, nodes->size(), wall)};

    if (!labeling)
    {
        std::cout << "refused: no flavor labeling makes the byte actions source-independent; exact summary count "
                  << *semigroup << "\n";

        return std::nullopt;
    }

    auto state_flavor{state_flavors(table, *graph, *nodes, labeling->flavor_of, wall)};

    if (!state_flavor)
    {
        return std::nullopt;
    }

    const auto seed{boundary_seed(table, *state_flavor, labeling->sigma, wall)};

    if (!seed)
    {
        return std::nullopt;
    }

    return Carry{
            .width = wall,
            .kernel_subsets = nodes->size(),
            .semigroup = *semigroup,
            .state_flavor = std::move(*state_flavor),
            .sigma = labeling->sigma,
            .group = carry_group(labeling->sigma, wall),
            .seed = *seed};
}

} // namespace munch::tools::probes

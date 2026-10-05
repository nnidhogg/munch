#include "munch/tools/probes/wall_carry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "munch/tools/probes/wall_subsets.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The subset graph's node budget.
 */
constexpr std::size_t subset_graph_budget{4096};

/**
 * @brief The transfer closure's element budget.
 */
constexpr std::size_t transfer_budget{4096};

/**
 * @brief The labeling search's assignment budget.
 */
constexpr std::size_t labeling_budget{100'000};

/**
 * @brief The most kernel subsets the labeling search admits.
 */
constexpr std::size_t kernel_subset_cap{6};

/**
 * @brief The widest wall the labeling search admits.
 */
constexpr std::size_t wall_width_cap{6};

/**
 * @brief Per byte value, one permutation of flavors.
 */
using Byte_actions_t = std::array<std::vector<int>, byte_count>;

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
    Byte_actions_t sigma{};
};

/**
 * @brief Returns where a byte sends each member of a kernel node: position i of the source subset maps to the position
 *        of its successor in the target subset, a dying accepting member continuing through the initial state's
 *        transition.
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

    const auto successor{graph.successor[node][static_cast<std::size_t>(byte)]};

    const auto& target{graph.subsets[successor]};

    std::vector<std::size_t> mapped(source.size());

    std::set<std::size_t> hit{};

    for (std::size_t at{0}; at < source.size(); ++at)
    {
        // A dying member continues through the initial state's transition when it accepts, and is eliminated otherwise.
        const auto to{scan_step(table, source[at], static_cast<unsigned char>(byte))};

        if (!to)
        {
            return std::nullopt;
        }

        const auto found{std::ranges::lower_bound(target, *to)};

        mapped[at] = static_cast<std::size_t>(found - target.begin());

        const auto [position, inserted]{hit.insert(mapped[at])};

        if (!inserted)
        {
            return std::nullopt;
        }
    }

    return mapped;
}

/**
 * @brief Returns the composition of two transfers, the first applied before the second.
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

        auto& [target, map]{result[at]};

        target = end;

        map.resize(wall);

        const auto through_second{[&after](const std::size_t position) { return after[position]; }};

        std::ranges::transform(before | std::views::take(wall), map.begin(), through_second);
    }

    return result;
}

/**
 * @brief Returns the identity permutation of a number of flavors.
 * @param wall The number of flavors.
 * @return 0, 1, ..., wall - 1.
 */
std::vector<int> identity(const std::size_t wall)
{
    std::vector<int> result(wall);

    std::ranges::iota(result, 0);

    return result;
}

/**
 * @brief Returns the permutation each byte acts by under a labeling, when every edge on the byte derives the same one.
 * @param edges The kernel's edges.
 * @param flavor_of Per kernel node's rank, the flavor of each member position.
 * @param wall The number of flavors.
 * @return Per byte, its permutation, empty for a byte no edge carries; std::nullopt when two edges on one byte derive
 *         different permutations.
 */
std::optional<Byte_actions_t> byte_actions(
        const std::vector<Kernel_edge>& edges, const std::vector<std::vector<int>>& flavor_of, const std::size_t wall)
{
    Byte_actions_t trial{};

    for (const auto& [source, byte, target, map] : edges)
    {
        std::vector<int> derived(wall, -1);

        for (std::size_t member{0}; member < wall; ++member)
        {
            const auto from{static_cast<std::size_t>(flavor_of[source][member])};

            derived[from] = flavor_of[target][map[member]];
        }

        auto& slot{trial[static_cast<std::size_t>(byte)]};

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
 * @brief Returns the kernel: the nodes whose floor and width both equal the wall floor, which must be successor-closed.
 *        A kernel that is not is refused with `refused: the kernel is not successor-closed`.
 * @param graph The subset graph.
 * @param floor Per node, its floor.
 * @param wall The wall floor.
 * @return The kernel's nodes, ascending, std::nullopt on the refusal.
 */
std::optional<std::vector<std::size_t>> kernel_of(
        const Subset_graph& graph, const std::vector<std::size_t>& floor, const std::size_t wall)
{
    const auto count{graph.subsets.size()};

    std::vector<Flag> kernel(count, Flag::off);

    for (std::size_t node{0}; node < count; ++node)
    {
        kernel[node] = floor[node] == wall && node_width(graph, node) == wall ? Flag::on : Flag::off;
    }

    const auto in_kernel{[&kernel](const std::size_t node) { return kernel[node] == Flag::on; }};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (!in_kernel(node))
        {
            continue;
        }

        if (!std::ranges::all_of(graph.successor[node], in_kernel))
        {
            std::cout << "refused: the kernel is not successor-closed\n";

            return std::nullopt;
        }
    }

    std::vector<std::size_t> nodes{};

    std::ranges::copy_if(std::views::iota(std::size_t{0}, count), std::back_inserter(nodes), in_kernel);

    return nodes;
}

/**
 * @brief Returns the kernel's edges, every kernel node's in ascending byte order, the nodes in ascending order. A byte
 *        that eliminates or merges a member is refused with `refused: a kernel byte action eliminates or merges`.
 * @param table The table.
 * @param graph The subset graph.
 * @param nodes The kernel's nodes, ascending.
 * @return The edges, std::nullopt on the refusal.
 */
std::optional<std::vector<Kernel_edge>> kernel_edges(
        const Table& table, const Subset_graph& graph, const std::vector<std::size_t>& nodes)
{
    std::map<std::size_t, std::size_t> dense{};

    for (const auto& [at, node] : std::views::enumerate(nodes))
    {
        dense.emplace(node, static_cast<std::size_t>(at));
    }

    std::vector<Kernel_edge> edges{};

    for (const auto node : nodes)
    {
        for (int byte{0}; byte < byte_count; ++byte)
        {
            const auto mapped{member_map(table, graph, node, byte)};

            if (!mapped)
            {
                std::cout << "refused: a kernel byte action eliminates or merges\n";

                return std::nullopt;
            }

            const auto successor{graph.successor[node][static_cast<std::size_t>(byte)]};

            edges.push_back(
                    Kernel_edge{.source = dense.at(node), .byte = byte, .target = dense.at(successor), .map = *mapped});
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
    // Planning composes nonempty chunks only, so the empty word's identity is outside the count.
    std::map<int, Summary_t> letters{};

    for (const auto& [source, byte, target, map] : edges)
    {
        auto& letter{letters[byte]};

        letter.resize(kernel_size);

        letter[source] = {target, map};
    }

    std::set<Summary_t> semigroup{};

    std::deque<Summary_t> queue{};

    for (const auto& [byte, letter] : letters)
    {
        const auto [position, inserted]{semigroup.insert(letter)};

        if (inserted)
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

            const auto [position, inserted]{semigroup.insert(product)};

            if (inserted)
            {
                queue.push_back(product);
            }

            if (semigroup.size() > transfer_budget)
            {
                std::cout << "refused: the transfer closure exceeds the budget\n";

                return std::nullopt;
            }
        }
    }

    return semigroup.size();
}

/**
 * @brief Returns whether the labeling search's assignments, the wall's factorial to the power of the non-base kernel
 *        nodes, stay within 100,000. The division test is exact: the floor of the budget over the factorial is the
 *        largest count whose product stays within the budget, so no intermediate can wrap.
 * @param wall The kernel subsets' width.
 * @param kernel_size The number of kernel nodes.
 * @return True when the search stays within the budget.
 */
bool is_labeling_within_budget(const std::size_t wall, const std::size_t kernel_size)
{
    // Dimensional caps cannot bound the product: a table with a tiny closure can still demand ten to the fourteenth
    // assignments, so the budget refuses before any permutation is materialized. A retained value is at most the budget
    // times the largest admitted factorial, far inside the type even after one further multiply.
    const auto factors{std::views::iota(std::size_t{1}, wall + 1)};

    const auto factorial{std::ranges::fold_left(factors, std::size_t{1}, std::multiplies{})};

    std::size_t assignments{1};

    for (std::size_t at{1}; at < kernel_size; ++at)
    {
        if (assignments > labeling_budget / factorial)
        {
            return false;
        }

        assignments *= factorial;
    }

    return assignments <= labeling_budget;
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
            flavor_of[at] = permutations[choice[at]];
        }

        if (const auto sigma{byte_actions(edges, flavor_of, wall)})
        {
            return Labeling{.flavor_of = flavor_of, .sigma = *sigma};
        }

        searching = false;

        // The first node keeps the identity labeling, so the counter runs from the second.
        for (std::size_t at{1}; at < kernel_size; ++at)
        {
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
 * @brief Returns every state's flavor under a labeling, which must agree wherever a state appears across kernel
 *        subsets. A state with two flavors is refused with `refused: a state carries two flavors across kernel
 *        subsets`.
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

    for (const auto& [node, flavors] : std::views::zip(nodes, flavor_of))
    {
        const auto& subset{graph.subsets[node]};

        for (std::size_t member{0}; member < wall; ++member)
        {
            const auto state{subset[member]};

            if (state_flavor[state] == -1)
            {
                state_flavor[state] = flavors[member];
            }
            else if (state_flavor[state] != flavors[member])
            {
                std::cout << "refused: a state carries two flavors across kernel subsets\n";

                return std::nullopt;
            }
        }
    }

    return state_flavor;
}

/**
 * @brief Returns the boundary seed: every live image of the initial state, its byte's permutation undone, must name one
 *        flavor. It is refused with `refused: a live initial-state image is unflavored` at a live image no kernel
 *        subset holds, `refused: the boundary seed is byte-dependent` at an image naming another flavor, and `refused:
 *        no restart entry reaches a flavored state` when the initial state has no live image.
 * @param table The table.
 * @param state_flavor Per state, its flavor.
 * @param sigma Per byte, the permutation of flavors it acts by.
 * @param wall The number of flavors.
 * @return The seed, std::nullopt on a refusal.
 */
std::optional<int> boundary_seed(
        const Table& table, const std::vector<int>& state_flavor, const Byte_actions_t& sigma, const std::size_t wall)
{
    int seed{-1};

    for (int byte{0}; byte < byte_count; ++byte)
    {
        const auto to{table.next[table.init][static_cast<std::size_t>(byte)]};

        if (to == dead)
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

        const auto flavors{perm | std::views::take(wall)};

        const auto found{std::ranges::find(flavors, state_flavor[static_cast<std::size_t>(to)])};

        const auto undone{found == flavors.end() ? -1 : static_cast<int>(found - flavors.begin())};

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
 * @brief Returns the carry group: the identity and the bytes' permutations closed under composition.
 * @param sigma Per byte, the permutation of flavors it acts by.
 * @param wall The number of flavors.
 * @return The group's elements.
 */
std::set<std::vector<int>> carry_group(const Byte_actions_t& sigma, const std::size_t wall)
{
    std::set<std::vector<int>> group{identity(wall)};

    std::deque<std::vector<int>> pending{};

    for (const auto& permutation : sigma)
    {
        const auto [position, inserted]{group.insert(permutation)};

        if (inserted)
        {
            pending.push_back(permutation);
        }
    }

    const std::set<std::vector<int>> generators{group};

    while (!pending.empty())
    {
        const auto element{pending.front()};

        pending.pop_front();

        for (const auto& generator : generators)
        {
            const auto through_generator{
                    [&generator](const int flavor) { return generator[static_cast<std::size_t>(flavor)]; }};

            std::vector<int> product(wall);

            std::ranges::transform(element | std::views::take(wall), product.begin(), through_generator);

            const auto [position, inserted]{group.insert(product)};

            if (inserted)
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
    // The shipped window certificate refuses nullable token sets wholesale, and the synthesis refuses them the same
    // way.
    if (table.accept[table.init] == Flag::on)
    {
        std::cout << "refused: the token set is nullable\n";

        return std::nullopt;
    }

    if (!zero_lag(table).holds)
    {
        std::cout << "refused: the zero-lag premise fails\n";

        return std::nullopt;
    }

    const auto graph{subset_graph(table, subset_graph_budget)};

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

    // The resource caps run before any factorial or closure work.
    if (nodes->size() > kernel_subset_cap)
    {
        std::cout << "refused: the kernel has too many subsets for the labeling search\n";

        return std::nullopt;
    }

    if (wall > wall_width_cap)
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

    const auto& [flavor_of, sigma]{*labeling};

    auto state_flavor{state_flavors(table, *graph, *nodes, flavor_of, wall)};

    if (!state_flavor)
    {
        return std::nullopt;
    }

    const auto seed{boundary_seed(table, *state_flavor, sigma, wall)};

    if (!seed)
    {
        return std::nullopt;
    }

    auto group{carry_group(sigma, wall)};

    return Carry{
            .width = wall,
            .kernel_subsets = nodes->size(),
            .semigroup = *semigroup,
            .state_flavor = std::move(*state_flavor),
            .sigma = sigma,
            .group = std::move(group),
            .seed = *seed};
}

} // namespace munch::tools::probes

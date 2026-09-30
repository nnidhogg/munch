#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_CARRY_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_CARRY_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <vector>

#include "munch/tools/probes/wall_table.hpp"

/**
 * @brief The carry of an absolute wall, Carry and synthesize: the kernel's flavor labeling, one permutation of flavors
 *        per byte, the group they generate and the boundary seed, or a refusal with its reason.
 */
namespace munch::tools::probes
{
/**
 * @brief A synthesized carry: the true scan's flavor after any prefix is the ordered product of its bytes' permutations
 *        applied to the seed.
 */
struct Carry
{
    /**
     * @brief The wall floor, the kernel subsets' common width and the number of flavors.
     */
    std::size_t width{};

    /**
     * @brief The number of subsets in the kernel.
     */
    std::size_t kernel_subsets{};

    /**
     * @brief The number of elements of the kernel's transfer semigroup over nonempty factors.
     */
    std::size_t semigroup{};

    /**
     * @brief Per table state, its flavor, -1 for a state no kernel subset holds.
     */
    std::vector<int> state_flavor{};

    /**
     * @brief Per byte, the permutation of flavors it acts by.
     */
    std::array<std::vector<int>, 256> sigma{};

    /**
     * @brief The permutations the bytes' permutations generate under composition, the identity among them.
     */
    std::set<std::vector<int>> group{};

    /**
     * @brief The flavor at a token boundary, before any byte.
     */
    int seed{-1};
};

/**
 * @brief Synthesizes a table's carry. The table must be non-nullable, hold the zero-lag premise, keep its subset graph
 *        within 4,096 nodes and have a wall floor of two or more; the kernel, the constant-width nodes at the wall
 *        floor, must be successor-closed, hold at most six subsets of width at most six, and map its members
 *        bijectively under every byte; the transfer closure must stay within 4,096 elements and the labeling search
 *        within 100,000 assignments; one flavor labeling must make every byte act by one permutation, every state carry
 *        one flavor, and the initial state's live images name one seed. The first condition that fails is printed on
 *        standard output as a `refused: ` line.
 * @param table The table.
 * @return The carry, std::nullopt on a refusal.
 */
[[nodiscard]] std::optional<Carry> synthesize(const Table& table);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_CARRY_HPP

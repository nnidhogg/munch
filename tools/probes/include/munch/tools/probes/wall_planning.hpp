#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PLANNING_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PLANNING_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_table.hpp"

/**
 * @brief Windows certified behind the resolved flavor and the plans they license, held to the serial scan, window_walk,
 *        check_theorem, Plan, plan, Tally and run.
 */
namespace munch::tools::probes
{
/**
 * @brief The shipped window decision's cloud walk over a table from a chosen starting cloud: every live state for
 *        flavor -1, the unconditional decision, or the states of one flavor alone, the conditional one. Each cloud
 *        member carries its token's origin, the window's start for a token begun inside it and unknown otherwise; a
 *        member whose byte has a transition advances, and when any member accepts, the initial state's transition on
 *        the byte starts a token there. The window certifies when every surviving member carries one known origin.
 * @param table The table.
 * @param carry The carry naming each state's flavor; read only for a flavor of 0 or more.
 * @param reentrant Whether some transition enters the initial state, so that advancing from it starts no token.
 * @param window The window.
 * @param flavor The flavor whose states start the walk, -1 for every live state.
 * @return The certified origin, std::nullopt for an empty window, a nullable table, an empty starting cloud, a cloud
 *         that dies, or survivors that disagree.
 */
[[nodiscard]] std::optional<std::size_t> window_walk(
        const Table& table, const Carry& carry, bool reentrant, std::string_view window, int flavor);

/**
 * @brief Holds the carry to the true maximal-munch scan, restarts included: over 200 trials of up to 400 bytes drawn
 *        from an alphabet by one Lcg64 seeded 0x5eed5eed5eed5eed, the scan's state after every byte must carry the
 *        flavor the byte permutations predict from the seed. A trial ends early where the scan fails.
 * @param table The table.
 * @param carry The table's carry.
 * @param alphabet The bytes the trials are drawn from.
 * @return The positions checked before the first misprediction, 80,000 when every trial runs its length and agrees.
 */
[[nodiscard]] std::size_t check_theorem(const Table& table, const Carry& carry, const std::string& alphabet);

/**
 * @brief The cuts of one carry-driven plan and the unconditional census taken beside it.
 */
struct Plan
{
    /**
     * @brief The cuts, strictly ascending, each inside the input.
     */
    std::vector<std::size_t> cuts{};

    /**
     * @brief The windows the unconditional walk certified among every window the plan tried.
     */
    std::size_t unconditional_certificates{};
};

/**
 * @brief Plans cuts driven by prefix carries: the flavor at every position follows from the seed by the byte
 *        permutations, and from every equal-division target the plan tries the two-to-four-byte windows at each
 *        position from the target, or one past the last cut, and cuts at the first occurrence plus the origin that the
 *        window certifies under the position's flavor, past the last cut and inside the input. Every tried window is
 *        also walked unconditionally and counted when it certifies.
 * @param table The table.
 * @param carry The table's carry.
 * @param reentrant Whether some transition enters the initial state.
 * @param input The input.
 * @param chunks The number of equal divisions, so chunks - 1 targets.
 * @param flip Whether each position is asked under the next flavor rather than its own, the deliberately wrong one.
 * @return The cuts and the unconditional census.
 */
[[nodiscard]] Plan plan(
        const Table& table, const Carry& carry, bool reentrant, std::string_view input, std::size_t chunks, bool flip);

/**
 * @brief The counts of one planning campaign.
 */
struct Tally
{
    /**
     * @brief The cuts the right-flavor plans made.
     */
    std::size_t cuts{};

    /**
     * @brief The right-flavor cuts off the serial segmentation.
     */
    std::size_t off_boundary{};

    /**
     * @brief The trials whose corpus the serial scan refused or whose spliced boundaries differ from the serial ones.
     */
    std::size_t splice_mismatches{};

    /**
     * @brief The windows the unconditional walk certified across the right-flavor plans.
     */
    std::size_t unconditional{};

    /**
     * @brief The wrong-flavor cuts off the serial segmentation.
     */
    std::size_t teeth_bad{};
};

/**
 * @brief Runs the planning campaign over 30 generated corpora: each planned in eight chunks under the right flavors,
 *        its cuts held to the serial segmentation and its chunks, each scanned alone, spliced and held to the serial
 *        boundaries, then planned under the wrong flavors, whose cuts are held to the segmentation too.
 * @param table The table.
 * @param carry The table's carry.
 * @param ticks Whether the corpora mix backtick strings among the double-quoted ones.
 * @return The campaign's counts.
 */
[[nodiscard]] Tally run(const Table& table, const Carry& carry, bool ticks);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PLANNING_HPP

#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PRICES_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PRICES_HPP

#include <cstddef>
#include <string>

#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_table.hpp"

/**
 * @brief The price tower of a carry and its hazard witnesses, Prices and price.
 */
namespace munch::tools::probes
{
/**
 * @brief The price tower of one carry: positional control, the bits of the seed's orbit; compositional control, the
 *        bits of the group's order; and the exact summary, the bits of the kernel's transfer semigroup.
 */
struct Prices
{
    /**
     * @brief The number of flavors in the seed's orbit under the group.
     */
    std::size_t orbit{};

    /**
     * @brief The bits that name a flavor of the orbit, the least b with 2^b at least the orbit's size.
     */
    std::size_t positional{};

    /**
     * @brief The bits that name an element of the group.
     */
    std::size_t compositional{};

    /**
     * @brief The bits that name an element of the transfer semigroup.
     */
    std::size_t summary{};

    /**
     * @brief The ordered pairs of distinct orbit flavors a hazard witness was found for.
     */
    std::size_t witnesses{};
};

/**
 * @brief Prices a carry and searches a hazard witness for every ordered pair of distinct flavors of the seed's orbit: a
 *        tokenizable input, a prefix of up to six bytes carrying the true flavor, a window of two or three bytes and a
 *        suffix of up to two, all over an alphabet, whose window certifies under the assumed flavor at a cut off the
 *        serial segmentation. It asserts that the orbit reaches every flavor, that every pair has a witness and that
 *        every witness's cut lies off the serial segmentation, and prints the tower on standard output as `<name>:
 *        orbit ..., positional bits ..., compositional bits ... (group ...), summary bits ... (semigroup ...),
 *        witnesses ...`.
 *
 * Faithfulness holds by construction: group elements are stored as permutations, so distinct elements differ on some
 * flavor. What the witnesses establish is a hazard relation: conditioning on the wrong flavor licenses a cut off the
 * serial segmentation. The three prices bind three distinct services, named exactly: the orbit prices the conditioned
 * flavor choice at a position; the group prices composable flavor transfer, owed only by a service required to compose
 * arbitrary factors; the semigroup prices exact kernel transfer, a stronger service the cut machinery never needs. None
 * of the three binds every scheme providing the same cuts: a serial flavor prepass realizes them without composing
 * anything, and rescanning the raw prefix from the initial state at each query carries zero bits, paying work instead.
 * A scheme-wide bit bound needs an explicit one-pass compositional interface and common-context fooling pairs, which
 * live with the width program's summary model, not here.
 * @param assertions The probe's assertions.
 * @param name The row's name, which opens the printed line and every failure.
 * @param table The table.
 * @param carry The table's carry.
 * @param alphabet The bytes the witnesses are built from.
 * @return The tower.
 */
[[nodiscard]] Prices price(
        Assertions& assertions, const std::string& name, const Table& table, const Carry& carry,
        const std::string& alphabet);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_PRICES_HPP

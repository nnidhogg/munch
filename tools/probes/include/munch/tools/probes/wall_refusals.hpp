#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_REFUSALS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_REFUSALS_HPP

#include "munch/tools/probes/assertions.hpp"

/**
 * @brief Every refusal of the synthesis, the premise, the decider and the window walk pinned on its own table,
 *        check_refusals.
 */
namespace munch::tools::probes
{
/**
 * @brief Runs the refusal battery, 22 fixtures in turn: the unflavored initial image at both ends of the byte order,
 *        the end-of-input premise at both byte ends of both products, the nullable scope for the synthesis and for the
 *        window walk, the closure budget, the assignments budget, the subset budgets of the synthesizer and the
 *        decider, both labeling caps on both sides of their thresholds, the floor below two, the byte-dependent seed in
 *        both directions, the group's generator at the last byte and the eroding chain. Each fixture pins a guard's
 *        existence and direction; a budget constant is pinned only where an instance sits at its boundary. The
 *        kernel-closure and member-map checks and the labeling-conflict search have no reachable negative fixture: a
 *        violating shape falls out of the kernel before either check fires, and a byte's action is state-determined, so
 *        subset-dependent behaviour needs a restart asymmetry no small table produces. Every refusal prints its
 *        `refused: ` line on standard output.
 * @param assertions The probe's assertions.
 */
void check_refusals(Assertions& assertions);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_REFUSALS_HPP

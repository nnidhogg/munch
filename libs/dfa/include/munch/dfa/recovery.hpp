#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_RECOVERY_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_RECOVERY_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief The lag of the token set: the longest run of nonaccepting states a scan can traverse after
 *        leaving an accepting state, or nothing when that run is unbounded.
 *
 * States that can no longer reach an accepting one still count: a failed lookahead buffers bytes whether or
 * not the excursion could still accept, so the measure counts every defined continuation. Zero is the premise
 * under which a scheme restarting at every accept executes serial maximal munch exactly; a bounded value
 * prices the checkpoint a rollback-aware scheme must carry; an unbounded region, reported as nothing, carries
 * a cycle witness in the tables themselves.
 * @param simulator The compiled token set.
 * @return The lag, or std::nullopt when a post-accept nonaccepting cycle makes it unbounded.
 */
[[nodiscard]] std::optional<std::size_t> lag(const Simulator& simulator);

/**
 * @brief Whether every byte that opens a post-accept nonaccepting stretch is dead from the initial state.
 *
 * A rescue is a rollback after a failed lookahead that lets the scan continue where a scheme restarting at
 * every accept would have declared the input malformed. On a rescue-free token set every rollback fires into
 * an instant dead end, so the two schemes agree on every input. The gate is sufficient and not necessary: on
 * {a, abc, bc} it returns false though no rescue exists there. Zero-lag sets pass vacuously; the gate is
 * strictly weaker than zero lag.
 * @param simulator The compiled token set.
 * @return True when no stretch-opening byte starts a viable token from the initial state; false says only
 *         that this gate did not establish rescue-freeness.
 */
[[nodiscard]] bool rescue_free(const Simulator& simulator);

/**
 * @brief The first anchored-certified start in the tail at or after an offset, under the exact
 *        complete-repair invariance contract: every completely tokenizable repair of whatever preceded
 *        the tail places a token boundary there, the tail's end being the end of the input.
 *
 * The anchored decider is exact for that complete-repair question, where the certificate walk is merely
 * sound: certificates quantify over every input containing their evidence and cannot use the end of
 * input, while this query can, so on a repairable tail it answers at or before any certificate. The
 * certificates' own guarantee also binds repairs whose scans merely commit through their evidence without
 * completing, a larger set this decider does not speak about, so neither subsumes the other outright.
 * Decided by one scenario play per reachable state, no repair enumerated. When no repair of any prefix
 * makes the whole tokenizable, every position is vacuously invariant and this query deliberately refuses
 * instead of answering; nullable token sets sit outside the underlying model and are refused outright, as
 * for is_split_window().
 * @param simulator The compiled token set.
 * @param tail The preserved suffix of the input, its end the end of input.
 * @param from The offset the search starts at; at or past the tail's size finds nothing.
 * @return The first anchored-certified position, or std::nullopt when none exists or the tail is
 *         beyond repair.
 */
[[nodiscard]] std::optional<std::size_t> next_anchored_start(
        const Simulator& simulator, std::string_view tail, std::size_t from);

/**
 * @brief A shortest repair for the tail: a byte string of minimal length whose concatenation with the
 *        tail is completely tokenizable, empty when the tail already tokenizes.
 *
 * Existence and minimality are exact: the minimal repair is a shortest path to a completing crossing
 * entry, and a refusal certifies that no repair of any length exists. Nullable token sets are refused,
 * as for next_anchored_start().
 * @param simulator The compiled token set.
 * @param tail The preserved suffix of the input.
 * @return A minimal repair, or std::nullopt when the tail is beyond repair.
 */
[[nodiscard]] std::optional<std::string> minimal_repair(const Simulator& simulator, std::string_view tail);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_RECOVERY_HPP

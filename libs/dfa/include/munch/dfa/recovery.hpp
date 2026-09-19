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
 * @brief What the search for a rescue found.
 */
struct Rescue
{
    /**
     * @brief A completely tokenizable input holding a token whose scan read past the token's end before rolling
     *        back to it, the shortest such input; empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: rescue-free when the
     * search exhausted, and undetermined when the cap stopped it.
     */
    bool exhaustive{};
};

/**
 * @brief The number of search states rescue() holds before giving up unless told otherwise, generous for the token
 *        sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t rescue_cap{1U << 20U};

/**
 * @brief Whether some completely tokenizable input makes the scan roll back, with a witness.
 *
 * A rescue is a rollback after a failed lookahead that lets the scan continue where a scheme restarting at
 * every accept would have declared the input malformed: a token of a completely tokenizable input whose scan
 * consumed at least one byte past the token's end, by a transition into a nonaccepting state, before dying
 * there, at a missing transition or the end of the input, and rolling back. On a rescue-free token set no such
 * token exists, so the two schemes emit the same tokens on every completely tokenizable input. Zero-lag sets
 * are rescue-free, and so is {a, abc, bc} with lag one: the stretch after the accepted a is entered on b, but
 * every completely tokenizable continuation begins with the token bc, whose c closes the longer token abc, so
 * the scan never rolls back to a.
 *
 * Decided exactly by the same boundary-guessing search as boundary_difference(): the input is built byte by
 * byte with every closed segment's run kept alive, a closed run that accepts abandoning the branch, so that
 * the surviving markings are the maximal-munch ones, and a closed run that survives a byte is the rollback
 * looked for. The search starts at the initial state and reaches every position a scan can stand in, so the
 * accepting states no input reaches never enter it.
 * @param simulator The compiled token set.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one
 *        the cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from
 *         an exhaustive search proves the token set rescue-free.
 */
[[nodiscard]] Rescue rescue(const Simulator& simulator, std::size_t cap = rescue_cap);

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
 * instead of answering. A nullable token set is decided through the positive-width equivalent the simulator
 * compiled, as every decision here is.
 * @param simulator The compiled token set.
 * @param tail The preserved suffix of the input, its end the end of input.
 * @param from The offset the search starts at; at or past the tail's size finds nothing.
 * @return The first anchored-certified position, or std::nullopt when none exists or the tail is
 *         beyond repair.
 */
[[nodiscard]] std::optional<std::size_t> next_anchored_start(
        const Simulator& simulator, std::string_view tail, std::size_t from);

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
 * @brief A shortest repair for the tail: a byte string of minimal length whose concatenation with the
 *        tail is completely tokenizable, empty when the tail already tokenizes.
 *
 * Existence and minimality are exact: the minimal repair is a shortest path to a completing crossing
 * entry, and a refusal certifies that no repair of any length exists.
 * @param simulator The compiled token set.
 * @param tail The preserved suffix of the input.
 * @return A minimal repair, or std::nullopt when the tail is beyond repair.
 */
[[nodiscard]] std::optional<std::string> minimal_repair(const Simulator& simulator, std::string_view tail);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_RECOVERY_HPP

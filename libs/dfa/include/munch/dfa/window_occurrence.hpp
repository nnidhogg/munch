#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_OCCURRENCE_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_OCCURRENCE_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief What the search for an occurrence of a window found.
 */
struct Occurrence
{
    /**
     * @brief A completely tokenizable input containing the window, the shortest one; empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: occurring in no completely
     * tokenizable input when the search exhausted, and undetermined when the cap stopped it. A caller that treats
     * the second as the first would call a certificate vacuous whose window may well occur.
     */
    bool exhaustive{};
};

/**
 * @brief The number of search states window_occurrence() holds before giving up unless told otherwise, generous for
 *        the token sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t occurrence_cap{1U << 20U};

/**
 * @brief Decides whether the given byte string occurs in some completely tokenizable input, returning one that
 *        contains it.
 *
 * The question a certificate leaves open. A certified split window (W, o) promises where the token covering the
 * occurrence's final byte begins in every completely tokenizable input containing W, and the promise is conditional
 * on occurrence: a window no completely tokenizable input contains satisfies it vacuously, so is_split_window() may
 * certify it and it anchors nothing. This decision splits the two readings of a certificate. A window
 * is_split_window() certifies is an occurring certificate when a witness comes back, and a vacuous one when an
 * exhaustive search finds none; the decision says nothing about the certificate itself, only whether it is about any
 * input. The decision is exact where the certificate is model-relative, so a window is_split_window() refuses may
 * occur or not, and this call answers that as well.
 *
 * Decided by the same boundary-guessing search as rescue() and boundary_difference(): the input is built byte by
 * byte, breadth first, with every closed segment's run kept alive, a closed run that accepts abandoning the branch,
 * so that the surviving markings are the maximal-munch ones. Beside the scan a window matcher guesses where the
 * occurrence begins, as the scan guesses where its tokens end, and reads the window byte by byte from there; the
 * input is a witness once the whole window has been read and the segment being read closes. A witness is therefore
 * a shortest completely tokenizable input containing the window, and never empty, so that an empty one means none:
 * the empty window, contained in every input, has a shortest token as its witness. The search starts at the initial
 * state and reaches every position a scan can stand in, so the accepting states no input reaches never enter it,
 * and a nullable token set is decided through the positive-width equivalent the simulator compiled, as every
 * decision here is.
 * @param simulator The compiled token set.
 * @param window The byte string to find.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from an
 *         exhaustive search proves that no completely tokenizable input contains the window.
 */
[[nodiscard]] Occurrence window_occurrence(
        const Simulator& simulator, std::string_view window, std::size_t cap = occurrence_cap);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_OCCURRENCE_HPP

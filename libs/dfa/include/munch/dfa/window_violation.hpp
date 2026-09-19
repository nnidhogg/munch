#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_VIOLATION_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_VIOLATION_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief What the search for a violation of a window certificate found.
 */
struct Violation
{
    /**
     * @brief A completely tokenizable input containing an occurrence of the window whose covering token begins
     *        elsewhere than the certified origin, the shortest one; empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: the certificate proved
     * exact when the search exhausted, and undetermined when the cap stopped it. A caller that treats the second as
     * the first would cut at a window some input covers from elsewhere, which is the whole failure a certificate
     * exists to exclude.
     */
    bool exhaustive{};
};

/**
 * @brief The number of search states window_violation() holds before giving up unless told otherwise, generous for
 *        the token sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t violation_cap{1U << 20U};

/**
 * @brief Decides whether the window certificate (W, o) is violated by some completely tokenizable input, returning
 *        one that violates it.
 *
 * A certified split window (W, o) promises: in every completely tokenizable input and at every occurrence of W in it,
 * the token covering the occurrence's final byte begins exactly o bytes into the occurrence. A violation is a
 * completely tokenizable input holding an occurrence whose covering token begins elsewhere, and this decision searches
 * for one. The certificate quantifies over completed scans and nothing else, so an exhaustive search that finds no
 * violation proves the certificate exact over every completely tokenizable input, not relative to any model of the
 * scanner. is_split_window() decides the same certificate through the conservative cloud model, and the two stand in
 * one relation: a window it certifies at o has no violation at o, the model only ever over-approximating the scan,
 * while a window it refuses is refused relative to the model, and this decision settles it either way, with the
 * witness where the refusal was right and a proof where it was conservative. The relation to window_occurrence() is
 * the vacuous reading: a window no completely tokenizable input contains has no violation at any origin, its
 * certificate holding of no input, so a certificate proved exact here anchors something only where that decision
 * places the window.
 *
 * Decided by the same boundary-guessing search as rescue(), boundary_difference() and window_occurrence(): the input
 * is built byte by byte, breadth first, with every closed segment's run kept alive, a closed run that accepts
 * abandoning the branch, so that the surviving markings are the maximal-munch ones. Beside the scan the window
 * matcher of window_occurrence() guesses where the occurrence begins and reads the window byte by byte from there,
 * and beside the matcher one bit records whether the latest token start sits at the origin, set at every guessed
 * boundary. The occurrence violates the certificate when the window's final byte is read with the bit clear, and the
 * input is a witness once the segment being read then closes; an occurrence read through with the bit set conforms,
 * and its branch is dropped while the branches on which the matcher waits for a later occurrence carry on. A witness
 * is therefore a shortest violating input, and never empty. The search starts at the initial state and reaches every
 * position a scan can stand in, so the accepting states no input reaches never enter it, and a nullable token set is
 * decided through the positive-width equivalent the simulator compiled, as every decision here is.
 * @param simulator The compiled token set.
 * @param window The byte string of the certificate, non-empty.
 * @param origin The offset into the window at which the certificate places the covering token's start, inside it.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from an
 *         exhaustive search proves that no completely tokenizable input violates the certificate.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it, neither being a certificate.
 */
[[nodiscard]] Violation window_violation(
        const Simulator& simulator, std::string_view window, std::size_t origin, std::size_t cap = violation_cap);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_WINDOW_VIOLATION_HPP

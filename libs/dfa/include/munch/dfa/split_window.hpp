#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP

#include <cstddef>
#include <optional>
#include <string_view>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief Decides whether the given byte string is a certified split window, returning the covering origin.
 *
 * A certified split window (W, o) promises: in every completely tokenizable input containing W, the token
 * covering the occurrence's final byte begins exactly o bytes into the occurrence. The certificate is
 * conditional on occurrence; a window no completely tokenizable input contains satisfies it vacuously, and
 * this decision does not establish that an occurrence exists. A caller that has just found W in its input at
 * hand holds that occurrence, and on completely tokenizable input the promise applies to it directly; on
 * malformed input the promise carries nothing at all.
 *
 * The decision runs the conservative cloud model over the compiled tables, a set of hypotheses about where
 * the scan could be that only ever over-approximates the true state: every live state starts as a hypothesis
 * whose token began before the window, each byte advances hypotheses deterministically, a fresh token may
 * begin exactly where some represented history just ended one, and reading from a non-re-entrant initial
 * state begins a token at that offset. The window is certified when every surviving hypothesis agrees on one
 * in-window origin. A refusal is model-relative: the model deliberately refuses some windows a greedy scanner
 * would allow, and refusal never proves that no certificate exists semantically. On non-empty token sets this
 * coincides at length one with is_split_point(), a nullable set being decided through the positive-width
 * equivalent the simulator compiled, and an empty token set refuses everything on both sides.
 * @param simulator The compiled token set.
 * @param window The byte string to decide.
 * @return The in-window origin every covering token begins at, or std::nullopt when the window is refused.
 */
[[nodiscard]] std::optional<std::size_t> is_split_window(const Simulator& simulator, std::string_view window);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP

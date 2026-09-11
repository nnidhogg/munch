#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_UNROLL_START_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_UNROLL_START_HPP

#include "munch/dfa/dfa.hpp"

namespace munch::dfa
{
/**
 * @brief Returns the positive-width equivalent of a DFA: the same automaton entered through a fresh start state
 *        that carries the old one's transitions and does not accept, or the DFA itself when its start does not
 *        accept.
 *
 * A maximal-munch scan never emits an empty token, so a token set that matches the empty string scans exactly as
 * the set with the empty word removed, and that is what the unrolled automaton recognizes: the old start stays,
 * reachable only where the automaton returned to it, and the new one is entered once and never re-entered. Every
 * decision made over the compiled tables, the byte and window certificates, the recovery queries and the rest,
 * assumes a start state that neither accepts nor is re-entered; the Simulator compiles a nullable set through this
 * function so that each of them meets one, and a caller reasoning over the Dfa itself, a probe cross-checking the
 * decisions against its own model, applies it for the same reason.
 * @param dfa The DFA.
 * @return The unrolled DFA, one state larger, or a copy of the given one when nothing was unrolled.
 */
[[nodiscard]] Dfa unroll_start(const Dfa& dfa);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_UNROLL_START_HPP

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
 * assumes a start state that does not accept; the Simulator compiles a nullable set through this function so that
 * each of them meets one, and a caller reasoning over the Dfa itself, a probe cross-checking the decisions against
 * its own model, applies it for the same reason. Whether the start is re-entered is another matter, which this
 * function settles for the fresh start alone: a start that does not accept is returned as it is, and the start of
 * `[\n]*b` is returned to on every newline, which Simulator::init_reentrant() reports and every decision allows
 * for by withdrawing the start's exemption.
 *
 * The fresh start is Dfa::state_count(), one past the highest identifier the definition names, and the automaton
 * returned spans one past that again, so this function's one precondition on its argument is that the span of the
 * identifiers plus one be representable: a highest identifier at most the largest std::size_t less two. A
 * definition naming a state at the largest std::size_t has no representable span of its own, its count is the
 * wrap, zero, and the automaton returned is entered through one of the DFA's own states, the accepting start this
 * function exists to replace among them; one naming a state one below that has a representable span and a free
 * identifier above it, and the automaton returned is entered through that identifier, but the count it reports is
 * the wrap, zero, which is no count of anything. The subset construction numbers its states densely from zero and
 * never comes near either; dfa::Builder takes whatever identifiers a caller names, so a DFA assembled by hand owes
 * the bound itself. The Simulator refuses a definition whose count reaches its table entry's sentinel before it
 * unrolls anything, far below either, so the count every compiled decision reads is the span it names.
 * @param dfa The DFA, the span of its state identifiers plus one representable.
 * @return The unrolled DFA, one state larger, or a copy of the given one when nothing was unrolled.
 */
[[nodiscard]] Dfa unroll_start(const Dfa& dfa);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_UNROLL_START_HPP

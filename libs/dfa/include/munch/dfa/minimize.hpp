#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MINIMIZE_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MINIMIZE_HPP

#include "munch/dfa/dfa.hpp"

namespace munch::dfa
{
/**
 * @brief Returns an equivalent DFA with no two states interchangeable under its transition structure.
 *
 * States are merged when no input distinguishes them, using Moore partition refinement: states start out grouped by
 * their accept token, and groups are split until every pair of states in a group agrees, symbol by symbol, on the
 * group of the state it moves to. States accepting different tokens are never merged, so the token reported for any
 * input is unchanged.
 *
 * The refinement runs over a partial transition function and treats a missing transition as distinct from one into a
 * state that cannot accept, which is what the scanner needs: the two differ in how far a longest match reads before
 * failing. That is weaker than Myhill-Nerode minimality, under which every state with an empty right language is
 * equivalent.
 *
 * The result is minimal in the usual sense whenever the input is trim: every state reachable from the initial
 * state, and every state co-accessible, meaning some continuation from it accepts. Neither is checked or repaired
 * here. Refinement merges equivalent states but never drops an unreachable one, so an island the initial state
 * cannot reach survives; and a subexpression denoting the empty language, such as any_of() over an empty set,
 * leaves a chain of states with empty right language that Myhill-Nerode would collapse. The pattern as a whole
 * need not be empty for the latter: choice(text("a"), concat(text("b"), any_of(Set{}))) matches only "a" and still
 * keeps a reachable dead b branch. Subset construction supplies reachability, so a DFA arriving from
 * Builder::subset_construction() needs only co-accessibility; a hand-built DFA may satisfy neither.
 * @param dfa The DFA to minimize.
 * @return An equivalent DFA with no two states interchangeable.
 */
[[nodiscard]] Dfa minimize(const Dfa& dfa);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MINIMIZE_HPP

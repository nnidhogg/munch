#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_DETERMINIZE_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_DETERMINIZE_HPP

#include <cstddef>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/nfa/nfa.hpp"
#include "munch/nfa/token.hpp"

namespace munch::core
{
/**
 * @brief Converts an NFA to the equivalent DFA.
 *
 * Implemented by subset construction. The name states the effect rather than the algorithm, matching dfa::minimize(),
 * which is Moore partition refinement without saying so: which algorithm performs the conversion is an implementation
 * choice, and the API should not have to change when it does.
 *
 * It lives in core rather than beside either automaton because it is a bridge. Placing it in dfa would make the DFA
 * library depend on the NFA library, which today it does not, and placing it in nfa would be worse still, since the
 * conversion needs most of dfa's construction API to produce its result. core is the only layer that already sees both.
 * @param nfa The NFA to convert.
 * @param state_limit The largest number of DFA states to discover before throwing; zero means unlimited.
 * @return The constructed DFA.
 * @throws State_limit_error If the state limit is exceeded.
 * @throws std::runtime_error If the NFA numbers a state beyond the determinizer's 32-bit dense index, whose identifiers
 *         are used as given rather than remapped.
 */
[[nodiscard]] dfa::Dfa determinize(const nfa::Nfa& nfa, std::size_t state_limit = 0);

/**
 * @brief Walks determinize()'s subset construction and reports the accepting candidates of every subset it discovers.
 *
 * The traversal is determinize()'s own, so a diagnosis judges exactly the subsets the build discovers rather than
 * mirroring the walk with a second implementation. One entry per reachable subset holding at least one accepting state,
 * in discovery order.
 * @param nfa The NFA to walk.
 * @param state_limit The largest number of subsets to discover before throwing; zero means unlimited.
 * @return The accepting candidate tokens, one list per accepting subset.
 * @throws State_limit_error If the state limit is exceeded.
 */
[[nodiscard]] std::vector<std::vector<nfa::Token>> reachable_candidates(
        const nfa::Nfa& nfa, std::size_t state_limit = 0);

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_DETERMINIZE_HPP

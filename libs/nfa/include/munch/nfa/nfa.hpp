#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_NFA_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_NFA_HPP

#include <optional>
#include <set>
#include <unordered_map>

#include "munch/nfa/label.hpp"
#include "munch/nfa/token.hpp"

namespace munch::nfa
{
/**
 * @brief Represents a non-deterministic finite automaton (NFA).
 *
 * Provides methods for querying states, transitions, and accept states, as well as advancing the NFA and computing
 * epsilon closures.
 */
class Nfa
{
public:
    /**
     * @brief Type representing an NFA state identifier.
     */
    using State_t = std::size_t;

    /**
     * @brief Ordered set of NFA states.
     *
     * Using std::set ensures deterministic iteration order so state-sets can be safely used as keys during DFA subset
     * construction.
     */
    using States_t = std::set<State_t>;

    /**
     * @brief NFA transition key type.
     *
     * Represents a pair of `(from_state, Label)` used as the key in transition maps.
     */
    using Key_t = std::pair<State_t, Label>;

    /**
     * @brief Hash functor used to combine state and label into a transition key hash.
     */
    struct Hash
    {
        std::size_t operator()(const Key_t& key) const noexcept;
    };

    /**
     * @brief NFA transition table type.
     *
     * Maps each `(state, label)` pair to a set of destination states.
     */
    using Transitions_t = std::unordered_map<Key_t, States_t, Hash>;

    /**
     * @brief Accept-state table type for the NFA.
     *
     * Maps accepting states to an optional token, indicating the token accepted by that state.
     */
    using Accept_states_t = std::unordered_map<State_t, std::optional<Token>>;

    /**
     * @brief Constructs an NFA with the given initial state, transitions, and accept states.
     * @param init_state The initial state of the NFA.
     * @param transitions The transition table.
     * @param accept_states The accept states and their associated tokens (optional).
     */
    Nfa(State_t init_state, Transitions_t transitions, Accept_states_t accept_states);

    /**
     * @brief Returns the initial state of the NFA.
     * @return The initial state identifier.
     */
    [[nodiscard]] State_t init_state() const noexcept;

    /**
     * @brief Returns the transition table of the NFA.
     * @return Reference to the transitions map.
     */
    [[nodiscard]] const Transitions_t& transitions() const noexcept;

    /**
     * @brief Returns the accept states and their associated tokens.
     * @return Reference to the accept states map.
     */
    [[nodiscard]] const Accept_states_t& accept_states() const noexcept;

    /**
     * @brief Computes the epsilon closure of a set of states in the NFA.
     * @param nfa The NFA to operate on.
     * @param states The set of states to compute the closure for.
     * @return The set of states reachable via epsilon transitions.
     */
    [[nodiscard]] static States_t epsilon_closure(const Nfa& nfa, const States_t& states);

    /**
     * @brief Advances the NFA from a set of states on an input symbol.
     * @param nfa The NFA to advance.
     * @param states The current set of states.
     * @param symbol The input symbol.
     * @return The set of next states reachable on the symbol.
     */
    [[nodiscard]] static States_t advance(const Nfa& nfa, const States_t& states, char symbol);

    /**
     * @brief Checks if any state in the set is an accept state and returns its token if so.
     * @param nfa The NFA to check.
     * @param states The set of states to check.
     * @return The associated token if any state is accepting, otherwise std::nullopt.
     */
    [[nodiscard]] static std::optional<Token> has_accept_token(const Nfa& nfa, const States_t& states);

private:
    State_t init_state_;

    Transitions_t transitions_;

    Accept_states_t accept_states_;
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_NFA_HPP

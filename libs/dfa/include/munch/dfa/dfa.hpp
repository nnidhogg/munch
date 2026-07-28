#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_DFA_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_DFA_HPP

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

#include "munch/dfa/label.hpp"
#include "munch/dfa/token.hpp"

namespace munch::dfa
{
/**
 * @brief Represents a deterministic finite automaton (DFA).
 *
 * Holds the definition of the automaton: its initial state, transitions, and accept states, kept as the maps the DFA
 * was constructed from so they stay available for inspection and further construction steps. Running a DFA over input
 * is the responsibility of the Simulator, which compiles the definition into a form optimized for execution.
 */
class Dfa
{
public:
    /**
     * @brief Type representing a DFA state identifier.
     */
    using State_t = std::size_t;

    /**
     * @brief DFA transition key type.
     *
     * Represents a pair of `(from_state, Label)` used as the key in transition maps.
     */
    using Key_t = std::pair<State_t, Label>;

    /**
     * @brief Hash functor for computing transition key hashes.
     */
    struct Hash
    {
        std::size_t operator()(const Key_t& key) const noexcept;
    };

    /**
     * @brief Transition table for the DFA.
     *
     * Maps `(state, label)` pairs to a deterministic destination state.
     */
    using Transitions_t = std::unordered_map<Key_t, State_t, Hash>;

    /**
     * @brief Accept-state table for the DFA.
     *
     * Maps accepting states to their associated token.
     */
    using Accept_states_t = std::unordered_map<State_t, Token>;

    /**
     * @brief Constructs a DFA with the given initial state, transitions, and accept states.
     * @param init_state The initial state of the DFA.
     * @param transitions The transition table.
     * @param accept_states The accept states and their associated tokens.
     */
    Dfa(State_t init_state, Transitions_t transitions, Accept_states_t accept_states);

    /**
     * @brief Returns the initial state of the DFA.
     * @return The initial state identifier.
     */
    [[nodiscard]] State_t init_state() const noexcept;

    /**
     * @brief Returns the transition table of the DFA.
     * @return Reference to the transitions map.
     */
    [[nodiscard]] const Transitions_t& transitions() const noexcept;

    /**
     * @brief Returns the accept states and their associated tokens.
     * @return Reference to the accept states map.
     */
    [[nodiscard]] const Accept_states_t& accept_states() const noexcept;

    /**
     * @brief Advances the DFA from a given state on an input symbol.
     * @param dfa The DFA to advance.
     * @param state The current state.
     * @param symbol The input symbol.
     * @return The next state if a transition exists, otherwise std::nullopt.
     */
    [[nodiscard]] static std::optional<State_t> advance(const Dfa& dfa, State_t state, char symbol);

    /**
     * @brief Checks if a state is an accept state and returns its token if so.
     * @param dfa The DFA to check.
     * @param state The state to check.
     * @return The associated token if the state is accepting, otherwise std::nullopt.
     */
    [[nodiscard]] static std::optional<Token> has_accept_token(const Dfa& dfa, State_t state);

private:
    State_t init_state_;

    Transitions_t transitions_;

    Accept_states_t accept_states_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_DFA_HPP

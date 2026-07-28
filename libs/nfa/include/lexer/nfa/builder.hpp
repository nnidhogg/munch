#ifndef LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_BUILDER_HPP
#define LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_BUILDER_HPP

#include "lexer/nfa/label.hpp"
#include "lexer/nfa/nfa.hpp"

namespace lexer::nfa
{
/**
 * @brief Builder class for constructing NFA objects.
 *
 * Allows incremental construction of an NFA by adding states, transitions, epsilon transitions, and accept states.
 */
class Builder
{
public:
    /**
     * @brief Constructs a new NFA Builder.
     */
    Builder();

    /**
     * @brief Returns the initial state of the NFA.
     * @return The initial state identifier.
     */
    [[nodiscard]] Nfa::State_t init_state() const noexcept;

    /**
     * @brief Generates and returns the next available state identifier.
     * @return The next state identifier.
     */
    [[nodiscard]] Nfa::State_t next_state() noexcept;

    /**
     * @brief Returns the transition table of the NFA.
     * @return Reference to the transitions map.
     */
    [[nodiscard]] const Nfa::Transitions_t& transitions() const noexcept;

    /**
     * @brief Returns the accept states and their associated tokens.
     * @return Reference to the accept states map.
     */
    [[nodiscard]] const Nfa::Accept_states_t& accept_states() const noexcept;

    /**
     * @brief Adds a transition from one state to another on a given label.
     * @param from The source state.
     * @param label The transition label.
     * @param to The destination state.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_transition(Nfa::State_t from, const Label& label, Nfa::State_t to);

    /**
     * @brief Adds an epsilon (empty string) transition from one state to another.
     * @param from The source state.
     * @param to The destination state.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_epsilon_transition(Nfa::State_t from, Nfa::State_t to);

    /**
     * @brief Marks a state as an accept state.
     * @param accept_state The state to mark as accepting.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_accept_state(Nfa::State_t accept_state);

    /**
     * @brief Marks a state as an accept state with the associated token.
     * @param accept_state The state to mark as accepting.
     * @param token The token associated with this accept state.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_accept_state(Nfa::State_t accept_state, const Token& token);

    /**
     * @brief Sets the accept states for the NFA.
     * @param accept_states The accept states map to set.
     * @return Reference to this Builder for chaining.
     */
    Builder& set_accept_states(Nfa::Accept_states_t accept_states);

    /**
     * @brief Sets the accept token for all accept states.
     * @param token The token to associate with all accept states.
     * @return Reference to this Builder for chaining.
     */
    Builder& set_accept_token(const Token& token);

    /**
     * @brief Returns a new Builder with all state indices offset by the given value.
     * @param offset The value to offset state indices by.
     * @return A new Builder with offset state indices.
     */
    [[nodiscard]] Builder offset(Nfa::State_t offset) const;

    /**
     * @brief Returns a new Builder by appending another Builder's NFA.
     * @param other The Builder to append.
     * @return A new Builder representing the appended NFA.
     */
    [[nodiscard]] Builder append(const Builder& other) const;

    /**
     * @brief Returns a new Builder by merging another Builder's NFA.
     * @param other The Builder to merge.
     * @return A new Builder representing the merged NFA.
     */
    [[nodiscard]] Builder merge(const Builder& other) const;

    /**
     * @brief Builds and returns the constructed NFA.
     * @return The constructed NFA object.
     */
    [[nodiscard]] Nfa build() const;

private:
    Builder(Nfa::State_t init_state, Nfa::State_t next_state, Nfa::Transitions_t transitions,
            Nfa::Accept_states_t accept_states);

    Nfa::State_t init_state_;

    Nfa::State_t next_state_;

    Nfa::Transitions_t transitions_;

    Nfa::Accept_states_t accept_states_;
};

} // namespace lexer::nfa

#endif // LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_BUILDER_HPP

#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BUILDER_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BUILDER_HPP

#include "munch/dfa/dfa.hpp"
#include "munch/dfa/label.hpp"
#include "munch/dfa/token.hpp"

namespace munch::dfa
{
/**
 * @brief Builder class for constructing DFA objects.
 *
 * Allows incremental construction of a DFA by adding states, transitions, and accept states.
 */
class Builder
{
public:
    /**
     * @brief Constructs a new DFA Builder.
     */
    Builder() noexcept;

    /**
     * @brief Returns the initial state of the DFA.
     * @return The initial state identifier.
     */
    [[nodiscard]] Dfa::State_t init_state() const noexcept;

    /**
     * @brief Generates and returns the next available state identifier.
     * @return The next state identifier.
     */
    [[nodiscard]] Dfa::State_t next_state() noexcept;

    /**
     * @brief Adds a transition from one state to another on a given label.
     * @param from The source state.
     * @param label The transition label.
     * @param to The destination state.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_transition(Dfa::State_t from, const Label& label, Dfa::State_t to);

    /**
     * @brief Marks a state as an accept state with the associated token.
     * @param accept_state The state to mark as accepting.
     * @param token The token associated with this accept state.
     * @return Reference to this Builder for chaining.
     */
    Builder& add_accept_state(Dfa::State_t accept_state, const Token& token);

    /**
     * @brief Builds and returns the constructed DFA.
     *
     * Moves the accumulated transitions and accept states out of the Builder, so the Builder should not be reused
     * afterwards; construct a new one for further building.
     * @return The constructed DFA object.
     */
    [[nodiscard]] Dfa build();

private:
    Dfa::State_t init_state_;

    Dfa::State_t next_state_;

    Dfa::Transitions_t transitions_;

    Dfa::Accept_states_t accept_states_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BUILDER_HPP

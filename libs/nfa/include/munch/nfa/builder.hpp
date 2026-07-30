#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_BUILDER_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_BUILDER_HPP

#include <span>

#include "munch/nfa/label.hpp"
#include "munch/nfa/nfa.hpp"

namespace munch::nfa
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
     * @brief Returns a new Builder with a new initial state ε-linked to the previous initial state.
     *
     * The prepended state has no incoming transitions, giving repetition constructions a loop-back target that is
     * distinct from any state of the enclosed NFA.
     * @return A new Builder with the prepended initial state.
     */
    [[nodiscard]] Builder prepend_init_state() const;

    /**
     * @brief Returns a new Builder by appending another Builder's NFA.
     * @param other The Builder to append.
     * @return A new Builder representing the appended NFA.
     */
    [[nodiscard]] Builder append(const Builder& other) const;

    /**
     * @brief Returns a new Builder recognizing the union of this and another Builder's NFA.
     *
     * A fresh start state is created with ε-transitions to both operands' initial states (Thompson union), so the
     * union stays correct even when an operand's initial state has incoming transitions. Both operands' accept
     * states are kept.
     * @param other The Builder to merge.
     * @return A new Builder representing the merged NFA.
     */
    [[nodiscard]] Builder merge(const Builder& other) const;

    /**
     * @brief Merges the builders into one union: a fresh initial state epsilon-branches to every alternative.
     *
     * The n-ary counterpart of merge(). A fold over merge() copies the accumulated union once per alternative
     * and chains one extra initial state each, quadratic work that dominates lowering the generated Unicode
     * classes; this single pass renumbers every alternative once and adds one initial state in total.
     * @param builders The builders to merge; their contents are consumed.
     * @return The merged Builder.
     */
    [[nodiscard]] static Builder merge_all(std::span<Builder> builders);

    /**
     * @brief Builds and returns the constructed NFA, leaving the Builder intact.
     * @return The constructed NFA object.
     */
    [[nodiscard]] Nfa build() const&;

    /**
     * @brief Builds and returns the constructed NFA from an expiring Builder, moving its contents into the result.
     * @return The constructed NFA object.
     */
    [[nodiscard]] Nfa build() &&;

private:
    /**
     * @brief Constructs a Builder from explicit state, transition, and accept-state data.
     * @param init_state The initial state of the NFA.
     * @param next_state The next state identifier to hand out.
     * @param transitions The transition table.
     * @param accept_states The accept states and their associated tokens.
     */
    Builder(Nfa::State_t init_state, Nfa::State_t next_state, Nfa::Transitions_t transitions,
            Nfa::Accept_states_t accept_states);

    /// The initial state of the NFA.
    Nfa::State_t init_state_;

    /// The next unused state identifier.
    Nfa::State_t next_state_;

    /// The transition table under construction.
    Nfa::Transitions_t transitions_;

    /// The accept states and their associated tokens under construction.
    Nfa::Accept_states_t accept_states_;
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_BUILDER_HPP

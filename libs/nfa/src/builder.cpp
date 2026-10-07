#include "munch/nfa/builder.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace munch::nfa
{
namespace
{
/**
 * @brief The largest state identifier, which no allocation or renumbering may step past.
 */
constexpr Nfa::State_t largest_state{std::numeric_limits<Nfa::State_t>::max()};

/**
 * @brief Refuses a renumbering whose identifiers would leave the identifier range.
 * @throws std::runtime_error Always.
 */
[[noreturn]] void refuse_renumbering()
{
    throw std::runtime_error{"NFA state renumbering would overflow the identifier range"};
}

/**
 * @brief Returns the highest state identifier the builder holds, the allocator's next identifier included.
 *
 * The renumbering guard wants the identifier itself rather than a count, so an oversized builder is rejected before any
 * addition can wrap.
 * @param init_state The builder's initial state.
 * @param next_state The builder's next unused identifier.
 * @param transitions The builder's transition table.
 * @param accept_states The builder's accept states.
 * @return The highest identifier among them.
 */
[[nodiscard]] Nfa::State_t highest_state(
        const Nfa::State_t init_state, const Nfa::State_t next_state, const Nfa::Transitions_t& transitions,
        const Nfa::Accept_states_t& accept_states)
{
    auto highest{std::max(init_state, next_state)};

    for (const auto& [key, states] : transitions)
    {
        const auto& [from, label]{key};

        highest = std::ranges::fold_left(states, std::max(highest, from), std::ranges::max);
    }

    highest = std::ranges::fold_left(std::views::keys(accept_states), highest, std::ranges::max);

    return highest;
}

} // namespace

Builder::Builder() : init_state_{0}, next_state_{1}
{}

Nfa::State_t Builder::init_state() const noexcept
{
    return init_state_;
}

Nfa::State_t Builder::next_state()
{
    // A composition may legally park the cursor at the last identifier; handing it out would wrap the cursor to zero
    // and the following allocation would silently reuse an existing state.
    if (next_state_ == largest_state)
    {
        throw std::runtime_error{"NFA state allocator is exhausted"};
    }

    return next_state_++;
}

const Nfa::Transitions_t& Builder::transitions() const noexcept
{
    return transitions_;
}

const Nfa::Accept_states_t& Builder::accept_states() const noexcept
{
    return accept_states_;
}

Builder& Builder::add_transition(const Nfa::State_t from, const Label& label, const Nfa::State_t to)
{
    transitions_[{from, label}].insert(to);

    return *this;
}

Builder& Builder::add_epsilon_transition(const Nfa::State_t from, const Nfa::State_t to)
{
    transitions_[{from, Label::epsilon()}].insert(to);

    return *this;
}

Builder& Builder::add_accept_state(const Nfa::State_t accept_state)
{
    accept_states_.insert_or_assign(accept_state, std::nullopt);

    return *this;
}

Builder& Builder::add_accept_state(const Nfa::State_t accept_state, const Token& token)
{
    accept_states_.insert_or_assign(accept_state, token);

    return *this;
}

Builder& Builder::set_accept_states(Nfa::Accept_states_t accept_states)
{
    accept_states_ = std::move(accept_states);

    return *this;
}

Builder& Builder::set_accept_token(const Token& token)
{
    const auto with_token{[&token](const Nfa::State_t state) { return std::pair{state, token}; }};

    const auto with_tokens{std::views::keys(accept_states_) | std::views::transform(with_token)};

    Nfa::Accept_states_t accept_states{with_tokens.begin(), with_tokens.end()};

    return set_accept_states(std::move(accept_states));
}

Builder Builder::offset(const Nfa::State_t offset) const
{
    // A shift that wraps any identifier would silently collide renumbered states with existing ones and change the
    // language; composing an automaton that numbers states near the top of the range refuses instead.
    const auto highest{highest_state(init_state_, next_state_, transitions_, accept_states_)};

    const auto headroom{largest_state - offset};

    if (highest > headroom)
    {
        refuse_renumbering();
    }

    const auto shift{[offset](const Nfa::State_t state) { return state + offset; }};

    Nfa::Transitions_t transitions{};

    for (const auto& [key, states] : transitions_)
    {
        const auto shifted_targets{states | std::views::transform(shift)};

        const auto& [state, label]{key};

        const auto shifted{shift(state)};

        Nfa::States_t targets{shifted_targets.begin(), shifted_targets.end()};

        transitions[{shifted, label}] = std::move(targets);
    }

    const auto shift_accept{[offset](const Nfa::Accept_states_t::value_type& accept) {
        const auto& [state, token]{accept};

        return std::pair{state + offset, token};
    }};

    const auto shifted_accepts{accept_states_ | std::views::transform(shift_accept)};

    Nfa::Accept_states_t accept_states{shifted_accepts.begin(), shifted_accepts.end()};

    const auto shifted_init{shift(init_state_)};

    const auto shifted_next{shift(next_state_)};

    return {shifted_init, shifted_next, std::move(transitions), std::move(accept_states)};
}

Builder Builder::prepend_init_state() const
{
    if (next_state_ == largest_state)
    {
        refuse_renumbering();
    }

    Builder nfa{next_state_, next_state_ + 1, transitions_, accept_states_};

    nfa.add_epsilon_transition(next_state_, init_state_);

    return nfa;
}

Builder Builder::append(const Builder& other) const
{
    const auto offset_nfa{other.offset(next_state_)};

    Builder nfa{init_state_, offset_nfa.next_state_, transitions_, accept_states_};

    // Add ε transition from current accept states to offset initial state.
    for (const auto accept_state : std::views::keys(nfa.accept_states_))
    {
        nfa.add_epsilon_transition(accept_state, offset_nfa.init_state_);
    }

    nfa.transitions_.insert(offset_nfa.transitions_.begin(), offset_nfa.transitions_.end());

    // Current accept states are replaced by ε transitions.
    nfa.accept_states_ = offset_nfa.accept_states_;

    return nfa;
}

Builder Builder::merge(const Builder& other) const
{
    const auto offset_nfa{other.offset(next_state_)};

    const auto init_state{offset_nfa.next_state_};

    if (init_state == largest_state)
    {
        refuse_renumbering();
    }

    Builder nfa{init_state, init_state + 1, transitions_, accept_states_};

    nfa.add_epsilon_transition(init_state, init_state_);

    nfa.add_epsilon_transition(init_state, offset_nfa.init_state_);

    nfa.transitions_.insert(offset_nfa.transitions_.begin(), offset_nfa.transitions_.end());

    nfa.accept_states_.insert(offset_nfa.accept_states_.begin(), offset_nfa.accept_states_.end());

    return nfa;
}

Builder Builder::merge_all(const std::span<const Builder> builders)
{
    Builder nfa{};

    for (const auto& builder : builders)
    {
        auto offset_nfa{builder.offset(nfa.next_state_)};

        nfa.next_state_ = offset_nfa.next_state_;

        nfa.add_epsilon_transition(nfa.init_state_, offset_nfa.init_state_);

        // The renumbered states are disjoint from everything merged so far, so the nodes splice without clashes.
        nfa.transitions_.merge(offset_nfa.transitions_);

        nfa.accept_states_.merge(offset_nfa.accept_states_);
    }

    return nfa;
}

Nfa Builder::build() const&
{
    return {init_state_, transitions_, accept_states_};
}

Nfa Builder::build() &&
{
    return {init_state_, std::move(transitions_), std::move(accept_states_)};
}

Builder::Builder(
        const Nfa::State_t init_state, const Nfa::State_t next_state, Nfa::Transitions_t transitions,
        Nfa::Accept_states_t accept_states)
    : init_state_{init_state}
    , next_state_{next_state}
    , transitions_{std::move(transitions)}
    , accept_states_{std::move(accept_states)}
{}

} // namespace munch::nfa

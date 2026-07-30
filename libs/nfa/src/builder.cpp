#include "munch/nfa/builder.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace munch::nfa
{
Builder::Builder() : init_state_{0}, next_state_{1}
{}

Builder::Builder(
        const Nfa::State_t init_state, const Nfa::State_t next_state, Nfa::Transitions_t transitions,
        Nfa::Accept_states_t accept_states)
    : init_state_{init_state}
    , next_state_{next_state}
    , transitions_{std::move(transitions)}
    , accept_states_{std::move(accept_states)}
{}

Nfa::State_t Builder::init_state() const noexcept
{
    return init_state_;
}

Nfa::State_t Builder::next_state() noexcept
{
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
    accept_states_.emplace(accept_state, std::nullopt);

    return *this;
}

Builder& Builder::add_accept_state(const Nfa::State_t accept_state, const Token& token)
{
    accept_states_.emplace(accept_state, token);

    return *this;
}

Builder& Builder::set_accept_states(Nfa::Accept_states_t accept_states)
{
    accept_states_ = std::move(accept_states);

    return *this;
}

Builder& Builder::set_accept_token(const Token& token)
{
    const auto transform{[&token](const auto state) { return std::pair{state, token}; }};

    const auto view{std::views::keys(accept_states_) | std::views::transform(transform)};

    Nfa::Accept_states_t accept_states{view.begin(), view.end()};

    return set_accept_states(std::move(accept_states));
}

Builder Builder::offset(const std::size_t offset) const
{
    Nfa::Transitions_t transitions;

    for (const auto& [key, states] : transitions_)
    {
        const auto view{states | std::views::transform([offset](const auto state) { return state + offset; })};

        const auto& [state, transition]{key};

        transitions[{state + offset, transition}] = {view.begin(), view.end()};
    }

    const auto transform{[offset](const auto& pair) { return std::pair{pair.first + offset, pair.second}; }};

    const auto view{accept_states_ | std::views::transform(transform)};

    Nfa::Accept_states_t accept_states{view.begin(), view.end()};

    return {init_state_ + offset, next_state_ + offset, std::move(transitions), std::move(accept_states)};
}

Builder Builder::prepend_init_state() const
{
    Builder nfa{next_state_, next_state_ + 1, transitions_, accept_states_};

    nfa.add_epsilon_transition(next_state_, init_state_);

    return nfa;
}

Builder Builder::append(const Builder& other) const
{
    const auto offset_nfa{other.offset(next_state_)};

    Builder nfa{init_state_, offset_nfa.next_state_, transitions_, accept_states_};

    // Add ε transition from current accept states to offset initial state.
    std::ranges::for_each(std::views::keys(nfa.accept_states_), [&nfa, &offset_nfa](const auto accept_state) {
        nfa.add_epsilon_transition(accept_state, offset_nfa.init_state_);
    });

    nfa.transitions_.insert(offset_nfa.transitions_.begin(), offset_nfa.transitions_.end());

    // Current accept states are replaced by ε transitions.
    nfa.accept_states_ = offset_nfa.accept_states_;

    return nfa;
}

Builder Builder::merge(const Builder& other) const
{
    const auto offset_nfa{other.offset(next_state_)};

    const auto init_state{offset_nfa.next_state_};

    Builder nfa{init_state, init_state + 1, transitions_, accept_states_};

    nfa.add_epsilon_transition(init_state, init_state_);
    nfa.add_epsilon_transition(init_state, offset_nfa.init_state_);

    nfa.transitions_.insert(offset_nfa.transitions_.begin(), offset_nfa.transitions_.end());

    nfa.accept_states_.insert(offset_nfa.accept_states_.begin(), offset_nfa.accept_states_.end());

    return nfa;
}

Builder Builder::merge_all(const std::span<const Builder> builders)
{
    Builder nfa;

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

} // namespace munch::nfa

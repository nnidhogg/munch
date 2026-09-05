#include "munch/dfa/builder.hpp"

#include <algorithm>
#include <utility>

namespace munch::dfa
{
Builder::Builder() noexcept : init_state_{0}, next_state_{1}
{}

Dfa::State_t Builder::init_state() const noexcept
{
    return init_state_;
}

Dfa::State_t Builder::next_state() noexcept
{
    return next_state_++;
}

Builder& Builder::add_transition(const Dfa::State_t from, const Label& label, const Dfa::State_t to)
{
    transitions_[{from, label}] = to;

    // A caller naming its own identifiers is as much a namer as next_state() is, so the counter tracks both.
    next_state_ = std::max({next_state_, from + 1, to + 1});

    return *this;
}

Builder& Builder::add_accept_state(const Dfa::State_t accept_state, const Token& token)
{
    accept_states_.insert_or_assign(accept_state, token);

    next_state_ = std::max(next_state_, accept_state + 1);

    return *this;
}

Dfa Builder::build() const&
{
    return {init_state_, transitions_, accept_states_};
}

Dfa Builder::build() &&
{
    return {init_state_, std::move(transitions_), std::move(accept_states_)};
}

} // namespace munch::dfa

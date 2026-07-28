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

    return *this;
}

Builder& Builder::add_accept_state(const Dfa::State_t accept_state, const Token& token)
{
    accept_states_.insert_or_assign(accept_state, token);

    return *this;
}

Dfa Builder::build()
{
    return {init_state_, std::move(transitions_), std::move(accept_states_)};
}

} // namespace munch::dfa

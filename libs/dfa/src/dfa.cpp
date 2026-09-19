#include "munch/dfa/dfa.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <ranges>
#include <utility>

namespace munch::dfa
{
namespace
{
/**
 * @brief The number of states a definition spans: one past the highest identifier it names anywhere.
 * @param init_state The initial state.
 * @param transitions The transition table.
 * @param accept_states The accept states and their associated tokens.
 * @return The state count, zero when the highest identifier is the largest std::size_t and no count holds it.
 */
Dfa::State_t span_of(
        const Dfa::State_t init_state, const Dfa::Transitions_t& transitions, const Dfa::Accept_states_t& accept_states)
{
    auto highest{init_state};

    for (const auto& [key, to] : transitions)
    {
        highest = std::max({highest, key.first, to});
    }

    for (const auto& state : accept_states | std::views::keys)
    {
        highest = std::max(highest, state);
    }

    return highest + 1;
}

} // namespace

std::size_t Dfa::Hash::operator()(const Key_t& key) const noexcept
{
    std::size_t seed{};
    return boost::hash_combine(seed, key.first), boost::hash_combine(seed, Label::Hash{}(key.second)), seed;
}

Dfa::Dfa(const State_t init_state, Transitions_t transitions, Accept_states_t accept_states)
    : init_state_{init_state}
    , state_count_{span_of(init_state, transitions, accept_states)}
    , transitions_{std::move(transitions)}
    , accept_states_{std::move(accept_states)}
{}

Dfa::State_t Dfa::init_state() const noexcept
{
    return init_state_;
}

Dfa::State_t Dfa::state_count() const noexcept
{
    return state_count_;
}

const Dfa::Transitions_t& Dfa::transitions() const noexcept
{
    return transitions_;
}

const Dfa::Accept_states_t& Dfa::accept_states() const noexcept
{
    return accept_states_;
}

std::optional<Dfa::State_t> Dfa::advance(const State_t state, const char symbol) const
{
    const auto iterator{transitions_.find({state, Label{symbol}})};

    return iterator != transitions_.cend() ? std::optional{iterator->second} : std::nullopt;
}

std::optional<Token> Dfa::has_accept_token(const State_t state) const
{
    const auto iterator{accept_states_.find(state)};

    return iterator != accept_states_.cend() ? std::optional{iterator->second} : std::nullopt;
}

} // namespace munch::dfa

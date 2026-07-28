#include "lexer/dfa/dfa.hpp"

#include <boost/container_hash/hash.hpp>

namespace lexer::dfa
{
std::size_t Dfa::Hash::operator()(const Key_t& key) const noexcept
{
    std::size_t seed{};
    return boost::hash_combine(seed, key.first), boost::hash_combine(seed, Label::Hash{}(key.second)), seed;
}

Dfa::Dfa(const State_t init_state, Transitions_t transitions, Accept_states_t accept_states)
    : init_state_{init_state}, transitions_{std::move(transitions)}, accept_states_{std::move(accept_states)}
{}

Dfa::State_t Dfa::init_state() const noexcept
{
    return init_state_;
}

const Dfa::Transitions_t& Dfa::transitions() const noexcept
{
    return transitions_;
}

const Dfa::Accept_states_t& Dfa::accept_states() const noexcept
{
    return accept_states_;
}

std::optional<Dfa::State_t> Dfa::advance(const Dfa& dfa, const State_t state, const char symbol)
{
    const auto iterator{dfa.transitions_.find({state, Label{symbol}})};

    return iterator != dfa.transitions_.cend() ? std::optional{iterator->second} : std::nullopt;
}

std::optional<Token> Dfa::has_accept_token(const Dfa& dfa, const State_t state)
{
    const auto iterator{dfa.accept_states_.find(state)};

    return iterator != dfa.accept_states_.cend() ? std::optional{iterator->second} : std::nullopt;
}

} // namespace lexer::dfa

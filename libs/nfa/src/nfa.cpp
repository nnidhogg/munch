#include "munch/nfa/nfa.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <queue>
#include <ranges>

namespace munch::nfa
{
std::size_t Nfa::Hash::operator()(const Key_t& key) const noexcept
{
    std::size_t seed{};
    boost::hash_combine(seed, key.first);
    boost::hash_combine(seed, Label::Hash{}(key.second));
    return seed;
}

Nfa::Nfa(const State_t init_state, Transitions_t transitions, Accept_states_t accept_states)
    : init_state_{init_state}, transitions_{std::move(transitions)}, accept_states_{std::move(accept_states)}
{}

Nfa::State_t Nfa::init_state() const noexcept
{
    return init_state_;
}

const Nfa::Transitions_t& Nfa::transitions() const noexcept
{
    return transitions_;
}

const Nfa::Accept_states_t& Nfa::accept_states() const noexcept
{
    return accept_states_;
}

Nfa::States_t Nfa::epsilon_closure(const States_t& states) const
{
    auto result{states};

    std::queue queue{states.begin(), states.end()};

    while (!queue.empty())
    {
        const std::pair transition{queue.front(), Label::epsilon()};

        queue.pop();

        if (const auto iterator = transitions_.find(transition); iterator != transitions_.end())
        {
            std::ranges::for_each(iterator->second, [&result, &queue](const auto state) {
                if (result.insert(state).second)
                {
                    queue.push(state);
                }
            });
        }
    }

    return result;
}

Nfa::States_t Nfa::advance(const States_t& states, const char symbol) const
{
    States_t result;

    for (const auto& state : states)
    {
        if (const auto iterator = transitions_.find({state, Label{symbol}}); iterator != transitions_.end())
        {
            result.insert(iterator->second.begin(), iterator->second.end());
        }
    }

    return epsilon_closure(result);
}

std::optional<Token> Nfa::has_accept_token(const States_t& states) const
{
    const auto has_state{[this](const auto state) { return accept_states_.contains(state); }};

    const auto has_token{[this](const auto state) { return accept_states_.at(state).has_value(); }};

    const auto get_token{[this](const auto state) { return accept_states_.at(state).value(); }};

    auto view{
            states | std::views::filter(has_state) | std::views::filter(has_token) | std::views::transform(get_token)};

    const auto comparator{[](const auto& lhs, const auto& rhs) { return lhs < rhs; }};

    if (auto iterator = std::ranges::min_element(view, comparator); iterator != view.end())
    {
        return {*iterator};
    }

    return std::nullopt;
}

} // namespace munch::nfa

#include "munch/nfa/nfa.hpp"

#include <boost/container_hash/hash.hpp>
#include <queue>

namespace munch::nfa
{
std::size_t Nfa::Hash::operator()(const Key_t& key) const noexcept
{
    const auto& [state, label]{key};

    const auto label_hash{Label::Hash{}(label)};

    std::size_t seed{};

    boost::hash_combine(seed, state);

    boost::hash_combine(seed, label_hash);

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
        const Key_t key{queue.front(), Label::epsilon()};

        queue.pop();

        const auto found{transitions_.find(key)};

        if (found == transitions_.end())
        {
            continue;
        }

        const auto& [found_key, targets]{*found};

        for (const auto state : targets)
        {
            const auto [position, inserted]{result.insert(state)};

            if (inserted)
            {
                queue.push(state);
            }
        }
    }

    return result;
}

Nfa::States_t Nfa::advance(const States_t& states, const char symbol) const
{
    States_t result{};

    for (const auto state : states)
    {
        const Key_t key{state, Label{symbol}};

        const auto found{transitions_.find(key)};

        if (found == transitions_.end())
        {
            continue;
        }

        const auto& [found_key, targets]{*found};

        result.insert(targets.begin(), targets.end());
    }

    return epsilon_closure(result);
}

std::optional<Token> Nfa::has_accept_token(const States_t& states) const
{
    std::optional<Token> best{};

    for (const auto state : states)
    {
        const auto found{accept_states_.find(state)};

        if (found == accept_states_.cend())
        {
            continue;
        }

        const auto& [accept_state, token]{*found};

        if (!token)
        {
            continue;
        }

        // A later token replaces the best only when it wins by priority, then identifier.
        if (!best || *token < *best)
        {
            best = token;
        }
    }

    return best;
}

} // namespace munch::nfa

#include "lexer/nfa/nfa.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <queue>
#include <ranges>

namespace lexer::nfa
{
std::size_t Nfa::Hash::operator()(const Key_t& key) const noexcept
{
    std::size_t seed{};
    return boost::hash_combine(seed, key.first), boost::hash_combine(seed, Label::Hash{}(key.second)), seed;
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

Nfa::States_t Nfa::epsilon_closure(const Nfa& nfa, const States_t& states)
{
    auto result{states};

    std::queue queue{states.begin(), states.end()};

    while (!queue.empty())
    {
        const std::pair transition{queue.front(), Label::epsilon()};

        queue.pop();

        const auto filter{[&nfa](const auto key) { return nfa.transitions().contains(key); }};

        const auto transform{[&nfa](const auto key) { return nfa.transitions().at(key); }};

        auto view{
                std::views::single(transition) | std::views::filter(filter) | std::views::transform(transform) |
                std::views::join};

        std::ranges::for_each(view, [&result, &queue](const auto state) {
            if (result.insert(state).second)
            {
                queue.push(state);
            }
        });
    }

    return result;
}

Nfa::States_t Nfa::advance(const Nfa& nfa, const States_t& states, const char symbol)
{
    const auto filter{[&nfa, symbol](const auto& state) { return nfa.transitions().contains({state, Label{symbol}}); }};

    const auto transform{[&nfa, symbol](const auto& state) { return nfa.transitions().at({state, Label{symbol}}); }};

    States_t result;

    const auto insert{[&result](const auto& elements) { result.insert(elements.begin(), elements.end()); }};

    std::ranges::for_each(states | std::views::filter(filter) | std::views::transform(transform), insert);

    return epsilon_closure(nfa, result);
}

std::optional<Token> Nfa::has_accept_token(const Nfa& nfa, const States_t& states)
{
    const auto has_state{[&nfa](const auto state) { return nfa.accept_states().contains(state); }};

    const auto has_token{[&nfa](const auto state) { return nfa.accept_states().at(state).has_value(); }};

    const auto get_token{[&nfa](const auto state) { return nfa.accept_states().at(state).value(); }};

    auto view{
            states | std::views::filter(has_state) | std::views::filter(has_token) | std::views::transform(get_token)};

    const auto comparator{[](const auto& lhs, const auto& rhs) { return lhs < rhs; }};

    if (auto iterator = std::ranges::min_element(view, comparator); iterator != view.end())
    {
        return {*iterator};
    }

    return std::nullopt;
}

} // namespace lexer::nfa

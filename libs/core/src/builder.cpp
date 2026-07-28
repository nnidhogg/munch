#include "lexer/core/builder.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <numeric>
#include <queue>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "lexer/dfa/builder.hpp"
#include "lexer/dfa/minimize.hpp"

namespace
{
struct Hash
{
    std::size_t operator()(const lexer::nfa::Nfa::States_t& states) const noexcept
    {
        const auto hash{[](auto seed, const auto element) { return boost::hash_combine(seed, element), seed; }};

        return std::accumulate(states.cbegin(), states.cend(), static_cast<std::size_t>(0), hash);
    }
};

auto build_symbol_table(const lexer::nfa::Nfa& nfa)
{
    std::unordered_map<size_t, std::unordered_set<lexer::nfa::Label::Symbol_t>> result;

    const auto filter{[](const auto& pair) { return pair.second.is_symbol(); }};

    auto view{nfa.transitions() | std::views::keys | std::views::filter(filter)};

    std::ranges::for_each(view, [&result](const auto& pair) { result[pair.first].insert(pair.second.symbol()); });

    return result;
}

lexer::nfa::Builder to_nfa(const lexer::dfa::Dfa& dfa, const lexer::nfa::Token& token)
{
    lexer::nfa::Builder result;

    // The DFA state identifiers are not reused as-is, as the NFA builder owns the allocation of its own states.
    std::unordered_map<lexer::dfa::Dfa::State_t, lexer::nfa::Nfa::State_t> dfa_nfa_map{
            {dfa.init_state(), result.init_state()}};

    const auto state{[&result, &dfa_nfa_map](const auto dfa_state) {
        const auto iterator{dfa_nfa_map.find(dfa_state)};

        return iterator != dfa_nfa_map.cend() ? iterator->second
                                              : dfa_nfa_map.emplace(dfa_state, result.next_state()).first->second;
    }};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        result.add_transition(state(from), lexer::nfa::Label{label.symbol()}, state(to));
    }

    // The DFA token carries no priority, so the token the pattern was registered with is restored instead.
    const auto add_accept_state{[&result, &state, &token](const auto accept_state) {
        result.add_accept_state(state(accept_state), token);
    }};

    std::ranges::for_each(std::views::keys(dfa.accept_states()), add_accept_state);

    return result;
}

} // namespace

namespace lexer::core
{
Lexer Builder::build() const
{
    return Lexer{dfa()};
}

nfa::Nfa Builder::nfa() const
{
    return thompson_construction().build();
}

dfa::Dfa Builder::dfa() const
{
    return dfa::minimize(subset_construction(nfa()));
}

void Builder::add_token(const regex::Regex& regex, const nfa::Token& token)
{
    patterns_.push_back({.nfa = regex::to_nfa(regex).set_accept_token(token), .token = token});
}

nfa::Builder Builder::thompson_construction() const
{
    const auto merge{[](const auto& nfa, const auto& pattern) {
        return nfa.merge(to_nfa(dfa::minimize(subset_construction(pattern.nfa.build())), pattern.token));
    }};

    return std::accumulate(patterns_.cbegin(), patterns_.cend(), nfa::Builder{}, merge);
}

dfa::Dfa Builder::subset_construction(const nfa::Nfa& nfa)
{
    dfa::Builder dfa;

    const auto symbol_table{build_symbol_table(nfa)};

    const auto initial_states{nfa::Nfa::epsilon_closure(nfa, {nfa.init_state()})};

    std::unordered_map<nfa::Nfa::States_t, dfa::Dfa::State_t, Hash> nfa_dfa_map{{initial_states, dfa.init_state()}};

    std::queue<nfa::Nfa::States_t> nfa_queue{{initial_states}};

    while (!nfa_queue.empty())
    {
        const auto nfa_states{nfa_queue.front()};

        nfa_queue.pop();

        const auto dfa_state{nfa_dfa_map.at(nfa_states)};

        if (const auto token = nfa::Nfa::has_accept_token(nfa, nfa_states); token)
        {
            dfa.add_accept_state(dfa_state, dfa::Token{token->id()});
        }

        const auto filter{[&symbol_table](const auto state) { return symbol_table.contains(state); }};

        const auto transform{[&symbol_table](const auto state) { return symbol_table.at(state); }};

        auto view{nfa_states | std::views::filter(filter) | std::views::transform(transform) | std::views::join};

        std::ranges::for_each(view, [&](const auto symbol) {
            const auto next_states{nfa::Nfa::advance(nfa, nfa_states, symbol)};

            if (next_states.empty())
            {
                return;
            }

            auto iterator{nfa_dfa_map.find(next_states)};

            // A state identifier is only allocated when the state set is new, keeping the identifiers dense.
            if (iterator == nfa_dfa_map.cend())
            {
                iterator = nfa_dfa_map.emplace(next_states, dfa.next_state()).first;

                nfa_queue.push(next_states);
            }

            dfa.add_transition(dfa_state, dfa::Label{symbol}, iterator->second);
        });
    }

    return dfa.build();
}

} // namespace lexer::core

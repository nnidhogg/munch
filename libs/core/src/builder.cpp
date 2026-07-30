#include "munch/core/builder.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <numeric>
#include <queue>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/minimize.hpp"

namespace
{
/**
 * @brief Hasher for a set of NFA states, used to key the memo table in subset construction.
 *
 * Combines the hashes of the individual state identifiers in ascending order, since munch::nfa::Nfa::States_t is a
 * std::set and therefore iterates in a fixed, insertion-order-independent sequence for a given set of members.
 */
struct Hash
{
    /**
     * @brief Computes the combined hash of a set of NFA states.
     * @param states The set of NFA states to hash.
     * @return The combined hash value.
     */
    std::size_t operator()(const munch::nfa::Nfa::States_t& states) const noexcept
    {
        const auto hash{[](auto seed, const auto element) { return boost::hash_combine(seed, element), seed; }};

        return std::accumulate(states.cbegin(), states.cend(), static_cast<std::size_t>(0), hash);
    }
};

/**
 * @brief Builds a lookup table from each NFA state to the set of input symbols it has an outgoing transition on.
 * @param nfa The NFA to index.
 * @return A map from state identifier to the set of symbols with an outgoing transition from that state.
 */
auto build_symbol_table(const munch::nfa::Nfa& nfa)
{
    std::unordered_map<size_t, std::unordered_set<munch::nfa::Label::Symbol_t>> result;

    const auto filter{[](const auto& pair) { return pair.second.is_symbol(); }};

    auto view{nfa.transitions() | std::views::keys | std::views::filter(filter)};

    std::ranges::for_each(view, [&result](const auto& pair) { result[pair.first].insert(pair.second.symbol()); });

    return result;
}

/**
 * @brief Converts a DFA into an equivalent NFA builder, restoring the token the DFA's pattern was registered with.
 * @param dfa The DFA to convert.
 * @param token The token to mark the resulting NFA's accept states with.
 * @return An NFA builder recognizing the same language as dfa, accepting with token.
 */
munch::nfa::Builder to_nfa(const munch::dfa::Dfa& dfa, const munch::nfa::Token& token)
{
    munch::nfa::Builder result;

    // The DFA state identifiers are not reused as-is, as the NFA builder owns the allocation of its own states.
    std::unordered_map<munch::dfa::Dfa::State_t, munch::nfa::Nfa::State_t> dfa_nfa_map{
            {dfa.init_state(), result.init_state()}};

    const auto state{[&result, &dfa_nfa_map](const auto dfa_state) {
        const auto iterator{dfa_nfa_map.find(dfa_state)};

        return iterator != dfa_nfa_map.cend() ? iterator->second :
                                                dfa_nfa_map.emplace(dfa_state, result.next_state()).first->second;
    }};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        result.add_transition(state(from), munch::nfa::Label{label.symbol()}, state(to));
    }

    const auto add_accept_state{[&result, &state, &token](const auto accept_state) {
        result.add_accept_state(state(accept_state), token);
    }};

    std::ranges::for_each(std::views::keys(dfa.accept_states()), add_accept_state);

    return result;
}

} // namespace

namespace munch::core
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

Builder::Diagnostics Builder::diagnose() const
{
    const auto merged{nfa()};

    const auto symbol_table{build_symbol_table(merged)};

    const auto initial_states{merged.epsilon_closure({merged.init_state()})};

    std::unordered_set<nfa::Nfa::States_t, Hash> visited{initial_states};

    std::queue<nfa::Nfa::States_t> nfa_queue{{initial_states}};

    std::set<std::size_t> winners;

    std::set<std::pair<std::size_t, std::size_t>> ties;

    // The walk mirrors subset_construction(): it visits exactly the reachable state sets, which is what makes
    // absence a proof. A token no reachable set awards is dead for every input there is.
    while (!nfa_queue.empty())
    {
        const auto nfa_states{nfa_queue.front()};

        nfa_queue.pop();

        std::vector<nfa::Token> candidates;

        for (const auto state : nfa_states)
        {
            const auto iterator{merged.accept_states().find(state)};

            if (iterator != merged.accept_states().cend() && iterator->second)
            {
                candidates.push_back(*iterator->second);
            }
        }

        if (!candidates.empty())
        {
            const auto winner{std::ranges::min(candidates, [](const auto& lhs, const auto& rhs) { return lhs < rhs; })};

            winners.insert(winner.id());

            // Every distinct identifier sharing the winner's priority ties with it: the scan still picks one
            // deterministically, but by registered value rather than by anything the grammar said.
            std::set<std::size_t> minimal_ids;

            for (const auto& candidate : candidates)
            {
                if (candidate.priority() == winner.priority())
                {
                    minimal_ids.insert(candidate.id());
                }
            }

            for (auto first{minimal_ids.cbegin()}; first != minimal_ids.cend(); ++first)
            {
                for (auto second{std::next(first)}; second != minimal_ids.cend(); ++second)
                {
                    ties.emplace(*first, *second);
                }
            }
        }

        const auto filter{[&symbol_table](const auto state) { return symbol_table.contains(state); }};

        const auto transform{[&symbol_table](const auto state) { return symbol_table.at(state); }};

        auto view{nfa_states | std::views::filter(filter) | std::views::transform(transform) | std::views::join};

        std::ranges::for_each(view, [&](const auto symbol) {
            const auto next_states{merged.advance(nfa_states, symbol)};

            if (!next_states.empty() && visited.insert(next_states).second)
            {
                nfa_queue.push(next_states);
            }
        });
    }

    Diagnostics result;

    for (const auto& pattern : patterns_)
    {
        const auto id{pattern.token.id()};

        if (!winners.contains(id) && !std::ranges::contains(result.dead_tokens, id))
        {
            result.dead_tokens.push_back(id);
        }
    }

    result.equal_priority_ties.assign(ties.cbegin(), ties.cend());

    return result;
}

void Builder::add_token(const regex::Regex& regex, const nfa::Token& token)
{
    patterns_.push_back({.nfa = regex::to_nfa(regex).set_accept_token(token), .token = token});
}

nfa::Builder Builder::thompson_construction() const
{
    if (patterns_.empty())
    {
        return {};
    }

    const auto lower{[](const auto& pattern) {
        return to_nfa(dfa::minimize(subset_construction(pattern.nfa.build())), pattern.token);
    }};

    const auto merge{[&lower](const auto& nfa, const auto& pattern) { return nfa.merge(lower(pattern)); }};

    return std::accumulate(std::next(patterns_.cbegin()), patterns_.cend(), lower(patterns_.front()), merge);
}

dfa::Dfa Builder::subset_construction(const nfa::Nfa& nfa)
{
    dfa::Builder dfa;

    const auto symbol_table{build_symbol_table(nfa)};

    const auto initial_states{nfa.epsilon_closure({nfa.init_state()})};

    std::unordered_map<nfa::Nfa::States_t, dfa::Dfa::State_t, Hash> nfa_dfa_map{{initial_states, dfa.init_state()}};

    std::queue<nfa::Nfa::States_t> nfa_queue{{initial_states}};

    while (!nfa_queue.empty())
    {
        const auto nfa_states{nfa_queue.front()};

        nfa_queue.pop();

        const auto dfa_state{nfa_dfa_map.at(nfa_states)};

        if (const auto token = nfa.has_accept_token(nfa_states); token)
        {
            dfa.add_accept_state(dfa_state, dfa::Token{token->id()});
        }

        const auto filter{[&symbol_table](const auto state) { return symbol_table.contains(state); }};

        const auto transform{[&symbol_table](const auto state) { return symbol_table.at(state); }};

        auto view{nfa_states | std::views::filter(filter) | std::views::transform(transform) | std::views::join};

        std::ranges::for_each(view, [&](const auto symbol) {
            const auto next_states{nfa.advance(nfa_states, symbol)};

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

    return std::move(dfa).build();
}

} // namespace munch::core

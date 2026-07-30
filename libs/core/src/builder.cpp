#include "munch/core/builder.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/dfa/minimize.hpp"

namespace
{
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
    return dfa::minimize(subset_construction(nfa(), state_limit_));
}

Builder::Diagnostics Builder::diagnose() const
{
    const auto merged{nfa()};

    std::set<std::size_t> winners;

    std::set<std::pair<std::size_t, std::size_t>> ties;

    // The walk is subset_construction()'s own traversal, so it visits exactly the reachable state sets, which is
    // what makes absence a proof: a token no reachable set awards is dead for every input there is.
    for (const auto& candidates : reachable_candidates(merged, state_limit_))
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

    const auto lower{[this](const auto& pattern) {
        return to_nfa(dfa::minimize(subset_construction(pattern.nfa.build(), state_limit_)), pattern.token);
    }};

    std::vector<nfa::Builder> lowered;

    lowered.reserve(patterns_.size());

    for (const auto& pattern : patterns_)
    {
        lowered.push_back(lower(pattern));
    }

    return nfa::Builder::merge_all(lowered);
}

} // namespace munch::core

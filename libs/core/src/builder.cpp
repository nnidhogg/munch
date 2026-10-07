#include "munch/core/builder.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/core/determinize.hpp"
#include "munch/dfa/minimize.hpp"

namespace munch::core
{
namespace
{
/**
 * @brief Converts a DFA into an equivalent NFA builder, restoring the token the DFA's pattern was registered with.
 * @param dfa The DFA to convert.
 * @param token The token to mark the resulting NFA's accept states with.
 * @return An NFA builder recognizing the same language as dfa, accepting with token.
 */
[[nodiscard]] nfa::Builder to_nfa(const dfa::Dfa& dfa, const nfa::Token& token)
{
    nfa::Builder result{};

    // The DFA state identifiers are not reused as-is, as the NFA builder owns the allocation of its own states.
    std::unordered_map<dfa::Dfa::State_t, nfa::Nfa::State_t> dfa_nfa_map{{dfa.init_state(), result.init_state()}};

    const auto state{[&result, &dfa_nfa_map](const dfa::Dfa::State_t dfa_state) {
        if (const auto found{dfa_nfa_map.find(dfa_state)}; found != dfa_nfa_map.cend())
        {
            const auto& [known_dfa_state, known_nfa_state]{*found};

            return known_nfa_state;
        }

        const auto fresh{result.next_state()};

        dfa_nfa_map.emplace(dfa_state, fresh);

        return fresh;
    }};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        const nfa::Label symbol{label.symbol()};

        // The target is numbered before the source, which fixes the NFA's state numbering.
        const auto nfa_to{state(to)};

        const auto nfa_from{state(from)};

        result.add_transition(nfa_from, symbol, nfa_to);
    }

    for (const auto accept_state : std::views::keys(dfa.accept_states()))
    {
        const auto nfa_state{state(accept_state)};

        result.add_accept_state(nfa_state, token);
    }

    return result;
}

} // namespace

void Builder::add_token(const regex::Regex& regex, const nfa::Token& token)
{
    auto automaton{regex::to_nfa(regex)};

    automaton.set_accept_token(token);

    patterns_.push_back({.nfa = std::move(automaton), .token = token});
}

Lexer Builder::build() const
{
    const auto compiled{dfa()};

    return Lexer{compiled, ignored_, payloads_};
}

dfa::Dfa Builder::dfa() const
{
    const auto merged{nfa()};

    const auto determinized{determinize(merged, state_limit_)};

    return dfa::minimize(determinized);
}

nfa::Nfa Builder::nfa() const
{
    auto merged{merged_nfa()};

    return std::move(merged).build();
}

Builder::Diagnostics Builder::diagnose() const
{
    const auto merged{nfa()};

    std::set<std::size_t> winners{};

    std::set<std::pair<std::size_t, std::size_t>> ties{};

    const auto record_ties{[&ties](const std::set<std::size_t>& ids) {
        for (auto first{ids.cbegin()}; first != ids.cend(); ++first)
        {
            for (auto second{std::next(first)}; second != ids.cend(); ++second)
            {
                ties.emplace(*first, *second);
            }
        }
    }};

    // The walk is determinize()'s own traversal, so it visits exactly the reachable state sets, which is what makes
    // absence a proof: a token no reachable set awards is dead for every input there is.
    for (const auto& candidates : reachable_candidates(merged, state_limit_))
    {
        const auto winner{std::ranges::min(candidates, std::less{})};

        winners.insert(winner.id());

        const auto minimal{
                [&winner](const nfa::Token& candidate) { return candidate.priority() == winner.priority(); }};

        // Every distinct identifier sharing the winner's priority ties with it: the scan still picks one
        // deterministically, but by registered value rather than by anything the grammar said.
        auto minimal_view{candidates | std::views::filter(minimal) | std::views::transform(&nfa::Token::id)};

        const std::set<std::size_t> minimal_ids{minimal_view.begin(), minimal_view.end()};

        record_ties(minimal_ids);
    }

    Diagnostics result{};

    for (const auto& [automaton, token] : patterns_)
    {
        const auto id{token.id()};

        if (!winners.contains(id) && !std::ranges::contains(result.dead_tokens, id))
        {
            result.dead_tokens.push_back(id);
        }
    }

    result.equal_priority_ties.assign(ties.cbegin(), ties.cend());

    return result;
}

nfa::Builder Builder::merged_nfa() const
{
    if (patterns_.empty())
    {
        return {};
    }

    const auto lower{[this](const Pattern& pattern) {
        const auto built{pattern.nfa.build()};

        const auto determinized{determinize(built, state_limit_)};

        const auto minimized{dfa::minimize(determinized)};

        return to_nfa(minimized, pattern.token);
    }};

    std::vector<nfa::Builder> lowered{};

    lowered.reserve(patterns_.size());

    std::ranges::transform(patterns_, std::back_inserter(lowered), lower);

    return nfa::Builder::merge_all(lowered);
}

} // namespace munch::core

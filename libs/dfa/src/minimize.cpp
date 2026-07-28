#include "lexer/dfa/minimize.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <unordered_map>
#include <vector>

#include "lexer/dfa/builder.hpp"

namespace lexer::dfa
{
namespace
{
/**
 * @brief A state's group, i.e. its position in the current partition.
 */
using Group_t = std::size_t;

/**
 * @brief What a state looks like under the current partition: its own group and the group each symbol moves it to.
 *
 * Two states are distinguishable exactly when their signatures differ. A missing transition is part of the
 * signature, as a state that rejects a symbol must not merge with one that consumes it.
 */
using Signature_t = std::pair<Group_t, std::vector<std::pair<Label::Symbol_t, Group_t>>>;

/**
 * @brief Groups the states of the initial partition by their accept token.
 * @param dfa The DFA whose states are partitioned.
 * @return The group of each state present in the DFA.
 */
std::unordered_map<Dfa::State_t, Group_t> partition_by_token(const Dfa& dfa)
{
    // Group 0 holds the non-accepting states; each distinct token claims a group of its own.
    std::unordered_map<Dfa::State_t, Group_t> group;

    std::unordered_map<std::size_t, Group_t> token_group;

    const auto state_group{[&dfa, &token_group](const Dfa::State_t state) {
        const auto iterator{dfa.accept_states().find(state)};

        return iterator != dfa.accept_states().cend()
                       ? token_group.try_emplace(iterator->second.id(), token_group.size() + 1).first->second
                       : 0;
    }};

    group.emplace(dfa.init_state(), state_group(dfa.init_state()));

    for (const auto& [key, to] : dfa.transitions())
    {
        group.try_emplace(key.first, state_group(key.first));

        group.try_emplace(to, state_group(to));
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        group.try_emplace(state, state_group(state));
    }

    return group;
}

/**
 * @brief Splits the groups of a partition until no group holds distinguishable states.
 * @param dfa The DFA whose states are partitioned.
 * @param group The initial partition, refined in place.
 */
void refine(const Dfa& dfa, std::unordered_map<Dfa::State_t, Group_t>& group)
{
    for (std::size_t groups{0};;)
    {
        std::unordered_map<Dfa::State_t, Signature_t> signatures;

        for (const auto& [state, state_group] : group)
        {
            signatures[state].first = state_group;
        }

        for (const auto& [key, to] : dfa.transitions())
        {
            signatures[key.first].second.emplace_back(key.second.symbol(), group.at(to));
        }

        // A map keyed by signature both groups equal states and numbers the groups deterministically.
        std::map<Signature_t, Group_t> refined;

        for (auto& [state, signature] : signatures)
        {
            std::ranges::sort(signature.second);

            group[state] = refined.try_emplace(std::move(signature), refined.size()).first->second;
        }

        if (refined.size() == groups)
        {
            return;
        }

        groups = refined.size();
    }
}

} // namespace

Dfa minimize(const Dfa& dfa)
{
    auto group{partition_by_token(dfa)};

    refine(dfa, group);

    // Each group collapses into one state of the minimal DFA; the group of the initial state becomes its initial
    // state. Merged states agree on their transitions and token by construction, so duplicates are identical.
    Builder builder;

    std::unordered_map<Group_t, Dfa::State_t> state_of;

    state_of.emplace(group.at(dfa.init_state()), builder.init_state());

    const auto state{[&builder, &group, &state_of](const Dfa::State_t old_state) {
        const auto iterator{state_of.find(group.at(old_state))};

        return iterator != state_of.cend() ? iterator->second
                                           : state_of.emplace(group.at(old_state), builder.next_state()).first->second;
    }};

    for (const auto& [key, to] : dfa.transitions())
    {
        builder.add_transition(state(key.first), key.second, state(to));
    }

    for (const auto& [accept_state, token] : dfa.accept_states())
    {
        builder.add_accept_state(state(accept_state), token);
    }

    return builder.build();
}

} // namespace lexer::dfa

#include "munch/dfa/minimize.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/dfa/builder.hpp"

namespace munch::dfa
{
namespace
{
/**
 * @brief A state's group, i.e. its position in the current partition.
 */
using Group_t = std::size_t;

/**
 * @brief A partition of the states: the group each state is in.
 */
using Partition_t = std::unordered_map<Dfa::State_t, Group_t>;

/**
 * @brief What a state looks like under the current partition: its own group and the group each symbol moves it to.
 *
 * Two states are distinguishable exactly when their signatures differ. A missing transition is part of the signature,
 * as a state that rejects a symbol must not merge with one that consumes it.
 */
struct Signature
{
    /**
     * @brief Ordered by the group, then by the edges, so signatures key the map that numbers the groups.
     */
    auto operator<=>(const Signature&) const = default;

    /**
     * @brief The state's own group.
     */
    Group_t group{};

    /**
     * @brief The group each symbol moves the state to, sorted by symbol.
     */
    std::vector<std::pair<Label::Symbol_t, Group_t>> edges{};
};

/**
 * @brief Groups the states of the initial partition by their accept token.
 * @param dfa The DFA whose states are partitioned.
 * @return The group of each state present in the DFA.
 */
[[nodiscard]] Partition_t partition_by_token(const Dfa& dfa)
{
    // Group 0 holds the non-accepting states; each distinct token claims a group of its own.
    Partition_t group{};

    std::unordered_map<std::size_t, Group_t> token_group{};

    const auto state_group{[&dfa, &token_group](const Dfa::State_t state) -> Group_t {
        const auto found{dfa.accept_states().find(state)};

        if (found == dfa.accept_states().cend())
        {
            return 0;
        }

        const auto& [accept_state, token]{*found};

        const auto fresh{token_group.size() + 1};

        const auto [entry, inserted]{token_group.try_emplace(token.id(), fresh)};

        const auto& [id, token_group_number]{*entry};

        return token_group_number;
    }};

    const auto init{dfa.init_state()};

    const auto init_group{state_group(init)};

    group.emplace(init, init_group);

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        const auto from_group{state_group(from)};

        group.try_emplace(from, from_group);

        const auto to_group{state_group(to)};

        group.try_emplace(to, to_group);
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        const auto accept_group{state_group(state)};

        group.try_emplace(state, accept_group);
    }

    return group;
}

/**
 * @brief Splits the groups of a partition until no group holds distinguishable states.
 * @param dfa The DFA whose states are partitioned.
 * @param group The initial partition.
 * @return The refined partition.
 */
[[nodiscard]] Partition_t refine(const Dfa& dfa, Partition_t group)
{
    for (std::size_t groups{0};;)
    {
        std::unordered_map<Dfa::State_t, Signature> signatures{};

        for (const auto& [state, state_group] : group)
        {
            signatures[state].group = state_group;
        }

        for (const auto& [key, to] : dfa.transitions())
        {
            const auto& [from, label]{key};

            signatures[from].edges.emplace_back(label.symbol(), group.at(to));
        }

        // A map keyed by signature both groups equal states and numbers the groups deterministically.
        std::map<Signature, Group_t> refined{};

        for (auto& [state, signature] : signatures)
        {
            std::ranges::sort(signature.edges);

            const auto fresh{refined.size()};

            const auto [entry, inserted]{refined.try_emplace(std::move(signature), fresh)};

            const auto& [key, refined_group]{*entry};

            group[state] = refined_group;
        }

        if (refined.size() == groups)
        {
            return group;
        }

        groups = refined.size();
    }
}

} // namespace

Dfa minimize(const Dfa& dfa)
{
    auto by_token{partition_by_token(dfa)};

    const auto group{refine(dfa, std::move(by_token))};

    // Each group collapses into one state of the refined DFA; the group of the initial state becomes its initial state.
    // Merged states agree on their transitions and token by construction, so duplicates are identical.
    Builder builder{};

    std::unordered_map<Group_t, Dfa::State_t> state_of{};

    const auto init_group{group.at(dfa.init_state())};

    state_of.emplace(init_group, builder.init_state());

    const auto state{[&builder, &group, &state_of](const Dfa::State_t old_state) {
        const auto old_group{group.at(old_state)};

        if (const auto found{state_of.find(old_group)}; found != state_of.cend())
        {
            const auto& [found_group, found_state]{*found};

            return found_state;
        }

        const auto fresh{builder.next_state()};

        const auto [entry, inserted]{state_of.emplace(old_group, fresh)};

        const auto& [new_group, new_state]{*entry};

        return new_state;
    }};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        // The target is numbered before the source, which fixes the refined DFA's state numbering.
        const auto new_to{state(to)};

        const auto new_from{state(from)};

        builder.add_transition(new_from, label, new_to);
    }

    for (const auto& [accept_state, token] : dfa.accept_states())
    {
        const auto new_state{state(accept_state)};

        builder.add_accept_state(new_state, token);
    }

    return std::move(builder).build();
}

} // namespace munch::dfa

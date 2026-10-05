#include "munch/dfa/recovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::dfa
{
namespace
{
/**
 * @brief The maximal-munch jump table over a tail.
 *
 * One entry per tail offset for where the maximal token beginning there ends, and one per offset plus the tail's end
 * for whether the suffix beginning there tokenizes completely; built right to left, so each suffix's answer is one
 * lookup past its own token's end.
 */
struct Jump_table
{
    /**
     * @brief The committed end of the maximal token beginning at each offset, or std::nullopt where none accepts.
     */
    std::vector<std::optional<std::size_t>> end{};

    /**
     * @brief Whether the suffix beginning at each offset tokenizes completely; the entry at the tail's size, the empty
     *        suffix, is true.
     */
    std::vector<bool> tokenizes{};
};

/**
 * @brief A crossing scenario that completes: its first in-tail boundary begins a suffix that tokenizes completely.
 */
struct Crossing
{
    /**
     * @brief The scenario's first in-tail boundary.
     */
    std::size_t boundary{};

    /**
     * @brief The shortest word reaching the scenario's entry state, the repair realizing it.
     */
    std::string via{};
};

/**
 * @brief Where a state stands to the post-accept nonaccepting region.
 */
enum class Mark : std::uint8_t
{
    /**
     * @brief Not in the region.
     */
    outside,

    /**
     * @brief In the region, its depth not yet known.
     */
    unresolved,

    /**
     * @brief In the region, its depth known.
     */
    resolved
};

/**
 * @brief The post-accept nonaccepting region of a token set: every state's mark and the region's states in the order
 *        they were entered.
 */
struct Region
{
    /**
     * @brief Per state, where it stands to the region.
     */
    std::vector<Mark> mark{};

    /**
     * @brief The states of the region, in the breadth-first order they were entered.
     */
    std::vector<std::size_t> members{};
};

/**
 * @brief Runs maximal munch from a state over the tail's suffix and reports where it last accepted.
 *
 * The run follows transitions while the table has them and records every accept, a state accepting before it reads
 * counting as an accept at the start.
 * @param simulator The compiled token set.
 * @param state The state the run starts in.
 * @param tail The tail being walked.
 * @param from The offset in the tail the run starts at.
 * @return The offset one past the last accept, or std::nullopt when the run never accepts.
 */
[[nodiscard]] std::optional<std::size_t> maximal_run(
        const Simulator& simulator, const std::size_t state, const std::string_view tail, const std::size_t from)
{
    std::optional<std::size_t> last{};

    if (simulator.is_accepting(state))
    {
        last = from;
    }

    auto current{state};

    for (std::size_t at{from}; at < tail.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(tail[at])};

        const auto next{simulator.step(current, byte)};

        if (!next)
        {
            break;
        }

        current = *next;

        if (simulator.is_accepting(current))
        {
            last = at + 1;
        }
    }

    return last;
}

/**
 * @brief Builds the maximal-munch jump table over a tail.
 *
 * Each offset's token end is the maximal run from the initial state there, and a suffix tokenizes exactly when its
 * token ends where a tokenizing suffix begins, which the right-to-left order has already decided.
 * @param simulator The compiled token set.
 * @param tail The tail the table covers.
 * @return The table, sized to the tail with the empty suffix's entry.
 */
[[nodiscard]] Jump_table build_jump_table(const Simulator& simulator, const std::string_view tail)
{
    Jump_table table{
            .end = std::vector<std::optional<std::size_t>>(tail.size()),
            .tokenizes = std::vector<bool>(tail.size() + 1)};

    // The empty suffix tokenizes; walking right to left, every other suffix's answer is one lookup past its token.
    table.tokenizes[tail.size()] = true;

    for (std::size_t offset{tail.size()}; offset > 0;)
    {
        --offset;

        const auto end{maximal_run(simulator, simulator.init_state(), tail, offset)};

        table.end[offset] = end;

        table.tokenizes[offset] = end.has_value() && table.tokenizes[*end];
    }

    return table;
}

/**
 * @brief Lists the states a scan can stand in when it crosses into the tail mid-token.
 *
 * The states reachable from the initial state by at least one transition, found breadth first so that each carries a
 * shortest word reaching it, which doubles as the repair realizing that crossing.
 * @param simulator The compiled token set.
 * @return The crossing entries with their witnesses, in state order.
 */
[[nodiscard]] std::vector<std::pair<std::size_t, std::string>> crossing_entries(const Simulator& simulator)
{
    std::map<std::size_t, std::string> seen{};

    std::vector<std::size_t> frontier{};

    const auto expand{[&](const std::size_t from, const std::string& via) {
        for (std::size_t byte{0}; byte < Simulator::symbol_count; ++byte)
        {
            const auto to{simulator.step(from, static_cast<unsigned char>(byte))};

            if (!to)
            {
                continue;
            }

            auto word{via + static_cast<char>(byte)};

            const auto [entry, added]{seen.emplace(*to, std::move(word))};

            if (!added)
            {
                continue;
            }

            frontier.push_back(*to);
        }
    }};

    expand(simulator.init_state(), std::string{});

    for (std::size_t at{0}; at < frontier.size(); ++at)
    {
        const auto state{frontier[at]};

        const auto& via{seen.at(state)};

        expand(state, via);
    }

    return {seen.begin(), seen.end()};
}

/**
 * @brief Lists the crossing scenarios that complete over a tail.
 * @param simulator The compiled token set.
 * @param tail The tail being walked.
 * @param table The tail's jump table.
 * @return Each completing scenario's first boundary and its witness, in state order.
 */
[[nodiscard]] std::vector<Crossing> completing_crossings(
        const Simulator& simulator, const std::string_view tail, const Jump_table& table)
{
    std::vector<Crossing> completing{};

    for (const auto& [entry, via] : crossing_entries(simulator))
    {
        // The maximal run from the entry over the whole tail: its last accept is the scenario's first in-tail boundary,
        // zero when the entry itself accepts.
        const auto boundary{maximal_run(simulator, entry, tail, 0)};

        if (boundary && table.tokenizes[*boundary])
        {
            completing.push_back({.boundary = *boundary, .via = via});
        }
    }

    return completing;
}

/**
 * @brief Collects the post-accept nonaccepting region: nonaccepting successors of accepting states, closed under
 *        nonaccepting transitions.
 * @param simulator The compiled token set.
 * @return The region, every member marked unresolved.
 */
[[nodiscard]] Region post_accept_region(const Simulator& simulator)
{
    Region region{.mark = std::vector<Mark>(simulator.state_count(), Mark::outside), .members = {}};

    const auto enter_successors{[&](const std::size_t state) {
        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto to{simulator.step(state, static_cast<unsigned char>(symbol))};

            if (!to || simulator.is_accepting(*to) || region.mark[*to] != Mark::outside)
            {
                continue;
            }

            region.mark[*to] = Mark::unresolved;

            region.members.push_back(*to);
        }
    }};

    // An accepting state no input reaches opens no stretch; for an accepting state, live means reachable.
    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (simulator.is_accepting(state) && simulator.is_live(state))
        {
            enter_successors(state);
        }
    }

    for (std::size_t at{0}; at < region.members.size(); ++at)
    {
        enter_successors(region.members[at]);
    }

    return region;
}

/**
 * @brief Returns the longest path through the region measured in states, by repeated sink peeling.
 * @param simulator The compiled token set.
 * @param region The post-accept region, its marks resolved in turn.
 * @return The longest path, or std::nullopt when the states left over close a nonaccepting cycle.
 */
[[nodiscard]] std::optional<std::size_t> longest_region_path(const Simulator& simulator, Region region)
{
    std::vector<std::size_t> depth(simulator.state_count(), 0);

    const auto resolved_depth{[&](const std::size_t state) -> std::optional<std::size_t> {
        std::size_t deepest{0};

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto to{simulator.step(state, static_cast<unsigned char>(symbol))};

            if (!to || simulator.is_accepting(*to))
            {
                continue;
            }

            if (region.mark[*to] != Mark::resolved)
            {
                return std::nullopt;
            }

            deepest = std::max(deepest, depth[*to]);
        }

        return deepest;
    }};

    auto remaining{region.members};

    std::size_t longest{0};

    for (bool shrank{true}; shrank && !remaining.empty();)
    {
        shrank = false;

        std::vector<std::size_t> keep{};

        for (const auto state : remaining)
        {
            const auto deepest{resolved_depth(state)};

            if (!deepest)
            {
                keep.push_back(state);

                continue;
            }

            depth[state] = 1 + *deepest;

            region.mark[state] = Mark::resolved;

            longest = std::max(longest, depth[state]);

            shrank = true;
        }

        remaining = std::move(keep);
    }

    // The leftover states close a nonaccepting cycle: unbounded.
    if (!remaining.empty())
    {
        return std::nullopt;
    }

    return longest;
}

} // namespace

std::optional<std::size_t> next_anchored_start(
        const Simulator& simulator, const std::string_view tail, const std::size_t from)
{
    if (from >= tail.size())
    {
        return std::nullopt;
    }

    const auto table{build_jump_table(simulator, tail)};

    // Every completing scenario votes for its boundary chain; a position is anchored-certified when every completing
    // scenario contains it. No completing scenario means the tail is beyond repair and every position only vacuously
    // invariant, which is deliberately a refusal.
    std::vector<std::size_t> votes(tail.size(), 0);

    std::size_t completing{0};

    const auto vote{[&](const std::size_t first) {
        ++completing;

        for (auto at{first}; at < tail.size(); at = *table.end[at])
        {
            ++votes[at];
        }
    }};

    if (table.tokenizes[0])
    {
        vote(0);
    }

    for (const auto& [boundary, via] : completing_crossings(simulator, tail, table))
    {
        vote(boundary);
    }

    if (completing == 0)
    {
        return std::nullopt;
    }

    const auto anchored{[&](const std::size_t at) { return votes[at] == completing; }};

    const auto positions{std::views::iota(from, tail.size())};

    const auto found{std::ranges::find_if(positions, anchored)};

    if (found == positions.end())
    {
        return std::nullopt;
    }

    return *found;
}

std::optional<std::size_t> lag(const Simulator& simulator)
{
    // The post-accept nonaccepting region: nonaccepting successors of accepting states, closed under nonaccepting
    // transitions; a cycle inside it is the unboundedness witness, and otherwise the lag is the longest path measured
    // in states.
    auto region{post_accept_region(simulator)};

    return longest_region_path(simulator, std::move(region));
}

std::optional<std::string> minimal_repair(const Simulator& simulator, const std::string_view tail)
{
    const auto table{build_jump_table(simulator, tail)};

    if (table.tokenizes[0])
    {
        return std::string{};
    }

    // The minimal repair is a shortest path to a completing crossing entry; the entries carry shortest witnesses by
    // construction, so the cheapest completing one is the answer, and none completing is a certificate that no repair
    // of any length exists.
    std::optional<std::string> best{};

    for (const auto& [boundary, via] : completing_crossings(simulator, tail, table))
    {
        if (!best || via.size() < best->size())
        {
            best = via;
        }
    }

    return best;
}

} // namespace munch::dfa

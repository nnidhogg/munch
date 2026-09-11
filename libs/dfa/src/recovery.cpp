#include "munch/dfa/recovery.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
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
 * One entry per tail offset for where the maximal token beginning there ends, and one per offset plus the
 * tail's end for whether the suffix beginning there tokenizes completely; built right to left, so each suffix's
 * answer is one lookup past its own token's end.
 */
struct Jump_table
{
    /**
     * @brief The committed end of the maximal token beginning at each offset, or std::nullopt where none accepts.
     */
    std::vector<std::optional<std::size_t>> end;

    /**
     * @brief Whether the suffix beginning at each offset tokenizes completely; the entry at the tail's size,
     *        the empty suffix, is true.
     */
    std::vector<bool> tokenizes;
};

/**
 * @brief Runs maximal munch from a state over the tail's suffix and reports where it last accepted.
 *
 * The run follows transitions while the table has them and records every accept, a state accepting before it
 * reads counting as an accept at the start.
 * @param simulator The compiled token set.
 * @param state The state the run starts in.
 * @param tail The tail being walked.
 * @param from The offset in the tail the run starts at.
 * @return The offset one past the last accept, or std::nullopt when the run never accepts.
 */
std::optional<std::size_t> maximal_run(
        const Simulator& simulator, const std::size_t state, const std::string_view tail, const std::size_t from)
{
    std::optional<std::size_t> last;

    if (simulator.is_accepting(state))
    {
        last = from;
    }

    auto current{state};

    for (std::size_t at{from}; at < tail.size(); ++at)
    {
        const auto next{simulator.step(current, static_cast<unsigned char>(tail[at]))};

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
 * Each offset's token end is the maximal run from the initial state there, and a suffix tokenizes exactly when
 * its token ends where a tokenizing suffix begins, which the right-to-left order has already decided.
 * @param simulator The compiled token set.
 * @param tail The tail the table covers.
 * @return The table, sized to the tail with the empty suffix's entry.
 */
Jump_table build_jump_table(const Simulator& simulator, const std::string_view tail)
{
    Jump_table table{
            .end = std::vector<std::optional<std::size_t>>(tail.size()),
            .tokenizes = std::vector<bool>(tail.size() + 1)};

    // The empty suffix tokenizes; walking right to left, every other suffix's answer is one lookup past its token.
    table.tokenizes[tail.size()] = true;

    for (std::size_t offset{tail.size()}; offset > 0;)
    {
        --offset;

        table.end[offset] = maximal_run(simulator, simulator.init_state(), tail, offset);

        table.tokenizes[offset] = table.end[offset].has_value() && table.tokenizes[*table.end[offset]];
    }

    return table;
}

/**
 * @brief Lists the states a scan can stand in when it crosses into the tail mid-token.
 *
 * The states reachable from the initial state by at least one transition, found breadth first so that each
 * carries a shortest word reaching it, which doubles as the repair realizing that crossing.
 * @param simulator The compiled token set.
 * @return The crossing entries with their witnesses, in state order.
 */
std::vector<std::pair<std::size_t, std::string>> crossing_entries(const Simulator& simulator)
{
    std::map<std::size_t, std::string> seen;

    std::vector<std::size_t> frontier;

    // Breadth first from the initial state, so the first word to reach a state is a shortest one.
    const auto expand{[&](const std::size_t from, const std::string& via) {
        for (std::size_t byte{0}; byte < Simulator::symbol_count; ++byte)
        {
            if (const auto to{simulator.step(from, static_cast<unsigned char>(byte))};
                to && seen.emplace(*to, via + static_cast<char>(byte)).second)
            {
                frontier.push_back(*to);
            }
        }
    }};

    expand(simulator.init_state(), std::string{});

    for (std::size_t at{0}; at < frontier.size(); ++at)
    {
        expand(frontier[at], seen.at(frontier[at]));
    }

    return {seen.begin(), seen.end()};
}

/**
 * @brief Finds the first in-tail token boundary of one crossing scenario.
 *
 * The maximal run from the entry over the whole tail; its last accept is the boundary, zero when the entry
 * itself accepts.
 * @param simulator The compiled token set.
 * @param tail The tail being walked.
 * @param entry The state the scan crosses into the tail in.
 * @return The boundary as an offset into the tail, or std::nullopt when the run never accepts.
 */
std::optional<std::size_t> scenario_boundary(
        const Simulator& simulator, const std::string_view tail, const std::size_t entry)
{
    return maximal_run(simulator, entry, tail, 0);
}

} // namespace

std::optional<std::size_t> lag(const Simulator& simulator)
{
    // The post-accept nonaccepting region: nonaccepting successors of accepting states, closed under
    // nonaccepting transitions; a cycle inside it is the unboundedness witness, and otherwise the lag is
    // the longest path measured in states.
    enum class Mark : char
    {
        outside,
        unresolved,
        resolved
    };

    std::vector<Mark> mark(simulator.state_count(), Mark::outside);

    std::vector<std::size_t> frontier;

    const auto enter{[&](const std::size_t state) {
        if (mark[state] == Mark::outside)
        {
            mark[state] = Mark::unresolved;

            frontier.push_back(state);
        }
    }};

    // An accepting state no input reaches opens no stretch; for an accepting state, live means reachable.
    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_accepting(state) || !simulator.is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            if (const auto to{simulator.step(state, static_cast<unsigned char>(symbol))};
                to && !simulator.is_accepting(*to))
            {
                enter(*to);
            }
        }
    }

    for (std::size_t at{0}; at < frontier.size(); ++at)
    {
        const auto from{frontier[at]};

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            if (const auto to{simulator.step(from, static_cast<unsigned char>(symbol))};
                to && !simulator.is_accepting(*to))
            {
                enter(*to);
            }
        }
    }

    // Longest path by repeated sink peeling; anything left over closes a cycle.
    std::vector<std::size_t> depth(simulator.state_count(), 0);

    auto remaining{frontier};

    std::size_t longest{0};

    for (bool shrank{true}; shrank && !remaining.empty();)
    {
        shrank = false;

        std::vector<std::size_t> keep;

        for (const auto state : remaining)
        {
            std::optional<std::size_t> deepest{0};

            for (std::size_t symbol{0}; symbol < Simulator::symbol_count && deepest; ++symbol)
            {
                const auto to{simulator.step(state, static_cast<unsigned char>(symbol))};

                if (!to || simulator.is_accepting(*to))
                {
                    continue;
                }

                if (mark[*to] == Mark::resolved)
                {
                    deepest = std::max(*deepest, depth[*to]);
                }
                else
                {
                    deepest = std::nullopt; // a successor still unresolved: not yet a sink
                }
            }

            if (deepest)
            {
                depth[state] = 1 + *deepest;

                mark[state] = Mark::resolved;

                longest = std::max(longest, depth[state]);

                shrank = true;
            }
            else
            {
                keep.push_back(state);
            }
        }

        remaining = std::move(keep);
    }

    if (!remaining.empty())
    {
        return std::nullopt; // the leftover states close a nonaccepting cycle: unbounded
    }

    return longest;
}

bool rescue_free(const Simulator& simulator)
{
    // As in lag(): an accepting state no input reaches cannot refute rescue-freeness.
    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_accepting(state) || !simulator.is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto opened{simulator.step(state, static_cast<unsigned char>(symbol))};

            if (!opened || simulator.is_accepting(*opened))
            {
                continue;
            }

            // A stretch opens on this byte; the gate needs it dead from the initial state, where dead
            // means no transition or one that can never reach acceptance.
            const auto entered{simulator.step(simulator.init_state(), static_cast<unsigned char>(symbol))};

            if (entered && simulator.is_live(*entered))
            {
                return false;
            }
        }
    }

    return true;
}

std::optional<std::size_t> next_anchored_start(
        const Simulator& simulator, const std::string_view tail, const std::size_t from)
{
    // Nullable token sets sit outside the model, exactly as for is_split_window().
    if (simulator.is_accepting(simulator.init_state()) || from >= tail.size())
    {
        return std::nullopt;
    }

    const auto table{build_jump_table(simulator, tail)};

    // Every completing scenario votes for its boundary chain; a position is anchored-certified when
    // every completing scenario contains it. No completing scenario means the tail is beyond repair and
    // every position only vacuously invariant, which is deliberately a refusal.
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

    for (const auto& [entry, via] : crossing_entries(simulator))
    {
        const auto boundary{scenario_boundary(simulator, tail, entry)};

        if (boundary && table.tokenizes[*boundary])
        {
            vote(*boundary);
        }
    }

    if (completing == 0)
    {
        return std::nullopt;
    }

    for (auto at{from}; at < tail.size(); ++at)
    {
        if (votes[at] == completing)
        {
            return at;
        }
    }

    return std::nullopt;
}

std::optional<std::string> minimal_repair(const Simulator& simulator, const std::string_view tail)
{
    if (simulator.is_accepting(simulator.init_state()))
    {
        return std::nullopt;
    }

    const auto table{build_jump_table(simulator, tail)};

    if (table.tokenizes[0])
    {
        return std::string{};
    }

    // The minimal repair is a shortest path to a completing crossing entry; the entries carry shortest
    // witnesses by construction, so the cheapest completing one is the answer, and none completing is a
    // certificate that no repair of any length exists.
    std::optional<std::string> best;

    for (const auto& [entry, via] : crossing_entries(simulator))
    {
        const auto boundary{scenario_boundary(simulator, tail, entry)};

        if (boundary && table.tokenizes[*boundary] && (!best || via.size() < best->size()))
        {
            best = via;
        }
    }

    return best;
}

} // namespace munch::dfa

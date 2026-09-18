#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/boundary_difference.hpp"
#include "munch/dfa/recovery.hpp"
#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
namespace
{
// The one search every boundary-guessing decision runs, and the two decisions that run it: rescue() and
// boundary_difference() each ask whether some completely tokenizable input makes an event happen, and both answer it
// by reading an input byte by byte while guessing where its tokens end, breadth first, so that the witness found is a
// shortest one. They are two instances of one search over one key, differing only in how many scans they walk and
// what raises the key's mark, which is why they live in one unit: the search, its key and the moves over it are
// private to this file, and a decision added later joins them here rather than being handed them across a header.
//
// The other recovery decisions, which walk the compiled machine rather than guessed inputs, stay in recovery.cpp.

/**
 * @brief A state as the search stores it: the width of the Simulator's table entry, which bounds the states a
 *        compiled token set can have, so every state a scan stands in fits; narrowed because a position holds a set
 *        of them and a search holds many positions.
 */
using State_t = std::uint32_t;

/**
 * @brief One scan's position in a search that guesses token boundaries: the run of the segment being read, and the
 *        runs of the segments already closed.
 *
 * A closed run is carried because a later accept on it proves the close was not the longest match, which is how a
 * guessed marking is held to maximal munch without tracking how far back the last accept was. The guesses that
 * survive are exactly the greedy segmentation, which is what makes guessing boundaries sound.
 */
struct Position
{
    std::size_t reading{};

    std::vector<State_t> closed{};

    auto operator<=>(const Position&) const = default;
};

/**
 * @brief A key of the search: the positions of the scans in progress, and whether the event searched for has
 *        happened on this branch.
 *
 * The decisions share this key because they share what a branch's future depends on: where each scan stands, and
 * whether the event has already happened, since after it a branch only has to reach a close. The event is one bit
 * rather than a record of where it happened, so two branches agreeing on the scans and the bit have the same futures
 * and are searched once. rescue() walks one scan and marks a branch once a closed run has survived a byte;
 * boundary_difference() walks two and marks a branch once the two markings have diverged.
 */
struct Key
{
    std::vector<Position> scans{};

    bool marked{};

    auto operator<=>(const Key&) const = default;
};

/**
 * @brief What a search found.
 */
struct Outcome
{
    /**
     * @brief The input the search stopped at, empty when it found none.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question rather than stopping at the cap.
     */
    bool exhaustive{};
};

/**
 * @brief What one byte does to a key: the keys it leads to, and whether the input may end on it.
 */
struct Step
{
    /**
     * @brief The keys the byte leads to, none when no scan survives it; a view of the expansion's own buffer,
     *        good until the expansion is next called, whose keys the search moves out.
     */
    std::span<Key> successors{};

    /**
     * @brief Whether the input may end on the byte as the witness looked for; the successors are not read then.
     */
    bool ends{};
};

/**
 * @brief How the search reached a key: the key the byte was read from, null for the start, and the byte.
 */
struct Parent
{
    const Key* from{};

    char byte{};
};

/**
 * @brief The keys visited, each with the parent that led to it.
 */
using Seen = std::map<Key, Parent>;

/**
 * @brief Sorts a set of closed runs and drops the repeats, so that two positions holding the same runs compare equal.
 * @param states The runs, in any order.
 */
void dedup(std::vector<State_t>& states)
{
    std::ranges::sort(states);

    const auto duplicates{std::ranges::unique(states)};

    states.erase(duplicates.begin(), duplicates.end());
}

/**
 * @brief Advances a position by one byte.
 * @param simulator The compiled token set the position scans.
 * @param position The position, advanced in place.
 * @param byte The byte read.
 * @return False abandons the branch: either the segment being read died, so it can never close and the input can
 *         never be finished, or a closed run accepted and the marking is not the greedy one. True leaves in closed
 *         exactly the runs that survived the byte.
 */
[[nodiscard]] bool advance(const Simulator& simulator, Position& position, const unsigned char byte)
{
    const auto next{simulator.step(position.reading, byte)};

    if (!next)
    {
        return false;
    }

    position.reading = *next;

    std::vector<State_t> survived;

    for (const auto state : position.closed)
    {
        const auto moved{simulator.step(state, byte)};

        if (!moved)
        {
            continue;
        }

        if (simulator.is_accepting(*moved))
        {
            return false;
        }

        survived.push_back(static_cast<State_t>(*moved));
    }

    dedup(survived);

    position.closed = std::move(survived);

    return true;
}

/**
 * @brief Closes the segment being read: its run stays alive as a closed run and a fresh one starts.
 * @param simulator The compiled token set the position scans.
 * @param position The position at the close.
 * @return The position after it.
 */
[[nodiscard]] Position close(const Simulator& simulator, Position position)
{
    position.closed.push_back(static_cast<State_t>(position.reading));

    dedup(position.closed);

    position.reading = simulator.init_state();

    return position;
}

/**
 * @brief The bytes that led the search to a key, read back through the parents it recorded.
 * @param seen The keys visited.
 * @param at The key the last byte was read from.
 * @param last The last byte.
 * @return The input, first byte first.
 */
[[nodiscard]] std::string trail(const Seen& seen, const Key* at, const char last)
{
    std::string out{last};

    for (auto parent{seen.at(*at)}; parent.from != nullptr; parent = seen.at(*parent.from))
    {
        out.push_back(parent.byte);
    }

    std::ranges::reverse(out);

    return out;
}

/**
 * @brief The breadth-first search over inputs that every boundary-guessing decision runs: from a start key, every
 *        byte in turn, the keys it reaches recorded with the byte that led there, until an input ends as a witness or
 *        no key is left. Bytes are explored in order and keys by length, so a witness is a shortest one.
 * @param start The key before any byte.
 * @param cap The largest number of keys to hold before giving up.
 * @param expand Given a key and a byte, the keys the byte leads to and whether the input may end on that byte as the
 *        witness looked for, as a Step.
 * @return The witness and whether the question was settled: an empty witness with the search exhausted is a proof
 *         that none exists, an empty witness with it stopped at the cap says nothing.
 */
template <typename Expand>
[[nodiscard]] Outcome search(const Key& start, const std::size_t cap, Expand expand)
{
    Seen seen{{start, Parent{.from = nullptr, .byte = '\0'}}};

    std::deque<const Key*> frontier{&seen.begin()->first};

    while (!frontier.empty())
    {
        if (seen.size() > cap)
        {
            return {.witness = {}, .exhaustive = false};
        }

        const auto* const at{frontier.front()};

        frontier.pop_front();

        for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
        {
            const auto byte{static_cast<unsigned char>(value)};

            const Step step{expand(*at, byte)};

            if (step.ends)
            {
                return {.witness = trail(seen, at, static_cast<char>(byte)), .exhaustive = true};
            }

            for (auto& next : step.successors)
            {
                if (const auto [entry, added]{
                            seen.try_emplace(std::move(next), Parent{.from = at, .byte = static_cast<char>(byte)})};
                    added)
                {
                    frontier.push_back(&entry->first);
                }
            }
        }
    }

    return {.witness = {}, .exhaustive = true};
}

} // namespace

Rescue rescue(const Simulator& simulator, const std::size_t cap)
{
    // The key marks a branch once some closed run has survived a byte, which is the rollback looked for. A run that
    // survives its first byte raises the mark; one that survived earlier leaves it raised, so the two need no telling
    // apart.
    const Key start{.scans = {Position{.reading = simulator.init_state(), .closed = {}}}, .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors;

    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto position{at.scans.front()};

        if (!advance(simulator, position, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto rescued{at.marked || !position.closed.empty()};

        const auto closes{simulator.is_accepting(position.reading)};

        // The input may end here when the segment being read closes on this byte; with a rollback behind it, that
        // is the witness, and the closed runs still alive never accepted, as every kept branch requires.
        if (closes && rescued)
        {
            return {.successors = successors, .ends = true};
        }

        successors.push_back(Key{.scans = {position}, .marked = rescued});

        if (closes)
        {
            successors.push_back(Key{.scans = {close(simulator, position)}, .marked = rescued});
        }

        return {.successors = successors, .ends = false};
    }};

    const auto [witness, exhaustive]{search(start, cap, expand)};

    return {.witness = witness, .exhaustive = exhaustive};
}

Difference boundary_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // The key holds the two scans' positions, mine first, and marks a branch once the two markings have diverged.
    const Key start{
            .scans =
                    {Position{.reading = simulator.init_state(), .closed = {}},
                     Position{.reading = other.init_state(), .closed = {}}},
            .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors;

    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto mine{at.scans[0]};

        auto theirs{at.scans[1]};

        if (!advance(simulator, mine, byte) || !advance(other, theirs, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto mine_closes{simulator.is_accepting(mine.reading)};

        const auto theirs_closes{other.is_accepting(theirs.reading)};

        // The input may end here only if both sides close their last segment on this byte.
        if (mine_closes && theirs_closes && at.marked)
        {
            return {.successors = successors, .ends = true};
        }

        for (const auto mine_closed : {false, true})
        {
            for (const auto theirs_closed : {false, true})
            {
                if ((mine_closed && !mine_closes) || (theirs_closed && !theirs_closes))
                {
                    continue;
                }

                const auto mine_next{mine_closed ? close(simulator, mine) : mine};

                const auto theirs_next{theirs_closed ? close(other, theirs) : theirs};

                successors.push_back(
                        Key{.scans = {mine_next, theirs_next}, .marked = at.marked || mine_closed != theirs_closed});
            }
        }

        return {.successors = successors, .ends = false};
    }};

    const auto [witness, exhaustive]{search(start, cap, expand)};

    return {.witness = witness, .exhaustive = exhaustive};
}

} // namespace munch::dfa

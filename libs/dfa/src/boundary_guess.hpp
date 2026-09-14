#ifndef MUNCH_LIBS_DFA_SRC_BOUNDARY_GUESS_HPP
#define MUNCH_LIBS_DFA_SRC_BOUNDARY_GUESS_HPP

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa::guess
{
/**
 * @brief A state as the searches store it: narrowed, since a position holds a set of them and a product holds many.
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
 * @brief Sorts a set of closed runs and drops the repeats, so that two positions holding the same runs compare equal.
 * @param states The runs, in any order.
 */
inline void dedup(std::vector<State_t>& states)
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
[[nodiscard]] inline bool advance(const Simulator& simulator, Position& position, const unsigned char byte)
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
[[nodiscard]] inline Position close(const Simulator& simulator, Position position)
{
    position.closed.push_back(static_cast<State_t>(position.reading));

    dedup(position.closed);

    position.reading = simulator.init_state();

    return position;
}

/**
 * @brief The bytes that led a search to a key, read back through the parents the search recorded.
 * @param seen The keys visited, each with its parent and the byte that led to it, the start's parent null.
 * @param at The key the last byte was read from.
 * @param last The last byte.
 * @return The input, first byte first.
 */
template <typename Key>
[[nodiscard]] std::string trail(const std::map<Key, std::pair<const Key*, char>>& seen, const Key* at, const char last)
{
    std::string out{last};

    for (auto step{seen.at(*at)}; step.first != nullptr; step = seen.at(*step.first))
    {
        out.push_back(step.second);
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
 * @param expand Given a key and a byte, appends the keys the byte leads to and returns whether the input may end on
 *        that byte as the witness looked for; a byte no scan survives appends nothing.
 * @return The witness and whether the question was settled: an empty witness with the search exhausted is a proof
 *         that none exists, an empty witness with it stopped at the cap says nothing.
 */
template <typename Key, typename Expand>
[[nodiscard]] Outcome search(const Key& start, const std::size_t cap, Expand expand)
{
    std::map<Key, std::pair<const Key*, char>> seen{{start, {nullptr, '\0'}}};

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

            std::vector<Key> successors;

            if (expand(*at, byte, successors))
            {
                return {.witness = trail(seen, at, static_cast<char>(byte)), .exhaustive = true};
            }

            for (auto& next : successors)
            {
                if (const auto [entry, added]{seen.try_emplace(std::move(next), at, static_cast<char>(byte))}; added)
                {
                    frontier.push_back(&entry->first);
                }
            }
        }
    }

    return {.witness = {}, .exhaustive = true};
}

} // namespace munch::dfa::guess

#endif // MUNCH_LIBS_DFA_SRC_BOUNDARY_GUESS_HPP

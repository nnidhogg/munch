#include "munch/dfa/boundary_difference.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace munch::dfa
{
namespace
{
/**
 * @brief A state as the search stores it: narrowed, since a position holds a set of them and the product holds many.
 */
using State_t = std::uint32_t;

} // namespace

Difference boundary_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // One side's position: the run of the segment being read, and the runs of segments already closed. A closed run
    // is carried because a later accept on it proves the close was not the longest match, which is how a guessed
    // marking is held to maximal munch without tracking how far back the last accept was.
    struct Side
    {
        std::size_t reading{};

        std::vector<State_t> closed{};

        auto operator<=>(const Side&) const = default;
    };

    using Key = std::tuple<Side, Side, bool>;

    // Advances one side by a byte. False abandons the branch: either the segment being read died, so it can never
    // close and the input can never be finished, or a closed run accepted and the marking is not the greedy one.
    const auto advance{[](const Simulator& sim, Side& side, const unsigned char byte) {
        const auto next{sim.step(side.reading, byte)};

        if (!next)
        {
            return false;
        }

        side.reading = *next;

        std::vector<State_t> survived;

        for (const auto state : side.closed)
        {
            const auto moved{sim.step(state, byte)};

            if (!moved)
            {
                continue;
            }

            if (sim.is_accepting(*moved))
            {
                return false;
            }

            survived.push_back(static_cast<State_t>(*moved));
        }

        std::ranges::sort(survived);

        const auto duplicates{std::ranges::unique(survived)};

        survived.erase(duplicates.begin(), duplicates.end());

        side.closed = std::move(survived);

        return true;
    }};

    // Closing a segment keeps the run that produced it alive as a closed run and starts a fresh one.
    const auto close{[](const Simulator& sim, Side side) {
        side.closed.push_back(static_cast<State_t>(side.reading));

        std::ranges::sort(side.closed);

        const auto duplicates{std::ranges::unique(side.closed)};

        side.closed.erase(duplicates.begin(), duplicates.end());

        side.reading = sim.init_state();

        return side;
    }};

    const Key start{
            Side{.reading = simulator.init_state(), .closed = {}}, Side{.reading = other.init_state(), .closed = {}},
            false};

    std::map<Key, std::pair<const Key*, char>> seen{{start, {nullptr, '\0'}}};

    std::deque<const Key*> frontier{&seen.begin()->first};

    const auto trail{[&seen](const Key* at, const char last) {
        std::string out{last};

        for (auto step{seen.at(*at)}; step.first != nullptr; step = seen.at(*step.first))
        {
            out.push_back(step.second);
        }

        std::ranges::reverse(out);

        return out;
    }};

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

            auto mine{std::get<0>(*at)};

            auto theirs{std::get<1>(*at)};

            if (!advance(simulator, mine, byte) || !advance(other, theirs, byte))
            {
                continue;
            }

            const auto mine_closes{simulator.is_accepting(mine.reading)};

            const auto theirs_closes{other.is_accepting(theirs.reading)};

            // The input may end here only if both sides close their last segment on this byte.
            if (mine_closes && theirs_closes && std::get<2>(*at))
            {
                return {.witness = trail(at, static_cast<char>(byte)), .exhaustive = true};
            }

            for (const auto mine_closed : {false, true})
            {
                for (const auto theirs_closed : {false, true})
                {
                    if ((mine_closed && !mine_closes) || (theirs_closed && !theirs_closes))
                    {
                        continue;
                    }

                    const Key next{
                            mine_closed ? close(simulator, mine) : mine, theirs_closed ? close(other, theirs) : theirs,
                            std::get<2>(*at) || mine_closed != theirs_closed};

                    if (const auto [entry, added]{seen.try_emplace(next, at, static_cast<char>(byte))}; added)
                    {
                        frontier.push_back(&entry->first);
                    }
                }
            }
        }
    }

    return {.witness = {}, .exhaustive = true};
}

} // namespace munch::dfa

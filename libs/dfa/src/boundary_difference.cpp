#include "munch/dfa/boundary_difference.hpp"

#include <cstddef>
#include <tuple>
#include <vector>

#include "boundary_guess.hpp"

namespace munch::dfa
{
Difference boundary_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // A key of the search: the two scans' positions, and whether the two markings have diverged.
    using Key = std::tuple<guess::Position, guess::Position, bool>;

    const Key start{
            guess::Position{.reading = simulator.init_state(), .closed = {}},
            guess::Position{.reading = other.init_state(), .closed = {}}, false};

    const auto expand{[&](const Key& at, const unsigned char byte, std::vector<Key>& successors) {
        auto mine{std::get<0>(at)};

        auto theirs{std::get<1>(at)};

        if (!guess::advance(simulator, mine, byte) || !guess::advance(other, theirs, byte))
        {
            return false;
        }

        const auto mine_closes{simulator.is_accepting(mine.reading)};

        const auto theirs_closes{other.is_accepting(theirs.reading)};

        // The input may end here only if both sides close their last segment on this byte.
        if (mine_closes && theirs_closes && std::get<2>(at))
        {
            return true;
        }

        for (const auto mine_closed : {false, true})
        {
            for (const auto theirs_closed : {false, true})
            {
                if ((mine_closed && !mine_closes) || (theirs_closed && !theirs_closes))
                {
                    continue;
                }

                successors.emplace_back(
                        mine_closed ? guess::close(simulator, mine) : mine,
                        theirs_closed ? guess::close(other, theirs) : theirs,
                        std::get<2>(at) || mine_closed != theirs_closed);
            }
        }

        return false;
    }};

    const auto found{guess::search(start, cap, expand)};

    return {.witness = found.witness, .exhaustive = found.exhaustive};
}

} // namespace munch::dfa

#include "munch/dfa/unroll_start.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace munch::dfa
{
Dfa unroll_start(const Dfa& dfa)
{
    if (!dfa.has_accept_token(dfa.init_state()))
    {
        return dfa;
    }

    // One past the highest state in use, so the fresh start collides with nothing.
    auto start{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        start = std::max({start, key.first, to});
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        start = std::max(start, state);
    }

    ++start;

    auto transitions{dfa.transitions()};

    for (const auto& [key, to] : dfa.transitions())
    {
        if (const auto& [from, label]{key}; from == dfa.init_state())
        {
            transitions.emplace(Dfa::Key_t{start, label}, to);
        }
    }

    return Dfa{start, std::move(transitions), dfa.accept_states()};
}

} // namespace munch::dfa

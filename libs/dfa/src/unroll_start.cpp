#include "munch/dfa/unroll_start.hpp"

#include <utility>

namespace munch::dfa
{
Dfa unroll_start(const Dfa& dfa)
{
    if (!dfa.has_accept_token(dfa.init_state()))
    {
        return dfa;
    }

    // One past the highest identifier in use, which no state uses while the span is representable, and which the
    // automaton returned spans one past again: the header asks the caller for both. The subset construction numbers
    // densely and never comes near the bound; a hand-built DFA owes it itself.
    const auto start{dfa.state_count()};

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

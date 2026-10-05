#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Any_of& any_of)
{
    // Creates a transition for every symbol in the set to the same accept state:
    //
    // (q0) --s[0]--> (q1)
    // (q0) --s[1]--> (q1)
    //  ...
    // (q0) --s[n]--> (q1)
    nfa::Builder nfa{};

    const auto accept_state{nfa.next_state()};

    for (const auto symbol : any_of.set.symbols())
    {
        nfa.add_transition(nfa.init_state(), nfa::Label{symbol}, accept_state);
    }

    nfa.add_accept_state(accept_state);

    return nfa;
}

} // namespace munch::regex

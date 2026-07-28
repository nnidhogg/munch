#include <algorithm>
#include <stdexcept>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Choice& choice)
{
    if (choice.regexes.empty())
    {
        throw std::invalid_argument("Choice must hold at least one regex");
    }

    /**
     * Connect all NFAs with ε transitions into a default constructed empty NFA.
     *
     *     / --ε--> (q1)
     * (q0) ---ε--> (q2)
     *     \ --ε--> (q3)
     */
    nfa::Builder nfa;

    std::ranges::for_each(choice.regexes, [&nfa](const auto& regex) { nfa = nfa.merge(to_nfa(regex)); });

    return nfa;
}

} // namespace munch::regex

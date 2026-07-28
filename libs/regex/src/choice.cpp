#include <algorithm>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
nfa::Builder to_nfa(const Choice& choice)
{
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

} // namespace lexer::regex

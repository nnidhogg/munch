#include <stdexcept>
#include <vector>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Choice& choice)
{
    if (choice.regexes.empty())
    {
        throw std::invalid_argument("Choice must hold at least one regex");
    }

    // A single alternative is no union at all; lowering it directly keeps the extra start state out.
    if (choice.regexes.size() == 1)
    {
        return to_nfa(choice.regexes.front());
    }

    /**
     * Union the alternatives under one fresh start state:
     *
     *      / --ε--> (q1)
     * (q0) - --ε--> (q2)
     *      \ --ε--> (q3)
     *
     * merge_all() renumbers each alternative once; folding merge() instead would copy the accumulated union per
     * alternative, quadratic work on the generated Unicode classes' hundreds of alternatives.
     */
    std::vector<nfa::Builder> alternatives;

    alternatives.reserve(choice.regexes.size());

    for (const auto& regex : choice.regexes)
    {
        alternatives.push_back(to_nfa(regex));
    }

    return nfa::Builder::merge_all(alternatives);
}

} // namespace munch::regex

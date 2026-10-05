#include <ranges>
#include <stdexcept>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Concat& concat)
{
    if (concat.regexes.empty())
    {
        throw std::invalid_argument{"Concat must hold at least one regex"};
    }

    // Concatenate all NFAs with ε transitions in sequence:
    //
    // (q0) --ε--> (q1) --ε--> (q2) --ε--> (q3)
    nfa::Builder nfa{to_nfa(concat.regexes.front())};

    for (const auto& regex : concat.regexes | std::views::drop(1))
    {
        const auto next{to_nfa(regex)};

        nfa = nfa.append(next);
    }

    return nfa;
}

} // namespace munch::regex

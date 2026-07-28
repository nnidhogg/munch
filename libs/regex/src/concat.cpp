#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Concat& concat)
{
    if (concat.regexes.empty())
    {
        throw std::invalid_argument("Concat must hold at least one regex");
    }

    /**
     * Concatenate all NFAs with ε transitions in sequence.
     *
     * (q0) --ε--> (q1) --ε--> (q2) --ε--> (q3)
     */
    nfa::Builder nfa{to_nfa(concat.regexes.front())};

    std::ranges::for_each(
            concat.regexes | std::views::drop(1), [&nfa](const auto& regex) { nfa = nfa.append(to_nfa(regex)); });

    return nfa;
}

} // namespace munch::regex

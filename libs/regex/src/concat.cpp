#include <algorithm>
#include <ranges>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
nfa::Builder to_nfa(const Concat& concat)
{
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

} // namespace lexer::regex

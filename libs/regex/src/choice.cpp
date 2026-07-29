#include <algorithm>
#include <ranges>
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
     * Fold the alternatives into a union, each merge contributing a fresh start state:
     *
     *      / --ε--> (q1)
     * (q0)
     *      \ --ε--> (q2)
     *
     * Seeding from the first alternative rather than an empty NFA keeps a dead state out of the union; merge()
     * makes this safe regardless of the seed's shape.
     */
    auto nfa{to_nfa(choice.regexes.front())};

    std::ranges::for_each(choice.regexes | std::views::drop(1), [&nfa](const auto& regex) {
        nfa = nfa.merge(to_nfa(regex));
    });

    return nfa;
}

} // namespace munch::regex

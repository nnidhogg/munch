#include "munch/regex/patterns.hpp"

#include "munch/regex/set.hpp"

namespace munch::regex::patterns
{
Regex identifier()
{
    const auto head{any_of(Set::alpha() + '_')};

    const auto tail{kleene(any_of(Set::alphanum() + '_'))};

    return concat(head, tail);
}

Regex decimal_integer()
{
    const auto digit{any_of(Set::digits())};

    return plus(digit);
}

Regex decimal_float()
{
    const auto digits{decimal_integer()};

    return concat(digits, text("."), digits);
}

} // namespace munch::regex::patterns

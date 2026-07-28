#include "munch/regex/patterns.hpp"

namespace munch::regex::patterns
{
Regex identifier()
{
    return concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')));
}

Regex decimal_integer()
{
    return plus(any_of(Set::digits()));
}

Regex decimal_float()
{
    return concat(plus(any_of(Set::digits())), text("."), plus(any_of(Set::digits())));
}

} // namespace munch::regex::patterns

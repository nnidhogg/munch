#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Regex& regex)
{
    const auto lower{[]<typename T>(const T& node) { return to_nfa(node); }};

    return std::visit(lower, regex.node);
}

} // namespace munch::regex

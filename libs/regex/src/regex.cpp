#include "munch/regex/regex.hpp"

namespace munch::regex
{
nfa::Builder to_nfa(const Regex& regex)
{
    return std::visit([](const auto& node) { return to_nfa(node); }, regex.node);
}

} // namespace munch::regex

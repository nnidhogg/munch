#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
nfa::Builder to_nfa(const Regex& regex)
{
    return std::visit([](const auto& node) { return to_nfa(node); }, regex.node);
}

} // namespace lexer::regex

#include "munch/tools/audit/token_set.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/regex/set.hpp"

namespace munch::tools::audit
{
void exclude(regex::Regex& regex, const unsigned char byte)
{
    using namespace munch::regex;

    std::visit(
            [byte](auto& node) {
                using Node = std::decay_t<decltype(node)>;

                if constexpr (std::is_same_v<Node, Any_of>)
                {
                    node.set -= static_cast<char>(byte);
                }
                else if constexpr (std::is_same_v<Node, Concat>)
                {
                    for (auto& part : node.regexes)
                    {
                        exclude(part, byte);
                    }
                }
                else if constexpr (std::is_same_v<Node, Choice>)
                {
                    // An alternative that cannot lose the byte is dropped; the caller checked that one remains.
                    std::erase_if(node.regexes, [byte](const Regex& part) { return !can_lose(part, byte); });

                    for (auto& part : node.regexes)
                    {
                        exclude(part, byte);
                    }
                }
                else if constexpr (std::is_same_v<Node, Repeat>)
                {
                    exclude(*node.regex, byte);
                }
                else
                {
                    static_assert(std::is_same_v<Node, Text>); // holds no set to narrow, and can_lose() cleared it
                }
            },
            regex.node);
}

core::Lexer compile(const Token_set& set)
{
    core::Builder builder;

    std::vector<std::size_t> discarded;

    for (const auto& rule : set.rules)
    {
        builder.add_token(rule.regex, rule.id, rule.priority);

        if (rule.discarded)
        {
            discarded.push_back(rule.id);
        }
    }

    builder.set_ignored_tokens(std::move(discarded));

    return builder.build();
}

bool can_lose(const regex::Regex& regex, const unsigned char byte)
{
    using namespace munch::regex;

    return std::visit(
            [byte](const auto& node) -> bool {
                using Node = std::decay_t<decltype(node)>;

                if constexpr (std::is_same_v<Node, Any_of>)
                {
                    return !(node.set - static_cast<char>(byte)).symbols().empty();
                }
                else if constexpr (std::is_same_v<Node, Text>)
                {
                    return node.text.find(static_cast<char>(byte)) == std::string::npos;
                }
                else if constexpr (std::is_same_v<Node, Concat>)
                {
                    return std::ranges::all_of(
                            node.regexes, [byte](const Regex& part) { return can_lose(part, byte); });
                }
                else if constexpr (std::is_same_v<Node, Choice>)
                {
                    return std::ranges::any_of(
                            node.regexes, [byte](const Regex& part) { return can_lose(part, byte); });
                }
                else
                {
                    static_assert(std::is_same_v<Node, Repeat>);

                    return can_lose(*node.regex, byte);
                }
            },
            regex.node);
}

} // namespace munch::tools::audit

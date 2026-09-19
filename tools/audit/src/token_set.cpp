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
namespace
{
/**
 * @brief Whether a repetition may match its sub-pattern no times at all.
 * @param kind The repetition kind.
 * @return True for the star and the optional, and for a counted repetition whose minimum is zero.
 */
[[nodiscard]] bool zero_minimum(const regex::Repeat::Kind_t& kind)
{
    using namespace munch::regex;

    return std::visit(
            []<typename Kind>(const Kind& repetition) {
                if constexpr (std::is_same_v<Kind, Kleene> || std::is_same_v<Kind, Optional>)
                {
                    return true;
                }
                else if constexpr (std::is_same_v<Kind, Plus>)
                {
                    return false;
                }
                else if constexpr (std::is_same_v<Kind, Exact>)
                {
                    return repetition.count == 0;
                }
                else if constexpr (std::is_same_v<Kind, At_least>)
                {
                    return repetition.min == 0;
                }
                else
                {
                    // A repetition kind added without a minimum read here is a compile error, not a silent zero.
                    static_assert(std::is_same_v<Kind, Range>, "Unhandled repetition kind");

                    return repetition.min == 0;
                }
            },
            kind);
}

} // namespace

void exclude(regex::Regex& regex, const unsigned char byte)
{
    using namespace munch::regex;

    // A repetition that may run zero times loses the byte by not running, so one whose sub-pattern cannot lose it is
    // deleted, leaving the empty string where it stood, as a choice drops an alternative that cannot lose the byte.
    // can_lose() cleared one of the two for every caller.
    if (const auto* repeat{std::get_if<Repeat>(&regex.node)};
        repeat && zero_minimum(repeat->kind) && !can_lose(*repeat->regex, byte))
    {
        regex.node = Text{.text = {}};

        return;
    }

    std::visit(
            [byte]<typename Node>(Node& node) {
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
            [byte]<typename Node>(const Node& node) -> bool {
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

                    // Running no times loses the byte whatever the sub-pattern spells, and leaves the empty string.
                    return zero_minimum(node.kind) || can_lose(*node.regex, byte);
                }
            },
            regex.node);
}

} // namespace munch::tools::audit

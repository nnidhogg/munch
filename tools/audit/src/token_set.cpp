#include "munch/tools/audit/token_set.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>
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
 * @brief Returns whether a repetition may match its sub-pattern no times at all.
 * @param kind The repetition kind.
 * @return True for the star and the optional, and for a counted repetition whose minimum is zero.
 */
[[nodiscard]] bool has_zero_minimum(const regex::Repeat::Kind_t& kind)
{
    /**
     * @brief Returns whether a repetition of one kind may match its sub-pattern no times at all.
     * @tparam Kind The kind's type.
     * @param repetition The repetition kind.
     * @return True when it may.
     */
    const auto zero_minimum{[]<typename Kind>(const Kind& repetition) {
        if constexpr (std::is_same_v<Kind, regex::Kleene> || std::is_same_v<Kind, regex::Optional>)
        {
            return true;
        }
        else if constexpr (std::is_same_v<Kind, regex::Plus>)
        {
            return false;
        }
        else if constexpr (std::is_same_v<Kind, regex::Exact>)
        {
            return repetition.count == 0;
        }
        else if constexpr (std::is_same_v<Kind, regex::At_least>)
        {
            return repetition.min == 0;
        }
        else
        {
            // A repetition kind added without a minimum read here is a compile error, not a silent zero.
            static_assert(std::is_same_v<Kind, regex::Range>, "Unhandled repetition kind");

            return repetition.min == 0;
        }
    }};

    return std::visit(zero_minimum, kind);
}

} // namespace

void exclude(regex::Regex& regex, const unsigned char byte)
{
    // A repetition that may run zero times loses the byte by not running, so one whose sub-pattern cannot lose it is
    // deleted, leaving the empty string where it stood, as a choice drops an alternative that cannot lose the byte.
    // can_lose() cleared one of the two for every caller.
    const auto deleted{[&regex, byte] {
        if (!std::holds_alternative<regex::Repeat>(regex.node))
        {
            return false;
        }

        const auto& [kind, repeated]{std::get<regex::Repeat>(regex.node)};

        return has_zero_minimum(kind) && !can_lose(*repeated, byte);
    }()};

    if (deleted)
    {
        regex.node = regex::Text{.text = {}};

        return;
    }

    /**
     * @brief Returns whether an alternative cannot lose the byte, which drops it from a choice.
     * @param part The alternative.
     * @return True when it cannot.
     */
    const auto keeps_byte{[byte](const regex::Regex& part) { return !can_lose(part, byte); }};

    /**
     * @brief Takes the byte off a pattern of one kind.
     * @tparam Node The node's type.
     * @param node The pattern.
     */
    const auto narrow{[byte, &keeps_byte]<typename Node>(Node& node) {
        if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            node.set -= static_cast<char>(byte);
        }
        else if constexpr (std::is_same_v<Node, regex::Concat>)
        {
            for (auto& part : node.regexes)
            {
                exclude(part, byte);
            }
        }
        else if constexpr (std::is_same_v<Node, regex::Choice>)
        {
            // An alternative that cannot lose the byte is dropped; the caller checked that one remains.
            std::erase_if(node.regexes, keeps_byte);

            for (auto& part : node.regexes)
            {
                exclude(part, byte);
            }
        }
        else if constexpr (std::is_same_v<Node, regex::Repeat>)
        {
            exclude(*node.regex, byte);
        }
        else
        {
            // A text holds no set to narrow, and can_lose() cleared it.
            static_assert(std::is_same_v<Node, regex::Text>);
        }
    }};

    std::visit(narrow, regex.node);
}

core::Lexer compile(const Token_set& set)
{
    core::Builder builder{};

    std::vector<std::size_t> discarded{};

    for (const auto& [rule_regex, id, priority, is_discarded] : set.rules)
    {
        builder.add_token(rule_regex, id, priority);

        if (is_discarded)
        {
            discarded.push_back(id);
        }
    }

    builder.set_ignored_tokens(std::move(discarded));

    return builder.build();
}

bool can_lose(const regex::Regex& regex, const unsigned char byte)
{
    /**
     * @brief Returns whether a part can lose the byte.
     * @param part The part.
     * @return True when it can.
     */
    const auto loses{[byte](const regex::Regex& part) { return can_lose(part, byte); }};

    /**
     * @brief Returns whether a pattern of one kind can lose the byte.
     * @tparam Node The node's type.
     * @param node The pattern.
     * @return True when it can.
     */
    const auto loses_node{[byte, &loses]<typename Node>(const Node& node) -> bool {
        if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            const auto remaining{node.set - static_cast<char>(byte)};

            return !remaining.symbols().empty();
        }
        else if constexpr (std::is_same_v<Node, regex::Text>)
        {
            return !node.text.contains(static_cast<char>(byte));
        }
        else if constexpr (std::is_same_v<Node, regex::Concat>)
        {
            return std::ranges::all_of(node.regexes, loses);
        }
        else if constexpr (std::is_same_v<Node, regex::Choice>)
        {
            return std::ranges::any_of(node.regexes, loses);
        }
        else
        {
            static_assert(std::is_same_v<Node, regex::Repeat>);

            // Running no times loses the byte whatever the sub-pattern spells, and leaves the empty string.
            return has_zero_minimum(node.kind) || can_lose(*node.regex, byte);
        }
    }};

    return std::visit(loses_node, regex.node);
}

} // namespace munch::tools::audit

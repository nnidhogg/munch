#include "munch/tools/audit/price.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/tools/audit/report.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The tokens the blame names for a byte, in rule order, each once.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @return The token ids.
 */
[[nodiscard]] std::vector<std::size_t> consumers(const core::Lexer& lexer, const unsigned char byte)
{
    std::set<std::size_t> tokens;

    for (const auto& entry : blame(lexer))
    {
        if (entry.byte == byte)
        {
            tokens.insert(entry.token);
        }
    }

    return {tokens.begin(), tokens.end()};
}

/**
 * @brief Whether some token of the compiled set matches the byte on its own.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @return True when a one-byte input of it tokenizes.
 */
[[nodiscard]] bool matched_alone(const core::Lexer& lexer, const unsigned char byte)
{
    const std::string input(1, static_cast<char>(byte));

    return lexer.tokenize<std::size_t>(input).length == 1;
}

/**
 * @brief Whether a pattern admits a byte anywhere: in a set, a text, or below.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool admits(const regex::Regex& regex, const unsigned char byte)
{
    using namespace regex;

    return std::visit(
            [byte]<typename Node>(const Node& node) {
                if constexpr (std::is_same_v<Node, Any_of>)
                {
                    return node.set.symbols().contains(static_cast<char>(byte));
                }
                else if constexpr (std::is_same_v<Node, Text>)
                {
                    return node.text.contains(static_cast<char>(byte));
                }
                else if constexpr (std::is_same_v<Node, Repeat>)
                {
                    return admits(*node.regex, byte);
                }
                else
                {
                    return std::ranges::any_of(node.regexes, [byte](const Regex& part) { return admits(part, byte); });
                }
            },
            regex.node);
}

/**
 * @brief The parts of a pattern's top-level sequence, the pattern itself as the one part when it is no sequence.
 * @param regex The pattern.
 * @return The parts.
 */
[[nodiscard]] const std::vector<regex::Regex>& parts_of(const regex::Regex& regex)
{
    static const std::vector<regex::Regex> none;

    const auto* concat{std::get_if<regex::Concat>(&regex.node)};

    return concat ? concat->regexes : none;
}

/**
 * @brief Whether a pattern is a run over a class the byte is in: one or more of a set, possibly the sole part of a
 *        sequence.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it is.
 */
[[nodiscard]] bool is_run(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    const auto& node{parts.size() == 1 ? parts.front().node : regex.node};

    const auto* repeat{std::get_if<regex::Repeat>(&node)};

    if (!repeat ||
        (!std::holds_alternative<regex::Kleene>(repeat->kind) && !std::holds_alternative<regex::Plus>(repeat->kind)))
    {
        return false;
    }

    const auto* set{std::get_if<regex::Any_of>(&(*repeat->regex).node)};

    return set && set->set.symbols().contains(static_cast<char>(byte));
}

/**
 * @brief Whether a sequence ends in the byte and holds it nowhere else: a body then its terminator.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_terminated(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    return parts.size() >= 2 && admits(parts.back(), byte) &&
           std::ranges::none_of(parts | std::views::take(parts.size() - 1), [byte](const regex::Regex& part) {
               return admits(part, byte);
           });
}

/**
 * @brief Whether a sequence opens with a fixed text the byte is not in, followed by a body it is in.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_delimited(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    if (parts.size() < 2)
    {
        return false;
    }

    const auto* opener{std::get_if<regex::Text>(&parts.front().node)};

    return opener && !opener->text.empty() && !opener->text.contains(static_cast<char>(byte));
}

/**
 * @brief The bytes a compiled set certifies exactly.
 * @param lexer The compiled set.
 * @return The bytes, ascending.
 */
[[nodiscard]] std::vector<unsigned char> exact_bytes(const core::Lexer& lexer)
{
    std::vector<unsigned char> bytes;

    for (std::size_t value{0}; value < 256; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)))
        {
            bytes.push_back(static_cast<unsigned char>(value));
        }
    }

    return bytes;
}

/**
 * @brief Where the byte stands over an edited set, against the bytes certified before any edit.
 * @param edited The set after the edit.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte priced.
 * @return The outcome.
 */
[[nodiscard]] Outcome outcome(
        const Token_set& edited, const std::vector<unsigned char>& before, const unsigned char byte)
{
    const auto lexer{compile(edited)};

    Outcome after{
            .exact = lexer.is_split_point(static_cast<char>(byte)),
            .modulo = lexer.is_split_point_ignoring(static_cast<char>(byte)),
            .gained = {}};

    std::ranges::set_difference(exact_bytes(lexer), before, std::back_inserter(after.gained));

    std::erase(after.gained, byte);

    return after;
}

/**
 * @brief The pattern a shape's edit leaves a token with: a terminated one its body, a delimited one its opener.
 * @param regex The token's pattern, of that shape for the byte.
 * @param shape The shape, terminated or delimited.
 * @return The pattern after the edit.
 */
[[nodiscard]] regex::Regex edited_by(const regex::Regex& regex, const Shape shape)
{
    const auto& parts{parts_of(regex)};

    if (shape == Shape::delimited)
    {
        return parts.front();
    }

    std::vector<regex::Regex> body{parts.begin(), parts.end() - 1};

    return body.size() == 1 ? std::move(body.front()) : regex::Regex{.node = regex::Concat{.regexes = std::move(body)}};
}

} // namespace

Shape shape_of(const regex::Regex& regex, const unsigned char byte)
{
    // A terminator or a body may spell the byte out, so the shapes with an edit of their own come before fixed.
    if (is_run(regex, byte))
    {
        return Shape::run;
    }

    if (is_terminated(regex, byte))
    {
        return Shape::terminated;
    }

    if (is_delimited(regex, byte))
    {
        return Shape::delimited;
    }

    return can_lose(regex, byte) ? Shape::other : Shape::fixed;
}

Pricing price(const Token_set& set, const unsigned char byte)
{
    auto edited{set};

    // Room for the one token the analysis may add, taken now: GCC 13 misreads the move a later reallocation would
    // make of a rule's pattern as a read of something uninitialized, and the build treats the warning as an error.
    edited.rules.reserve(set.rules.size() + 1);

    auto lexer{compile(edited)};

    const auto before{exact_bytes(lexer)};

    Pricing pricing{
            .byte = byte,
            .exact_before = lexer.is_split_point(static_cast<char>(byte)),
            .modulo_before = lexer.is_split_point_ignoring(static_cast<char>(byte)),
            .steps = {},
            .immovable = {},
            .gained = {},
            .choices = {},
            .together = std::nullopt};

    if (pricing.exact_before)
    {
        return pricing;
    }

    // Ids and priorities for a token of the byte's own, past every existing one so nothing else moves.
    auto next_id{0UZ};

    auto next_priority{0UZ};

    for (const auto& rule : set.rules)
    {
        next_id = std::max(next_id, rule.id + 1);

        next_priority = std::max(next_priority, rule.priority + 1);
    }

    // What each consuming token's shape offers on its own, before the uniform narrowing below edits anything, and
    // what every consumer taking its own shape's edit gives together.
    auto together{set};

    together.rules.reserve(set.rules.size() + 1);

    auto offered{false};

    auto narrowed_discarded{false};

    for (const auto token : consumers(lexer, byte))
    {
        const auto rule{std::ranges::find(set.rules, token, &Token_rule::id)};

        if (rule == set.rules.end())
        {
            continue;
        }

        for (const auto shape : {Shape::terminated, Shape::delimited})
        {
            if (shape == Shape::terminated ? is_terminated(rule->regex, byte) : is_delimited(rule->regex, byte))
            {
                auto alone{set};

                std::ranges::find(alone.rules, token, &Token_rule::id)->regex = edited_by(rule->regex, shape);

                pricing.choices.push_back({.token = token, .shape = shape, .after = outcome(alone, before, byte)});
            }
        }

        auto& taken{*std::ranges::find(together.rules, token, &Token_rule::id)};

        switch (shape_of(rule->regex, byte))
        {
        case Shape::terminated:
        case Shape::delimited:
            taken.regex = edited_by(rule->regex, shape_of(rule->regex, byte));

            offered = true;

            break;
        case Shape::run:
        case Shape::other:
            exclude(taken.regex, byte);

            narrowed_discarded = narrowed_discarded || rule->discarded;

            break;
        case Shape::fixed:
            break;
        }
    }

    if (offered)
    {
        if (!matched_alone(compile(together), byte))
        {
            together.rules.emplace_back(
                    regex::text(std::string(1, static_cast<char>(byte))), next_id, next_priority, narrowed_discarded);
        }

        pricing.together = outcome(together, before, byte);
    }

    for (const auto token : consumers(lexer, byte))
    {
        const auto rule{std::ranges::find(edited.rules, token, &Token_rule::id)};

        if (rule == edited.rules.end())
        {
            continue;
        }

        if (!can_lose(rule->regex, byte))
        {
            pricing.immovable.push_back(token);

            continue;
        }

        const auto shape{shape_of(rule->regex, byte)};

        exclude(rule->regex, byte);

        lexer = compile(edited);

        Price_step step{
                .token = token,
                .shape = shape,
                .separated = false,
                .separated_discarded = rule->discarded,
                .exact = false,
                .modulo = false};

        if (!matched_alone(lexer, byte))
        {
            edited.rules.emplace_back(
                    regex::text(std::string(1, static_cast<char>(byte))), next_id, next_priority, rule->discarded);

            step.separated = true;

            lexer = compile(edited);
        }

        step.exact = lexer.is_split_point(static_cast<char>(byte));

        step.modulo = lexer.is_split_point_ignoring(static_cast<char>(byte));

        pricing.steps.push_back(step);

        if (step.exact)
        {
            break;
        }
    }

    const auto after{exact_bytes(lexer)};

    std::ranges::set_difference(after, before, std::back_inserter(pricing.gained));

    std::erase(pricing.gained, byte);

    return pricing;
}

} // namespace munch::tools::audit

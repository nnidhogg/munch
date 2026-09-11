#include "munch/tools/audit/price.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
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

} // namespace

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
            .gained = {}};

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

        exclude(rule->regex, byte);

        lexer = compile(edited);

        Price_step step{
                .token = token,
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

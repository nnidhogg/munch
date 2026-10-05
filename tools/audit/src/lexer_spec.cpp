#include "munch/tools/audit/lexer_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::tools::audit
{
Spec_error::Spec_error(const std::string& message, const std::size_t line)
    : std::runtime_error{std::format("line {}: {}", line, message)}, line_{line}
{}

std::size_t Spec_error::line() const noexcept
{
    return line_;
}

core::Lexer build(const Lexer_spec& spec, const std::string_view condition)
{
    return compile(token_set(spec, condition));
}

Token_set token_set(const Lexer_spec& spec, const std::string_view condition)
{
    Token_set set{};

    // A generator's own number, higher winning, lands below every index on the builder's lower-wins scale.
    static constexpr auto highest_priority{std::numeric_limits<std::size_t>::max() / 2};

    for (const auto index : active_rules(spec, condition))
    {
        const auto& [pattern, expression, conditions, action, token, priority, line]{spec.rules[index]};

        if (priority && *priority > highest_priority)
        {
            throw Spec_error{std::format("the priority {} lies beyond the scale", *priority), line};
        }

        try
        {
            auto regex{regex::parse(expression, spec.definitions, spec.parse)};

            set.rules.push_back(
                    {.regex = std::move(regex),
                     .id = index,
                     .priority = priority ? highest_priority - *priority : index,
                     .discarded = !token.has_value()});
        }
        catch (const regex::Syntax_error& refused)
        {
            throw Spec_error{std::format("the pattern '{}' is refused: {}", pattern, refused.what()), line};
        }
    }

    return set;
}

std::vector<std::size_t> active_rules(const Lexer_spec& spec, const std::string_view condition)
{
    const auto declares_inclusive{[condition](const Lexer_spec::Condition& declared) {
        return declared.name == condition && !declared.exclusive;
    }};

    const auto inclusive{condition == initial_condition || std::ranges::any_of(spec.conditions, declares_inclusive)};

    const auto names_condition{
            [condition](const std::string& name) { return name == condition || name == every_condition; }};

    std::vector<std::size_t> active{};

    for (const auto [index, rule] : std::views::enumerate(spec.rules))
    {
        const auto& [pattern, expression, conditions, action, token, priority, line]{rule};

        const auto named{std::ranges::any_of(conditions, names_condition)};

        if (named || (conditions.empty() && inclusive))
        {
            active.push_back(static_cast<std::size_t>(index));
        }
    }

    return active;
}

} // namespace munch::tools::audit

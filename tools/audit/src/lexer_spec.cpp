#include "munch/tools/audit/lexer_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace munch::tools::audit
{
Spec_error::Spec_error(const std::string& message, const std::size_t line)
    : std::runtime_error{"line " + std::to_string(line) + ": " + message}, line_{line}
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
    // A generator's own number, higher winning, lands below every index on the builder's lower-wins scale.
    constexpr auto top{std::numeric_limits<std::size_t>::max() / 2};

    Token_set set;

    for (const auto index : active_rules(spec, condition))
    {
        const auto& rule{spec.rules[index]};

        if (rule.priority && *rule.priority > top)
        {
            throw Spec_error{"the priority " + std::to_string(*rule.priority) + " lies beyond the scale", rule.line};
        }

        try
        {
            set.rules.push_back(
                    {.regex = regex::parse(rule.expression, spec.definitions, spec.parse),
                     .id = index,
                     .priority = rule.priority ? top - *rule.priority : index,
                     .discarded = !rule.token.has_value()});
        }
        catch (const regex::Syntax_error& refused)
        {
            throw Spec_error{"the pattern '" + rule.pattern + "' is refused: " + refused.what(), rule.line};
        }
    }

    return set;
}

std::vector<std::size_t> active_rules(const Lexer_spec& spec, const std::string_view condition)
{
    const auto inclusive{
            condition == "INITIAL" ||
            std::ranges::any_of(spec.conditions, [condition](const Lexer_spec::Condition& declared) {
                return declared.name == condition && !declared.exclusive;
            })};

    std::vector<std::size_t> active;

    for (std::size_t index{0}; index < spec.rules.size(); ++index)
    {
        const auto& conditions{spec.rules[index].conditions};

        const auto named{std::ranges::any_of(
                conditions, [condition](const std::string& name) { return name == condition || name == "*"; })};

        if (named || (conditions.empty() && inclusive))
        {
            active.push_back(index);
        }
    }

    return active;
}

} // namespace munch::tools::audit

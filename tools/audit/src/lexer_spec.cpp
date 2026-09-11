#include "munch/tools/audit/lexer_spec.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The line with leading and trailing blanks removed.
 * @param line The line.
 * @return The trimmed view.
 */
[[nodiscard]] std::string_view trimmed(std::string_view line) noexcept
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
    {
        line.remove_prefix(1);
    }

    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    {
        line.remove_suffix(1);
    }

    return line;
}

/**
 * @brief The index just past a string or character literal or a comment opening at an index, or the index itself when
 *        none opens there; one left open runs to the end, and a line comment stops short of its newline.
 * @param code The stretch of C.
 * @param at The index.
 * @return The index to resume at.
 */
[[nodiscard]] std::size_t skipped(const std::string_view code, const std::size_t at) noexcept
{
    const auto byte{code[at]};

    if (byte == '"' || byte == '\'')
    {
        auto close{at + 1};

        while (close < code.size() && code[close] != byte)
        {
            close += code[close] == '\\' ? 2 : 1;
        }

        return std::min(close + 1, code.size());
    }

    if (byte == '/' && at + 1 < code.size() && code[at + 1] == '/')
    {
        return std::min(code.find('\n', at), code.size());
    }

    if (byte == '/' && at + 1 < code.size() && code[at + 1] == '*')
    {
        const auto close{code.find("*/", at + 2)};

        return close == std::string_view::npos ? code.size() : close + 2;
    }

    return at;
}

} // namespace

Spec_error::Spec_error(const std::string& message, const std::size_t line)
    : std::runtime_error{"line " + std::to_string(line) + ": " + message}, line_{line}
{}

std::size_t Spec_error::line() const noexcept
{
    return line_;
}

std::optional<std::string> returned(const std::string_view action, const Returning_t& returning)
{
    const auto is_word_byte{
            [](const char byte) { return std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '_'; }};

    // The earliest whole-word occurrence of any returning form.
    std::optional<std::pair<std::size_t, std::string_view>> first;

    const auto consider{[&](const std::string_view name) {
        for (auto at{action.find(name)}; at != std::string_view::npos; at = action.find(name, at + name.size()))
        {
            const auto before{at == 0 ? ' ' : action[at - 1]};

            const auto after{at + name.size() < action.size() ? action[at + name.size()] : ' '};

            if (!is_word_byte(before) && !is_word_byte(after))
            {
                if (!first || at < first->first)
                {
                    first = std::pair{at, name};
                }

                return;
            }
        }
    }};

    consider("return");

    for (const auto& name : returning)
    {
        consider(name);
    }

    if (!first)
    {
        return std::nullopt;
    }

    const auto [at, name]{*first};

    const auto rest{trimmed(action.substr(at + name.size()))};

    const auto through_semicolon{[rest](const std::size_t from) {
        const auto expression{trimmed(rest.substr(from, rest.find(';') - from))};

        return expression.empty() ? std::nullopt : std::optional{std::string{expression}};
    }};

    if (name == "return")
    {
        return through_semicolon(0);
    }

    if (rest.starts_with('='))
    {
        return through_semicolon(1);
    }

    if (!rest.starts_with('('))
    {
        return std::string{name};
    }

    // The first argument: up to the comma or the close at depth one, string and character literals skipped.
    std::size_t depth{0};

    for (std::size_t inner{0}; inner < rest.size(); ++inner)
    {
        const auto byte{rest[inner]};

        if (byte == '"' || byte == '\'')
        {
            for (++inner; inner < rest.size() && rest[inner] != byte; ++inner)
            {
                if (rest[inner] == '\\')
                {
                    ++inner;
                }
            }
        }
        else if (byte == '(')
        {
            ++depth;
        }
        else if ((byte == ')' && --depth == 0) || (byte == ',' && depth == 1))
        {
            const auto expression{trimmed(rest.substr(1, inner - 1))};

            return expression.empty() ? std::optional{std::string{name}} : std::optional{std::string{expression}};
        }
    }

    return std::string{name};
}

core::Lexer build(const Lexer_spec& spec, const std::string_view condition)
{
    return compile(token_set(spec, condition));
}

std::optional<std::size_t> action_end(const std::string_view code) noexcept
{
    std::size_t depth{0};

    for (std::size_t at{0}; at < code.size();)
    {
        if (const auto past{skipped(code, at)}; past != at)
        {
            at = past;

            continue;
        }

        if (code[at] == '{')
        {
            ++depth;
        }
        else if (code[at] == '}' && depth > 0)
        {
            --depth;
        }
        else if (code[at] == '\n' && depth == 0)
        {
            return at;
        }

        ++at;
    }

    return depth == 0 ? std::optional{code.size()} : std::nullopt;
}

std::optional<std::size_t> brace_close(const std::string_view code) noexcept
{
    std::size_t depth{0};

    for (std::size_t at{0}; at < code.size();)
    {
        if (const auto past{skipped(code, at)}; past != at)
        {
            at = past;

            continue;
        }

        if (code[at] == '{')
        {
            ++depth;
        }
        else if (code[at] == '}' && --depth == 0)
        {
            return at + 1;
        }

        ++at;
    }

    return std::nullopt;
}

Token_set token_set(const Lexer_spec& spec, const std::string_view condition)
{
    if (std::ranges::any_of(spec.options, [](const std::string& option) {
            return option == "case-insensitive" || option == "caseless" || option == "i";
        }))
    {
        throw Spec_error{"%option case-insensitive is not modelled: patterns are read as written", 0};
    }

    Token_set set;

    for (const auto index : active_rules(spec, condition))
    {
        const auto& rule{spec.rules[index]};

        try
        {
            set.rules.push_back(
                    {.regex = regex::parse(rule.expression, spec.definitions),
                     .id = index,
                     .priority = index,
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

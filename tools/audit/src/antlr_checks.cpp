#include "munch/tools/audit/antlr_checks.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "munch/tools/audit/antlr_alphabet.hpp"
#include "munch/tools/audit/antlr_commands.hpp"
#include "munch/tools/audit/antlr_cursor.hpp"
#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements antlr_checks.hpp: each check over the recorded grammar is private to this unit.

/**
 * @brief Refuses a closure whose body can match the empty string, ANTLR's error 153, and a non-greedy loop in a rule an
 *        alternative of which can.
 *
 * A rule matches the empty string when one of its alternatives does, and an alternative through the rules it reaches,
 * so the answer is the least fixed point over the grammar: nothing nullable, then whatever the formulas add, until they
 * add nothing. A closure over a body that matches it is ANTLR's error 153.
 * @param tables What the reading recorded.
 * @throws Spec_error If a closure's body or such an alternative matches the empty string.
 */
void check_nullability(const Grammar_tables& tables)
{
    std::set<std::string, std::less<>> nullable;

    for (auto growing{true}; growing;)
    {
        growing = false;

        for (const auto& [name, formula] : tables.nullability)
        {
            if (!nullable.contains(name) && matches_empty(formula, nullable))
            {
                nullable.insert(name);

                growing = true;
            }
        }
    }

    for (const auto& [rule, line, body] : tables.closures)
    {
        if (matches_empty(body, nullable))
        {
            const auto message{
                    "the rule " + rule +
                    " contains a closure with at least one alternative that can match the empty string, which ANTLR "
                    "rejects"};

            throw Spec_error{message, line};
        }
    }

    // ANTLR's empty match reaches the rule's end at the loop's decision and stops the loop, wherever the empty
    // alternative stands, so a loop in such a rule is refused; the alternatives were kept until the fixed point above
    // could say which of them matches the empty string.
    for (const auto& [rule, line, body] : tables.lazy_empty)
    {
        if (matches_empty(body, nullable))
        {
            const std::string message{
                    "a non-greedy loop is read only in a rule no alternative of which can match the empty string, "
                    "since ANTLR's empty match reaches the rule's end at the loop's decision and stops the loop, which "
                    "the byte reading cannot express"};

            throw Spec_error{message, line};
        }
    }
}

/**
 * @brief Refuses a rule holding a non-greedy loop that another rule references.
 *
 * A rule another rule references is inlined into that one, whose rest reaches past it, and a non-greedy loop stops
 * where the rest of the surrounding rule matches; so a rule holding such a loop is read only while nothing references
 * it, which the whole grammar has to be in to say.
 * @param tables What the reading recorded.
 * @param spec The scanner, its rules and definitions read.
 * @throws Spec_error If another rule references such a rule.
 */
void check_lazy_rules(const Grammar_tables& tables, const Lexer_spec& spec)
{
    for (const auto& [name, line] : tables.lazy_rules)
    {
        const auto reference{'{' + name + '}'};

        const auto referenced{
                std::ranges::any_of(
                        spec.rules,
                        [&reference](const Lexer_spec::Rule& rule) { return rule.expression.contains(reference); }) ||
                std::ranges::any_of(spec.definitions, [&reference, &name](const auto& definition) {
                    const auto& [defined, expression]{definition};

                    return defined != name && expression.contains(reference);
                })};

        if (referenced)
        {
            const auto message{
                    "the rule " + name +
                    " holds a non-greedy loop and another rule references it, so what ANTLR stops the loop at is the "
                    "rest of that rule and not of this one"};

            throw Spec_error{message, line};
        }
    }
}

/**
 * @brief The implicit tokens a combined grammar makes of the literals its parser rules use, in the order they are first
 *        used.
 *
 * The literals the parser rules use are implicit tokens ahead of every explicit rule, unless a rule spells exactly that
 * literal in a shape ANTLR maps it onto, when the parser's literal is that rule's token; two rules spelling it leave
 * ANTLR no token to map it onto, and it rejects the parser's use of the literal.
 * @param tables What the reading recorded.
 * @param spec The scanner, its rules and definitions read.
 * @param case_insensitive Whether the grammar's `caseInsensitive` option is set.
 * @return The implicit tokens' rules, the k-th named `T__k` by ANTLR.
 * @throws Spec_error If two rules spell one of the literals, a literal beyond ASCII is folded, or a rule is named as an
 *         implicit token is.
 */
[[nodiscard]] std::vector<Lexer_spec::Rule> implicit_tokens(
        const Grammar_tables& tables, const Lexer_spec& spec, const bool case_insensitive)
{
    std::vector<Lexer_spec::Rule> implicit;

    for (const auto& [text, line] : tables.parser_literals)
    {
        const auto aliased{tables.aliases.find(text)};

        if (aliased != tables.aliases.end() && aliased->second > 1)
        {
            const auto message{
                    "two lexer rules spell " + text +
                    ", so ANTLR maps it onto neither and rejects the parser's use of it: cannot create implicit token "
                    "for string literal in non-combined grammar: " +
                    text};

            throw Spec_error{message, line};
        }

        const auto placed{
                std::ranges::any_of(implicit, [&text](const Lexer_spec::Rule& rule) { return rule.pattern == text; })};

        if (aliased != tables.aliases.end() || placed)
        {
            continue;
        }

        Antlr_cursor reader{text};

        std::ignore = reader.accept('\'');

        const auto bytes{reader.literal().bytes};

        if (case_insensitive && holds_beyond_ascii(bytes))
        {
            throw Spec_error{std::string{unfoldable}, line};
        }

        implicit.push_back(
                {.pattern = text,
                 .expression = case_insensitive ? caseless(bytes) : quoted(bytes),
                 .conditions = {},
                 .action = {},
                 .token = text,
                 .priority = std::nullopt,
                 .line = line});
    }

    // ANTLR's error 51 again: a combined grammar's implicit tokens are rules named `T__k`, so an explicit rule of that
    // name is a redefinition, in ANTLR's words, the implicit one having no line.
    for (std::size_t index{0}; index < implicit.size(); ++index)
    {
        const auto name{"T__" + std::to_string(index)};

        if (spec.definitions.contains(name))
        {
            throw Spec_error{
                    std::format("rule {} redefinition; previous at line 0", name), tables.definition_lines.at(name)};
        }
    }

    return implicit;
}

/**
 * @brief Refuses a mode or a channel named as ANTLR reserves or as another token, mode or channel is named.
 *
 * ANTLR's errors 173 and 172: the names its commands and channels reserve are no mode's and no channel's, DEFAULT_MODE
 * reopening the default mode aside; and its errors 170, 161 and 162: a mode's name is no token's, a channel's name is
 * no token's and no mode's, the tokens being the rules with a token type of their own and the `tokens` entries,
 * anywhere in the grammar, the default mode's own name among the modes.
 * @param tables What the reading recorded.
 * @throws Spec_error At the first such name.
 */
void check_names(const Grammar_tables& tables)
{
    for (const auto& [name, line, rules] : tables.sections)
    {
        if (name != "DEFAULT_MODE" && std::ranges::contains(reserved_names, name))
        {
            throw Spec_error{std::format("cannot use or declare mode with reserved name {}", name), line};
        }

        if (name != "DEFAULT_MODE" && (tables.rule_names.contains(name) || tables.tokens.contains(name)))
        {
            throw Spec_error{std::format("mode {} conflicts with token with same name", name), line};
        }
    }

    if (tables.tokens.contains("DEFAULT_MODE"))
    {
        throw Spec_error{"mode DEFAULT_MODE conflicts with token with same name", tables.tokens_line};
    }

    for (const auto& channel : tables.channels)
    {
        if (std::ranges::contains(reserved_names, channel))
        {
            throw Spec_error{
                    std::format("cannot use or declare channel with reserved name {}", channel), tables.channels_line};
        }

        if (tables.rule_names.contains(channel) || tables.tokens.contains(channel))
        {
            throw Spec_error{
                    std::format("channel {} conflicts with token with same name", channel), tables.channels_line};
        }

        if (declares_mode(tables, channel))
        {
            throw Spec_error{
                    std::format("channel {} conflicts with mode with same name", channel), tables.channels_line};
        }
    }
}

/**
 * @brief Resolves the names the `type` commands carry, a `T__k` becoming the literal of the k-th implicit token.
 *
 * ANTLR's error 175: a `type` names a token the grammar has, anywhere in it: a rule that is no fragment and has a token
 * type of its own, a `tokens` entry, or `T__k`, the k-th of the implicit tokens a combined grammar makes of the
 * parser's literals no rule spells, numbered in the order the parser rules use them; a rule typed `T__k` emits that
 * literal's token, named by the literal as the implicit rule's is.
 * @param tables What the reading recorded.
 * @param implicit The implicit tokens' rules.
 * @param spec The scanner, whose rules' tokens are renamed.
 * @throws Spec_error If a name is reserved or names no token the grammar has.
 */
void resolve_types(const Grammar_tables& tables, const std::vector<Lexer_spec::Rule>& implicit, Lexer_spec& spec)
{
    for (const auto& [name, line] : tables.typed_names)
    {
        // ANTLR's error 171: a `type` may not name what its commands and channels reserve, declared or not.
        if (std::ranges::contains(reserved_names, name))
        {
            throw Spec_error{std::format("cannot use or declare token with reserved name {}", name), line};
        }

        if (tables.rule_names.contains(name) || tables.tokens.contains(name))
        {
            continue;
        }

        const auto digits{name.starts_with("T__") ? name.substr(3) : std::string{}};

        // The spelling is exact, `T__0` and never `T__00`, as ANTLR's own table names them.
        const auto index{is_number(digits) && digits.size() <= 6 ? std::optional{std::stoul(digits)} : std::nullopt};

        const auto numbered{index && digits == std::to_string(*index) && *index < implicit.size()};

        if (!numbered)
        {
            throw Spec_error{name + " is not a recognized token name", line};
        }

        const auto& literal{*implicit[*index].token};

        for (auto& rule : spec.rules)
        {
            if (rule.token == name)
            {
                rule.token = literal;
            }
        }
    }
}

/**
 * @brief Refuses a `mode` section holding no rule that is no fragment, ANTLR's error 145, and a mode command naming no
 *        mode, its error 176.
 *
 * Each `mode` section is held to it on its own, a reopened mode's sections each.
 * @param tables What the reading recorded.
 * @throws Spec_error At the first such section or command.
 */
void check_modes(const Grammar_tables& tables)
{
    for (const auto& [mode, line, rules] : tables.sections)
    {
        if (rules == 0)
        {
            throw Spec_error{std::format("lexer mode {} must contain at least one non-fragment rule", mode), line};
        }
    }

    // ANTLR's error 176: a `mode` or `pushMode` names a mode the grammar declares, before or after it, or DEFAULT_MODE;
    // a number names a mode by its index and is kept as written.
    for (const auto& [name, line] : tables.mode_names)
    {
        const auto declared{name == "DEFAULT_MODE" || declares_mode(tables, name)};

        if (!declared)
        {
            throw Spec_error{name + " is not a recognized mode name", line};
        }
    }
}

} // namespace

void finish_grammar(const Grammar_tables& tables, const bool case_insensitive, Lexer_spec& spec)
{
    check_nullability(tables);

    check_lazy_rules(tables, spec);

    auto implicit{implicit_tokens(tables, spec, case_insensitive)};

    check_names(tables);

    resolve_types(tables, implicit, spec);

    spec.rules.insert(
            spec.rules.begin(), std::make_move_iterator(implicit.begin()), std::make_move_iterator(implicit.end()));

    check_modes(tables);
}

bool declares_mode(const Grammar_tables& tables, const std::string_view name)
{
    return std::ranges::any_of(tables.sections, [name](const Mode_section& section) { return section.name == name; });
}

} // namespace munch::tools::audit

#include "munch/tools/audit/antlr_grammar.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/antlr_checks.hpp"
#include "munch/tools/audit/antlr_commands.hpp"
#include "munch/tools/audit/antlr_expression.hpp"
#include "munch/tools/audit/antlr_members.hpp"
#include "munch/tools/audit/antlr_rule.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Returns whether a byte is a capital letter, which a lexer rule's name begins with and a parser rule's does
 *        not.
 * @param byte The byte.
 * @return True for A to Z.
 */
[[nodiscard]] constexpr bool is_capital(const char byte) noexcept
{
    return byte >= 'A' && byte <= 'Z';
}

/**
 * @brief Returns whether a lexer rule spells a parser literal in a shape ANTLR maps the literal onto.
 *
 * In a combined grammar ANTLR maps a parser rule's literal onto the lexer rule that spells it, and what spells it is
 * what ANTLR's own tree patterns match: a rule that is no fragment and carries no options, of one alternative that is
 * the literal alone, bare of element options, the literal and one action, or the literal and one or two commands of
 * which at most one takes an argument; the grammar's comments are in no pattern. A rule of any other shape spelling the
 * literal leaves the parser's literal an implicit token of its own, placed ahead of the rule.
 * @param alternatives The rule's outermost alternatives.
 * @param fragment Whether the rule is a fragment.
 * @param optioned Whether the rule carries options.
 * @return True when it does.
 */
[[nodiscard]] bool is_aliased_literal(
        const std::vector<Alternative>& alternatives, const bool fragment, const bool optioned)
{
    const auto& [pattern, expression, commands, clause, spelling, acted, empty, nullable]{alternatives.front()};

    if (fragment || optioned || alternatives.size() != 1 || spelling.empty())
    {
        return false;
    }

    const auto& [read, error]{commands_of(commands)};

    const auto takes_argument{[](const Command& command) { return !command.argument.empty(); }};

    const auto called{std::ranges::count_if(read, takes_argument)};

    return read.empty() || (!acted && read.size() <= 2 && called <= 1);
}

} // namespace

Grammar_reader::Grammar_reader(const std::string_view source) : Antlr_cursor{source}
{}

Lexer_spec Grammar_reader::read()
{
    Lexer_spec spec{};

    // Only a lexer grammar may declare modes, so where the rules come from decides whether a `mode` line is ANTLR.
    auto context{grammar_declaration(spec)};

    for (skip_blanks(); peek(); skip_blanks())
    {
        item(spec, context);
    }

    refuse_lexer_class(spec, context.lexer_class, context.members, context.actions_code);

    finish_grammar(tables_, context.case_insensitive, spec);

    return spec;
}

Grammar_reader::Item_context Grammar_reader::grammar_declaration(Lexer_spec& spec)
{
    skip_blanks();

    const auto declared{at_};

    auto keyword{identifier()};

    if (keyword == "lexer" || keyword == "parser")
    {
        skip_blanks();

        keyword = std::format("{} {}", identifier(), keyword);
    }

    if (!keyword.starts_with("grammar"))
    {
        fail("expected a grammar declaration");
    }

    if (keyword.ends_with("parser"))
    {
        fail("a parser grammar has no lexer rules");
    }

    const auto lexer_only{keyword.ends_with("lexer")};

    skip_blanks();

    const auto name{identifier()};

    if (name.empty())
    {
        fail("the grammar has no name");
    }

    skip_blanks();

    expect(';', "';' to end the grammar declaration");

    spec.line = line_of(declared);

    // A combined grammar's lexer is generated as a class of the grammar's name and `Lexer`.
    return {.lexer_only = lexer_only,
            .case_insensitive = false,
            .mode = {},
            .members = {},
            .actions_code = {},
            .lexer_class = lexer_only ? std::string{name} : std::format("{}Lexer", name)};
}

void Grammar_reader::item(Lexer_spec& spec, Item_context& context)
{
    const auto opened{at_};

    if (peek() == '@')
    {
        named_action(context);

        return;
    }

    const auto word{identifier()};

    if (word.empty())
    {
        fail(std::format("unexpected '{}' at the top level", *peek()));
    }

    if (word == "options")
    {
        if (const auto set{options_block(spec.options)})
        {
            context.case_insensitive = *set;
        }

        return;
    }

    if (word == "tokens")
    {
        tokens_spec(opened, context.lexer_only);

        return;
    }

    if (word == "channels")
    {
        channels_spec(opened, context.lexer_only);

        return;
    }

    if (word == "import")
    {
        fail("the grammar imports another, whose rules are not here to read");
    }

    if (word == "mode")
    {
        mode_spec(spec, opened, context);

        return;
    }

    at_ = opened;

    if (word == "fragment" || is_capital(word.front()))
    {
        lexer_rule(spec, context.mode, context.case_insensitive);
    }
    else if (context.lexer_only)
    {
        // ANTLR's error 53, in its words: a lexer grammar holds lexer rules alone.
        fail(std::format("parser rule {} not allowed in lexer", word));
    }
    else
    {
        parser_rule();
    }
}

void Grammar_reader::named_action(Item_context& context)
{
    ++at_;

    skip_blanks();

    auto named{identifier()};

    skip_blanks();

    static constexpr std::string_view scope_separator{"::"};

    if (at(scope_separator))
    {
        at_ += scope_separator.size();

        skip_blanks();

        const auto scope{identifier()};

        named = std::format("{}{}{}", named, scope_separator, scope);
    }

    while (peek() && *peek() != '{')
    {
        ++at_;
    }

    const auto opened{at_};

    skip_block();

    const auto action{text_.substr(opened, at_ - opened)};

    static constexpr std::array<std::string_view, 2> definitions{"definitions", "lexer::definitions"};

    const auto out_of_line{std::ranges::contains(definitions, named)};

    // The lexer's header holds every action but the definitions, which follow it in the source file, and the parser's.
    if (!out_of_line && !named.starts_with("parser::"))
    {
        context.actions_code += action;

        context.actions_code += '\n';
    }

    static constexpr std::array<std::string_view, 4> insertions{
            "members", "lexer::members", "declarations", "lexer::declarations"};

    if (std::ranges::contains(insertions, named) || out_of_line)
    {
        context.members.push_back({.code = action, .line = line_of(opened), .out_of_line = out_of_line});
    }
}

std::optional<bool> Grammar_reader::options_block(std::vector<std::string>& options)
{
    skip_blanks();

    expect('{', "'{' to open the options block");

    std::optional<bool> case_insensitive{};

    for (skip_blanks(); peek() != '}'; skip_blanks())
    {
        const auto name{identifier()};

        if (name.empty())
        {
            fail("an option needs a name");
        }

        skip_blanks();

        expect('=', "'=' after the option's name");

        skip_blanks();

        // ANTLR reads the value with the lexer it reads the grammar with, one token: a name, dotted or not, a number, a
        // quoted string or a brace block, the blanks and comments around it no part of it.
        const auto begin{at_};

        if (peek() == '\'')
        {
            ++at_;

            std::ignore = literal();
        }
        else if (peek() == '{')
        {
            skip_block();
        }
        else
        {
            while (peek() && (is_name_byte(*peek()) || *peek() == '.'))
            {
                ++at_;
            }
        }

        const std::string value{text_.substr(begin, at_ - begin)};

        if (value.empty())
        {
            fail("an option needs a value");
        }

        skip_blanks();

        expect(';', "';' to end the option");

        options.push_back(std::format("{}={}", name, value));

        // ANTLR takes `true` and `false` and no other spelling, `TRUE` among them: another value is its warning 84 and
        // sets nothing, so the grammar's option stays off and a rule's option leaves the grammar's in force.
        if (name == "caseInsensitive" && (value == "true" || value == "false"))
        {
            case_insensitive = value == "true";
        }
    }

    ++at_;

    return case_insensitive;
}

void Grammar_reader::tokens_spec(const std::size_t opened, const bool lexer_only)
{
    // A lexer grammar's block gives its names token types ahead of every rule; a combined grammar's goes to the parser
    // it builds, its lexer none the wiser.
    std::set<std::string, std::less<>> names{};

    tables_.tokens_line = line_of(opened);

    names_block(names, "token", true);

    if (lexer_only)
    {
        tables_.tokens.merge(names);
    }
}

void Grammar_reader::names_block(
        std::set<std::string, std::less<>>& names, const std::string_view kind, const bool may_be_empty)
{
    skip_blanks();

    expect('{', std::format("'{{' to open the {}s block", kind));

    skip_blanks();

    if (may_be_empty && accept('}'))
    {
        return;
    }

    const auto refuse_at_end{[this, kind] {
        if (!peek())
        {
            fail(std::format("expected '}}' to close the {}s block", kind));
        }
    }};

    for (;;)
    {
        refuse_at_end();

        const auto name{identifier()};

        if (name.empty())
        {
            fail(std::format(
                    "syntax error: '{}' stands where a {} name should, which ANTLR's parser rejects while matching a "
                    "{}s block",
                    *peek(), kind, kind));
        }

        names.insert(name);

        skip_blanks();

        if (accept(','))
        {
            skip_blanks();

            continue;
        }

        if (accept('}'))
        {
            return;
        }

        refuse_at_end();

        // The word standing where a comma or the brace should, `TWO` in `{ ONE TWO }`, or else the byte there.
        const auto next{identifier()};

        const auto standing{next.empty() ? std::string{*peek()} : next};

        fail(std::format(
                "syntax error: '{}' stands after the {} {} where ',' or '}}' should, which ANTLR's parser rejects "
                "while matching a {}s block",
                standing, kind, name, kind));
    }
}

void Grammar_reader::channels_spec(const std::size_t opened, const bool lexer_only)
{
    // Only a lexer grammar may declare channels, ANTLR's error 164 in a combined one.
    if (!lexer_only)
    {
        fail("custom channels are not supported in combined grammars");
    }

    tables_.channels_line = line_of(opened);

    names_block(tables_.channels, "channel", false);
}

void Grammar_reader::mode_spec(Lexer_spec& spec, const std::size_t opened, Item_context& context)
{
    auto& mode{context.mode};

    if (!context.lexer_only)
    {
        fail("lexical modes are only allowed in lexer grammars, so a combined grammar declaring one is no "
             "ANTLR grammar");
    }

    skip_blanks();

    mode = identifier();

    if (mode.empty())
    {
        fail("a mode needs a name");
    }

    skip_blanks();

    expect(';', "';' to end the mode line");

    // The default mode is reported under the name INITIAL, as every reader's default condition is, so a mode of the
    // grammar's own named INITIAL, which ANTLR allows, could not be told from it.
    if (mode == initial_condition)
    {
        fail("a mode named INITIAL is refused: the audit reports ANTLR's default mode under that name, so "
             "a mode of the grammar's own called INITIAL cannot be told from it");
    }

    // ANTLR's parser takes a mode line only after a rule, a fragment counting, its syntax error 50 in its own words
    // where none stands before it.
    if (spec.definitions.empty())
    {
        at_ = opened;

        fail("syntax error: 'mode' came as a complete surprise to me");
    }

    // A mode named again reopens it, DEFAULT_MODE reopening the default mode, whose rules name no condition; each
    // section is held to ANTLR's error 145 on its own.
    const auto declared{declares_mode(tables_, mode)};

    tables_.sections.push_back({.name = mode, .line = line_of(opened), .rules = 0});

    if (mode == default_mode)
    {
        mode.clear();
    }
    else if (!declared)
    {
        spec.conditions.push_back({.name = mode, .exclusive = true});
    }
}

void Grammar_reader::lexer_rule(Lexer_spec& spec, const std::string& mode, const bool case_insensitive)
{
    const auto opened{at_};

    const auto [name, fragment]{rule_name()};

    skip_blanks();

    const auto optioned{at("options")};

    const auto rule_case_insensitive{optioned ? rule_options(case_insensitive) : case_insensitive};

    skip_blanks();

    expect(':', std::format("':' after the rule {}", name));

    Expression_reader body{text_, at_, tables_, name, rule_case_insensitive};

    const auto alternatives{body.alternatives()};

    at_ = body.offset();

    if (body.is_lazy())
    {
        tables_.lazy_rules.push_back({.text = name, .line = line_of(opened)});
    }

    const auto aliased{is_aliased_literal(alternatives, fragment, optioned)};

    if (aliased)
    {
        ++tables_.aliases[alternatives.front().spelling];
    }

    // ANTLR gives a rule a token type of its own where a lexer grammar's tokens block names it or the rule spells a
    // literal in the shape is_aliased_literal() tests, and no type at all to another rule whose commands set the type:
    // its own type is what a type command setting zero, ANTLR's value for none, leaves the token with.
    const auto typed{aliased || tables_.tokens.contains(name)};

    const auto whole{define(spec, name, alternatives, opened)};

    if (fragment)
    {
        return;
    }

    const auto carries_commands{[](const Alternative& alternative) { return !alternative.commands.empty(); }};

    const auto commanded{std::ranges::any_of(alternatives, carries_commands)};

    // A command ends the rule's single outermost alternative, so a rule of several alternatives carries none: one rule
    // is one token however many alternatives it has.
    if (alternatives.size() > 1 && commanded)
    {
        at_ = opened;

        fail(std::format(
                "a command must be the last element of the single outermost alternative of a lexer rule, so the "
                "commands on the alternatives of {} are no ANTLR grammar",
                name));
    }

    const auto line{line_of(opened)};

    record_rule(spec, mode, alternatives, whole, {.name = name, .line = line, .typed = typed}, aliased);
}

Grammar_reader::Rule_name Grammar_reader::rule_name()
{
    const auto opened{at_};

    auto name{identifier()};

    const auto fragment{name == "fragment"};

    if (fragment)
    {
        skip_blanks();

        name = identifier();

        // A fragment is a lexer rule and named as one, capitalised: ANTLR's parser refuses any other name in its own
        // words, its error 50.
        if (name.empty() || !is_capital(name.front()))
        {
            at_ -= name.size();

            fail(std::format(
                    "syntax error: mismatched input '{}' expecting TOKEN_REF while matching a lexer rule", name));
        }
    }

    if (name.empty())
    {
        fail("a rule needs a name");
    }

    // ANTLR's error 159: the names its lexer commands and channels reserve are no rule's.
    if (std::ranges::contains(reserved_names, name))
    {
        at_ = opened;

        fail(std::format("cannot declare a rule with reserved name {}", name));
    }

    return {.name = std::move(name), .fragment = fragment};
}

bool Grammar_reader::rule_options(const bool case_insensitive)
{
    std::ignore = identifier();

    skip_blanks();

    std::vector<std::string> ignored{};

    const auto set{options_block(ignored)};

    return set.value_or(case_insensitive);
}

std::string Grammar_reader::define(
        Lexer_spec& spec, const std::string& name, const std::vector<Alternative>& alternatives,
        const std::size_t opened)
{
    // The whole rule is what a reference to it expands to, and it matches the empty string when one of its alternatives
    // does, which is what a closure over a reference to it needs; an empty alternative makes it optional.
    std::string whole{};

    auto filled{0UZ};

    auto optional{false};

    auto nullable{never_empty()};

    for (const auto& [pattern, expression, commands, clause, spelling, acted, empty, alternative_nullable] :
         alternatives)
    {
        nullable = either_empty(std::move(nullable), alternative_nullable);

        if (empty)
        {
            optional = true;

            continue;
        }

        join_onto(whole, expression, "|");

        ++filled;
    }

    if (filled == 0)
    {
        at_ = opened;

        fail(std::format("the rule {} matches nothing but the empty string", name));
    }

    if (filled > 1 || optional)
    {
        whole = grouped(whole, optional);
    }

    // ANTLR's error 51: a rule name is the grammar's, whatever mode either definition stands in.
    if (spec.definitions.contains(name))
    {
        const auto message{
                std::format("rule {} redefinition; previous at line {}", name, tables_.definition_lines.at(name))};

        throw Spec_error{message, line_of(opened)};
    }

    spec.definitions.insert_or_assign(name, whole);

    tables_.definition_lines.insert_or_assign(name, line_of(opened));

    tables_.nullability.insert_or_assign(name, std::move(nullable));

    return whole;
}

void Grammar_reader::record_rule(
        Lexer_spec& spec, const std::string& mode, const std::vector<Alternative>& alternatives,
        const std::string& whole, const Commanded_rule& rule, const bool aliased)
{
    std::string pattern{};

    for (const auto& alternative : alternatives)
    {
        join_onto(pattern, alternative.pattern, " | ");
    }

    const auto& commands{alternatives.front().commands};

    const auto [token, retyped]{apply_commands(alternatives.front(), rule, *this, tables_)};

    auto conditions{mode.empty() ? std::vector<std::string>{} : std::vector{mode}};

    auto action{commands.empty() ? std::string{} : std::format("-> {}", without_trailing_blanks(commands))};

    spec.rules.push_back(
            {.pattern = std::move(pattern),
             .expression = whole,
             .conditions = std::move(conditions),
             .action = std::move(action),
             .token = token,
             .priority = std::nullopt,
             .line = rule.line});

    if (aliased || !retyped)
    {
        tables_.rule_names.insert(rule.name);
    }

    if (!tables_.sections.empty())
    {
        ++tables_.sections.back().rules;
    }
}

void Grammar_reader::parser_rule()
{
    // Through the `;` that ends the rule, actions and argument blocks stepped over as ANTLR's lexer reads them, the
    // literals kept: a quoted `]` inside an argument block closes nothing, so `r[const char* s="]'x'"]` uses no 'x'.
    for (;;)
    {
        skip_blanks();

        if (!peek())
        {
            fail("a rule never ends");
        }

        const auto byte{*peek()};

        if (byte == ';')
        {
            ++at_;

            break;
        }

        if (byte == '{')
        {
            skip_block();

            continue;
        }

        if (byte == '[')
        {
            skip_argument();

            continue;
        }

        if (byte == '<')
        {
            // Element options, `<fail='z'>` on a predicate: metadata on the element, and a string among the values is
            // no literal the parser uses.
            ++at_;

            element_options();

            continue;
        }

        if (byte == '\'')
        {
            const auto opened{at_};

            ++at_;

            std::ignore = literal();

            const std::string text{text_.substr(opened, at_ - opened)};

            const auto known{std::ranges::contains(tables_.parser_literals, text, &Occurrence::text)};

            if (!known)
            {
                tables_.parser_literals.push_back({.text = text, .line = line_of(opened)});
            }

            continue;
        }

        ++at_;
    }

    // The keywords match whole, since a rule may be named catchProduction.
    const auto handler{[this] {
        static constexpr std::array<std::string_view, 2> keywords{"catch", "finally"};

        for (const auto keyword : keywords)
        {
            const auto past{at_ + keyword.size()};

            const auto word_goes_on{past < end_ && is_name_byte(text_[past])};

            if (at(keyword) && !word_goes_on)
            {
                return true;
            }
        }

        return false;
    }};

    for (skip_blanks(); handler(); skip_blanks())
    {
        std::ignore = identifier();

        skip_blanks();

        if (peek() == '[')
        {
            skip_argument();

            skip_blanks();
        }

        skip_block();
    }
}

void Grammar_reader::skip_argument()
{
    // No comment is recognised inside an argument block, so a `]` inside what looks like one closes it.
    skip_bracketed('[', ']', false, "an argument block never closes");
}

} // namespace munch::tools::audit

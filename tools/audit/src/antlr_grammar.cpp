#include "munch/tools/audit/antlr_grammar.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
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
// Implements antlr_grammar.hpp: the test of a rule spelling a parser literal is private to this unit.

/**
 * @brief Whether a lexer rule spells a parser literal in a shape ANTLR maps the literal onto.
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

    const auto called{std::ranges::count_if(read, [](const Command& command) { return !command.argument.empty(); })};

    return read.empty() || (!acted && read.size() <= 2 && called <= 1);
}

} // namespace

Grammar_reader::Grammar_reader(const std::string_view source) : Antlr_cursor{source}
{}

Lexer_spec Grammar_reader::read()
{
    Lexer_spec spec;

    // Only a lexer grammar may declare modes, so where the rules come from decides whether a `mode` line is ANTLR.
    Item_context context{
            .lexer_only = grammar_declaration(spec),
            .case_insensitive = false,
            .mode = {},
            .members = {},
            .actions_code = {}};

    for (skip_blanks(); peek(); skip_blanks())
    {
        item(spec, context);
    }

    refuse_lexer_class(spec, context.members, context.actions_code);

    finish_grammar(tables_, context.case_insensitive, spec);

    return spec;
}

bool Grammar_reader::grammar_declaration(Lexer_spec& spec)
{
    skip_blanks();

    const auto declared{at_};

    auto keyword{identifier()};

    if (keyword == "lexer" || keyword == "parser")
    {
        skip_blanks();

        keyword = identifier() + " " + keyword;
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

    if (identifier().empty())
    {
        fail("the grammar has no name");
    }

    skip_blanks();

    expect(';', "';' to end the grammar declaration");

    spec.line = line_of(declared);

    return lexer_only;
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

    if (word == "fragment" || (word.front() >= 'A' && word.front() <= 'Z'))
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

    if (at("::"))
    {
        at_ += 2;

        skip_blanks();

        named = named + "::" + identifier();
    }

    while (peek() && *peek() != '{')
    {
        ++at_;
    }

    const auto opened{at_};

    skip_block();

    static constexpr std::string_view insertions[]{"members", "lexer::members", "declarations", "lexer::declarations"};

    context.actions_code += text_.substr(opened, at_ - opened);

    context.actions_code += '\n';

    if (std::ranges::find(insertions, named) != std::ranges::end(insertions))
    {
        context.members.push_back({.code = text_.substr(opened, at_ - opened), .line = line_of(opened)});
    }
}

std::optional<bool> Grammar_reader::options_block(std::vector<std::string>& options)
{
    skip_blanks();

    expect('{', "'{' to open the options block");

    std::optional<bool> case_insensitive;

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

        options.push_back(name + '=' + value);

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
    std::set<std::string, std::less<>> names;

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

    for (;;)
    {
        if (!peek())
        {
            fail(std::format("expected '}}' to close the {}s block", kind));
        }

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

        if (!peek())
        {
            fail(std::format("expected '}}' to close the {}s block", kind));
        }

        // The word standing where a comma or the brace should, `TWO` in `{ ONE TWO }`, or else the byte there.
        const auto next{identifier()};

        fail(std::format(
                "syntax error: '{}' stands after the {} {} where ',' or '}}' should, which ANTLR's parser rejects "
                "while matching a {}s block",
                next.empty() ? std::string{*peek()} : next, kind, name, kind));
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
    if (mode == "INITIAL")
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

    if (mode == "DEFAULT_MODE")
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

    auto rule_case_insensitive{case_insensitive};

    skip_blanks();

    const auto optioned{at("options")};

    if (optioned)
    {
        std::ignore = identifier();

        skip_blanks();

        std::vector<std::string> ignored;

        if (const auto set{options_block(ignored)})
        {
            rule_case_insensitive = *set;
        }
    }

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

    // A command ends the rule's single outermost alternative, so a rule of several alternatives carries none: one rule
    // is one token however many alternatives it has.
    const auto commanded{std::ranges::any_of(
            alternatives, [](const Alternative& alternative) { return !alternative.commands.empty(); })};

    if (alternatives.size() > 1 && commanded)
    {
        at_ = opened;

        fail(std::format(
                "a command must be the last element of the single outermost alternative of a lexer rule, so the "
                "commands on the alternatives of {} are no ANTLR grammar",
                name));
    }

    const auto line{line_of(opened)};

    std::string pattern;

    for (const auto& [text, expression, commands, clause, spelling, acted, empty, alternative_nullable] : alternatives)
    {
        pattern += (pattern.empty() ? "" : " | ") + text;
    }

    const auto& commands{alternatives.front().commands};

    const auto [token, retyped]{
            apply_commands(alternatives.front(), {.name = name, .line = line, .typed = typed}, *this, tables_)};

    spec.rules.push_back(
            {.pattern = std::move(pattern),
             .expression = whole,
             .conditions = mode.empty() ? std::vector<std::string>{} : std::vector{mode},
             .action = commands.empty() ? std::string{} : "-> " + without_trailing_blanks(commands),
             .token = token,
             .priority = std::nullopt,
             .line = line});

    if (aliased || !retyped)
    {
        tables_.rule_names.insert(name);
    }

    if (!tables_.sections.empty())
    {
        ++tables_.sections.back().rules;
    }
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
        if (name.empty() || name.front() < 'A' || name.front() > 'Z')
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

std::string Grammar_reader::define(
        Lexer_spec& spec, const std::string& name, const std::vector<Alternative>& alternatives,
        const std::size_t opened)
{
    // The whole rule is what a reference to it expands to, and it matches the empty string when one of its alternatives
    // does, which is what a closure over a reference to it needs; an empty alternative makes it optional.
    std::string whole;

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

        whole += (whole.empty() ? "" : "|") + expression;

        ++filled;
    }

    if (filled == 0)
    {
        at_ = opened;

        fail(std::format("the rule {} matches nothing but the empty string", name));
    }

    if (filled > 1 || optional)
    {
        whole = "(" + whole + ")" + (optional ? "?" : "");
    }

    // ANTLR's error 51: a rule name is the grammar's, whatever mode either definition stands in.
    if (spec.definitions.contains(name))
    {
        throw Spec_error{
                std::format("rule {} redefinition; previous at line {}", name, tables_.definition_lines.at(name)),
                line_of(opened)};
    }

    spec.definitions.insert_or_assign(name, whole);

    tables_.definition_lines.insert_or_assign(name, line_of(opened));

    tables_.nullability.insert_or_assign(name, std::move(nullable));

    return whole;
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

            const auto known{std::ranges::any_of(tables_.parser_literals, [&text](const Occurrence& seen) {
                const auto& [spelling, line]{seen};

                return spelling == text;
            })};

            if (!known)
            {
                tables_.parser_literals.push_back({.text = text, .line = line_of(opened)});
            }

            continue;
        }

        ++at_;
    }

    // Exception handlers after the rule: the keywords whole, since a rule may be named catchProduction.
    const auto handler{[this] {
        for (const std::string_view keyword : {"catch", "finally"})
        {
            if (at(keyword) && !(at_ + keyword.size() < end_ && is_name_byte(text_[at_ + keyword.size()])))
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
    const auto opened{at_};

    std::size_t depth{0};

    do
    {
        if (!peek())
        {
            at_ = opened;

            fail("an argument block never closes");
        }

        const auto byte{next("']'")};

        if (byte == '\'' || byte == '"')
        {
            skip_quoted(byte);
        }
        else if (byte == '[')
        {
            ++depth;
        }
        else if (byte == ']')
        {
            --depth;
        }
    } while (depth > 0);
}

} // namespace munch::tools::audit

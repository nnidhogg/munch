#include "munch/tools/audit/read_antlr.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace munch::tools::audit;

namespace
{
/**
 * @brief What one match attempt of a token set answers: the token, if any, and the length.
 */
using Match_t = munch::core::Lexer::Match<std::size_t>;

/**
 * @brief The ANTLR idioms a combined grammar may hold: options, named actions, parser rules whose literals become
 *        implicit tokens, fragments, the commands such a grammar can carry, several alternatives under one rule,
 *        ranges, sets, negation, the dot, a non-greedy loop, escapes and an inert action inside a rule.
 *
 * Valid ANTLR as it stands, which `antlr4 Idioms.g4` accepts with no complaint: every command ends the single outermost
 * alternative of its rule, and no mode is declared, modes being a lexer grammar's alone.
 */
constexpr std::string_view idioms{R"(grammar Idioms;

options { language = Cpp; }

@lexer::members {
    /** The parser's own count, as Spark's grammar comments it: the apostrophe is prose. */
    int nesting = 0; // braces { inside } code
}

tokens { EXTRA }

statement : 'if' expression 'then' statement | IDENT '=' expression ';' ;
expression : IDENT | NUMBER ;
catchProduction : 'if' IDENT 'then' statement ;

IF      : 'if' ;
IDENT   : LETTER (LETTER | DIGIT)* ;
NUMBER  : DIGIT+ ('.' DIGIT+)? ;
STRING  : '"' ~["\\\n]* '"' ;
COMMENT : '/*' .*? '*/' -> channel(HIDDEN) ;
LINE    : '//' ~[\r\n]* -> skip ;
TAB     : '\t' { /* the count stays in the members block */ } -> type(WS) ;
WS      : [ \r\n\f]+ -> skip ;
SIGN    : '+' | '-' ;
LOW     : 'a'..'f' ;

fragment LETTER : [a-zA-Z_] ;
fragment DIGIT  : [0-9] ;
)"};

/**
 * @brief The idioms only a lexer grammar may hold: modes as the start conditions, the mode commands, and the rules
 *        before the first `mode` line as the default mode's.
 *
 * Valid ANTLR as it stands, which `antlr4 Nested.g4` accepts with no complaint.
 */
constexpr std::string_view nested{R"(lexer grammar Nested;

OPEN    : '{' -> pushMode(INNER) ;
BLANK   : [ \t]+ -> skip ;

mode INNER;

CLOSE   : '}' -> popMode ;
WORD    : ~[}]+ ;
SIGN    : [+\-*] ;
)"};

/**
 * @brief Where a grammar is refused and in what words.
 */
struct Refusal
{
    /**
     * @brief The line the refusal names, none when the grammar is read.
     */
    std::optional<std::size_t> line{};

    /**
     * @brief The refusal's words, empty when the grammar is read.
     */
    std::string message{};
};

/**
 * @brief Returns the refusal of a grammar.
 * @param source The grammar's text.
 * @return The refusal, no line and no words when the grammar is read.
 */
Refusal refusal_at(const std::string_view source)
{
    try
    {
        std::ignore = read_antlr(source);
    }
    catch (const Spec_error& error)
    {
        return {.line = error.line(), .message = error.what()};
    }

    return {.line = std::nullopt, .message = {}};
}

/**
 * @brief Returns the line a grammar is refused at.
 * @param source The grammar's text.
 * @return The line, none when the grammar is read.
 */
std::optional<std::size_t> line_of(const std::string_view source)
{
    return refusal_at(source).line;
}

/**
 * @brief Returns what the refusal of a grammar says.
 * @param source The grammar's text.
 * @return The refusal, empty when the grammar is read.
 */
std::string refusal_of(const std::string_view source)
{
    return refusal_at(source).message;
}

/**
 * @brief Returns the one scanner a grammar declares, the first where it declares more.
 * @param grammar The grammar's text.
 * @return The scanner.
 */
Lexer_spec scanner_of(const std::string_view grammar)
{
    return read_antlr(grammar).front();
}

/**
 * @brief Returns the first rule of a grammar's scanner.
 * @param grammar The grammar's text.
 * @return The rule.
 */
Lexer_spec::Rule first_rule_of(const std::string_view grammar)
{
    return scanner_of(grammar).rules.front();
}

/**
 * @brief Returns the lengths of the successive tokens the lexer built from a grammar cuts an input into, a byte no
 *        token begins counted as a length of zero and stepped over.
 * @param grammar The grammar's text.
 * @param input The input.
 * @return The lengths.
 */
std::vector<std::size_t> lengths_of(const std::string_view grammar, const std::string_view input)
{
    const auto scanners{read_antlr(grammar)};

    const auto lexer{build(scanners.front(), "INITIAL")};

    std::vector<std::size_t> lengths{};

    for (std::size_t at{0}; at < input.size();)
    {
        const auto from{input.begin() + static_cast<std::ptrdiff_t>(at)};

        const auto [token, length]{lexer.tokenize<std::size_t>(from, input.end())};

        lengths.push_back(length);

        at += std::max<std::size_t>(length, 1);
    }

    return lengths;
}

/**
 * @brief Returns the token a lexer's longest match at the start of an input emits.
 * @param lexer The lexer.
 * @param input The input.
 * @return The token, none where no token matches.
 */
std::optional<std::size_t> token_at(const munch::core::Lexer& lexer, const std::string_view input)
{
    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    return token;
}

/**
 * @brief Returns how long a lexer's longest match at the start of an input is.
 * @param lexer The lexer.
 * @param input The input.
 * @return The length, zero where no token matches.
 */
std::size_t length_at(const munch::core::Lexer& lexer, const std::string_view input)
{
    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    return length;
}

} // namespace

TEST(Read_antlr_test, Reads_a_combined_grammar_with_its_idioms)
{
    const auto spec{scanner_of(idioms)};

    EXPECT_EQ(spec.line, 1U);
    EXPECT_EQ(spec.options, (std::vector<std::string>{"language=Cpp"}));

    // The parser's literals lead, except 'if', which IF spells; then the lexer rules in order, one rule each. The
    // parser rule named catchProduction is a rule, not an exception handler.
    ASSERT_EQ(spec.rules.size(), 13U);

    EXPECT_EQ(spec.rules[0].pattern, "'then'");
    EXPECT_EQ(spec.rules[0].expression, R"("then")");
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"'then'"});
    EXPECT_EQ(spec.rules[0].line, 12U);
    EXPECT_EQ(spec.rules[1].pattern, "'='");
    EXPECT_EQ(spec.rules[2].pattern, "';'");

    EXPECT_EQ(spec.rules[3].token, std::optional<std::string>{"IF"});
    EXPECT_EQ(spec.rules[3].expression, R"("if")");
    EXPECT_EQ(spec.rules[3].line, 16U);

    EXPECT_EQ(spec.rules[4].pattern, "LETTER (LETTER | DIGIT)*");
    EXPECT_EQ(spec.rules[4].expression, "{LETTER}({LETTER}|{DIGIT})*");
    EXPECT_EQ(spec.rules[5].expression, R"({DIGIT}+("."{DIGIT}+)?)");

    // A negated set admits every other scalar, written as the code point ranges the parser reads.
    EXPECT_EQ(
            spec.rules[6].expression,
            R"("\""[\u{0}-\u{9}\u{b}-\u{21}\u{23}-\u{5b}\u{5d}-\u{7f}\u{80}-\u{10ffff}]*"\"")");

    // The block comment: the loop over what holds no star-slash, then the closer.
    EXPECT_EQ(spec.rules[7].token, std::nullopt);
    EXPECT_EQ(spec.rules[7].action, "-> channel(HIDDEN)");
    EXPECT_TRUE(spec.rules[7].expression.starts_with(R"("/*"(()"));
    EXPECT_TRUE(spec.rules[7].expression.ends_with(R"("*/")"));

    EXPECT_EQ(spec.rules[8].token, std::nullopt);
    EXPECT_EQ(spec.rules[8].action, "-> skip");

    // An inert action inside the rule, a comment in braces, is skipped; type() renames the token.
    EXPECT_EQ(spec.rules[9].expression, R"("\t")");
    EXPECT_EQ(spec.rules[9].token, std::optional<std::string>{"WS"});

    EXPECT_EQ(spec.rules[10].pattern, R"([ \r\n\f]+)");
    EXPECT_EQ(spec.rules[10].token, std::nullopt);

    // A rule of several alternatives is one rule, its pattern the alternatives as written and its expression their
    // choice, since a command could not sit on one of them.
    EXPECT_EQ(spec.rules[11].pattern, "'+' | '-'");
    EXPECT_EQ(spec.rules[11].expression, R"(("+"|"-"))");
    EXPECT_EQ(spec.rules[11].token, std::optional<std::string>{"SIGN"});
    EXPECT_EQ(spec.rules[11].action, "");

    EXPECT_EQ(spec.rules[12].expression, "[a-f]");

    // A combined grammar declares no mode, so every rule is the default condition's, and the fragments are definitions
    // rather than rules.
    EXPECT_EQ(spec.rules[12].conditions, std::vector<std::string>{});
    EXPECT_TRUE(spec.conditions.empty());
    EXPECT_EQ(spec.definitions.at("LETTER"), "[A-Z_a-z]");
    EXPECT_EQ(spec.definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("IF"), R"("if")");
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
}

TEST(Read_antlr_test, A_parser_literal_is_the_token_of_the_lexer_rule_ANTLR_maps_it_onto)
{
    const auto rules_of{[](const std::string_view rule) {
        const auto grammar{std::format("grammar A;\ns : 'x' Y EOF ;\n{}\nY : 'y' ;\n", rule)};

        return scanner_of(grammar).rules;
    }};

    // antlr 4.13.2 maps the parser's 'x' onto a rule that spells it as the literal alone, whatever comments stand in
    // the rule, with one action after it, or with one or two commands of which at most one takes an argument: its token
    // file names 'x' the rule's type and its lexer skips or hides the input as the rule says.
    for (const std::string_view rule :
         {"X : 'x' -> skip ;", "X : 'x' /* comment */ -> skip ;", "X : 'x' { /* inert */ } ;", "X : 'x' { } ;",
          "X : 'x' -> channel(HIDDEN), skip ;", "X : 'x' -> skip, type(Y) ;", "/** doc */ X : 'x' -> skip ;"})
    {
        const auto rules{rules_of(rule)};

        ASSERT_EQ(rules.size(), 2U) << rule;
        EXPECT_EQ(rules.front().line, 3U) << rule;
        EXPECT_EQ(rules.front().expression, R"("x")") << rule;
    }

    const std::vector<std::pair<std::string_view, std::optional<std::string>>> mapped{
            {"X : 'x' -> skip ;", std::nullopt},
            {"X : 'x' /* comment */ -> skip ;", std::nullopt},
            {"X : 'x' { /* inert */ } ;", "X"},
            {"X : 'x' -> skip, type(Y) ;", "Y"}};

    for (const auto& [rule, token] : mapped)
    {
        const auto rules{rules_of(rule)};

        EXPECT_EQ(rules.front().token, token) << rule;
    }

    // Any other shape spelling 'x' leaves the parser's literal an implicit token of its own ahead of every rule, which
    // antlr 4.13.2 numbers T__0 and emits for the input: a rule with options, an action and a command together, two
    // commands with arguments, three commands, a group, an action before the literal and two after, and element options
    // on the literal, even empty ones, since the pattern matches the bare literal alone.
    for (const std::string_view rule :
         {"X options { caseInsensitive = false; } : 'x' -> skip ;", "X : 'x' { } -> skip ;",
          "X : 'x' -> channel(HIDDEN), type(Y) ;", "X : 'x' -> channel(HIDDEN), type(Y), skip ;", "X : ('x') -> skip ;",
          "X : { } 'x' ;", "X : 'x' { } { } ;", "fragment X : 'x' ;", "X : 'x'<> -> skip ;", "X : 'x'<a=b> ;"})
    {
        const auto rules{rules_of(rule)};

        ASSERT_GE(rules.size(), 2U) << rule;
        EXPECT_EQ(rules.front().pattern, "'x'") << rule;
        EXPECT_EQ(rules.front().token, std::optional<std::string>{"'x'"}) << rule;
        EXPECT_EQ(rules.front().line, 2U) << rule;
    }

    // The literal is matched as written, so an escape spells itself and a folded grammar's literal spells its rule's;
    // and a literal a lexer rule spells in a mode of its own is no combined grammar's concern.
    const auto escaped{scanner_of("grammar A;\ns : '\\n' Y EOF ;\nNL : '\\n' -> skip ;\nY : 'y' ;\n")};

    const auto folded{first_rule_of(
            "grammar A;\noptions { caseInsensitive = true; }\ns : 'x' Y EOF ;\nX : 'x' -> skip ;\nY : 'y' ;\n")};

    EXPECT_EQ(escaped.rules.size(), 2U);
    EXPECT_EQ(folded.token, std::nullopt);

    // Two rules spelling the parser's literal leave ANTLR no token to map it onto, its error 126 at the parser's use of
    // the literal; two rules spelling a literal no parser rule uses are its warning 184 and are read.
    const auto [doubled_line, doubled_message]{
            refusal_at("grammar A;\ns : 'x' Y EOF ;\nX : 'x' -> skip ;\nZ : 'x' ;\nY : 'y' ;\n")};

    EXPECT_EQ(doubled_line, 2);
    EXPECT_TRUE(
            doubled_message.contains("cannot create implicit token for string literal in non-combined grammar: 'x'"));
    EXPECT_EQ(line_of("grammar A;\ns : Y EOF ;\nX : 'x' -> skip ;\nZ : 'x' ;\nY : 'y' ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_parser_rule_argument_block_is_skipped_as_ANTLR_lexes_it)
{
    const auto patterns_of{[](const std::string_view rules) {
        const auto grammar{std::format("grammar P;\n{}\nA : 'a' ;\nB : 'b' ;\n", rules)};

        const auto spec{scanner_of(grammar)};

        std::vector<std::string> patterns{};

        std::ranges::transform(spec.rules, std::back_inserter(patterns), &Lexer_spec::Rule::pattern);

        return patterns;
    }};

    // antlr 4.13.2 lexes the block between a parser rule's brackets as one ARG_ACTION, in which brackets nest and a
    // "..." or a '...' is skipped whole, a backslash escaping the byte after it: a `]` or a literal inside a quoted
    // string is no part of the grammar, so `r[const char* p="]'x'"]` uses no 'x' and the input x is no token.
    EXPECT_EQ(patterns_of(R"(r[const char* p="]'x'"] : A ;)"), (std::vector<std::string>{"'a'", "'b'"}));
    EXPECT_EQ(patterns_of(R"(r[const char* p="\"]'x'"] : A ;)"), (std::vector<std::string>{"'a'", "'b'"}));
    EXPECT_EQ(patterns_of(R"(r[char p=']'] : A 'y' ;)"), (std::vector<std::string>{"'y'", "'a'", "'b'"}));
    EXPECT_EQ(patterns_of(R"(r[int a[2]] : A 'y' ;)"), (std::vector<std::string>{"'y'", "'a'", "'b'"}));
    EXPECT_EQ(patterns_of(R"(r[int a[2] = {'x'}] : A 'y' ;)"), (std::vector<std::string>{"'y'", "'a'", "'b'"}));
    EXPECT_EQ(
            patterns_of(R"(r[int x] returns [int y] locals [const char* p="]'x'"] : A 'y' ;)"),
            (std::vector<std::string>{"'y'", "'a'", "'b'"}));
    EXPECT_EQ(
            patterns_of(R"(r : e["]'x'"] 'y' ;
e[const char* p] : A ;)"),
            (std::vector<std::string>{"'y'", "'a'", "'b'"}));
    EXPECT_EQ(patterns_of("r : A ;\ncatch[Exception e] { }"), (std::vector<std::string>{"'a'", "'b'"}));
    EXPECT_EQ(
            patterns_of("r : A 'y' ;\ncatch[Exception e = \"]'x'\"] { }"),
            (std::vector<std::string>{"'y'", "'a'", "'b'"}));

    // The lexer built sees no x either.
    const auto skipped{scanner_of("grammar P;\nr[const char* p=\"]'x'\"] : A ;\nA : 'a' ;\nB : 'b' ;\n")};

    const auto lexer{build(skipped, "INITIAL")};

    EXPECT_EQ(length_at(lexer, "x"), 0U);
    EXPECT_EQ(token_at(lexer, "a"), std::optional<std::size_t>{0});

    // A block never closed is refused at the rule's line.
    EXPECT_EQ(line_of("grammar P;\nr[const char* p=\"]\" : A ;\nA : 'a' ;\n"), 2);
}

TEST(Read_antlr_test, A_quoted_literal_never_closed_in_a_block_is_refused_at_its_line)
{
    EXPECT_EQ(refusal_of("grammar G;r{'"), "line 1: a quoted literal never closes");
    EXPECT_EQ(refusal_of(R"(grammar G;r{'\)"), "line 1: a quoted literal never closes");
    EXPECT_EQ(refusal_of("grammar G;\nr : A {\n\"x ;\nA : 'a' ;\n"), "line 3: a quoted literal never closes");
    EXPECT_EQ(refusal_of("grammar G;\nr[char c = \"] : A ;\nA : 'a' ;\n"), "line 2: a quoted literal never closes");
}

TEST(Read_antlr_test, What_ANTLR_refuses_of_rule_names_and_types_is_refused_in_its_words)
{
    // Error 51: a rule name is the grammar's, whatever mode either definition stands in, fragments included.
    const auto [redefined_line, redefined_message]{refusal_at("lexer grammar D;\nX : 'a' ;\nX : 'b' ;\n")};

    EXPECT_EQ(redefined_message, "line 3: rule X redefinition; previous at line 2");
    EXPECT_EQ(redefined_line, 3);
    EXPECT_EQ(
            refusal_of("lexer grammar D;\nX : 'a' ;\nmode M;\nfragment X : 'b' ;\n"),
            "line 4: rule X redefinition; previous at line 2");

    // Error 175: a `type` names a rule that is no fragment or a `tokens` entry, before or after it, else nothing.
    const auto [unknown_line, unknown_message]{refusal_at("lexer grammar T;\nX : 'a' -> type(UNKNOWN_TOKEN) ;\n")};

    EXPECT_EQ(unknown_message, "line 2: UNKNOWN_TOKEN is not a recognized token name");
    EXPECT_EQ(unknown_line, 2);
    EXPECT_EQ(
            refusal_of("lexer grammar T;\nX : 'a' -> type(F) ;\nfragment F : 'f' ;\n"),
            "line 2: F is not a recognized token name");
    EXPECT_TRUE(refusal_of("lexer grammar T;\nX : 'a' -> type(Y) ;\nY : 'b' ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar T;\ntokens { Z }\nX : 'a' -> type(Z) ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar T;\nX : 'a' -> type(3) ;\n").empty());

    // A rule carrying a type command has no token type of its own, unless it spells one literal, so a `type` naming it
    // is error 175; a skip leaves the type, and a number in the command counts as setting it.
    EXPECT_EQ(
            refusal_of("lexer grammar V;\ntokens { BASE }\nY : [a]+ -> type(BASE) ;\nZ : 'b' -> type(Y) ;\n"),
            "line 4: Y is not a recognized token name");
    EXPECT_EQ(
            refusal_of("lexer grammar V;\nY : [a]+ -> type(7) ;\nZ : 'b' -> type(Y) ;\n"),
            "line 3: Y is not a recognized token name");
    EXPECT_EQ(
            refusal_of("lexer grammar V;\nY : 'a' 'b' -> type(W) ;\nW : 'b' -> type(Y) ;\n"),
            "line 3: Y is not a recognized token name");
    EXPECT_TRUE(
            refusal_of("lexer grammar V;\ntokens { BASE }\nY : 'a' -> type(BASE) ;\nZ : 'b' -> type(Y) ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar V;\nY : [a]+ -> skip ;\nZ : 'b' -> type(Y) ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar V;\nY : 'a' | 'b' ;\nZ : 'w' -> type(Y) ;\n").empty());

    // T__k names the k-th implicit token a combined grammar makes of the parser's literals no rule spells, in the order
    // the parser uses them, and the rule typed so emits that literal's token; past them, or in a lexer grammar, the
    // name is error 175.
    constexpr std::string_view implicit_grammar{
            "grammar I;\nr : 'a' 'b' 'c' 'a' ;\nB : 'b' ;\nX : 'x' -> type(T__1) ;\nZ : 'z' -> type(T__0) ;\n"};

    const auto implicit{scanner_of(implicit_grammar).rules};

    ASSERT_EQ(implicit.size(), 5U);
    EXPECT_EQ(implicit[0].token, std::optional<std::string>{"'a'"});
    EXPECT_EQ(implicit[1].token, std::optional<std::string>{"'c'"});
    EXPECT_EQ(implicit[3].token, std::optional<std::string>{"'c'"});
    EXPECT_EQ(implicit[4].token, std::optional<std::string>{"'a'"});
    EXPECT_EQ(
            refusal_of("grammar I;\nr : 'a' 'b' 'c' ;\nB : 'b' ;\nZ : 'z' -> type(T__2) ;\n"),
            "line 4: T__2 is not a recognized token name");
    EXPECT_EQ(refusal_of("lexer grammar L;\nX : 'x' -> type(T__0) ;\n"), "line 2: T__0 is not a recognized token name");
}

TEST(Read_antlr_test, What_ANTLR_refuses_of_modes_is_refused_in_its_words)
{
    // A mode named again reopens it, the sections' rules one mode's; a mode line before any rule, a fragment counting,
    // is ANTLR's syntax error 50.
    const auto reopened{scanner_of("lexer grammar R;\nA : 'z' ;\nmode IN;\nX : 'a' ;\nmode IN;\nY : 'b' ;\n")};

    ASSERT_EQ(reopened.conditions.size(), 1U);
    EXPECT_EQ(active_rules(reopened, "IN"), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(
            refusal_of("lexer grammar R;\nmode IN;\nX : 'a' ;\n"),
            "line 2: syntax error: 'mode' came as a complete surprise to me");
    EXPECT_EQ(
            refusal_of("lexer grammar R;\noptions { caseInsensitive = true; }\nmode IN;\nX : 'a' ;\n"),
            "line 3: syntax error: 'mode' came as a complete surprise to me");
    EXPECT_TRUE(refusal_of("lexer grammar R;\nfragment F : 'f' ;\nmode IN;\nX : 'a' ;\n").empty());

    // `mode DEFAULT_MODE;` reopens the default mode, whose rules name no condition, and each `mode` section is held to
    // error 145 on its own, a reopened mode's empty section included; a `mode` or `pushMode` naming no mode is error
    // 176, a number or DEFAULT_MODE being a mode.
    const auto reopened_default{
            scanner_of("lexer grammar T;\nX : 'a'+ ;\nB : 'b' ;\nmode DEFAULT_MODE;\nY : 'ab' ;\n")};

    EXPECT_TRUE(reopened_default.conditions.empty());
    ASSERT_EQ(reopened_default.rules.size(), 3U);
    EXPECT_TRUE(reopened_default.rules[2].conditions.empty());
    EXPECT_EQ(active_rules(reopened_default, "INITIAL"), (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_EQ(
            refusal_of("lexer grammar T;\nX : 'a' ;\nmode DEFAULT_MODE;\n"),
            "line 3: lexer mode DEFAULT_MODE must contain at least one non-fragment rule");
    EXPECT_EQ(
            refusal_of("lexer grammar T;\nX : 'a' ;\nmode IN;\nmode IN;\nY : 'b' ;\n"),
            "line 3: lexer mode IN must contain at least one non-fragment rule");
    EXPECT_EQ(
            refusal_of("lexer grammar T;\nX : 'a' -> mode(UNKNOWN_MODE) ;\n"),
            "line 2: UNKNOWN_MODE is not a recognized mode name");
    EXPECT_EQ(
            refusal_of("lexer grammar T;\nX : 'a' -> pushMode(UNKNOWN_MODE) ;\n"),
            "line 2: UNKNOWN_MODE is not a recognized mode name");
    EXPECT_TRUE(refusal_of("lexer grammar T;\nX : 'a' -> mode(1) ;\nmode IN;\nY : 'b' ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar T;\nX : 'a' -> pushMode(DEFAULT_MODE), mode(DEFAULT_MODE) ;\n").empty());
    EXPECT_EQ(
            refusal_of("grammar I;\nr : 'a' ;\nX : 'x' -> type(T__00) ;\n"),
            "line 3: T__00 is not a recognized token name");
}

TEST(Read_antlr_test, What_ANTLR_refuses_of_reserved_and_shared_names_is_refused_in_its_words)
{
    // Error 53: a lexer grammar holds no parser rule, wherever it stands. Error 159: a reserved name is no rule's.
    EXPECT_EQ(refusal_of("lexer grammar P;\nX : 'x' ;\nr : 'y' ;\n"), "line 3: parser rule r not allowed in lexer");
    EXPECT_EQ(refusal_of("lexer grammar P;\nr : 'r' ;\nX : 'x' ;\n"), "line 2: parser rule r not allowed in lexer");
    EXPECT_EQ(
            refusal_of("lexer grammar P;\nSKIP : 'a' ;\nY : 'y' ;\n"),
            "line 2: cannot declare a rule with reserved name SKIP");
    EXPECT_EQ(
            refusal_of("lexer grammar P;\nX : 'x' ;\nmode DEFAULT_MODE;\nDEFAULT_MODE : 'd' ;\n"),
            "line 4: cannot declare a rule with reserved name DEFAULT_MODE");

    // Error 51 for a combined grammar's implicit tokens: an explicit rule named T__k, k an implicit literal's number,
    // redefines it; T__1 beside one literal is a name like any other.
    EXPECT_EQ(
            refusal_of("grammar C;\nr : 'a' ;\nT__0 : 'b' ;\n"), "line 3: rule T__0 redefinition; previous at line 0");
    EXPECT_TRUE(refusal_of("grammar C;\nr : 'a' ;\nT__1 : 'b' ;\n").empty());

    // A combined grammar's `tokens` block goes to its parser: ANTLR 4.13.2 refuses a lexer `type` naming one of its
    // entries with error 175, as it refuses any name no lexer rule has.
    EXPECT_EQ(
            refusal_of("grammar G;\ntokens { B }\nr : B ;\nX : 'x' -> type(B) ;\n"),
            "line 4: B is not a recognized token name");

    // Errors 170, 161 and 162: a mode's name is no token's, a channel's no token's and no mode's, the tokens being the
    // rules with a type of their own and the `tokens` entries, anywhere; a fragment's name is nobody's.
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'x' ;\nmode X;\nY : 'y' ;\n"),
            "line 3: mode X conflicts with token with same name");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\ntokens { M }\nX : 'x' ;\nmode M;\nY : 'y' ;\n"),
            "line 4: mode M conflicts with token with same name");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'x' -> mode(M) ;\nmode M;\nY : 'y' ;\nmode Y;\nZ : 'z' ;\n"),
            "line 5: mode Y conflicts with token with same name");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nchannels { X }\nX : 'x' ;\n"),
            "line 2: channel X conflicts with token with same name");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\ntokens { T }\nchannels { T }\nY : 'y' ;\n"),
            "line 3: channel T conflicts with token with same name");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nchannels { M }\nX : 'x' ;\nmode M;\nY : 'y' ;\n"),
            "line 2: channel M conflicts with mode with same name");
    EXPECT_TRUE(refusal_of("lexer grammar N;\nX : [a]+ -> type(7) ;\nmode X;\nY : 'y' ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar N;\nfragment X : 'x' ;\nZ : X ;\nmode X;\nY : 'y' ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar N;\nchannels { C }\nfragment C : 'c' ;\nY : C ;\n").empty());

    // Errors 172 and 173: a reserved name is no channel's and no mode's; a `tokens` entry may take one, ANTLR 4.13.2
    // accepting `tokens { EOF }` and `tokens { SKIP }`, but DEFAULT_MODE among them is the default mode's name, its
    // error 170.
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nchannels { HIDDEN }\nX : 'x' ;\n"),
            "line 2: cannot use or declare channel with reserved name HIDDEN");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nchannels { DEFAULT_TOKEN_CHANNEL }\nX : 'x' ;\n"),
            "line 2: cannot use or declare channel with reserved name DEFAULT_TOKEN_CHANNEL");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'x' ;\nmode HIDDEN;\nY : 'y' ;\n"),
            "line 3: cannot use or declare mode with reserved name HIDDEN");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'x' ;\nmode EOF;\nY : 'y' ;\n"),
            "line 3: cannot use or declare mode with reserved name EOF");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\ntokens { DEFAULT_MODE }\nX : 'x' ;\n"),
            "line 2: mode DEFAULT_MODE conflicts with token with same name");
    EXPECT_TRUE(refusal_of("lexer grammar N;\ntokens { EOF }\nX : 'x' ;\n").empty());
    EXPECT_TRUE(refusal_of("lexer grammar N;\ntokens { SKIP }\nX : 'x' ;\n").empty());

    // Error 171: a `type` may not name a reserved name, declared in a `tokens` block or not, DEFAULT_MODE included
    // where no `tokens` entry makes it the default mode's conflict first.
    EXPECT_EQ(
            refusal_of("lexer grammar N;\ntokens { SKIP }\nX : 'a'+ -> type(SKIP) ;\n"),
            "line 3: cannot use or declare token with reserved name SKIP");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'a'+ -> type(HIDDEN) ;\n"),
            "line 2: cannot use or declare token with reserved name HIDDEN");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\nX : 'a'+ -> type(DEFAULT_MODE) ;\n"),
            "line 2: cannot use or declare token with reserved name DEFAULT_MODE");
    EXPECT_EQ(
            refusal_of("lexer grammar N;\ntokens { DEFAULT_MODE }\nX : 'a'+ -> type(DEFAULT_MODE) ;\n"),
            "line 2: mode DEFAULT_MODE conflicts with token with same name");
}

TEST(Read_antlr_test, A_fragment_an_inert_alternative_and_a_mode_s_rules_are_read_as_ANTLR_reads_them)
{
    // A fragment is named as a lexer rule is, capitalised, else ANTLR's parser refuses it in its words.
    EXPECT_EQ(
            refusal_of("lexer grammar A;\nfragment digit : [0-9] ;\nNUMBER : digit+ ;\n"),
            "line 2: syntax error: mismatched input 'digit' expecting TOKEN_REF while matching a lexer rule");
    EXPECT_EQ(
            refusal_of("grammar A;\nr : NUMBER ;\nfragment digit : [0-9] ;\nNUMBER : digit+ ;\n"),
            "line 3: syntax error: mismatched input 'digit' expecting TOKEN_REF while matching a lexer rule");
    EXPECT_TRUE(refusal_of("lexer grammar A;\nfragment Digit : [0-9] ;\nNUMBER : Digit+ ;\n").empty());

    // An alternative of an inert action alone is the empty alternative: `({} | 'a') 'b'` matches `b`, as ANTLR 4.13.2
    // has it, the group optional; an inert action beside an atom adds nothing.
    const std::vector<std::pair<std::string_view, std::string_view>> inert{
            {"lexer grammar A;\nX : ({} | 'a') 'b' ;\nY : . ;\n", R"(("a")?"b")"},
            {"lexer grammar A;\nX : ('a' | {}) 'b' ;\nY : . ;\n", R"(("a")?"b")"},
            {"lexer grammar A;\nX : ( | 'a') 'b' ;\nY : . ;\n", R"(("a")?"b")"},
            {"lexer grammar A;\nX : {} 'b' ;\nY : . ;\n", R"("b")"}};

    for (const auto& [grammar, definition] : inert)
    {
        const auto spec{scanner_of(grammar)};

        EXPECT_EQ(spec.definitions.at("X"), definition) << grammar;
    }

    // Error 145: a mode holds a rule that is no fragment.
    const auto [empty_line, empty_message]{refusal_at("lexer grammar E;\nX : 'a' ;\nmode IN;\n")};

    EXPECT_EQ(empty_message, "line 3: lexer mode IN must contain at least one non-fragment rule");
    EXPECT_EQ(empty_line, 3);
    EXPECT_EQ(
            refusal_of("lexer grammar E;\nX : 'a' ;\nmode IN;\nfragment F : 'f' ;\n"),
            "line 3: lexer mode IN must contain at least one non-fragment rule");
    EXPECT_TRUE(refusal_of("lexer grammar E;\nX : 'a' ;\nmode IN;\nY : 'b' ;\n").empty());

    // A mode named INITIAL, which ANTLR allows, would be reported under the default mode's name, so it is refused.
    const auto [initial_line, initial_message]{
            refusal_at("lexer grammar M;\nX : 'a' -> mode(INITIAL) ;\nmode INITIAL;\nY : 'b' ;\n")};

    EXPECT_TRUE(initial_message.contains("a mode named INITIAL is refused"));
    EXPECT_EQ(initial_line, 3);
}

TEST(Read_antlr_test, Reads_a_lexer_grammar_with_its_modes)
{
    const auto spec{scanner_of(nested)};

    EXPECT_EQ(spec.line, 1U);
    ASSERT_EQ(spec.rules.size(), 5U);

    // The rules before the first `mode` line are the default mode's, the ones after it the mode's, and the mode
    // commands are kept as the action's text.
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"OPEN"});
    EXPECT_EQ(spec.rules[0].action, "-> pushMode(INNER)");
    EXPECT_EQ(spec.rules[0].conditions, std::vector<std::string>{});
    EXPECT_EQ(spec.rules[1].token, std::nullopt);
    EXPECT_EQ(spec.rules[2].line, 8U);
    EXPECT_EQ(spec.rules[2].action, "-> popMode");
    EXPECT_EQ(spec.rules[2].conditions, (std::vector<std::string>{"INNER"}));
    EXPECT_EQ(spec.rules[3].expression, R"([\u{0}-\u{7c}\u{7e}-\u{7f}\u{80}-\u{10ffff}]+)");
    EXPECT_EQ(spec.rules[4].expression, R"([*+\-])");

    ASSERT_EQ(spec.conditions.size(), 1U);
    EXPECT_EQ(spec.conditions.front().name, "INNER");
    EXPECT_TRUE(spec.conditions.front().exclusive);
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(active_rules(spec, "INNER"), (std::vector<std::size_t>{2, 3, 4}));
}

TEST(Read_antlr_test, Case_insensitivity_doubles_every_letter)
{
    const auto spec{
            scanner_of("lexer grammar Ci;\noptions { caseInsensitive = true; }\nKW : 'if' ;\nID : [a-z]+ ;\n"
                       "EXACT options { caseInsensitive = false; } : 'x' ;\n")};

    ASSERT_EQ(spec.rules.size(), 3U);
    EXPECT_EQ(spec.rules[0].expression, "[iI][fF]");
    EXPECT_EQ(spec.rules[1].expression, "[A-Za-z]+");
    EXPECT_EQ(spec.rules[2].expression, R"("x")");

    // The dot admits every scalar in either case already, so folding it asks for no mapping and it is read.
    const auto every{scanner_of("lexer grammar D;\noptions { caseInsensitive = true; }\nX : . ;\n")};

    EXPECT_EQ(every.rules.front().expression, R"([\u{0}-\u{7f}\u{80}-\u{10ffff}])");
}

TEST(Read_antlr_test, A_range_under_case_insensitivity_folds_by_its_ends_as_ANTLR_folds_it)
{
    const auto expression_of{[](const std::string_view atom) {
        const auto grammar{std::format("lexer grammar R;\noptions {{ caseInsensitive = true; }}\nA : {} ;\n", atom)};

        return first_rule_of(grammar).expression;
    }};

    // antlr 4.13.2 folds a range by its two ends alone: where both are letters of one case the copy in the other case
    // is added, and where they differ in case or are no letters the range stands as written, its warning 185 remarking
    // on the first two. So `[b-y]` and `'b'..'y'` take B and Y and not A, `[A-t]` and `'A'..'t'` take U and a and not
    // u, `[0-Z]` takes A and not a, `[a-]` takes a and not A, and a member folds on its own.
    EXPECT_EQ(expression_of("[b-y]"), "[B-Yb-y]");
    EXPECT_EQ(expression_of("'b'..'y'"), "[B-Yb-y]");
    EXPECT_EQ(expression_of("[B-Y]"), "[B-Yb-y]");
    EXPECT_EQ(expression_of("[A-t]"), "[A-t]");
    EXPECT_EQ(expression_of("'A'..'t'"), "[A-t]");
    EXPECT_EQ(expression_of("[0-Z]"), "[0-Z]");
    EXPECT_EQ(expression_of("[0-z]"), "[0-z]");
    EXPECT_EQ(expression_of("[ -Z]"), "[ -Z]");
    EXPECT_EQ(expression_of("[A-a]"), "[A-a]");
    EXPECT_EQ(expression_of("[!-a]"), "[!-a]");
    EXPECT_EQ(expression_of(R"([\u0061-\u007f])"), R"([a-\x7f])");
    EXPECT_EQ(expression_of("[xA-t9]"), "[9A-tx]");
    EXPECT_EQ(expression_of("[q-q]"), "[Qq]");
    EXPECT_EQ(expression_of("'q'"), "[qQ]");
    EXPECT_EQ(expression_of("[0-9]"), "[0-9]");

    // A negation negates the set as ANTLR folded it: `~[A-t]` admits u and not U, `~[b-y]` admits neither B nor b.
    EXPECT_EQ(expression_of("~[A-t]"), R"([\u{0}-\u{40}\u{75}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(expression_of("~[b-y]"), R"([\u{0}-\u{41}\u{5a}-\u{61}\u{7a}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(expression_of("~('A'..'t' | 'x')"), R"([\u{0}-\u{40}\u{75}-\u{77}\u{79}-\u{7f}\u{80}-\u{10ffff}])");

    // Built, `[A-t]` cuts "uUa" into a byte for B and two for A, as antlr 4.13.2 does, and so does the rule's own
    // option.
    constexpr std::string_view mixed{"lexer grammar R;\noptions { caseInsensitive = true; }\nA : [A-t] ;\nB : . ;\n"};

    const auto mixed_scanner{scanner_of(mixed)};

    const auto lexer{build(mixed_scanner, "INITIAL")};

    EXPECT_EQ(token_at(lexer, "u"), std::optional<std::size_t>{1});
    EXPECT_EQ(token_at(lexer, "U"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(lexer, "a"), std::optional<std::size_t>{0});

    const auto own_option{first_rule_of("lexer grammar R;\nA options { caseInsensitive = true; } : [A-t] ;\n")};

    EXPECT_EQ(own_option.expression, "[A-t]");
}

TEST(Read_antlr_test, An_option_is_read_as_its_one_token_whatever_comments_stand_beside_it)
{
    // antlr 4.13.2 reads an option's value with the lexer it reads the grammar with, so a comment after the value is no
    // part of it: each of these folds "AB" onto 'ab', at the grammar and on the rule alike.
    const auto folded{
            scanner_of("grammar O;\noptions { caseInsensitive = true /* match either case */; }\n"
                       "s : 'ab' ;\nB : . ;\n")};

    EXPECT_EQ(folded.options, (std::vector<std::string>{"caseInsensitive=true"}));
    EXPECT_EQ(folded.rules.front().expression, "[aA][bB]");

    const auto on_the_rule{
            scanner_of("lexer grammar O;\nA options { caseInsensitive = true /* match either case */; } : 'ab' ;\n")};

    EXPECT_EQ(on_the_rule.rules.front().expression, "[aA][bB]");

    // A name, a dotted name, a quoted string and a brace block are the values ANTLR's own lexer reads, each one token,
    // and the blanks and comments around the `=` are no part of any.
    const auto forms{
            scanner_of("lexer grammar O;\n"
                       "options { language = Cpp; TokenLabelType = a.b.C; tokenVocab = 'Lex'; contextSuperClass = "
                       "{x::Y}; caseInsensitive /* here */ = /* there */ true; }\nA : 'a' ;\n")};

    const std::vector<std::string> recorded{
            "language=Cpp", "TokenLabelType=a.b.C", "tokenVocab='Lex'", "contextSuperClass={x::Y}",
            "caseInsensitive=true"};

    EXPECT_EQ(forms.options, recorded);
    EXPECT_EQ(forms.rules.front().expression, "[aA]");

    // ANTLR takes `true` and `false` and no other spelling: `TRUE` and `untrue` are its warning 84 and set nothing, so
    // the grammar's option stays off and a rule's option leaves the grammar's in force.
    const auto expression_of{[](const std::string_view options) {
        const auto grammar{std::format("lexer grammar O;\n{}\nA : 'a' ;\n", options)};

        return first_rule_of(grammar).expression;
    }};

    EXPECT_EQ(expression_of("options { caseInsensitive = TRUE; }"), R"("a")");
    EXPECT_EQ(expression_of("options { caseInsensitive = untrue; }"), R"("a")");
    EXPECT_EQ(expression_of("options { caseInsensitive = true; }"), "[aA]");

    const auto rule_expression_of{[](const std::string_view options) {
        const auto grammar{
                std::format("lexer grammar O;\noptions {{ caseInsensitive = true; }}\nA {} : 'a' ;\n", options)};

        return first_rule_of(grammar).expression;
    }};

    EXPECT_EQ(rule_expression_of("options { caseInsensitive = untrue; }"), "[aA]");
    EXPECT_EQ(rule_expression_of("options { caseInsensitive = TRUE; }"), "[aA]");
    EXPECT_EQ(rule_expression_of("options { caseInsensitive = false; }"), R"("a")");

    EXPECT_EQ(line_of("lexer grammar O;\noptions { caseInsensitive = ; }\nA : 'a' ;\n"), 2);
}

TEST(Read_antlr_test, Sets_beyond_ascii_and_negated_groups_read_as_scalars)
{
    // A negated group may hold a range with blanks around its operator, as Clojure's grammar writes one.
    const auto spec{
            scanner_of("lexer grammar U;\nLATIN : [a-z\\u00C0-\\u00FF]+ ;\nSPAN : '\\u0300'..'\\u036F' ;\n"
                       "REST : ~('\\r' | '\\n' | [ \\t]) ;\nBOM : '\\uFEFF' ;\n"
                       "HEAD : ~('0' .. '9' | '^' | '\\u00C0'..'\\u00FF') ;\n")};

    ASSERT_EQ(spec.rules.size(), 5U);
    EXPECT_EQ(spec.rules[0].expression, R"([\u{61}-\u{7a}\u{c0}-\u{ff}]+)");
    EXPECT_EQ(spec.rules[1].expression, R"([\u{300}-\u{36f}])");
    EXPECT_EQ(spec.rules[2].expression, R"([\u{0}-\u{8}\u{b}-\u{c}\u{e}-\u{1f}\u{21}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(spec.rules[3].expression, R"("\xef\xbb\xbf")");
    EXPECT_EQ(spec.rules[4].expression, R"([\u{0}-\u{2f}\u{3a}-\u{5d}\u{5f}-\u{7f}\u{80}-\u{bf}\u{100}-\u{10ffff}])");

    // Built, the scalars match as their encodings.
    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(length_at(lexer, "caf\xc3\xa9"), 5U);
    EXPECT_EQ(token_at(lexer, "\xcc\x81"), std::optional<std::size_t>{1});
    EXPECT_EQ(token_at(lexer, "\xe2\x82\xac"), std::optional<std::size_t>{2});
}

TEST(Read_antlr_test, A_surrogate_pair_is_the_character_it_encodes_and_a_surrogate_alone_never_matches)
{
    const auto grammar_of{
            [](const std::string_view body) { return std::format("lexer grammar S;\nA : {} ;\nB : . ;\n", body); }};

    const auto second_grammar_of{
            [](const std::string_view body) { return std::format("lexer grammar S;\nA : 'q' ;\nB : {} ;\n", body); }};

    const auto scanner_with{[&grammar_of](const std::string_view body) {
        const auto grammar{grammar_of(body)};

        return scanner_of(grammar);
    }};

    const auto expression_of{
            [&scanner_with](const std::string_view body) { return scanner_with(body).rules.front().expression; }};

    const auto refusal{[&grammar_of](const std::string_view body) {
        const auto grammar{grammar_of(body)};

        return refusal_of(grammar);
    }};

    // antlr 4.13.2 reads a literal into a UTF-16 string and walks it by code point, so a high surrogate escape and a
    // low one after it are the character the pair encodes: `'\uD83D\uDE00'` matches the four bytes of U+1F600, as
    // `'\u{1F600}'` and the character written out do, and not the six bytes of the two surrogates encoded apart.
    EXPECT_EQ(expression_of(R"('\uD83D\uDE00')"), R"("\xf0\x9f\x98\x80")");
    EXPECT_EQ(expression_of(R"('\u{1F600}')"), R"("\xf0\x9f\x98\x80")");
    EXPECT_EQ(expression_of("'\xf0\x9f\x98\x80'"), R"("\xf0\x9f\x98\x80")");
    EXPECT_EQ(expression_of(R"('a\uD83D\uDE00b')"), R"("a\xf0\x9f\x98\x80b")");

    const auto pair_scanner{scanner_with(R"('\uD83D\uDE00')")};

    const auto pair{build(pair_scanner, "INITIAL")};

    EXPECT_EQ(
            pair.tokenize<std::size_t>(std::string_view{"\xf0\x9f\x98\x80"}),
            (Match_t{.token = std::optional<std::size_t>{0}, .length = 4U}));
    EXPECT_EQ(length_at(pair, "\xed\xa0\xbd\xed\xb8\x80"), 0U);

    // A surrogate standing alone is a transition on a code point no UTF-8 input decodes to, which antlr 4.13.2 accepts
    // and never matches, the emoji and the letter after it going to B; the byte reading has no literal that never
    // matches, so each is refused at its line: a high one alone, a low one alone, the pair reversed, a pair and then a
    // high one, and two highs before the low.
    for (const std::string_view body :
         {R"('\uD83D')", R"('\uDE00')", R"('\uDE00\uD83D')", R"('\uD83Dx')", R"('\uD83D\uDE00\uD83D')",
          R"('\uD83D\uD83D\uDE00')", R"('\u{D83D}')", R"(~'\uD83D')", R"('\uD83D'..'\uDE00')"})
    {
        const auto grammar{second_grammar_of(body)};

        EXPECT_EQ(line_of(grammar), 3) << body;

        const auto refused{refusal(body)};

        EXPECT_TRUE(refused.contains("lone surrogate")) << body << ": " << refused;
    }

    // In a set the escapes stay apart, so `[\uD83D\uDE00]` holds two surrogates and not the emoji: a member that is one
    // matches nothing and is left out, as antlr 4.13.2 matches x and not the emoji for `[\uD83Dx]`, a set of nothing
    // else is refused, a span past them keeps what lies on either side, U+D7FF and U+E000 matching and the emoji not,
    // and a negation admits everything else, every character for `~[\uD83D]`.
    EXPECT_EQ(expression_of(R"([\uD83Dx])"), "[x]");
    EXPECT_EQ(expression_of(R"([\uD7FF-\uE000])"), R"([\u{d7ff}-\u{e000}])");
    EXPECT_EQ(expression_of(R"(~[\uD83D])"), R"([\u{0}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(expression_of("[\xf0\x9f\x98\x80]"), R"([\u{1f600}])");

    const auto span_scanner{scanner_with(R"([\uD7FF-\uE000])")};

    const auto span{build(span_scanner, "INITIAL")};

    EXPECT_EQ(token_at(span, "\xed\x9f\xbf"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(span, "\xee\x80\x80"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(span, "\xf0\x9f\x98\x80"), std::optional<std::size_t>{1});

    for (const std::string_view body : {R"([\uD83D\uDE00])", R"([\uD83D-\uDE00])", R"([\uD83D])"})
    {
        const auto grammar{second_grammar_of(body)};

        EXPECT_EQ(line_of(grammar), 3) << body;

        const auto refused{refusal(body)};

        EXPECT_TRUE(refused.contains("holds nothing but surrogates")) << body << ": " << refused;
    }

    EXPECT_EQ(refusal("[]"), "line 2: string literals and sets cannot be empty: []");

    const auto folded{
            first_rule_of("lexer grammar S;\noptions { caseInsensitive = true; }\nA : [\\uD83D\\uDE00a] ;\n")};

    EXPECT_EQ(folded.expression, "[Aa]");

    // Where antlr 4.13.2 needs one character, a range's end or a negated literal, it reads the literal's text and takes
    // one UTF-16 unit or one escape as one character: a pair of escapes, a character beyond the basic plane written
    // out, two characters and no character are its error 144, in its words, where `'\u{1F600}'` is one.
    EXPECT_EQ(
            refusal(R"(~'\uD83D\uDE00')"),
            R"(line 2: multi-character literals are not allowed in lexer sets: '\uD83D\uDE00')");
    EXPECT_EQ(
            refusal(R"('\uD83D\uDE00'..'\uD83D\uDE01')"),
            R"(line 2: multi-character literals are not allowed in lexer sets: '\uD83D\uDE00')");
    EXPECT_EQ(
            refusal(R"('a'..'\uD83D\uDE01')"),
            R"(line 2: multi-character literals are not allowed in lexer sets: '\uD83D\uDE01')");
    EXPECT_EQ(
            refusal("~'\xf0\x9f\x98\x80'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\xf0\x9f\x98\x80'");
    EXPECT_EQ(
            refusal("'\xf0\x9f\x98\x80'..'\xf0\x9f\x98\x81'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\xf0\x9f\x98\x80'");
    EXPECT_EQ(refusal("~('x' | 'ab')"), "line 2: multi-character literals are not allowed in lexer sets: 'ab'");
    EXPECT_EQ(refusal("''..'z'"), "line 2: multi-character literals are not allowed in lexer sets: ''");
    EXPECT_EQ(expression_of(R"('\u{1F600}'..'\u{1F601}')"), R"([\u{1f600}-\u{1f601}])");
    EXPECT_EQ(expression_of(R"(~'\u{1F600}')"), R"([\u{0}-\u{7f}\u{80}-\u{1f5ff}\u{1f601}-\u{10ffff}])");

    // A range whose end is below its start is its error 174, in its words.
    EXPECT_EQ(refusal("'z'..'a'"), "line 2: string literals and sets cannot be empty: 'z'..'a'");
    EXPECT_EQ(refusal("~('z'..'a')"), "line 2: string literals and sets cannot be empty: 'z'..'a'");
}

TEST(Read_antlr_test, A_non_greedy_loop_stops_where_the_rest_of_the_rule_first_matches)
{
    // ANTLR's rule is the fewest loop characters that still let the rest of the rule match, so the body neither runs
    // over an occurrence of the rest nor ends where the rest's own bytes complete one: `.*? 'aa'` on "aaa" matches "aa"
    // and leaves the third byte to B.
    const auto overlapping{scanner_of("lexer grammar N;\nA : .*? 'aa' ;\nB : 'a' ;\n")};

    const auto overlapping_lexer{build(overlapping, "INITIAL")};

    const auto [stopped_token, stopped_length]{overlapping_lexer.tokenize<std::size_t>(std::string_view{"aaa"})};

    EXPECT_EQ(stopped_length, 2U);
    EXPECT_EQ(stopped_token, std::optional<std::size_t>{0});

    // What the loop stops at is the whole rest of the rule, not the element after it: `.*? 'a' 'b'` stops at "ab", so
    // "aab" is one token of A rather than a byte of B and then "ab".
    const auto rest{scanner_of("lexer grammar N;\nA : .*? 'a' 'b' ;\nB : . ;\n")};

    const auto rest_lexer{build(rest, "INITIAL")};

    const auto [whole_token, whole_length]{rest_lexer.tokenize<std::size_t>(std::string_view{"aab"})};

    EXPECT_EQ(whole_length, 3U);
    EXPECT_EQ(whole_token, std::optional<std::size_t>{0});

    // A `+?` loop has one character before the rest can stop it, so "aaa" is one token of A; from the second character
    // on the overlap holds, so "aaaa" is A("aaa") and a byte left over rather than one token of four.
    const auto least_scanner{scanner_of("lexer grammar N;\nA : .+? 'aa' ;\nB : 'a' ;\n")};

    const auto least{build(least_scanner, "INITIAL")};

    EXPECT_EQ(length_at(least, "aaa"), 3U);
    EXPECT_EQ(length_at(least, "aaaa"), 3U);

    // Where the loop admits only what the terminator begins with, the body can be nothing but empty and the terminator
    // alone is the match, as ANTLR's own lexer has it: "aaa" is A("aa") and a byte left over.
    const auto degenerate{scanner_of("lexer grammar N;\nA : 'a'*? 'aa' ;\nB : . ;\n")};

    EXPECT_EQ(degenerate.rules.front().expression, R"("aa")");

    const auto degenerate_lexer{build(degenerate, "INITIAL")};

    EXPECT_EQ(length_at(degenerate_lexer, "aaa"), 2U);

    const auto once{scanner_of("lexer grammar N;\nA : 'a'+? 'a' ;\nB : . ;\n")};

    const auto once_lexer{build(once, "INITIAL")};

    EXPECT_EQ(length_at(once_lexer, "aaa"), 2U);

    // A terminator that overlaps no prefix of itself leaves the block comment as it was, the loop over what holds no
    // whole "*/".
    const auto comment{scanner_of("lexer grammar N;\nC : '/*' .*? '*/' ;\n")};

    const auto comment_lexer{build(comment, "INITIAL")};

    EXPECT_EQ(length_at(comment_lexer, "/* a */ /* b */"), 7U);

    // A literal of several characters aligns the loop to its length, and the rest cannot begin with its first byte, so
    // the greedy loop is the same language: antlr 4.13.2 reads all of "ababc" as A, as this does.
    const auto aligned{scanner_of("lexer grammar N;\nA : 'ab'*? 'c' ;\nB : . ;\n")};

    EXPECT_EQ(aligned.rules.front().expression, R"(("ab")*"c")");

    const auto aligned_lexer{build(aligned, "INITIAL")};

    EXPECT_EQ(length_at(aligned_lexer, "ababc"), 5U);

    // A folded literal's matches have its length too, so `caseInsensitive` changes nothing here: antlr 4.13.2 takes all
    // of "aBab/" as A, and so does this.
    const auto folded{scanner_of("lexer grammar N;\noptions { caseInsensitive = true; }\nA : 'ab'*? '/' ;\nB : . ;\n")};

    EXPECT_EQ(folded.rules.front().expression, R"(([aA][bB])*"/")");

    const auto folded_lexer{build(folded, "INITIAL")};

    EXPECT_EQ(length_at(folded_lexer, "aBab/"), 5U);
}

TEST(Read_antlr_test, A_non_greedy_loop_is_read_where_no_earlier_path_through_the_rule_can_stop_it)
{
    // antlr 4.13.2 follows the paths through the elements before the loop in the order their alternatives give them,
    // and the first to reach the rule's end stops the loop on every later one: `('a'|'aa') .*? 'a'` cuts "aaa" into
    // "aa" and "a", `('aa'|'a') .*? 'a'` takes all three, and `('a'|'aaa') .*? 'a'` takes all of "aaaa", which no
    // greedy rewrite of the group keeps apart, so a loop after elements of more than one length is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aa') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('aa'|'a') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aaa') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a'? .*? 'b' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|) .*? 'b' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F .*? 'b' ;\nY : . ;\nfragment F : 'a' ;\n"), 2);

    // Elements of one length reach the loop at one character together, whatever their order, and the loop is read:
    // antlr 4.13.2 takes all of "acxb", "abb" and "acb" as X, as this does.
    constexpr std::string_view aligned{"lexer grammar A;\nX : ('ab'|'ac') .*? 'b' ;\nY : . ;\n"};

    EXPECT_EQ(lengths_of(aligned, "acxb"), (std::vector<std::size_t>{4}));
    EXPECT_EQ(lengths_of(aligned, "abb"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(aligned, "acb"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : [ab] '\\u00E9' { } .*? 'b' ;\nY : . ;\n"), std::nullopt);

    // An earlier alternative of the rule is an earlier path too: `'ab' | 'a' .*? 'c'` cuts "abc" into "ab" and "c", the
    // first alternative's end stopping the second's loop, where `'a' .*? 'c' | 'ab'` takes all three; so the loop is
    // read where no character begins both an earlier alternative and its own.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'ab' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F | 'a' .*? 'c' ;\nY : . ;\nfragment F : 'x' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : '\\u00E9' | '\\u00FC' .*? 'c' ;\nY : . ;\n"), 2);

    // What an earlier alternative can begin with reaches past every element of it that can match the empty string, a
    // group with an empty alternative among them: antlr 4.13.2 cuts "abc" into "a" and two bytes for each of these, the
    // first alternative's 'a' ending the rule before the loop, so each is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : (('b'|)) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'?) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'*) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : { } ('b'|) ('d'|) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'a' | 'a' 'x'?? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'x' | 'a' .*? 'c' ;\nY : . ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F 'x' | 'a' .*? 'c' ;\nY : . ;\nfragment F : 'b' | ;\n"), 2);

    constexpr std::string_view leading{"lexer grammar A;\nX : 'a' .*? 'c' | 'ab' ;\nY : . ;\n"};

    EXPECT_EQ(lengths_of(leading, "abc"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(leading, "abbc"), (std::vector<std::size_t>{4}));
    EXPECT_EQ(lengths_of(leading, "ac"), (std::vector<std::size_t>{2}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | 'a' .*? 'b' ;\nY : . ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | '\\u00FC' .*? 'c' ;\nY : . ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_non_greedy_option_is_read_over_a_body_of_one_length_after_elements_of_one_length)
{
    // antlr 4.13.2 takes the body's alternatives in order after the bypass, and the first to reach the rule's end stops
    // the others: `('x'|'xa')?? 'a'` cuts "xaa" into "xa" and "a" where `('xa'|'x')?? 'a'` takes all three, and the
    // greedy option over either group takes all three; so a body of more than one length is refused, as is one an
    // earlier path can stop, `('a'|'aa') ('x')?? 'a'` on "aaxa" ending at the second character.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'xa')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('xa'|'x')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aa') ('x')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F?? 'a' ;\nY : . ;\nfragment F : 'x' ;\n"), 2);

    // A body of one length reaches the rest at one character however its alternatives stand, and the bypass never sees
    // the body's first character, so the greedy option is the same language: antlr 4.13.2 takes all of "xaa" and "xba",
    // "a" alone, and cuts "xa" into "x" for Y and "a" for X, as this does.
    constexpr std::string_view fixed{"lexer grammar A;\nX : ('xa'|'xb')?? 'a' ;\nY : . ;\n"};

    EXPECT_EQ(first_rule_of(fixed).expression, R"(("xa"|"xb")?"a")");
    EXPECT_EQ(lengths_of(fixed, "xaa"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(fixed, "xba"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(fixed, "a"), (std::vector<std::size_t>{1}));
    EXPECT_EQ(lengths_of(fixed, "xa"), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x'?? 'a' ;\nY : . ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [xy]?? 'a' ;\nY : . ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_non_greedy_loop_over_a_group_is_refused_rather_than_read_as_the_greedy_one)
{
    // ANTLR stops the loop at the fewest characters that let the rest match and takes the body's alternatives in order,
    // so on "xaa" the first of these matches "xa" and the second all three, where a greedy rewrite over the group reads
    // all three either way. The order of the alternatives is the whole difference, which the rewriting cannot see, so a
    // group body is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'xa')*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('xa'|'x')*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'yz')*? 'a' ;\nY : . ;\n"), 2);

    // A group of one alternative goes the same way: what the loop admits is read off the atom, and a group's matches
    // are not one length whatever stands inside it.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x')+? 'a' ;\nY : . ;\n"), 2);

    // A body of one character, the dot and a set among them, is read.
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a' ;\nY : . ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [ab]*? 'c' ;\nY : . ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a'..'f'*? 'g' ;\nY : . ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_closure_over_a_body_that_can_match_the_empty_string_is_refused_as_ANTLR_rejects_it)
{
    // antlr 4.13.2 rejects each of these with its error 153, "rule A contains a closure with at least one alternative
    // that can match an empty string", at the line of the rule holding the closure: the empty alternative, the nullable
    // group and the nullable rule alike, greedy or not.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )*? 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )* 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )+ 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )+? 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'?)* 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'*)* 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'? 'c'?)* 'd' ;\n"), 2);

    const auto closure{refusal_of("lexer grammar A;\nX : ('b' | )* 'a' ;\n")};

    EXPECT_TRUE(closure.contains("the rule X contains a closure")) << closure;

    // A body that reaches a rule waits for the whole grammar, the rule being able to stand below the closure, and the
    // answer runs through as many rules as reach it.
    EXPECT_EQ(line_of("lexer grammar A;\nfragment E : 'x'? ;\nX : E* 'b' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : 'x'? ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E+ 'b' ;\nfragment E : 'x' | ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : F ;\nfragment F : 'x'? ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : B* 'y' ;\nB : 'x'? ;\n"), 2);

    // What ANTLR accepts is read: an optional over an empty alternative, a closure over a rule that matches no empty
    // string, and the closures of the study's own grammars.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )? 'a' ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : 'x' ;\n"), std::nullopt);
    EXPECT_EQ(
            line_of("lexer grammar A;\nX : L (L | D)* ;\nfragment L : [a-z] ;\nfragment D : [0-9] ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : D+ ('.' D+)? ;\nfragment D : [0-9] ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_command_is_read_as_its_name_whatever_comments_stand_beside_it)
{
    const auto rule_of{[](const std::string_view clause) {
        const auto grammar{std::format("lexer grammar A;\nX : 'a' {}\nY : 'b' ;\nB : 'c' ;\n", clause)};

        return first_rule_of(grammar);
    }};

    // antlr 4.13.2 skips the token in each of these, the comment being no part of the command: a block comment, one
    // holding the `;` that ends the rule, and a line comment before the `;` on the next line.
    for (const std::string_view clause :
         {"-> skip /* comment */ ;", "-> skip /* ; */ ;", "-> skip // to the line end\n;"})
    {
        const auto rule{rule_of(clause)};

        EXPECT_EQ(rule.token, std::nullopt) << clause;
    }

    // The clause is kept as written, comments and all, since it is the account to hold against the grammar.
    const auto commented{rule_of("-> skip /* comment */ ;")};

    EXPECT_EQ(commented.action, "-> skip /* comment */");

    // A type command names its token whatever stands around it, and `-> more` is refused however it is spelled.
    for (const std::string_view clause : {"-> type ( B ) ;", "-> type(/* the parser's own */ B) ;"})
    {
        const auto rule{rule_of(clause)};

        EXPECT_EQ(rule.token, std::optional<std::string>{"B"}) << clause;
    }

    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more /* combine prefix */ ;\nY : 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more, pushMode(M) ;\nY : 'b' ;\nmode M;\nZ : 'c' ;\n"), 2);

    // The mode commands stay what they were, kept as the action's text.
    const auto pushing{rule_of("-> pushMode(M) ;\nmode M;\nZ : 'c' ;")};

    EXPECT_EQ(pushing.action, "-> pushMode(M)");
}

TEST(Read_antlr_test, An_escape_ANTLR_has_not_got_is_refused_in_its_words)
{
    const auto grammar_of{
            [](const std::string_view body) { return std::format("lexer grammar E;\nA : {} ;\nB : 'b' ;\n", body); }};

    const auto refusal{[&grammar_of](const std::string_view body) {
        const auto grammar{grammar_of(body)};

        return refusal_of(grammar);
    }};

    const auto expression_of{[&grammar_of](const std::string_view body) {
        const auto grammar{grammar_of(body)};

        return first_rule_of(grammar).expression;
    }};

    // antlr 4.13.2's lexer takes \b \t \n \f \r \' \\ and the two Unicode escapes in a literal, and its set reading
    // takes \b \t \n \f \r \\ \] \- \p{...} and the two Unicode escapes in a set: any other escape is its error 156, in
    // its words, the sequence as written, `\q` anywhere, `\]`, `\-`, `\"`, `\ ` and `\p` in a literal, `\'` and `\"` in
    // a set, and a character beyond ASCII whole.
    EXPECT_EQ(refusal(R"('\q')"), R"(line 2: invalid escape sequence \q)");
    EXPECT_EQ(refusal(R"([\q])"), R"(line 2: invalid escape sequence \q)");
    EXPECT_EQ(refusal(R"('\]')"), R"(line 2: invalid escape sequence \])");
    EXPECT_EQ(refusal(R"('\-')"), R"(line 2: invalid escape sequence \-)");
    EXPECT_EQ(refusal(R"('\"')"), R"(line 2: invalid escape sequence \")");
    EXPECT_EQ(refusal(R"('\ ')"), R"(line 2: invalid escape sequence \ )");
    EXPECT_EQ(refusal(R"('\p{L}')"), R"(line 2: invalid escape sequence \p)");
    EXPECT_EQ(refusal(R"([\'])"), R"(line 2: invalid escape sequence \')");
    EXPECT_EQ(refusal(R"([\"])"), R"(line 2: invalid escape sequence \")");
    EXPECT_EQ(refusal("'\\\xc3\xa9'"), "line 2: invalid escape sequence \\\xc3\xa9");
    EXPECT_EQ(refusal(R"('\q'..'z')"), R"(line 2: invalid escape sequence \q)");
    EXPECT_EQ(refusal(R"(~'\q')"), R"(line 2: invalid escape sequence \q)");
    EXPECT_EQ(refusal(R"('a\q')"), R"(line 2: invalid escape sequence \q)");
    EXPECT_EQ(refusal_of("grammar E;\nr : 'a\\q' ;\nX : 'x' ;\n"), R"(line 2: invalid escape sequence \q)");

    // The escapes it takes are read, each context's own; a property class in a set is refused for the tables the
    // library has not got, not as an escape ANTLR has not got.
    EXPECT_EQ(expression_of(R"('\'')"), R"("'")");
    EXPECT_EQ(expression_of(R"('\\')"), R"("\\")");
    EXPECT_EQ(line_of("lexer grammar E;\nA : [\\]\\-\\\\] ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar E;\nA : '\\b\\t\\n\\f\\r' ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar E;\nA : [\\b\\t\\n\\f\\r] ;\n"), std::nullopt);

    const auto property{refusal(R"([\p{L}])")};

    EXPECT_TRUE(property.contains("Unicode property")) << property;

    // antlr 4.13.2's lexer counts a braced escape's digits from the literal's opening quote, so one whose closing brace
    // stands twelve or more UTF-16 units past the quote is its error 156 too, the literal through the brace its words:
    // `'abcde\u{41}'` reads and `'abcdef\u{41}'` does not, two emoji before the escape count four units and three emoji
    // six, a second braced escape in one literal is always too far, and a set counts nothing.
    EXPECT_EQ(expression_of(R"('abcde\u{41}')"), R"("abcdeA")");
    EXPECT_EQ(refusal(R"('abcdef\u{41}')"), R"(line 2: invalid escape sequence 'abcdef\u{41})");
    EXPECT_EQ(expression_of(R"('ab\u{1F600}')"), R"("ab\xf0\x9f\x98\x80")");
    EXPECT_EQ(refusal(R"('abc\u{1F600}')"), R"(line 2: invalid escape sequence 'abc\u{1F600})");
    EXPECT_EQ(expression_of("'\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}'"), R"("\xf0\x9f\x98\x80\xf0\x9f\x98\x80A")");
    EXPECT_EQ(
            refusal("'\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}'"),
            "line 2: invalid escape sequence '\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}");
    EXPECT_EQ(
            expression_of("'\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\\u{41}'"),
            R"("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9A")");
    EXPECT_EQ(line_of("lexer grammar E;\nA : '\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\\u{41}' ;\n"), 2);
    EXPECT_EQ(refusal(R"('\u{1F600}\u{1F600}')"), R"(line 2: invalid escape sequence '\u{1F600}\u{1F600})");
    EXPECT_EQ(line_of("lexer grammar E;\nA : [abcdefghijk\\u{41}] ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar E;\nA : 'abcdefghijk\\u0041' ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_command_ANTLR_has_not_got_is_refused_in_its_words)
{
    // The clause stands on the grammar's third line, which the refusal names first.
    const auto refusal{[](const std::string_view clause) {
        const auto grammar{std::format("lexer grammar A;\nX : 'a'\n  {} ;\nY : 'b' ;\nmode M;\nZ : 'c' ;\n", clause)};

        return refusal_of(grammar);
    }};

    // antlr 4.13.2 knows skip, more, popMode, type, channel, mode and pushMode and no other command: `frob` is its
    // error 149, in the words it uses, at the line the command stands on, whether alone or after a known one.
    const std::string unknown{"line 3: lexer command frob does not exist or is not supported by the current target"};

    EXPECT_EQ(refusal("-> frob"), unknown);
    EXPECT_EQ(refusal("-> skip, frob"), unknown);
    EXPECT_EQ(refusal("-> frob(Q)"), unknown);

    // The four commands that take an argument refuse to go without one, its error 150, and the three that take none
    // refuse one, its error 151.
    EXPECT_EQ(refusal("-> type"), "line 3: missing argument for lexer command type");
    EXPECT_EQ(refusal("-> channel"), "line 3: missing argument for lexer command channel");
    EXPECT_EQ(refusal("-> mode"), "line 3: missing argument for lexer command mode");
    EXPECT_EQ(refusal("-> pushMode"), "line 3: missing argument for lexer command pushMode");
    EXPECT_EQ(refusal("-> skip(Q)"), "line 3: lexer command skip does not take any arguments");
    EXPECT_EQ(refusal("-> popMode(Q)"), "line 3: lexer command popMode does not take any arguments");
    EXPECT_EQ(refusal("-> more(Q)"), "line 3: lexer command more does not take any arguments");

    // The line named is the command's own: a command on the line after its clause's first is refused there.
    EXPECT_EQ(
            refusal("-> skip,\n  frob"),
            "line 4: lexer command frob does not exist or is not supported by the current target");

    // Parens holding nothing, blanks and comments aside, are no argument and no absence of one: antlr 4.13.2's parser
    // rejects them as a syntax error at the `)`, in these words, whichever command they follow, before it looks at the
    // command; the line named is the `)`'s.
    const std::string surprise{"syntax error: ')' came as a complete surprise to me while matching a lexer rule"};

    EXPECT_EQ(refusal("-> skip()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> popMode()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> type()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> type( /* c */ )"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> frob()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> skip, channel(\n)"), "line 4: " + surprise);

    // The commands are separated by commas: antlr 4.13.2's parser rejects `skip type(Y)` as its error 50, a syntax
    // error at `type`, before it looks at either command, its words varying with what it had taken before the name,
    // `'type' came as a complete surprise to me` after a bare `skip` and `extraneous input 'skip' expecting SEMI` after
    // `type(Y)`, while `skip, type(Y)` passes with its warning 179 on the pair, the comments around the comma no part
    // of it; the line named is the unseparated command's own.
    const std::string unseparated{
            "' stands with no comma before it, which ANTLR's parser rejects while matching a "
            "lexer rule"};

    EXPECT_EQ(refusal("-> skip type(Y)"), "line 3: syntax error: 'type" + unseparated);
    EXPECT_EQ(refusal("-> skip\n  frob"), "line 4: syntax error: 'frob" + unseparated);
    EXPECT_EQ(refusal("-> type(Y) skip"), "line 3: syntax error: 'skip" + unseparated);
    EXPECT_EQ(refusal("-> type(Y) skip()"), "line 3: syntax error: 'skip" + unseparated);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> skip, type(Y) ;\nY : 'b' ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> skip /* c */ , /* d */ type(Y) ;\nY : 'b' ;\n"), std::nullopt);

    // A comma stands between two commands and nowhere else: antlr 4.13.2's parser rejects `skip, ;`, `, skip` and
    // `skip,, type(Y)` as its error 50 too, at the `;` for the first and at the comma for the others, so each is
    // refused at its stray comma's line, before any command is looked at.
    const std::string stray{
            "syntax error: ',' stands where no command name follows it, which ANTLR's parser rejects while matching a "
            "lexer rule"};

    EXPECT_EQ(refusal("-> skip,"), "line 3: " + stray);
    EXPECT_EQ(refusal("-> , skip"), "line 3: " + stray);
    EXPECT_EQ(refusal("-> skip,, type(Y)"), "line 3: " + stray);
    EXPECT_EQ(refusal("-> skip,\n  , type(Y)"), "line 4: " + stray);
    EXPECT_EQ(refusal("-> frob,"), "line 3: " + stray);

    // The seven with their first letter capitalised name the code templates of ANTLR's targets, `LexerSkipCommand`,
    // which antlr 4.13.2 accepts with no diagnostic, expands into an action the generated lexer runs, `skip();`, and
    // leaves out of its own interpreter, which emits the token: refused by name, after ANTLR's own argument checks
    // under the spelling given. Any other spelling of the seven is its error 149.
    const auto capitalised{refusal("-> Skip")};

    EXPECT_TRUE(capitalised.contains("lexer command Skip names a code template")) << capitalised;
    EXPECT_TRUE(refusal("-> Type(Y)").contains("lexer command Type names a code template"));
    EXPECT_TRUE(refusal("-> PopMode").contains("lexer command PopMode names a code template"));
    EXPECT_EQ(refusal("-> Skip(Q)"), "line 3: lexer command Skip does not take any arguments");
    EXPECT_EQ(refusal("-> Type"), "line 3: missing argument for lexer command Type");
    EXPECT_EQ(
            refusal("-> SKIP"), "line 3: lexer command SKIP does not exist or is not supported by the current target");
    EXPECT_EQ(
            refusal("-> sKip"), "line 3: lexer command sKip does not exist or is not supported by the current target");

    // The seven as ANTLR takes them are read, a numeric type among them.
    EXPECT_EQ(refusal("-> skip"), "");
    EXPECT_EQ(refusal("-> popMode"), "");
    EXPECT_EQ(refusal("-> type(1)"), "");
    EXPECT_EQ(refusal("-> channel(HIDDEN), pushMode(M)"), "");
    EXPECT_EQ(refusal("-> mode(M)"), "");
}

TEST(Read_antlr_test, A_channel_command_hides_the_token_whatever_a_type_command_renames_it_to)
{
    const auto token_of{[](const std::string_view clause) {
        const auto grammar{std::format("lexer grammar A;\nX : 'a' {} ;\nB : 'b' ;\nC : 'c' ;\n", clause)};

        const auto rule{first_rule_of(grammar)};

        return rule.token;
    }};

    // antlr 4.13.2 emits the token on the hidden channel for either order, the type naming it and the channel keeping
    // it from the parser, so both leave it out of the stream a parser reads.
    EXPECT_EQ(token_of("-> channel(HIDDEN), type(B)"), std::nullopt);
    EXPECT_EQ(token_of("-> type(B), channel(HIDDEN)"), std::nullopt);
    EXPECT_EQ(token_of("-> channel(HIDDEN)"), std::nullopt);

    // The type is one field and the channel another: of `skip` and `type` the rightmost wins, which is why ANTLR warns
    // that the two are incompatible and emits the renamed token for "skip, type(B)".
    EXPECT_EQ(token_of("-> skip, type(B)"), std::optional<std::string>{"B"});
    EXPECT_EQ(token_of("-> type(B), skip"), std::nullopt);
    EXPECT_EQ(token_of("-> type(B), type(C)"), std::optional<std::string>{"C"});
    EXPECT_EQ(token_of("-> type(B)"), std::optional<std::string>{"B"});
    EXPECT_EQ(token_of("-> skip"), std::nullopt);

    // The default channel is the one a parser reads, so a command naming it leaves the token in the stream: ANTLR emits
    // it on channel zero, which its token dump shows by naming no channel at all.
    EXPECT_EQ(token_of("-> channel(DEFAULT_TOKEN_CHANNEL)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("-> channel(0)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("-> channel(DEFAULT_TOKEN_CHANNEL), type(B)"), std::optional<std::string>{"B"});

    // Two channel commands set one field, so the rightmost wins there as it does for the type.
    EXPECT_EQ(token_of("-> channel(HIDDEN), channel(DEFAULT_TOKEN_CHANNEL)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("-> channel(DEFAULT_TOKEN_CHANNEL), channel(HIDDEN)"), std::nullopt);
}

TEST(Read_antlr_test, A_channel_argument_is_resolved_as_ANTLR_resolves_it)
{
    // The command stands on the grammar's third line.
    const auto grammar_of{[](const std::string_view channels, const std::string_view clause) {
        return std::format(
                "lexer grammar A;\n{}\nX : 'a' {} ;\nB : 'b' ;\nmode M;\nC : 'c' -> popMode ;\n", channels, clause);
    }};

    const auto token_of{[&grammar_of](const std::string_view channels, const std::string_view clause) {
        const auto grammar{grammar_of(channels, clause)};

        return first_rule_of(grammar).token;
    }};

    const auto refusal{[&grammar_of](const std::string_view channels, const std::string_view clause) {
        const auto grammar{grammar_of(channels, clause)};

        return refusal_of(grammar);
    }};

    // antlr 4.13.2 reads a number as Integer.parseInt reads it, so `00` and `000` are the default channel and the token
    // stays where a parser reads it, `01` is HIDDEN and 2 and 99999 channels of their own, all hidden from a parser;
    // the rightmost channel command wins, so `channel(HIDDEN), channel(00)` leaves the token visible.
    EXPECT_EQ(token_of("", "-> channel(00)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("", "-> channel(000)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("", "-> channel( 0 )"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("", "-> channel(01)"), std::nullopt);
    EXPECT_EQ(token_of("", "-> channel(1)"), std::nullopt);
    EXPECT_EQ(token_of("", "-> channel(2)"), std::nullopt);
    EXPECT_EQ(token_of("", "-> channel(99999)"), std::nullopt);
    EXPECT_EQ(token_of("", "-> channel(HIDDEN)"), std::nullopt);
    EXPECT_EQ(token_of("", "-> channel(HIDDEN), channel(00)"), std::optional<std::string>{"X"});

    // A name the channels block declares is a channel from two up, comments in the block no part of any name; a name
    // nothing declares, a token's name among them, and a number beyond ANTLR's int are its error 177, and another of
    // its reserved names its error 172, each in its words at the command's line.
    EXPECT_EQ(token_of("channels { FOO, BAR }", "-> channel(BAR)"), std::nullopt);
    EXPECT_EQ(token_of("channels { /* x */ FOO /* y */ }", "-> channel(FOO)"), std::nullopt);
    EXPECT_EQ(token_of("channels { FOO }", "-> channel(0)"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("channels { FOO }", "-> channel(FOO), pushMode(M)"), std::nullopt);
    EXPECT_EQ(refusal("channels { FOO }", "-> channel(BAZ)"), "line 3: BAZ is not a recognized channel name");
    EXPECT_EQ(refusal("", "-> channel(FOO)"), "line 3: FOO is not a recognized channel name");
    EXPECT_EQ(refusal("", "-> channel(B)"), "line 3: B is not a recognized channel name");
    EXPECT_EQ(refusal("", "-> channel(2147483648)"), "line 3: 2147483648 is not a recognized channel name");
    EXPECT_EQ(refusal("", "-> channel(SKIP)"), "line 3: cannot use or declare channel with reserved name SKIP");
    EXPECT_EQ(refusal("", "-> channel(EOF)"), "line 3: cannot use or declare channel with reserved name EOF");
    EXPECT_EQ(
            refusal("", "-> channel(DEFAULT_MODE)"),
            "line 3: cannot use or declare channel with reserved name DEFAULT_MODE");

    // Only a lexer grammar declares channels: a block in a combined grammar is its error 164, in its words.
    EXPECT_EQ(
            refusal_of("grammar A;\nchannels { FOO }\ns : X ;\nX : 'a' -> channel(FOO) ;\n"),
            "line 2: custom channels are not supported in combined grammars");
}

TEST(Read_antlr_test, Element_options_are_metadata_on_the_element_and_no_token)
{
    const auto rules_of{[](const std::string_view rules) {
        const auto grammar{std::format("grammar E;\n{}\nX : 'x' ;\n", rules)};

        return scanner_of(grammar).rules;
    }};

    // antlr 4.13.2 reads `<fail='z'>` as an option on the predicate and no literal: its token file names X alone, and
    // its lexer cuts "xz" into X and a token recognition error at z.
    const auto predicate{rules_of("s : {false}?<fail='z'> X ;")};

    ASSERT_EQ(predicate.size(), 1U);
    EXPECT_EQ(predicate.front().token, std::optional<std::string>{"X"});

    // A name, a number, a string and a brace block are the values ANTLR's parser takes, on a token, a literal and a
    // rule reference alike, and an alternative's options stand at its start: antlr 4.13.2 accepts each with its warning
    // 83 on the option, and the input x is X in every one.
    const auto valued{rules_of("s : X<a=1> 'q'<b='z'> r<c={x}> ;\nr : X ;")};

    ASSERT_EQ(valued.size(), 2U);
    EXPECT_EQ(valued.front().pattern, "'q'");
    EXPECT_EQ(valued.back().token, std::optional<std::string>{"X"});

    for (const std::string_view rules :
         {"s : <assoc=right> X ;", R"(s : {false}?<fail={"z"}> X <assoc=right> ;)", "s : X<a> ;", "s : X <a=b> ;",
          "s : X<a='z'>* ;"})
    {
        const auto read{rules_of(rules)};

        EXPECT_EQ(read.size(), 1U) << rules;
    }

    // In a lexer rule ANTLR's parser takes them on a literal, a reference, the dot and a negated literal, before the
    // suffix, with any of the values and with nothing between the angles: antlr 4.13.2 accepts each with its warning 83
    // and emits X for the input x.
    const auto lexer_rule_of{[](const std::string_view body) {
        const auto grammar{std::format("lexer grammar E;\nX : {} ;\nfragment Y : 'y' ;\n", body)};

        return first_rule_of(grammar);
    }};

    const auto valued_rule{lexer_rule_of("'x'<a=b>")};

    EXPECT_EQ(valued_rule.token, std::optional<std::string>{"X"});

    const std::vector<std::pair<std::string_view, std::string_view>> optioned{
            {"'x'<a=b, c='d', e={f}, g=1, h>", R"("x")"},
            {"'x'<a=b.c>", R"("x")"},
            {"'x'<>", R"("x")"},
            {"'x'<a=b>+", R"("x"+)"}};

    for (const auto& [body, expression] : optioned)
    {
        const auto rule{lexer_rule_of(body)};

        EXPECT_EQ(rule.expression, expression) << body;
    }

    // Each dot of a qualified name or value is a token of its own to ANTLR's lexer, so blanks and comments may stand on
    // either side of it: antlr 4.13.2 accepts `'a'<foo=bar /* part */ . baz>` and `'a'<foo . bar>` and emits the rule's
    // token for a.
    for (const std::string_view body :
         {"'x'<foo=bar /* part */ . baz>", "'x'<foo . bar>", "'x'<foo=12, bar.baz, q='s'>"})
    {
        const auto rule{lexer_rule_of(body)};

        EXPECT_EQ(rule.token, std::optional<std::string>{"X"}) << body;
    }

    const std::vector<std::pair<std::string_view, std::string_view>> atoms{
            {"Y<a=b>", "{Y}"},
            {".<a=b>", R"([\u{0}-\u{7f}\u{80}-\u{10ffff}])"},
            {"~'x'<a=b>", R"([\u{0}-\u{77}\u{79}-\u{7f}\u{80}-\u{10ffff}])"}};

    for (const auto& [body, expression] : atoms)
    {
        const auto rule{lexer_rule_of(body)};

        EXPECT_EQ(rule.expression, expression) << body;
    }

    // On a set, a range or a group, and with a dotted name assigned, a name missing or the `>` missing, antlr 4.13.2
    // reports its error 50 and generates no lexer; each is refused at its line.
    for (const std::string_view body :
         {"[ab]<a=b>", "('x')<a=b>", "'a'..'z'<a=b>", "'x'<a.b=c>", "'x'<a=b,>", "'x'<'z'>", "'x'<a=b"})
    {
        const auto grammar{std::format("lexer grammar E;\nX : {} ;\n", body)};

        EXPECT_EQ(line_of(grammar), 2) << body;
    }

    for (const std::string_view rule : {"s : X<a='z' ;", "s : X<a='z',> ;", "s : X<'z'> ;"})
    {
        const auto grammar{std::format("grammar E;\n{}\nX : 'x' ;\n", rule)};

        EXPECT_EQ(line_of(grammar), 2) << rule;
    }
}

TEST(Read_antlr_test, A_type_of_EOF_is_refused_and_a_type_of_zero_is_the_rules_own)
{
    const auto grammar_of{
            [](const std::string_view rule) { return std::format("lexer grammar T;\n{}\nY : 'y' ;\n", rule); }};

    const auto token_of{[&grammar_of](const std::string_view rule) {
        const auto grammar{grammar_of(rule)};

        return first_rule_of(grammar).token;
    }};

    const auto refusal{[&grammar_of](const std::string_view rule) {
        const auto grammar{grammar_of(rule)};

        return refusal_of(grammar);
    }};

    // antlr 4.13.2's EOF is the token type minus one, which ends the token stream: for `type(EOF)` its lexer emits x as
    // `<EOF>` and nothing for the y after it, on the hidden channel too, `channel=1` and still nothing after, and the
    // same with a mode change or a skip and a type before it, the rightmost type winning; so the rule is refused by
    // name at the command's line.
    const std::string ended{
            "'-> type(EOF)' ends the token stream where the rule matches, so nothing after the match is a token, "
            "which the byte reading cannot express"};

    EXPECT_EQ(refusal("X : 'x' -> type(EOF) ;"), "line 2: " + ended);
    EXPECT_EQ(refusal("X : 'x' -> type(EOF), channel(HIDDEN) ;"), "line 2: " + ended);
    EXPECT_EQ(refusal("X : 'x' -> channel(HIDDEN), type(EOF) ;"), "line 2: " + ended);
    EXPECT_EQ(refusal("X : 'x' -> type(Y), skip, type(EOF) ;"), "line 2: " + ended);
    EXPECT_EQ(refusal("X : 'x'\n  -> type(EOF), mode(M) ;\nmode M;\nZ : 'z' ;"), "line 3: " + ended);

    // Where a later command sets the type again the stream goes on: antlr 4.13.2 emits y after `type(EOF), skip`, x as
    // Y on the hidden channel for `type(EOF), channel(HIDDEN), type(Y)` and x as Y for `type(EOF), type(Y)`.
    EXPECT_EQ(token_of("X : 'x' -> type(EOF), skip ;"), std::nullopt);
    EXPECT_EQ(token_of("X : 'x' -> type(EOF), channel(HIDDEN), type(Y) ;"), std::nullopt);
    EXPECT_EQ(token_of("X : 'x' -> type(EOF), type(Y) ;"), std::optional<std::string>{"Y"});

    // Zero is antlr 4.13.2's value for no type set, so its lexer emits the rule's own type in its place: `<'x'>`, that
    // is X, for `X : 'x' -> type(0)` and `type(00)`, whose shape gives the rule a type as a parser literal's alias, and
    // `<0>`, a token no rule names, for `X : 'x' 'y' -> type(0)`, `type(0), type(0)` and `type(1), type(0)`, whose rule
    // ANTLR gives no type of its own; a lexer grammar's tokens block gives X its type, `<X>`, where a combined
    // grammar's block goes to the parser alone and the lexer emits `<0>` still. Any other number names the token ANTLR
    // numbers so, `<'y'>` for `type(1)` here, and is kept as written.
    EXPECT_EQ(token_of("X : 'x' -> type(0) ;"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("X : 'x' -> type(00) ;"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("X : 'x' 'y' -> type(0) ;"), std::optional<std::string>{"0"});
    EXPECT_EQ(token_of("X : 'x' -> type(0), type(0) ;"), std::optional<std::string>{"0"});
    EXPECT_EQ(token_of("X : 'x' -> type(1), type(0) ;"), std::optional<std::string>{"0"});
    EXPECT_EQ(token_of("tokens { X }\nX : 'x' 'y' -> type(0) ;"), std::optional<std::string>{"X"});
    EXPECT_EQ(token_of("X : 'x' -> type(1) ;"), std::optional<std::string>{"1"});
    EXPECT_EQ(
            first_rule_of("grammar T;\ntokens { X }\ns : X ;\nX : 'x' 'y' -> type(0) ;\nY : 'y' ;\n").token,
            std::optional<std::string>{"0"});
}

TEST(Read_antlr_test, A_clause_ANTLR_parser_rejects_is_refused_at_the_byte_it_rejects)
{
    // The clause stands on the grammar's third line.
    const auto refusal{[](const std::string_view clause) {
        const auto grammar{std::format("lexer grammar A;\nX : 'a'\n  {} ;\nY : 'b' ;\n", clause)};

        return refusal_of(grammar);
    }};

    const std::string rejected{", which ANTLR's parser rejects while matching a lexer rule"};

    // antlr 4.13.2's parser takes a command's parens with one name or one number inside and nothing else, so `type(Y
    // Z)`, `type(Y.Z)`, `type(2Y)`, `channel(1 2)` and `type(Y (` are its error 50 at the second token, `extraneous
    // input 'Z' expecting RPAREN` and `mismatched input '.' expecting RPAREN` among its words, and `type('y')`,
    // `type(-1)` and `type(_Y)` at the quote, the minus and the underscore, which begin no name or number to it, `''y''
    // came as a complete surprise to me`.
    EXPECT_EQ(
            refusal("-> type(Y Z)"),
            "line 3: syntax error: 'Z' stands after the argument of type where ')' should close it" + rejected);
    EXPECT_EQ(
            refusal("-> type(Y.Z)"),
            "line 3: syntax error: '.' stands after the argument of type where ')' should close it" + rejected);
    EXPECT_EQ(
            refusal("-> type(2Y)"),
            "line 3: syntax error: 'Y' stands after the argument of type where ')' should close it" + rejected);
    EXPECT_EQ(
            refusal("-> channel(1 2)"),
            "line 3: syntax error: '2' stands after the argument of channel where ')' should close it" + rejected);
    EXPECT_EQ(
            refusal("-> type(Y ("),
            "line 3: syntax error: '(' stands after the argument of type where ')' should close it" + rejected);
    EXPECT_EQ(
            refusal("-> type('y')"),
            "line 3: syntax error: ''' stands where the argument of type should be a name or a number" + rejected);
    EXPECT_EQ(
            refusal("-> type(-1)"),
            "line 3: syntax error: '-' stands where the argument of type should be a name or a number" + rejected);
    EXPECT_EQ(
            refusal("-> type(_Y)"),
            "line 3: syntax error: '_' stands where the argument of type should be a name or a number" + rejected);

    // Parens the clause ends inside of, `type(Y` and `type(`, are its error 50 at the `;`, `missing RPAREN at ';'` and
    // `';' came as a complete surprise to me`, so the line named is the `;`'s own, the next line's here.
    const std::string unclosed{"syntax error: the '(' after type is never closed by ')'" + rejected};

    EXPECT_EQ(refusal("-> type(Y"), "line 3: " + unclosed);
    EXPECT_EQ(refusal("-> type("), "line 3: " + unclosed);
    EXPECT_EQ(refusal_of("lexer grammar A;\nX : 'a' -> type(Y\n;\nY : 'b' ;\n"), "line 3: " + unclosed);

    // An arrow no command follows is its error 50 at the `;`, `';' came as a complete surprise to me`, a comment
    // between the two being no command.
    EXPECT_EQ(refusal("->"), "line 3: syntax error: '->' has no command after it" + rejected);
    EXPECT_EQ(refusal("-> /* c */"), "line 3: syntax error: '->' has no command after it" + rejected);

    // A byte after a command where a comma or the `;` should stand, the second `)` of `type(Y))`, a `)` after `skip` or
    // after a comment, or a number, is its error 50 there, `extraneous input ')' expecting SEMI`; and a number where a
    // command's name should stand, `-> 5`, is `'5' came as a complete surprise to me`.
    EXPECT_EQ(
            refusal("-> type(Y))"),
            "line 3: syntax error: ')' stands after the command type where ',' or ';' should" + rejected);
    EXPECT_EQ(
            refusal("-> skip )"),
            "line 3: syntax error: ')' stands after the command skip where ',' or ';' should" + rejected);
    EXPECT_EQ(
            refusal("-> type(Y) /* c */ )"),
            "line 3: syntax error: ')' stands after the command type where ',' or ';' should" + rejected);
    EXPECT_EQ(
            refusal("-> skip 5"),
            "line 3: syntax error: '5' stands after the command skip where ',' or ';' should" + rejected);
    EXPECT_EQ(refusal("-> 5"), "line 3: syntax error: '5' stands where a command's name should" + rejected);

    // Blanks and comments inside the parens are no part of the argument.
    EXPECT_EQ(refusal("-> type( Y )"), "");
    EXPECT_EQ(refusal("-> type(Y /* c */)"), "");

    // A tokens or channels block holds names parted by commas: antlr 4.13.2 rejects `{ ONE TWO }` as its error 50 at
    // TWO, `extraneous input 'TWO' expecting RBRACE`, and a name missing after the brace or a comma, `{ }`, `{ ONE,
    // TWO, }` and `{ ONE, 'x' }`, as `'}' came as a complete surprise to me while looking for an identifier`, a tokens
    // block alone allowed to hold nothing.
    const auto refusal_with{[](const std::string_view block) {
        const auto grammar{std::format("lexer grammar A;\n{}\nX : 'x' ;\n", block)};

        return refusal_of(grammar);
    }};

    const std::string channels{", which ANTLR's parser rejects while matching a channels block"};

    EXPECT_EQ(
            refusal_with("channels { ONE TWO }"),
            "line 2: syntax error: 'TWO' stands after the channel ONE where ',' or '}' should" + channels);
    EXPECT_EQ(
            refusal_with("channels { ONE TWO, THREE }"),
            "line 2: syntax error: 'TWO' stands after the channel ONE where ',' or '}' should" + channels);
    EXPECT_EQ(
            refusal_with("channels { ONE, TWO, }"),
            "line 2: syntax error: '}' stands where a channel name should" + channels);
    EXPECT_EQ(refusal_with("channels { }"), "line 2: syntax error: '}' stands where a channel name should" + channels);
    EXPECT_EQ(
            refusal_with("channels { ONE, 'x' }"),
            "line 2: syntax error: ''' stands where a channel name should" + channels);
    EXPECT_EQ(refusal_with("channels { ONE, TWO }"), "");
    EXPECT_EQ(
            refusal_with("tokens { A B }"),
            "line 2: syntax error: 'B' stands after the token A where ',' or '}' should, which ANTLR's parser rejects "
            "while matching a tokens block");
    EXPECT_EQ(
            refusal_with("tokens { A, B, }"),
            "line 2: syntax error: '}' stands where a token name should, which ANTLR's parser rejects while matching a "
            "tokens block");
    EXPECT_EQ(refusal_with("tokens { }"), "");
    EXPECT_EQ(refusal_with("tokens { A, B }"), "");

    // A name begins with a letter to antlr 4.13.2, so a rule named `_X` is its error 50 at the underscore, `'_' came as
    // a complete surprise to me`.
    EXPECT_EQ(line_of("lexer grammar A;\n_X : 'x' ;\n"), 2);
}

TEST(Read_antlr_test, A_raw_line_break_inside_a_literal_or_a_set_is_refused_in_ANTLRs_words)
{
    // antlr 4.13.2's lexer ends a string literal at a raw carriage return or line feed, its error 152 at the quote,
    // `unterminated string literal`, in a lexer rule and in a parser rule alike, and a set at either, its error 50
    // `mismatched character '\r' expecting ']'`; a raw tab is a character of the literal, `<'a\u0009b'>`.
    EXPECT_EQ(refusal_of("lexer grammar D;\nX : 'a\rb' ;\nY : 'y' ;\n"), "line 2: unterminated string literal");
    EXPECT_EQ(refusal_of("lexer grammar D;\nX : 'a\nb' ;\nY : 'y' ;\n"), "line 2: unterminated string literal");
    EXPECT_EQ(refusal_of("grammar D;\ns : 'a\nb' ;\nX : 'x' ;\n"), "line 2: unterminated string literal");
    EXPECT_EQ(
            refusal_of("lexer grammar D;\nX : [a\rb] ;\nY : 'y' ;\n"),
            R"(line 2: syntax error: mismatched character '\r' expecting ']')");
    EXPECT_EQ(
            refusal_of("lexer grammar D;\nX : [a\nb] ;\nY : 'y' ;\n"),
            R"(line 2: syntax error: mismatched character '\n' expecting ']')");
    EXPECT_EQ(line_of("lexer grammar D;\nX : 'a\tb' ;\nY : 'y' ;\n"), std::nullopt);
}

TEST(Read_antlr_test, A_byte_order_mark_is_a_blank_wherever_ANTLR_drops_one)
{
    constexpr std::string_view bom{"\xEF\xBB\xBF"};

    // antlr 4.13.2's lexer reads U+FEFF as a token of its own that it skips, wherever it stands outside a literal, a
    // set or an action: a grammar opening with one, a lexer or a combined one, before a comment or after a blank, reads
    // as without it, x then y in its token stream.
    for (const std::string_view head : {"", "// comment\n", " "})
    {
        const auto grammar{std::format("{}{}lexer grammar B;\nX : 'x' ;\nY : 'y' ;\n", bom, head)};

        const auto spec{scanner_of(grammar)};

        ASSERT_EQ(spec.rules.size(), 2U) << head;
        EXPECT_EQ(spec.rules.front().token, std::optional<std::string>{"X"}) << head;
        EXPECT_EQ(spec.rules.back().token, std::optional<std::string>{"Y"}) << head;
    }

    const auto grammar{std::format("{}grammar B;\ns : X ;\nX : 'x' ;\n", bom)};

    EXPECT_EQ(scanner_of(grammar).rules.size(), 1U);

    // Between any two tokens the same: before a rule's colon, before its `;`, inside a clause and inside a command's
    // parens, after a literal and inside a channels block, antlr 4.13.2 accepts each and skips or emits the token as
    // the commands say.
    const auto rule_of{[](const std::string_view rule) {
        const auto grammar{std::format("lexer grammar B;\n{}\nY : 'y' ;\n", rule)};

        return first_rule_of(grammar);
    }};

    // Each rule with the mark in it, and the token it emits.
    const std::vector<std::pair<std::string, std::optional<std::string>>> marked{
            {std::format("X{}: 'x' ;", bom), "X"},
            {std::format("X : 'x' {};", bom), "X"},
            {std::format("X : 'x' -> {} skip ;", bom), std::nullopt},
            {std::format("X : 'x'{} -> skip ;", bom), std::nullopt},
            {std::format("X : 'x' -> type({}Y) ;", bom), "Y"},
            {std::format("X : 'x' -> type(Y{}) ;", bom), "Y"},
            {std::format("channels {{ ONE{}, TWO }}\nX : 'x' -> channel(ONE) ;", bom), std::nullopt}};

    for (const auto& [rule, token] : marked)
    {
        const auto read{rule_of(rule)};

        EXPECT_EQ(read.token, token) << rule;
    }

    // Inside a literal it is the character U+FEFF, which antlr 4.13.2 matches, its token dump showing the mark alone
    // between the quotes; inside a name it parts the name in two, `X Y`, which is its error 50 at Y, `extraneous input
    // 'Y' expecting COLON`.
    const auto in_literal{rule_of(std::format("X : '{}' ;", bom))};

    const auto in_name{std::format("lexer grammar B;\nX{}Y : 'x' ;\n", bom)};

    EXPECT_EQ(in_literal.expression, R"("\xef\xbb\xbf")");
    EXPECT_EQ(line_of(in_name), 2);
}

TEST(Read_antlr_test, Refusals_name_the_line)
{
    EXPECT_EQ(line_of("lexer grammar A;\nimport B;\n"), 2);
    EXPECT_EQ(line_of("parser grammar A;\n"), 1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' .*? [b] ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a' Y ;\nY : 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a'+ ;\n"), 2);

    // What the loop stops at is the rest of the whole rule: inside a group, and in a rule another rule references, that
    // rest reaches past what the loop's own sequence holds, so both are refused rather than read as the rest this
    // reading can see. ANTLR matches all of "abbc" for either shape.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a' .*? 'b') 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F 'c' ;\nfragment F : .*? 'b' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'b' ;\nY : X 'c' ;\n"), 2);

    // A loop in an outermost alternative of a rule nothing references is read.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | 'a' .*? 'b' ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' EOF ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' {p()}? 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' {more();} ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' {setType(Y);} 'b' ;\nY : 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [\\p{L}]+ ;\n"), 2);

    // ANTLR folds a character beyond ASCII with its Unicode case mappings, which the library has not got, so every
    // shape naming one under caseInsensitive is refused rather than folded as if it were ASCII.
    EXPECT_EQ(line_of("lexer grammar A;\noptions { caseInsensitive = true; }\nX : '\\u00E9' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\noptions { caseInsensitive = true; }\nX : [a\\u00E9]+ ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\noptions { caseInsensitive = true; }\nX : 'a'..'\\u00FF' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\noptions { caseInsensitive = true; }\nX : ~'\\u00E9' ;\n"), 3);
    EXPECT_EQ(line_of("grammar A;\noptions { caseInsensitive = true; }\nr : '\\u00E9' ;\nX : 'a' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more ;\n"), 2);

    // The two forms ANTLR itself rejects: a command on an alternative of a rule with several, which must end the single
    // outermost alternative, and a mode in a combined grammar, modes being a lexer grammar's alone.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> skip | 'b' -> type(Y) ;\nY : 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' | 'b' -> skip ;\n"), 2);
    EXPECT_EQ(line_of("grammar A;\nr : X ;\nX : 'a' ;\nmode INNER;\nY : 'b' ;\n"), 4);
    EXPECT_EQ(line_of("lexer grammar A;\n\nX : 'a' \n"), 3);
}

TEST(Read_antlr_test, A_members_action_that_defines_a_method_or_runs_code_is_refused_at_its_line)
{
    // ANTLR writes a `members` action of the lexer's into the body of the lexer class it generates, so a method defined
    // there stands in place of the runtime's own and the tokens the rules describe are not the tokens the scanner
    // emits: a `nextToken` of the grammar's own can join two matches into one token. A definition of one of the methods
    // deciding the stream is refused by name; a call is not a definition, an ordinary member is read, and a
    // `parser::members` action reaches no lexer.
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\npublic Token nextToken() { return null; }\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\n@lexer::members {\nvoid emit(Token t) { }\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nint skip(int n) throws E { return n; }\n}\nX : 'a' ;\n"), 2);

    // Any method the members define is refused, not a listed few: the generated lexer calls its own methods to decide
    // the tokens, `emit` reaching the token's end through `getCharIndex`, and which of them matter is the runtime's to
    // know. A field, a nested class and a call are read.
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nvoid note() { skip(); }\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\n@Override public int getCharIndex() { return 0; }\n}\nX : 'a' ;\n"),
            2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nboolean flag = false;\n}\nX : 'a' ;\n"), std::nullopt);

    // A field's initializer runs when the lexer is built as an initializer block does: Java runs the members' field
    // initializers after the runtime's constructor, whose `_mode` they may assign, so `int startupMode = (_mode = IN);`
    // leaves the lexer in mode IN before its first token where the reading models the default mode. An initializer that
    // is one value, a number, a literal or a name, decides nothing and is read; an assignment, a call or an operator
    // inside one is refused.
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\nint startupMode = (_mode = IN);\n}\nX : 'a' ;\nmode IN;\n"
                    "Y : 'a' ;\n"),
            2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\nprivate final int startupMode = (_mode = IN);\n}\nX : 'a' ;\n"
                    "mode IN;\nY : 'a' ;\n"),
            2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nint depth = f();\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nint depth = a + b;\n}\nX : 'a' ;\n"), 2);

    // An array declarator's brackets stand before the `=`, and a step is an operator: `int initial[] = { _mode = IN };`
    // and `int prior = _mode--;` each set the mode at construction and are refused; an array of values is read.
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\nint initial[] = { _mode = IN };\n}\nX : 'a' ;\nmode IN;\n"
                    "Y : 'a' ;\n"),
            2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nint prior = _mode--;\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\nint[] sizes = { 1, -2 }; int[] none = {};\n}\nX : 'a' ;\n"),
            std::nullopt);

    // The C++ target's members are fields of the lexer class too, and a field may be initialized with braces: `int
    // initial{(setMode(IN), 0)};` sets the mode at construction and is refused as `= (setMode(IN), 0)` is; `int
    // initial{7};` is a value, and a nested class's body or a method's is no initializer.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\nint initial{(setMode(IN), 0)};\n}\n"
                    "X : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "int initial{7}; int more[2]{-1, 2}; class Inner { int f() { return 1; } };\n}\nX : 'a' ;\n"),
            std::nullopt);

    // An array's brackets may stand between the name and the brace, and a member of a class of the grammar's own handed
    // the lexer, `Initializer startup{this};`, may set the mode in its constructor, so `this` is no value an
    // initializer may hold; a C# property's accessors, `{ get; set; }`, run when the property is used and initialize
    // nothing.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::declarations {\n"
                    "int initial[1]{(setMode(IN), 0)};\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct Initializer { Initializer(A* p) { p->setMode(IN); } }; Initializer startup{this};\n}\n"
                    "X : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct Holder { Holder(A* p) {} }; Holder keep = Holder(this);\n}\nX : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = CSharp; }\n@members {\n"
                    "public int Value { get; set; } int count = 0;\n}\nX : 'a' ;\n"),
            std::nullopt);

    // The accessors are C#'s alone: under the C++ target `get` is a name like any other, a type's among them, so `get
    // startup{get{this}};` and `int startup{init + (setMode(IN), 0)};` are initializers and refused, and a parenthesis
    // inside an array's bound, `int a[(1)]{...}`, opens no parameter list.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct get { get(A* p) { p->setMode(IN); } }; get startup{get{this}};\n}\nX : 'a' ;\n"
                    "mode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::declarations {\n"
                    "int init = 0; int startup{init + (setMode(IN), 0)};\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::declarations {\n"
                    "int startup[(1)]{(setMode(IN), 0)};\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);

    // A type is defined where its keyword's name is followed by the body or a base clause; `struct Initializer
    // startup{this};` names the type and declares a field, whose initializer hands the lexer over. The C++ target's
    // `definitions` are written at namespace scope in the source file, where `int helper()` is no method of the
    // lexer's; a macro an action defines, `START_IN_MODE` in the header, stands for its replacement in an initializer;
    // `sizeof(int)` inside an array's bound is no method's parameter list; and a C# accessor with a body may override a
    // property the runtime reads, `CharIndex` among them, and is refused as a method is.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct Initializer { Initializer(A* p) { p->setMode(IN); } }; struct Initializer "
                    "startup{this};\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::definitions {\n"
                    "int helper() { return 7; }\n}\nX : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@header {\n"
                    "#define START_IN_MODE (setMode(IN), 0)\n}\n@lexer::declarations {\n"
                    "int startup{START_IN_MODE};\n}\nX : 'a' ;\nmode IN;\nY : 'a' ;\n"),
            6);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@header {\n#define SLOTS 4\n}\n"
                    "@lexer::declarations {\nint startup{SLOTS}; int data[sizeof(int)]{0};\n}\nX : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = CSharp; }\n@members {\n"
                    "public override int CharIndex { get { Consume(); return 0; } }\n}\nX : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\n"
                    "int depth = -1; String s = \"x\"; int m = IN; boolean b = Boolean.TRUE;\n}\nX : 'a' ;\n"
                    "mode IN;\nY : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\n@header {\npackage p;\n}\nX : 'a' ;\n"), std::nullopt);
    EXPECT_EQ(
            line_of("grammar A;\n@parser::members {\npublic Token nextToken() { return null; }\n}\nr : X ;\n"
                    "X : 'a' ;\n"),
            std::nullopt);

    // ANTLR's lexer parts the `@`, the scope, the `::` and the name by nothing, so blanks and comments may stand at
    // each joint and `@lexer :: members` names what `@lexer::members` names.
    EXPECT_EQ(
            line_of("lexer grammar A;\n@lexer :: members {\npublic Token nextToken() { return null; }\n}\n"
                    "X : 'a' ;\n"),
            2);
    EXPECT_EQ(line_of("lexer grammar A;\n@ members {\npublic Token nextToken() { return null; }\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@lexer /* c */ :: members {\npublic Token nextToken() { return null; }\n}\n"
                    "X : 'a' ;\n"),
            2);

    // The action's code is the target language's, so a comment between any two tokens of a declaration is a blank and
    // declares what the declaration without it declares, while a name inside a comment or a string declares nothing at
    // all: a documentation comment or a field holding the text is prose and no override.
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\npublic Token nextToken /* c */ () { return null; }\n}\n"
                    "X : 'a' ;\n"),
            2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\npublic Token nextToken() /* c */ { return null; }\n}\n"
                    "X : 'a' ;\n"),
            2);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\npublic void skip() /* c */ { }\n}\nX : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\n/* nextToken() { example } */ boolean flag = false;\n}\n"
                    "X : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nString s = \"nextToken() { \";\n}\nX : 'a' ;\n"), std::nullopt);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\nToken t() { return super.nextToken(); }\n}\nX : 'a' ;\n"), 2);

    // A superclass may define any method of the lexer's, and its code is not in the grammar to read; and the C++
    // target's `declarations` and `definitions` actions are written into the lexer class as `members` is.
    EXPECT_EQ(line_of("lexer grammar A;\noptions { superClass=MyBase; }\nX : 'a' ;\n"), 1);
    EXPECT_EQ(line_of("lexer grammar A;\n@members {\n{ mode(M); }\n}\nX : 'a' ;\nmode M;\nY : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language=Cpp; }\n@lexer::declarations {\nvoid skip() override {}\n}\n"
                    "X : 'ab' -> skip ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language=Cpp; }\n@members {\nint nesting = 0;\n}\nX : 'a' ;\n"),
            std::nullopt);

    // The members are read without splicing, as Java reads them: a line comment ending in a backslash ends at the
    // newline and hides nothing under it.
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\n// path C:\\\npublic Token nextToken() { return null; }\n}\n"
                    "X : 'a' ;\n"),
            2);

    // An unscoped `members` action is written into both classes a combined grammar generates, the lexer's among them,
    // as ANTLR's grammar documentation has it, so it is the lexer's wherever the grammar declares it; only a
    // `parser::members` action leaves the lexer alone. A method of a class declared inside the block belongs to that
    // class and not to the lexer.
    EXPECT_EQ(line_of("grammar A;\n@members {\npublic Token nextToken() { return null; }\n}\nr : X ;\nX : 'a' ;\n"), 2);
    EXPECT_EQ(
            line_of("grammar A;\n@lexer::members {\npublic Token nextToken() { return null; }\n}\nr : X ;\n"
                    "X : 'a' ;\n"),
            2);
    EXPECT_EQ(
            line_of("lexer grammar A;\n@members {\nclass Inner { public Token nextToken() { return null; } }\n}\n"
                    "X : 'a' ;\n"),
            std::nullopt);

    // The members are the target language's. Java, C++ and C# declare a method by a parameter list and a body, C# an
    // expression body too, and any method one of them defines is refused; a target that declares a method otherwise,
    // Python's `def` among them, is refused by name instead.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language=CSharp; }\n@members {\n"
                    "public override IToken NextToken() { return null; }\n}\nX : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language=Cpp; }\n@members {\nint nesting = 0;\n}\nX : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language=Python3; }\n@members {\ndef nextToken(self):\n"
                    "    return None\n}\nX : 'a' ;\n"),
            3);

    // Java ends a line comment at any line terminator, a bare carriage return among them, so a comment written on a
    // carriage-return line ends where Java ends it and the declaration under it is read.
    EXPECT_EQ(
            line_of("lexer grammar A;\r@members { // c\rpublic Token nextToken() { return null; } }\rX : 'a' ;\r"), 1);
}

TEST(Read_antlr_test, An_out_of_line_definition_and_an_attributed_method_are_read_as_members)
{
    // The C++ target writes `definitions` into the source file after the lexer's constructor and accessors, where antlr
    // 4.13.2 puts `std::unique_ptr<antlr4::Token> A::nextToken() { ... }` as written: a definition qualified by the
    // lexer's class, `A` for a lexer grammar and `GLexer` for a combined `G`, defines the member `declarations`
    // declares and is refused as a method in the class is, at the action's line, while a function of its own, a call
    // qualified by the class and another class's member define nothing of the lexer's. The Java target writes
    // `definitions` nowhere.
    const auto defined{[](const std::string_view head, const std::string_view language, const std::string_view code) {
        const auto parser_rule{head.starts_with("grammar") ? "r : X ;\n" : ""};

        return line_of(std::format(
                "{};\noptions {{ language = {}; }}\n@lexer::definitions {{\n{}\n}}\n{}X : 'a' ;\n", head, language,
                code, parser_rule));
    }};

    const std::string_view next_token{
            "std::unique_ptr<antlr4::Token> A::nextToken() { return antlr4::Lexer::nextToken(); }"};

    EXPECT_EQ(defined("lexer grammar A", "Cpp", next_token), 3);
    EXPECT_EQ(defined("grammar G", "Cpp", "antlr4::Token* GLexer::emit() { return nullptr; }"), 3);
    EXPECT_EQ(defined("lexer grammar A", "Cpp", "namespace { int f() { return 1; } }\nvoid A::reset() { }"), 3);
    EXPECT_EQ(
            defined("lexer grammar A", "Cpp", "int helper() { if (A::check(1)) { return 1; } return 0; }"),
            std::nullopt);
    EXPECT_EQ(defined("lexer grammar A", "Cpp", "void Helper::run() { }"), std::nullopt);

    // An attribute-specifier sequence before the definition is no part of its head, its parentheses included, so the
    // attributed override, which antlr 4.13.2 writes into A.cpp as written, is refused as the bare one is.
    EXPECT_EQ(
            defined("lexer grammar A", "Cpp",
                    "[[deprecated(\"x\")]] std::unique_ptr<antlr4::Token> A::nextToken() { return Lexer::nextToken(); "
                    "}"),
            3);
    EXPECT_EQ(
            defined("lexer grammar A", "Cpp",
                    "[[nodiscard]] [[deprecated(\"x\")]] std::unique_ptr<antlr4::Token> A::nextToken() { return "
                    "nullptr; }"),
            3);
    EXPECT_EQ(
            defined("lexer grammar A", "Cpp",
                    "int helper() { [[maybe_unused]] int n{0}; if (A::check(1)) { n = 1; } return n; }"),
            std::nullopt);
    EXPECT_EQ(defined("grammar G", "Cpp", next_token), std::nullopt);
    EXPECT_EQ(defined("lexer grammar A", "Java", next_token), std::nullopt);

    // An attribute before a method is no declarator, C#'s `[Attr]` and C++'s `[[nodiscard]]` alike, so the attributed
    // override is refused as the bare one is, while a name inside an array's bound still declares nothing.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = CSharp; }\n@members {\n"
                    "[Attr] public override IToken NextToken() { return null; }\n}\nX : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = CSharp; }\n@members {\n"
                    "[Attr(1), Other] public override IToken NextToken() => null;\n}\nX : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::declarations {\n"
                    "[[nodiscard]] std::unique_ptr<antlr4::Token> nextToken() override { return nullptr; }\n}\n"
                    "X : 'a' ;\n"),
            3);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@lexer::declarations {\n"
                    "int data[2][sizeof(int)]{0};\n}\nX : 'a' ;\n"),
            std::nullopt);
}

TEST(Read_antlr_test, A_final_class_and_a_macro_the_header_never_sees_initialize_nothing)
{
    // `struct Inner final { ... };` defines a type, `final` standing in its head as C++ has it, with a base clause
    // after it or not, and the fields inside it belong to that type.
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct Inner final { int x = f(); };\n}\nX : 'a' ;\n"),
            std::nullopt);
    EXPECT_EQ(
            line_of("lexer grammar A;\noptions { language = Cpp; }\n@members {\n"
                    "struct Inner final : Base { int x = f(); };\n}\nX : 'a' ;\n"),
            std::nullopt);

    // antlr 4.13.2 writes `definitions` into the source file after the header's include and `parser::header` into the
    // parser's files, so a macro either defines is not defined where the lexer's header initializes a field, and
    // `START` there is the name it spells; one the lexer's header defines stands for its replacement.
    const auto initialized{[](const std::string_view action) {
        return line_of(std::format(
                "grammar G;\noptions {{ language = Cpp; }}\n{} {{\n#define START (setMode(1), 0)\n}}\n"
                "@lexer::declarations {{\nint startup{{START}};\n}}\nr : X ;\nX : 'a' ;\n",
                action));
    }};

    EXPECT_EQ(initialized("@lexer::definitions"), std::nullopt);
    EXPECT_EQ(initialized("@parser::header"), std::nullopt);
    EXPECT_EQ(initialized("@lexer::header"), 6);
    EXPECT_EQ(initialized("@header"), 6);
}

TEST(Read_antlr_test, An_empty_match_beside_a_non_greedy_loop_is_read_as_ANTLR_reads_it)
{
    // An alternative that can match the empty string reaches the rule's end at the loop's decision wherever it stands:
    // antlr 4.13.2 emits Y, Y for `X : | .*? 'a' ;` on "aa" and X, X of one character for `X : .*? 'a' | ;`, where the
    // greedy reading spans both, so either is refused.
    for (const std::string_view rule : {"X : | .*? 'a' ;", "X : .*? 'a' | ;", "X : 'b'? | 'c'*? 'a' ;"})
    {
        const auto grammar{std::format("lexer grammar A;\n{}\nY : . ;\n", rule)};

        EXPECT_TRUE(refusal_of(grammar).contains("no alternative of which can match the empty string")) << rule;
    }

    // Whether an alternative matches the empty string is a formula over the rules it reaches, which the grammar's fixed
    // point answers, so a reference to a rule that matches none leaves the loop read: antlr 4.13.2 emits FIELD twice on
    // `"abc" word` for the grammar below.
    const auto field{scanner_of(
            "lexer grammar A;\nFIELD : '\"' .*? '\"' | WORD ;\nfragment WORD : [a-z]+ ;\nWS : [ ]+ -> skip ;\n")};

    const auto referenced{scanner_of("lexer grammar A;\nX : 'a'*? 'b' | F ;\nfragment F : 'c' ;\n")};

    EXPECT_EQ(field.rules.size(), 2U);
    EXPECT_EQ(referenced.rules.size(), 1U);

    // A reference to a rule that does match the empty string is nullable, and the loop is refused.
    EXPECT_TRUE(refusal_of("lexer grammar A;\nX : 'a'*? 'b' | F ;\nfragment F : 'c'? ;\nY : . ;\n")
                        .contains("no alternative of which can match the empty string"));
}

TEST(Read_antlr_test, A_line_comment_ends_at_a_bare_carriage_return)
{
    // A `//` comment ends at a carriage return as at a newline, as ANTLR's lexer ends it, so a grammar with bare
    // carriage returns for line ends keeps the rule after a comment.
    const auto spec{scanner_of("lexer grammar A;\rX : 'x' ; // comment\rY : 'y' ;\r")};

    EXPECT_EQ(spec.rules.size(), 2U);
}

TEST(Read_antlr_test, A_grammar_of_fragments_alone_declares_no_scanner)
{
    // A grammar of fragments alone declares no scanner, as a parser grammar does not.
    EXPECT_TRUE(read_antlr("lexer grammar A;\nfragment X : 'x' ;\n").empty());
}

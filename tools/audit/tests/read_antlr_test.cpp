#include "munch/tools/audit/read_antlr.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace munch::tools::audit;

namespace
{
/**
 * @brief The ANTLR idioms a combined grammar may hold: options, named actions, parser rules whose literals become
 *        implicit tokens, fragments, the commands such a grammar can carry, several alternatives under one rule,
 *        ranges, sets, negation, the dot, a non-greedy loop, escapes and an inert action inside a rule.
 *
 * Valid ANTLR as it stands, which `antlr4 Idioms.g4` accepts with no complaint: every command ends the single
 * outermost alternative of its rule, and no mode is declared, modes being a lexer grammar's alone.
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
 * @brief The line a grammar is refused at, or -1 when it is read.
 * @param source The grammar's text.
 * @return The line.
 */
long line_of(const std::string_view source)
{
    try
    {
        std::ignore = read_antlr(source);
    }
    catch (const Spec_error& error)
    {
        return static_cast<long>(error.line());
    }

    return -1L;
}

/**
 * @brief Why a grammar is refused, or nothing when it is read.
 * @param source The grammar's text.
 * @return The message.
 */
std::string refusal_of(const std::string_view source)
{
    try
    {
        std::ignore = read_antlr(source);
    }
    catch (const Spec_error& error)
    {
        return error.what();
    }

    return {};
}

/**
 * @brief The lengths of the successive tokens the lexer built from a grammar cuts an input into, a byte no token
 *        begins counted as a length of zero and stepped over.
 * @param grammar The grammar's text.
 * @param input The input.
 * @return The lengths.
 */
std::vector<std::size_t> lengths_of(const std::string_view grammar, const std::string_view input)
{
    const auto lexer{build(read_antlr(grammar).front(), "INITIAL")};

    std::vector<std::size_t> lengths;

    for (std::size_t at{0}; at < input.size();)
    {
        const auto match{lexer.tokenize<std::size_t>(input.begin() + static_cast<long>(at), input.end())};

        lengths.push_back(match.length);

        at += std::max<std::size_t>(match.length, 1);
    }

    return lengths;
}

} // namespace

TEST(Read_antlr, Reads_a_combined_grammar_with_its_idioms)
{
    const auto spec{read_antlr(idioms).front()};

    EXPECT_EQ(spec.line, 1u);
    EXPECT_EQ(spec.options, (std::vector<std::string>{"language=Cpp"}));

    // The parser's literals lead, except 'if', which IF spells; then the lexer rules in order, one rule each. The
    // parser rule named catchProduction is a rule, not an exception handler.
    ASSERT_EQ(spec.rules.size(), 13u);

    EXPECT_EQ(spec.rules[0].pattern, "'then'");
    EXPECT_EQ(spec.rules[0].expression, R"("then")");
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"'then'"});
    EXPECT_EQ(spec.rules[0].line, 12u);
    EXPECT_EQ(spec.rules[1].pattern, "'='");
    EXPECT_EQ(spec.rules[2].pattern, "';'");

    EXPECT_EQ(spec.rules[3].token, std::optional<std::string>{"IF"});
    EXPECT_EQ(spec.rules[3].expression, R"("if")");
    EXPECT_EQ(spec.rules[3].line, 16u);

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

    // A combined grammar declares no mode, so every rule is the default condition's, and the fragments are
    // definitions rather than rules.
    EXPECT_EQ(spec.rules[12].conditions, std::vector<std::string>{});
    EXPECT_TRUE(spec.conditions.empty());
    EXPECT_EQ(spec.definitions.at("LETTER"), "[A-Z_a-z]");
    EXPECT_EQ(spec.definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("IF"), R"("if")");
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
}

TEST(Read_antlr, A_parser_literal_is_the_token_of_the_lexer_rule_ANTLR_maps_it_onto)
{
    // The rules of a combined grammar whose parser uses 'x': the lexer rule given, then Y.
    const auto rules_of{[](const std::string_view rule) {
        return read_antlr("grammar A;\ns : 'x' Y EOF ;\n" + std::string{rule} + "\nY : 'y' ;\n").front().rules;
    }};

    // antlr 4.13.2 maps the parser's 'x' onto a rule that spells it as the literal alone, whatever comments stand
    // in the rule, with one action after it, or with one or two commands of which at most one takes an argument:
    // its token file names 'x' the rule's type and its lexer skips or hides the input as the rule says.
    for (const std::string_view rule :
         {"X : 'x' -> skip ;", "X : 'x' /* comment */ -> skip ;", "X : 'x' { /* inert */ } ;", "X : 'x' { } ;",
          "X : 'x' -> channel(HIDDEN), skip ;", "X : 'x' -> skip, type(Y) ;", "/** doc */ X : 'x' -> skip ;"})
    {
        const auto rules{rules_of(rule)};

        ASSERT_EQ(rules.size(), 2u) << rule;
        EXPECT_EQ(rules.front().line, 3u) << rule;
        EXPECT_EQ(rules.front().expression, R"("x")") << rule;
    }

    EXPECT_EQ(rules_of("X : 'x' -> skip ;").front().token, std::nullopt);
    EXPECT_EQ(rules_of("X : 'x' /* comment */ -> skip ;").front().token, std::nullopt);
    EXPECT_EQ(rules_of("X : 'x' { /* inert */ } ;").front().token, std::optional<std::string>{"X"});
    EXPECT_EQ(rules_of("X : 'x' -> skip, type(Y) ;").front().token, std::optional<std::string>{"Y"});

    // Any other shape spelling 'x' leaves the parser's literal an implicit token of its own ahead of every rule,
    // which antlr 4.13.2 numbers T__0 and emits for the input: a rule with options, an action and a command
    // together, two commands with arguments, three commands, a group, an action before the literal and two after.
    for (const std::string_view rule :
         {"X options { caseInsensitive = false; } : 'x' -> skip ;", "X : 'x' { } -> skip ;",
          "X : 'x' -> channel(HIDDEN), type(Y) ;", "X : 'x' -> channel(HIDDEN), type(Y), skip ;", "X : ('x') -> skip ;",
          "X : { } 'x' ;", "X : 'x' { } { } ;", "fragment X : 'x' ;"})
    {
        const auto rules{rules_of(rule)};

        ASSERT_GE(rules.size(), 2u) << rule;
        EXPECT_EQ(rules.front().pattern, "'x'") << rule;
        EXPECT_EQ(rules.front().token, std::optional<std::string>{"'x'"}) << rule;
        EXPECT_EQ(rules.front().line, 2u) << rule;
    }

    // The literal is matched as written, so an escape spells itself and a folded grammar's literal spells its
    // rule's; and a literal a lexer rule spells in a mode of its own is no combined grammar's concern.
    EXPECT_EQ(read_antlr("grammar A;\ns : '\\n' Y EOF ;\nNL : '\\n' -> skip ;\nY : 'y' ;\n").front().rules.size(), 2u);
    EXPECT_EQ(
            read_antlr("grammar A;\noptions { caseInsensitive = true; }\ns : 'x' Y EOF ;\nX : 'x' -> skip ;\n"
                       "Y : 'y' ;\n")
                    .front()
                    .rules.front()
                    .token,
            std::nullopt);

    // Two rules spelling the parser's literal leave ANTLR no token to map it onto, its error 126 at the parser's
    // use of the literal; two rules spelling a literal no parser rule uses are its warning 184 and read as ever.
    EXPECT_EQ(line_of("grammar A;\ns : 'x' Y EOF ;\nX : 'x' -> skip ;\nZ : 'x' ;\nY : 'y' ;\n"), 2);
    EXPECT_TRUE(refusal_of("grammar A;\ns : 'x' Y EOF ;\nX : 'x' -> skip ;\nZ : 'x' ;\nY : 'y' ;\n")
                        .contains("cannot create implicit token for string literal in non-combined grammar: 'x'"));
    EXPECT_EQ(line_of("grammar A;\ns : Y EOF ;\nX : 'x' -> skip ;\nZ : 'x' ;\nY : 'y' ;\n"), -1);
}

TEST(Read_antlr, A_parser_rule_argument_block_is_skipped_as_ANTLR_lexes_it)
{
    // The patterns of the rules of a combined grammar holding the parser rules given, then A and B.
    const auto patterns_of{[](const std::string_view rules) {
        const auto spec{read_antlr("grammar P;\n" + std::string{rules} + "\nA : 'a' ;\nB : 'b' ;\n").front()};

        std::vector<std::string> patterns;

        for (const auto& rule : spec.rules)
        {
            patterns.push_back(rule.pattern);
        }

        return patterns;
    }};

    // antlr 4.13.2 lexes the block between a parser rule's brackets as one ARG_ACTION, in which brackets nest and a
    // "..." or a '...' is skipped whole, a backslash escaping the byte after it: a `]` or a literal inside a quoted
    // string is no part of the grammar, so `r[const char* p="]'x'"]` uses no 'x' and the input x is no token, where
    // reading up to the first `]` would make 'x' an implicit token ahead of every rule.
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
    const auto lexer{build(
            read_antlr("grammar P;\nr[const char* p=\"]'x'\"] : A ;\nA : 'a' ;\nB : 'b' ;\n").front(), "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"x"}).length, 0u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"a"}).token, std::optional<std::size_t>{0});

    // A block never closed is refused at the rule's line.
    EXPECT_EQ(line_of("grammar P;\nr[const char* p=\"]\" : A ;\nA : 'a' ;\n"), 2);
}

TEST(Read_antlr, Reads_a_lexer_grammar_with_its_modes)
{
    const auto spec{read_antlr(nested).front()};

    EXPECT_EQ(spec.line, 1u);
    ASSERT_EQ(spec.rules.size(), 5u);

    // The rules before the first `mode` line are the default mode's, the ones after it the mode's, and the mode
    // commands are kept as the action's text.
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"OPEN"});
    EXPECT_EQ(spec.rules[0].action, "-> pushMode(INNER)");
    EXPECT_EQ(spec.rules[0].conditions, std::vector<std::string>{});
    EXPECT_EQ(spec.rules[1].token, std::nullopt);
    EXPECT_EQ(spec.rules[2].line, 8u);
    EXPECT_EQ(spec.rules[2].action, "-> popMode");
    EXPECT_EQ(spec.rules[2].conditions, (std::vector<std::string>{"INNER"}));
    EXPECT_EQ(spec.rules[3].expression, R"([\u{0}-\u{7c}\u{7e}-\u{7f}\u{80}-\u{10ffff}]+)");
    EXPECT_EQ(spec.rules[4].expression, R"([*+\-])");

    ASSERT_EQ(spec.conditions.size(), 1u);
    EXPECT_EQ(spec.conditions.front().name, "INNER");
    EXPECT_TRUE(spec.conditions.front().exclusive);
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(active_rules(spec, "INNER"), (std::vector<std::size_t>{2, 3, 4}));
}

TEST(Read_antlr, Case_insensitivity_doubles_every_letter)
{
    const auto spec{read_antlr("lexer grammar Ci;\noptions { caseInsensitive = true; }\nKW : 'if' ;\nID : [a-z]+ ;\n"
                               "EXACT options { caseInsensitive = false; } : 'x' ;\n")
                            .front()};

    ASSERT_EQ(spec.rules.size(), 3u);
    EXPECT_EQ(spec.rules[0].expression, "[iI][fF]");
    EXPECT_EQ(spec.rules[1].expression, "[A-Za-z]+");
    EXPECT_EQ(spec.rules[2].expression, R"("x")");

    // The dot admits every scalar in either case already, so folding it asks for no mapping and it reads as ever.
    const auto every{read_antlr("lexer grammar D;\noptions { caseInsensitive = true; }\nX : . ;\n").front()};

    EXPECT_EQ(every.rules.front().expression, R"([\u{0}-\u{7f}\u{80}-\u{10ffff}])");
}

TEST(Read_antlr, A_range_under_case_insensitivity_folds_by_its_ends_as_ANTLR_folds_it)
{
    const auto expression_of{[](const std::string_view atom) {
        return read_antlr("lexer grammar R;\noptions { caseInsensitive = true; }\nA : " + std::string{atom} + " ;\n")
                .front()
                .rules.front()
                .expression;
    }};

    // antlr 4.13.2 folds a range by its two ends alone: where both are letters of one case the copy in the other
    // case is added, and where they differ in case or are no letters the range stands as written, its warning 185
    // remarking on the first two. So `[b-y]` and `'b'..'y'` take B and Y and not A, `[A-t]` and `'A'..'t'` take U
    // and a and not u, `[0-Z]` takes A and not a, `[a-]` takes a and not A, and a member folds on its own.
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
    EXPECT_EQ(expression_of("[\\u0061-\\u007f]"), R"([a-\x7f])");
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

    const auto lexer{build(read_antlr(mixed).front(), "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"u"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"U"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"a"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(
            read_antlr("lexer grammar R;\nA options { caseInsensitive = true; } : [A-t] ;\n")
                    .front()
                    .rules.front()
                    .expression,
            "[A-t]");
}

TEST(Read_antlr, An_option_is_read_as_its_one_token_whatever_comments_stand_beside_it)
{
    // antlr 4.13.2 reads an option's value with the lexer it reads the grammar with, so a comment after the value is
    // no part of it: each of these folds "AB" onto 'ab', at the grammar and on the rule alike.
    const auto folded{read_antlr("grammar O;\noptions { caseInsensitive = true /* match either case */; }\n"
                                 "s : 'ab' ;\nB : . ;\n")
                              .front()};

    EXPECT_EQ(folded.options, (std::vector<std::string>{"caseInsensitive=true"}));
    EXPECT_EQ(folded.rules.front().expression, "[aA][bB]");

    const auto on_the_rule{
            read_antlr("lexer grammar O;\nA options { caseInsensitive = true /* match either case */; } : 'ab' ;\n")
                    .front()};

    EXPECT_EQ(on_the_rule.rules.front().expression, "[aA][bB]");

    // A name, a dotted name, a quoted string and a brace block are the values ANTLR's own lexer reads, each one
    // token, and the blanks and comments around the `=` are no part of any.
    const auto forms{read_antlr("lexer grammar O;\noptions { language = Cpp; superClass = a.b.C; "
                                "tokenVocab = 'Lex'; contextSuperClass = {x::Y}; "
                                "caseInsensitive /* here */ = /* there */ true; }\nA : 'a' ;\n")
                             .front()};

    const std::vector<std::string> recorded{
            "language=Cpp", "superClass=a.b.C", "tokenVocab='Lex'", "contextSuperClass={x::Y}", "caseInsensitive=true"};

    EXPECT_EQ(forms.options, recorded);
    EXPECT_EQ(forms.rules.front().expression, "[aA]");

    // ANTLR takes `true` and `false` and no other spelling: `TRUE` and `untrue` are its warning 84 and set nothing,
    // so the grammar's option stays off and a rule's option leaves the grammar's in force.
    const auto expression_of{[](const std::string_view options) {
        return read_antlr("lexer grammar O;\n" + std::string{options} + "\nA : 'a' ;\n")
                .front()
                .rules.front()
                .expression;
    }};

    EXPECT_EQ(expression_of("options { caseInsensitive = TRUE; }"), R"("a")");
    EXPECT_EQ(expression_of("options { caseInsensitive = untrue; }"), R"("a")");
    EXPECT_EQ(expression_of("options { caseInsensitive = true; }"), "[aA]");

    const auto rule_expression_of{[](const std::string_view options) {
        return read_antlr(
                       "lexer grammar O;\noptions { caseInsensitive = true; }\nA " + std::string{options} +
                       " : 'a' ;\n")
                .front()
                .rules.front()
                .expression;
    }};

    EXPECT_EQ(rule_expression_of("options { caseInsensitive = untrue; }"), "[aA]");
    EXPECT_EQ(rule_expression_of("options { caseInsensitive = TRUE; }"), "[aA]");
    EXPECT_EQ(rule_expression_of("options { caseInsensitive = false; }"), R"("a")");

    EXPECT_EQ(line_of("lexer grammar O;\noptions { caseInsensitive = ; }\nA : 'a' ;\n"), 2);
}

TEST(Read_antlr, Sets_beyond_ascii_and_negated_groups_read_as_scalars)
{
    // A negated group may hold a range with blanks around its operator, as Clojure's grammar writes one.
    const auto spec{read_antlr("lexer grammar U;\nLATIN : [a-z\\u00C0-\\u00FF]+ ;\nSPAN : '\\u0300'..'\\u036F' ;\n"
                               "REST : ~('\\r' | '\\n' | [ \\t]) ;\nBOM : '\\uFEFF' ;\n"
                               "HEAD : ~('0' .. '9' | '^' | '\\u00C0'..'\\u00FF') ;\n")
                            .front()};

    ASSERT_EQ(spec.rules.size(), 5u);
    EXPECT_EQ(spec.rules[0].expression, R"([\u{61}-\u{7a}\u{c0}-\u{ff}]+)");
    EXPECT_EQ(spec.rules[1].expression, R"([\u{300}-\u{36f}])");
    EXPECT_EQ(spec.rules[2].expression, R"([\u{0}-\u{8}\u{b}-\u{c}\u{e}-\u{1f}\u{21}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(spec.rules[3].expression, R"("\xef\xbb\xbf")");
    EXPECT_EQ(spec.rules[4].expression, R"([\u{0}-\u{2f}\u{3a}-\u{5d}\u{5f}-\u{7f}\u{80}-\u{bf}\u{100}-\u{10ffff}])");

    // Built, the scalars match as their encodings.
    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"caf\xc3\xa9"}).length, 5u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xcc\x81"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xe2\x82\xac"}).token, std::optional<std::size_t>{2});
}

TEST(Read_antlr, A_surrogate_pair_is_the_character_it_encodes_and_a_surrogate_alone_never_matches)
{
    const auto rule_of{[](const std::string_view body) {
        return read_antlr("lexer grammar S;\nA : " + std::string{body} + " ;\nB : . ;\n").front();
    }};

    const auto expression_of{
            [&rule_of](const std::string_view body) { return rule_of(body).rules.front().expression; }};

    const auto refusal{[](const std::string_view body) {
        return refusal_of("lexer grammar S;\nA : " + std::string{body} + " ;\nB : . ;\n");
    }};

    // antlr 4.13.2 reads a literal into a UTF-16 string and walks it by code point, so a high surrogate escape and a
    // low one after it are the character the pair encodes: `'\uD83D\uDE00'` matches the four bytes of U+1F600, as
    // `'\u{1F600}'` and the character written out do, and not the six bytes of the two surrogates encoded apart.
    EXPECT_EQ(expression_of("'\\uD83D\\uDE00'"), "\"\\xf0\\x9f\\x98\\x80\"");
    EXPECT_EQ(expression_of("'\\u{1F600}'"), R"("\xf0\x9f\x98\x80")");
    EXPECT_EQ(expression_of("'\xf0\x9f\x98\x80'"), R"("\xf0\x9f\x98\x80")");
    EXPECT_EQ(expression_of("'a\\uD83D\\uDE00b'"), "\"a\\xf0\\x9f\\x98\\x80b\"");

    const auto pair{build(rule_of("'\\uD83D\\uDE00'"), "INITIAL")};

    EXPECT_EQ(pair.tokenize<std::size_t>(std::string{"\xf0\x9f\x98\x80"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(pair.tokenize<std::size_t>(std::string{"\xf0\x9f\x98\x80"}).length, 4u);
    EXPECT_EQ(pair.tokenize<std::size_t>(std::string{"\xed\xa0\xbd\xed\xb8\x80"}).length, 0u);

    // A surrogate standing alone is a transition on a code point no UTF-8 input decodes to, which antlr 4.13.2
    // accepts and never matches, the emoji and the letter after it going to B; the byte reading has no literal that
    // never matches, so each is refused at its line: a high one alone, a low one alone, the pair reversed, a pair
    // and then a high one, and two highs before the low.
    for (const std::string_view body :
         {"'\\uD83D'", "'\\uDE00'", "'\\uDE00\\uD83D'", "'\\uD83Dx'", "'\\uD83D\\uDE00\\uD83D'",
          "'\\uD83D\\uD83D\\uDE00'", "'\\u{D83D}'", "~'\\uD83D'", "'\\uD83D'..'\\uDE00'"})
    {
        EXPECT_EQ(line_of("lexer grammar S;\nA : 'q' ;\nB : " + std::string{body} + " ;\n"), 3) << body;
        EXPECT_TRUE(refusal(body).contains("lone surrogate")) << body << ": " << refusal(body);
    }

    // In a set the escapes stay apart, so `[\uD83D\uDE00]` holds two surrogates and not the emoji: a member that is
    // one matches nothing and is left out, as antlr 4.13.2 matches x and not the emoji for `[\uD83Dx]`, a set of
    // nothing else is refused, a span past them keeps what lies on either side, U+D7FF and U+E000 matching and the
    // emoji not, and a negation admits everything else, every character for `~[\uD83D]`.
    EXPECT_EQ(expression_of("[\\uD83Dx]"), "[x]");
    EXPECT_EQ(expression_of("[\\uD7FF-\\uE000]"), "[\\u{d7ff}-\\u{e000}]");
    EXPECT_EQ(expression_of("~[\\uD83D]"), "[\\u{0}-\\u{7f}\\u{80}-\\u{10ffff}]");
    EXPECT_EQ(expression_of("[\xf0\x9f\x98\x80]"), "[\\u{1f600}]");

    const auto span{build(rule_of("[\\uD7FF-\\uE000]"), "INITIAL")};

    EXPECT_EQ(span.tokenize<std::size_t>(std::string{"\xed\x9f\xbf"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(span.tokenize<std::size_t>(std::string{"\xee\x80\x80"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(span.tokenize<std::size_t>(std::string{"\xf0\x9f\x98\x80"}).token, std::optional<std::size_t>{1});

    for (const std::string_view body : {"[\\uD83D\\uDE00]", "[\\uD83D-\\uDE00]", "[\\uD83D]"})
    {
        EXPECT_EQ(line_of("lexer grammar S;\nA : 'q' ;\nB : " + std::string{body} + " ;\n"), 3) << body;
        EXPECT_TRUE(refusal(body).contains("holds nothing but surrogates")) << body << ": " << refusal(body);
    }

    EXPECT_EQ(refusal("[]"), "line 2: string literals and sets cannot be empty: []");
    EXPECT_EQ(
            read_antlr("lexer grammar S;\noptions { caseInsensitive = true; }\nA : [\\uD83D\\uDE00a] ;\n")
                    .front()
                    .rules.front()
                    .expression,
            "[Aa]");

    // Where antlr 4.13.2 needs one character, a range's end or a negated literal, it reads the literal's text and
    // takes one UTF-16 unit or one escape as one character: a pair of escapes, a character beyond the basic plane
    // written out, two characters and no character are its error 144, in its words, where `'\u{1F600}'` is one.
    EXPECT_EQ(
            refusal("~'\\uD83D\\uDE00'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\\uD83D\\uDE00'");
    EXPECT_EQ(
            refusal("'\\uD83D\\uDE00'..'\\uD83D\\uDE01'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\\uD83D\\uDE00'");
    EXPECT_EQ(
            refusal("'a'..'\\uD83D\\uDE01'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\\uD83D\\uDE01'");
    EXPECT_EQ(
            refusal("~'\xf0\x9f\x98\x80'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\xf0\x9f\x98\x80'");
    EXPECT_EQ(
            refusal("'\xf0\x9f\x98\x80'..'\xf0\x9f\x98\x81'"),
            "line 2: multi-character literals are not allowed in lexer sets: '\xf0\x9f\x98\x80'");
    EXPECT_EQ(refusal("~('x' | 'ab')"), "line 2: multi-character literals are not allowed in lexer sets: 'ab'");
    EXPECT_EQ(refusal("''..'z'"), "line 2: multi-character literals are not allowed in lexer sets: ''");
    EXPECT_EQ(expression_of("'\\u{1F600}'..'\\u{1F601}'"), "[\\u{1f600}-\\u{1f601}]");
    EXPECT_EQ(expression_of("~'\\u{1F600}'"), "[\\u{0}-\\u{7f}\\u{80}-\\u{1f5ff}\\u{1f601}-\\u{10ffff}]");

    // A range whose end is below its start is its error 174, in its words.
    EXPECT_EQ(refusal("'z'..'a'"), "line 2: string literals and sets cannot be empty: 'z'..'a'");
    EXPECT_EQ(refusal("~('z'..'a')"), "line 2: string literals and sets cannot be empty: 'z'..'a'");
}

TEST(Read_antlr, A_non_greedy_loop_stops_where_the_rest_of_the_rule_first_matches)
{
    // ANTLR's rule is the fewest loop characters that still let the rest of the rule match, so the body neither runs
    // over an occurrence of the rest nor ends where the rest's own bytes complete one: `.*? 'aa'` on "aaa" matches
    // "aa" and leaves the third byte to B, where the loop over what holds no whole "aa" would take all three.
    const auto overlapping{read_antlr("lexer grammar N;\nA : .*? 'aa' ;\nB : 'a' ;\n").front()};

    const auto stopped{build(overlapping, "INITIAL").tokenize<std::size_t>(std::string{"aaa"})};

    EXPECT_EQ(stopped.length, 2u);
    EXPECT_EQ(stopped.token, std::optional<std::size_t>{0});

    // What the loop stops at is the whole rest of the rule, not the element after it: `.*? 'a' 'b'` stops at "ab",
    // so "aab" is one token of A rather than a byte of B and then "ab".
    const auto rest{read_antlr("lexer grammar N;\nA : .*? 'a' 'b' ;\nB : . ;\n").front()};

    const auto whole{build(rest, "INITIAL").tokenize<std::size_t>(std::string{"aab"})};

    EXPECT_EQ(whole.length, 3u);
    EXPECT_EQ(whole.token, std::optional<std::size_t>{0});

    // A `+?` loop has one character before the rest can stop it, so "aaa" is one token of A; from the second
    // character on the overlap holds, so "aaaa" is A("aaa") and a byte left over rather than one token of four.
    const auto least{build(read_antlr("lexer grammar N;\nA : .+? 'aa' ;\nB : 'a' ;\n").front(), "INITIAL")};

    EXPECT_EQ(least.tokenize<std::size_t>(std::string{"aaa"}).length, 3u);
    EXPECT_EQ(least.tokenize<std::size_t>(std::string{"aaaa"}).length, 3u);

    // Where the loop admits only what the terminator begins with, the body can be nothing but empty and the
    // terminator alone is the match, as ANTLR's own lexer has it: "aaa" is A("aa") and a byte left over.
    const auto degenerate{read_antlr("lexer grammar N;\nA : 'a'*? 'aa' ;\nB : . ;\n").front()};

    EXPECT_EQ(degenerate.rules.front().expression, R"("aa")");
    EXPECT_EQ(build(degenerate, "INITIAL").tokenize<std::size_t>(std::string{"aaa"}).length, 2u);

    const auto once{read_antlr("lexer grammar N;\nA : 'a'+? 'a' ;\nB : . ;\n").front()};

    EXPECT_EQ(build(once, "INITIAL").tokenize<std::size_t>(std::string{"aaa"}).length, 2u);

    // A terminator that overlaps no prefix of itself leaves the block comment as it was, the loop over what holds
    // no whole "*/".
    const auto comment{read_antlr("lexer grammar N;\nC : '/*' .*? '*/' ;\n").front()};

    EXPECT_EQ(build(comment, "INITIAL").tokenize<std::size_t>(std::string{"/* a */ /* b */"}).length, 7u);

    // A literal of several characters aligns the loop to its length, and the rest cannot begin with its first byte,
    // so the greedy loop is the same language: antlr 4.13.2 reads all of "ababc" as A, as this does.
    const auto aligned{read_antlr("lexer grammar N;\nA : 'ab'*? 'c' ;\nB : . ;\n").front()};

    EXPECT_EQ(aligned.rules.front().expression, R"(("ab")*"c")");
    EXPECT_EQ(build(aligned, "INITIAL").tokenize<std::size_t>(std::string{"ababc"}).length, 5u);

    // A folded literal's matches have its length too, so `caseInsensitive` changes nothing here: antlr 4.13.2
    // takes all of "aBab/" as A, and so does this.
    const auto folded{
            read_antlr("lexer grammar N;\noptions { caseInsensitive = true; }\nA : 'ab'*? '/' ;\nB : . ;\n").front()};

    EXPECT_EQ(folded.rules.front().expression, R"(([aA][bB])*"/")");
    EXPECT_EQ(build(folded, "INITIAL").tokenize<std::size_t>(std::string{"aBab/"}).length, 5u);
}

TEST(Read_antlr, A_non_greedy_loop_is_read_where_no_earlier_path_through_the_rule_can_stop_it)
{
    // antlr 4.13.2 follows the paths through the elements before the loop in the order their alternatives give
    // them, and the first to reach the rule's end stops the loop on every later one: `('a'|'aa') .*? 'a'` cuts "aaa"
    // into "aa" and "a", `('aa'|'a') .*? 'a'` takes all three, and `('a'|'aaa') .*? 'a'` takes all of "aaaa", which
    // no greedy rewrite of the group keeps apart, so a loop after elements of more than one length is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aa') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('aa'|'a') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aaa') .*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a'? .*? 'b' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|) .*? 'b' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F .*? 'b' ;\nY : . ;\nfragment F : 'a' ;\n"), 2);

    // Elements of one length reach the loop at one character together, whatever their order, and the loop is read
    // as before: antlr 4.13.2 takes all of "acxb", "abb" and "acb" as X, as this does.
    constexpr std::string_view aligned{"lexer grammar A;\nX : ('ab'|'ac') .*? 'b' ;\nY : . ;\n"};

    EXPECT_EQ(lengths_of(aligned, "acxb"), (std::vector<std::size_t>{4}));
    EXPECT_EQ(lengths_of(aligned, "abb"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(aligned, "acb"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : [ab] '\\u00E9' { } .*? 'b' ;\nY : . ;\n"), -1);

    // An earlier alternative of the rule is an earlier path too: `'ab' | 'a' .*? 'c'` cuts "abc" into "ab" and "c",
    // the first alternative's end stopping the second's loop, where `'a' .*? 'c' | 'ab'` takes all three; so the
    // loop is read where no character begins both an earlier alternative and its own.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'ab' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F | 'a' .*? 'c' ;\nY : . ;\nfragment F : 'x' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : '\\u00E9' | '\\u00FC' .*? 'c' ;\nY : . ;\n"), 2);

    // What an earlier alternative can begin with reaches past every element of it that can match the empty string,
    // a group with an empty alternative among them: antlr 4.13.2 cuts "abc" into "a" and two bytes for each of
    // these, the first alternative's 'a' ending the rule before the loop, so each is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : (('b'|)) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'?) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'*) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : { } ('b'|) ('d'|) 'a' | 'a' .*? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'a' | 'a' 'x'?? 'c' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'|) 'x' | 'a' .*? 'c' ;\nY : . ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F 'x' | 'a' .*? 'c' ;\nY : . ;\nfragment F : 'b' | ;\n"), 2);

    constexpr std::string_view leading{"lexer grammar A;\nX : 'a' .*? 'c' | 'ab' ;\nY : . ;\n"};

    EXPECT_EQ(lengths_of(leading, "abc"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(leading, "abbc"), (std::vector<std::size_t>{4}));
    EXPECT_EQ(lengths_of(leading, "ac"), (std::vector<std::size_t>{2}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | 'a' .*? 'b' ;\nY : . ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | '\\u00FC' .*? 'c' ;\nY : . ;\n"), -1);
}

TEST(Read_antlr, A_non_greedy_option_is_read_over_a_body_of_one_length_after_elements_of_one_length)
{
    // antlr 4.13.2 takes the body's alternatives in order after the bypass, and the first to reach the rule's end
    // stops the others: `('x'|'xa')?? 'a'` cuts "xaa" into "xa" and "a" where `('xa'|'x')?? 'a'` takes all three,
    // and the greedy option over either group takes all three; so a body of more than one length is refused, as is
    // one an earlier path can stop, `('a'|'aa') ('x')?? 'a'` on "aaxa" ending at the second character.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'xa')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('xa'|'x')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a'|'aa') ('x')?? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F?? 'a' ;\nY : . ;\nfragment F : 'x' ;\n"), 2);

    // A body of one length reaches the rest at one character however its alternatives stand, and the bypass never
    // sees the body's first character, so the greedy option is the same language: antlr 4.13.2 takes all of "xaa"
    // and "xba", "a" alone, and cuts "xa" into "x" for Y and "a" for X, as this does.
    constexpr std::string_view fixed{"lexer grammar A;\nX : ('xa'|'xb')?? 'a' ;\nY : . ;\n"};

    EXPECT_EQ(read_antlr(fixed).front().rules.front().expression, R"(("xa"|"xb")?"a")");
    EXPECT_EQ(lengths_of(fixed, "xaa"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(fixed, "xba"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(lengths_of(fixed, "a"), (std::vector<std::size_t>{1}));
    EXPECT_EQ(lengths_of(fixed, "xa"), (std::vector<std::size_t>{1, 1}));
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x'?? 'a' ;\nY : . ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [xy]?? 'a' ;\nY : . ;\n"), -1);
}

TEST(Read_antlr, A_non_greedy_loop_over_a_group_is_refused_rather_than_read_as_the_greedy_one)
{
    // ANTLR stops the loop at the fewest characters that let the rest match and takes the body's alternatives in
    // order, so on "xaa" the first of these matches "xa" and the second all three, where a greedy rewrite over the
    // group reads all three either way. The order of the alternatives is the whole difference, which the rewriting
    // cannot see, so a group body is refused.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'xa')*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('xa'|'x')*? 'a' ;\nY : . ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x'|'yz')*? 'a' ;\nY : . ;\n"), 2);

    // A group of one alternative goes the same way: what the loop admits is read off the atom, and a group's
    // matches are not one length whatever stands inside it.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('x')+? 'a' ;\nY : . ;\n"), 2);

    // A body of one character, the dot and a set among them, is read as before.
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a' ;\nY : . ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [ab]*? 'c' ;\nY : . ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a'..'f'*? 'g' ;\nY : . ;\n"), -1);
}

TEST(Read_antlr, A_closure_over_a_body_that_can_match_the_empty_string_is_refused_as_ANTLR_rejects_it)
{
    // antlr 4.13.2 rejects each of these with its error 153, "rule A contains a closure with at least one
    // alternative that can match an empty string", at the line of the rule holding the closure: the empty
    // alternative, the nullable group and the nullable rule alike, greedy or not.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )*? 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )* 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )+ 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )+? 'a' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'?)* 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'*)* 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b'? 'c'?)* 'd' ;\n"), 2);

    EXPECT_TRUE(refusal_of("lexer grammar A;\nX : ('b' | )* 'a' ;\n").contains("the rule X contains a closure"))
            << refusal_of("lexer grammar A;\nX : ('b' | )* 'a' ;\n");

    // A body that reaches a rule waits for the whole grammar, the rule being able to stand below the closure, and
    // the answer runs through as many rules as reach it.
    EXPECT_EQ(line_of("lexer grammar A;\nfragment E : 'x'? ;\nX : E* 'b' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : 'x'? ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E+ 'b' ;\nfragment E : 'x' | ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : F ;\nfragment F : 'x'? ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : B* 'y' ;\nB : 'x'? ;\n"), 2);

    // What ANTLR accepts reads as ever: an optional over an empty alternative, a closure over a rule that matches
    // no empty string, and the closures of the study's own grammars.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('b' | )? 'a' ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : E* 'b' ;\nfragment E : 'x' ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : L (L | D)* ;\nfragment L : [a-z] ;\nfragment D : [0-9] ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : D+ ('.' D+)? ;\nfragment D : [0-9] ;\n"), -1);
}

TEST(Read_antlr, A_command_is_read_as_its_name_whatever_comments_stand_beside_it)
{
    const auto rule_of{[](const std::string_view clause) {
        return read_antlr("lexer grammar A;\nX : 'a' " + std::string{clause} + "\nY : 'b' ;\n").front().rules.front();
    }};

    // antlr 4.13.2 skips the token in each of these, the comment being no part of the command: a block comment, one
    // holding the `;` that ends the rule, and a line comment before the `;` on the next line.
    EXPECT_EQ(rule_of("-> skip /* comment */ ;").token, std::nullopt);
    EXPECT_EQ(rule_of("-> skip /* ; */ ;").token, std::nullopt);
    EXPECT_EQ(rule_of("-> skip // to the line end\n;").token, std::nullopt);

    // The clause is kept as written, comments and all, since it is the account to hold against the grammar.
    EXPECT_EQ(rule_of("-> skip /* comment */ ;").action, "-> skip /* comment */");

    // A type command names its token whatever stands around it, and `-> more` is refused however it is spelled.
    EXPECT_EQ(rule_of("-> type ( B ) ;").token, std::optional<std::string>{"B"});
    EXPECT_EQ(rule_of("-> type(/* the parser's own */ B) ;").token, std::optional<std::string>{"B"});
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more /* combine prefix */ ;\nY : 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more, pushMode(M) ;\nY : 'b' ;\nmode M;\nZ : 'c' ;\n"), 2);

    // The mode commands stay what they were, kept as the action's text.
    EXPECT_EQ(rule_of("-> pushMode(M) ;\nmode M;\nZ : 'c' ;").action, "-> pushMode(M)");
}

TEST(Read_antlr, An_escape_ANTLR_has_not_got_is_refused_in_its_words)
{
    const auto refusal{[](const std::string_view body) {
        return refusal_of("lexer grammar E;\nA : " + std::string{body} + " ;\nB : 'b' ;\n");
    }};

    const auto expression_of{[](const std::string_view body) {
        return read_antlr("lexer grammar E;\nA : " + std::string{body} + " ;\nB : 'b' ;\n")
                .front()
                .rules.front()
                .expression;
    }};

    // antlr 4.13.2's lexer takes \b \t \n \f \r \' \\ and the two Unicode escapes in a literal, and its set reading
    // takes \b \t \n \f \r \\ \] \- \p{...} and the two Unicode escapes in a set: any other escape is its error 156, in
    // its words, the sequence as written, `\q` anywhere, `\]`, `\-`, `\"`, `\ ` and `\p` in a literal, `\'` and `\"` in
    // a set, and a character beyond ASCII whole.
    EXPECT_EQ(refusal("'\\q'"), "line 2: invalid escape sequence \\q");
    EXPECT_EQ(refusal("[\\q]"), "line 2: invalid escape sequence \\q");
    EXPECT_EQ(refusal("'\\]'"), "line 2: invalid escape sequence \\]");
    EXPECT_EQ(refusal("'\\-'"), "line 2: invalid escape sequence \\-");
    EXPECT_EQ(refusal("'\\\"'"), "line 2: invalid escape sequence \\\"");
    EXPECT_EQ(refusal("'\\ '"), "line 2: invalid escape sequence \\ ");
    EXPECT_EQ(refusal("'\\p{L}'"), "line 2: invalid escape sequence \\p");
    EXPECT_EQ(refusal("[\\']"), "line 2: invalid escape sequence \\'");
    EXPECT_EQ(refusal("[\\\"]"), "line 2: invalid escape sequence \\\"");
    EXPECT_EQ(refusal("'\\\xc3\xa9'"), "line 2: invalid escape sequence \\\xc3\xa9");
    EXPECT_EQ(refusal("'\\q'..'z'"), "line 2: invalid escape sequence \\q");
    EXPECT_EQ(refusal("~'\\q'"), "line 2: invalid escape sequence \\q");
    EXPECT_EQ(refusal("'a\\q'"), "line 2: invalid escape sequence \\q");
    EXPECT_EQ(refusal_of("grammar E;\nr : 'a\\q' ;\nX : 'x' ;\n"), "line 2: invalid escape sequence \\q");

    // The escapes it takes read as ever, each context's own; a property class in a set is refused for the tables the
    // library has not got, not as an escape ANTLR has not got.
    EXPECT_EQ(expression_of("'\\''"), R"("'")");
    EXPECT_EQ(expression_of("'\\\\'"), R"("\\")");
    EXPECT_EQ(line_of("lexer grammar E;\nA : [\\]\\-\\\\] ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar E;\nA : '\\b\\t\\n\\f\\r' ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar E;\nA : [\\b\\t\\n\\f\\r] ;\n"), -1);
    EXPECT_TRUE(refusal("[\\p{L}]").contains("Unicode property")) << refusal("[\\p{L}]");

    // antlr 4.13.2's lexer counts a braced escape's digits from the literal's opening quote, so one whose closing
    // brace stands twelve or more UTF-16 units past the quote is its error 156 too, the literal through the brace
    // its words: `'abcde\u{41}'` reads and `'abcdef\u{41}'` does not, two emoji before the escape count four units and
    // three emoji six, a second braced escape in one literal is always too far, and a set counts nothing.
    EXPECT_EQ(expression_of("'abcde\\u{41}'"), R"("abcdeA")");
    EXPECT_EQ(refusal("'abcdef\\u{41}'"), "line 2: invalid escape sequence 'abcdef\\u{41}");
    EXPECT_EQ(expression_of("'ab\\u{1F600}'"), R"("ab\xf0\x9f\x98\x80")");
    EXPECT_EQ(refusal("'abc\\u{1F600}'"), "line 2: invalid escape sequence 'abc\\u{1F600}");
    EXPECT_EQ(expression_of("'\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}'"), R"("\xf0\x9f\x98\x80\xf0\x9f\x98\x80A")");
    EXPECT_EQ(
            refusal("'\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}'"),
            "line 2: invalid escape sequence '\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\\u{41}");
    EXPECT_EQ(
            expression_of("'\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\\u{41}'"),
            R"("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9A")");
    EXPECT_EQ(line_of("lexer grammar E;\nA : '\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\\u{41}' ;\n"), 2);
    EXPECT_EQ(refusal("'\\u{1F600}\\u{1F600}'"), "line 2: invalid escape sequence '\\u{1F600}\\u{1F600}");
    EXPECT_EQ(line_of("lexer grammar E;\nA : [abcdefghijk\\u{41}] ;\n"), -1);
    EXPECT_EQ(line_of("lexer grammar E;\nA : 'abcdefghijk\\u0041' ;\n"), -1);
}

TEST(Read_antlr, A_command_ANTLR_has_not_got_is_refused_in_its_words)
{
    // The refusal as the report prints it, the line first: the clause stands on the third line of the grammar.
    const auto refusal{[](const std::string_view clause) {
        return refusal_of(
                "lexer grammar A;\nX : 'a'\n  " + std::string{clause} + " ;\nY : 'b' ;\nmode M;\nZ : 'c' ;\n");
    }};

    // antlr 4.13.2 knows skip, more, popMode, type, channel, mode and pushMode and no other command: `frob` is its
    // error 149, in the words it uses, at the line the command stands on, whether alone or after a known one.
    const std::string unknown{"line 3: lexer command frob does not exist or is not supported by the current target"};

    EXPECT_EQ(refusal("-> frob"), unknown);
    EXPECT_EQ(refusal("-> skip, frob"), unknown);
    EXPECT_EQ(refusal("-> frob(Q)"), unknown);

    // The four commands that take an argument refuse to go without one, its error 150, and the three that take
    // none refuse one, its error 151.
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
    // rejects them as a syntax error at the `)`, in these words, whichever command they follow, before it looks at
    // the command; the line named is the `)`'s.
    const std::string surprise{"syntax error: ')' came as a complete surprise to me while matching a lexer rule"};

    EXPECT_EQ(refusal("-> skip()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> popMode()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> type()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> type( /* c */ )"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> frob()"), "line 3: " + surprise);
    EXPECT_EQ(refusal("-> skip, channel(\n)"), "line 4: " + surprise);

    // The seven with their first letter capitalised name the code templates of ANTLR's targets, `LexerSkipCommand`,
    // which antlr 4.13.2 accepts with no diagnostic, expands into an action the generated lexer runs, `skip();`, and
    // leaves out of its own interpreter, which emits the token: refused by name, after ANTLR's own argument checks
    // under the spelling given. Any other spelling of the seven is its error 149.
    EXPECT_TRUE(refusal("-> Skip").contains("lexer command Skip names a code template")) << refusal("-> Skip");
    EXPECT_TRUE(refusal("-> Type(Y)").contains("lexer command Type names a code template"));
    EXPECT_TRUE(refusal("-> PopMode").contains("lexer command PopMode names a code template"));
    EXPECT_EQ(refusal("-> Skip(Q)"), "line 3: lexer command Skip does not take any arguments");
    EXPECT_EQ(refusal("-> Type"), "line 3: missing argument for lexer command Type");
    EXPECT_EQ(
            refusal("-> SKIP"), "line 3: lexer command SKIP does not exist or is not supported by the current target");
    EXPECT_EQ(
            refusal("-> sKip"), "line 3: lexer command sKip does not exist or is not supported by the current target");

    // The seven as ANTLR takes them read as before, a numeric type among them.
    EXPECT_EQ(refusal("-> skip"), "");
    EXPECT_EQ(refusal("-> popMode"), "");
    EXPECT_EQ(refusal("-> type(1)"), "");
    EXPECT_EQ(refusal("-> channel(HIDDEN), pushMode(M)"), "");
    EXPECT_EQ(refusal("-> mode(M)"), "");
}

TEST(Read_antlr, A_channel_command_hides_the_token_whatever_a_type_command_renames_it_to)
{
    const auto rule_of{[](const std::string_view clause) {
        return read_antlr("lexer grammar A;\nX : 'a' " + std::string{clause} + " ;\nB : 'b' ;\nC : 'c' ;\n")
                .front()
                .rules.front();
    }};

    // antlr 4.13.2 emits the token on the hidden channel for either order, the type naming it and the channel
    // keeping it from the parser, so both leave it out of the stream a parser reads.
    EXPECT_EQ(rule_of("-> channel(HIDDEN), type(B)").token, std::nullopt);
    EXPECT_EQ(rule_of("-> type(B), channel(HIDDEN)").token, std::nullopt);
    EXPECT_EQ(rule_of("-> channel(HIDDEN)").token, std::nullopt);

    // The type is one field and the channel another: of `skip` and `type` the rightmost wins, which is why ANTLR
    // warns that the two are incompatible and emits the renamed token for "skip, type(B)".
    EXPECT_EQ(rule_of("-> skip, type(B)").token, std::optional<std::string>{"B"});
    EXPECT_EQ(rule_of("-> type(B), skip").token, std::nullopt);
    EXPECT_EQ(rule_of("-> type(B), type(C)").token, std::optional<std::string>{"C"});
    EXPECT_EQ(rule_of("-> type(B)").token, std::optional<std::string>{"B"});
    EXPECT_EQ(rule_of("-> skip").token, std::nullopt);

    // The default channel is the one a parser reads, so a command naming it leaves the token in the stream: ANTLR
    // emits it on channel zero, which its token dump shows by naming no channel at all.
    EXPECT_EQ(rule_of("-> channel(DEFAULT_TOKEN_CHANNEL)").token, std::optional<std::string>{"X"});
    EXPECT_EQ(rule_of("-> channel(0)").token, std::optional<std::string>{"X"});
    EXPECT_EQ(rule_of("-> channel(DEFAULT_TOKEN_CHANNEL), type(B)").token, std::optional<std::string>{"B"});

    // Two channel commands set one field, so the rightmost wins there as it does for the type.
    EXPECT_EQ(rule_of("-> channel(HIDDEN), channel(DEFAULT_TOKEN_CHANNEL)").token, std::optional<std::string>{"X"});
    EXPECT_EQ(rule_of("-> channel(DEFAULT_TOKEN_CHANNEL), channel(HIDDEN)").token, std::nullopt);
}

TEST(Read_antlr, A_channel_argument_is_resolved_as_ANTLR_resolves_it)
{
    // A lexer grammar, its channels block where given, the command on the third line.
    const auto grammar_of{[](const std::string_view channels, const std::string_view clause) {
        return "lexer grammar A;\n" + std::string{channels} + "\nX : 'a' " + std::string{clause} +
               " ;\nB : 'b' ;\nmode M;\nC : 'c' -> popMode ;\n";
    }};

    const auto token_of{[&grammar_of](const std::string_view channels, const std::string_view clause) {
        return read_antlr(grammar_of(channels, clause)).front().rules.front().token;
    }};

    const auto refusal{[&grammar_of](const std::string_view channels, const std::string_view clause) {
        return refusal_of(grammar_of(channels, clause));
    }};

    // antlr 4.13.2 reads a number as Integer.parseInt reads it, so `00` and `000` are the default channel and the
    // token stays where a parser reads it, `01` is HIDDEN and 2 and 99999 channels of their own, all hidden from a
    // parser; the rightmost channel command wins, so `channel(HIDDEN), channel(00)` leaves the token visible.
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

TEST(Read_antlr, Refusals_name_the_line)
{
    EXPECT_EQ(line_of("lexer grammar A;\nimport B;\n"), 2);
    EXPECT_EQ(line_of("parser grammar A;\n"), 1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' .*? [b] ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a' Y ;\nY : 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'a'+ ;\n"), 2);

    // What the loop stops at is the rest of the whole rule: inside a group, and in a rule another rule references,
    // that rest reaches past what the loop's own sequence holds, so both are refused rather than read as the rest
    // this reading can see. ANTLR matches all of "abbc" for either shape.
    EXPECT_EQ(line_of("lexer grammar A;\nX : ('a' .*? 'b') 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : F 'c' ;\nfragment F : .*? 'b' ;\n"), 3);
    EXPECT_EQ(line_of("lexer grammar A;\nX : .*? 'b' ;\nY : X 'c' ;\n"), 2);

    // A loop in an outermost alternative of a rule nothing references is read as before.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'x' | 'a' .*? 'b' ;\n"), -1);
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

    // The two forms ANTLR itself rejects: a command on an alternative of a rule with several, which must end the
    // single outermost alternative, and a mode in a combined grammar, modes being a lexer grammar's alone.
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> skip | 'b' -> type(Y) ;\nY : 'c' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' | 'b' -> skip ;\n"), 2);
    EXPECT_EQ(line_of("grammar A;\nr : X ;\nX : 'a' ;\nmode INNER;\nY : 'b' ;\n"), 4);
    EXPECT_EQ(line_of("lexer grammar A;\n\nX : 'a' \n"), 3);
}

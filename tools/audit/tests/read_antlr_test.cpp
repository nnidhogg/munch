#include "munch/tools/audit/read_antlr.hpp"

#include <gtest/gtest.h>

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
 * @brief The ANTLR idioms in one combined grammar: options, named actions, a parser rule whose literals become
 *        implicit tokens, fragments, modes, every command, per-alternative commands, ranges, sets, negation, the
 *        dot, a non-greedy loop, escapes and an action inside a rule.
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
TAB     : '\t' { nesting++; } -> type(WS) ;
WS      : [ \r\n]+ -> skip | '\f' -> channel(HIDDEN) ;
OPEN    : '{' -> pushMode(INNER) ;
LOW     : 'a'..'f' ;

fragment LETTER : [a-zA-Z_] ;
fragment DIGIT  : [0-9] ;

mode INNER;

CLOSE   : '}' -> popMode ;
WORD    : ~[}]+ ;
SIGN    : [+\-*] ;
)"};

} // namespace

TEST(Read_antlr, Reads_a_combined_grammar_with_its_idioms)
{
    const auto spec{read_antlr(idioms).front()};

    EXPECT_EQ(spec.line, 1u);
    EXPECT_EQ(spec.options, (std::vector<std::string>{"language=Cpp"}));

    // The parser's literals lead, except 'if', which IF spells; then the lexer rules in order, WS split in two. The
    // parser rule named catchProduction is a rule, not an exception handler.
    ASSERT_EQ(spec.rules.size(), 17u);

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

    // An action inside the rule is skipped; type() renames the token.
    EXPECT_EQ(spec.rules[9].expression, R"("\t")");
    EXPECT_EQ(spec.rules[9].token, std::optional<std::string>{"WS"});

    // Alternatives with different commands are separate rules, in order.
    EXPECT_EQ(spec.rules[10].pattern, R"([ \r\n]+)");
    EXPECT_EQ(spec.rules[10].token, std::nullopt);
    EXPECT_EQ(spec.rules[11].pattern, R"('\f')");
    EXPECT_EQ(spec.rules[11].action, "-> channel(HIDDEN)");

    EXPECT_EQ(spec.rules[12].token, std::optional<std::string>{"OPEN"});
    EXPECT_EQ(spec.rules[12].action, "-> pushMode(INNER)");
    EXPECT_EQ(spec.rules[13].expression, "[a-f]");

    // The mode's rules carry it as their condition, and the fragments are definitions rather than rules.
    EXPECT_EQ(spec.rules[13].conditions, std::vector<std::string>{});
    EXPECT_EQ(spec.rules[14].conditions, (std::vector<std::string>{"INNER"}));
    EXPECT_EQ(spec.rules[14].token, std::optional<std::string>{"CLOSE"});
    ASSERT_EQ(spec.conditions.size(), 1u);
    EXPECT_EQ(spec.conditions.front().name, "INNER");
    EXPECT_EQ(spec.definitions.at("LETTER"), "[A-Z_a-z]");
    EXPECT_EQ(spec.definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("IF"), R"("if")");
    EXPECT_EQ(spec.rules[16].expression, R"([*+\-])");
    EXPECT_EQ(active_rules(spec, "INNER"), (std::vector<std::size_t>{14, 15, 16}));
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

TEST(Read_antlr, Refusals_name_the_line)
{
    const auto line_of{[](const std::string_view source) {
        try
        {
            std::ignore = read_antlr(source);
        }
        catch (const Spec_error& error)
        {
            return static_cast<long>(error.line());
        }

        return -1L;
    }};

    EXPECT_EQ(line_of("lexer grammar A;\nimport B;\n"), 2);
    EXPECT_EQ(line_of("parser grammar A;\n"), 1);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' .*? [b] ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' EOF ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' {p()}? 'b' ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : [\\p{L}]+ ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\nX : 'a' -> more ;\n"), 2);
    EXPECT_EQ(line_of("lexer grammar A;\n\nX : 'a' \n"), 3);
}

#include "munch/tools/audit/read_antlr.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "munch/tools/audit/read_flex.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief The text of one of the grammars beside the tests.
 * @param name The file's name.
 * @return Its text.
 */
std::string grammar(const std::string_view name)
{
    std::ifstream stream{std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{name}};

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/**
 * @brief The ANTLR idioms in one combined grammar: options, named actions, a parser rule whose literals become
 *        implicit tokens, fragments, modes, every command, per-alternative commands, ranges, sets, negation, the
 *        dot, a non-greedy loop, escapes and an action inside a rule.
 */
constexpr std::string_view idioms{R"(grammar Idioms;

options { language = Cpp; }

@lexer::members {
    int nesting = 0; // braces { inside } code
}

tokens { EXTRA }

statement : 'if' expression 'then' statement | IDENT '=' expression ';' ;
expression : IDENT | NUMBER ;

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
)"};

} // namespace

TEST(Read_antlr, Reads_a_combined_grammar_with_its_idioms)
{
    const auto spec{read_antlr(idioms)};

    EXPECT_EQ(spec.line, 1u);
    EXPECT_EQ(spec.options, (std::vector<std::string>{"language=Cpp"}));

    // The parser's literals lead, except 'if', which IF spells; then the lexer rules in order, WS split in two.
    ASSERT_EQ(spec.rules.size(), 16u);

    EXPECT_EQ(spec.rules[0].pattern, "'then'");
    EXPECT_EQ(spec.rules[0].expression, "\"then\"");
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"'then'"});
    EXPECT_EQ(spec.rules[0].line, 11u);
    EXPECT_EQ(spec.rules[1].pattern, "'='");
    EXPECT_EQ(spec.rules[2].pattern, "';'");

    EXPECT_EQ(spec.rules[3].token, std::optional<std::string>{"IF"});
    EXPECT_EQ(spec.rules[3].expression, "\"if\"");
    EXPECT_EQ(spec.rules[3].line, 14u);

    EXPECT_EQ(spec.rules[4].pattern, "LETTER (LETTER | DIGIT)*");
    EXPECT_EQ(spec.rules[4].expression, "{LETTER}({LETTER}|{DIGIT})*");
    EXPECT_EQ(spec.rules[5].expression, "{DIGIT}+(\".\"{DIGIT}+)?");

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
    EXPECT_EQ(spec.rules[9].expression, "\"\\x09\"");
    EXPECT_EQ(spec.rules[9].token, std::optional<std::string>{"WS"});

    // Alternatives with different commands are separate rules, in order.
    EXPECT_EQ(spec.rules[10].pattern, "[ \\r\\n]+");
    EXPECT_EQ(spec.rules[10].token, std::nullopt);
    EXPECT_EQ(spec.rules[11].pattern, "'\\f'");
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
    EXPECT_EQ(spec.definitions.at("IF"), "\"if\"");
    EXPECT_EQ(active_rules(spec, "INNER"), (std::vector<std::size_t>{14, 15}));
}

TEST(Read_antlr, Case_insensitivity_doubles_every_letter)
{
    const auto spec{
            read_antlr("lexer grammar Ci;\noptions { caseInsensitive = true; }\nKW : 'if' ;\nID : [a-z]+ ;\n"
                       "EXACT options { caseInsensitive = false; } : 'x' ;\n")};

    ASSERT_EQ(spec.rules.size(), 3u);
    EXPECT_EQ(spec.rules[0].expression, "[iI][fF]");
    EXPECT_EQ(spec.rules[1].expression, "[A-Za-z]+");
    EXPECT_EQ(spec.rules[2].expression, "\"x\"");
}

TEST(Read_antlr, Sets_beyond_ascii_and_negated_groups_read_as_scalars)
{
    const auto spec{
            read_antlr("lexer grammar U;\nLATIN : [a-z\\u00C0-\\u00FF]+ ;\nSPAN : '\\u0300'..'\\u036F' ;\n"
                       "REST : ~('\\r' | '\\n' | [ \\t]) ;\nBOM : '\\uFEFF' ;\n")};

    ASSERT_EQ(spec.rules.size(), 4u);
    EXPECT_EQ(spec.rules[0].expression, R"([\u{61}-\u{7a}\u{c0}-\u{ff}]+)");
    EXPECT_EQ(spec.rules[1].expression, R"([\u{300}-\u{36f}])");
    EXPECT_EQ(spec.rules[2].expression, R"([\u{0}-\u{8}\u{b}-\u{c}\u{e}-\u{1f}\u{21}-\u{7f}\u{80}-\u{10ffff}])");
    EXPECT_EQ(spec.rules[3].expression, R"("\xef\xbb\xbf")");

    // Built, the scalars match as their encodings.
    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"caf\xc3\xa9"}).length, 5u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xcc\x81"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xe2\x82\xac"}).token, std::optional<std::size_t>{2});
}

TEST(Read_antlr, Every_grammar_read_through_antlr_cuts_as_its_flex_twin)
{
    for (const std::string_view name :
         {"c-like-conventional", "c-like-split-friendly", "c-like-block-comments", "json", "log-lines"})
    {
        const auto from_flex{build(read_flex(grammar(std::string{name} + ".l")), "INITIAL")};

        const auto from_antlr{build(read_antlr(grammar(std::string{name} + ".g4")), "INITIAL")};

        // No input the two tokenize is cut differently, over every input rather than a sample.
        const auto difference{from_flex.boundary_difference(from_antlr)};

        EXPECT_TRUE(difference.exhaustive) << name;
        EXPECT_TRUE(difference.witness.empty()) << name << ": " << difference.witness;

        // The certificates agree on every byte an encoding uses; the flex file, written over bytes, consumes the
        // bytes no UTF-8 encoding holds where the ANTLR reading never meets them, so those are left out.
        for (int value{0}; value < 256; ++value)
        {
            if (value == 0xC0 || value == 0xC1 || value >= 0xF5)
            {
                continue;
            }

            const auto byte{static_cast<char>(value)};

            EXPECT_EQ(from_flex.is_split_point(byte), from_antlr.is_split_point(byte)) << name << ' ' << value;
            EXPECT_EQ(from_flex.is_split_point_ignoring(byte), from_antlr.is_split_point_ignoring(byte))
                    << name << ' ' << value;
        }
    }
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

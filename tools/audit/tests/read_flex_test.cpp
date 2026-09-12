#include "munch/tools/audit/read_flex.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace munch::tools::audit;

namespace
{
/**
 * @brief A small C-like scanner in flex's own idiom: definitions, options, a start condition, code blocks, shared
 *        actions, a multi-line action, a discarding rule and an <<EOF>> rule.
 */
constexpr std::string_view c_like{R"(%{
#include "tokens.h"
%}
%option noyywrap
%x COMMENT

DIGIT    [0-9]
ID       [a-zA-Z_][a-zA-Z0-9_]*
    /* indented text is code and is skipped */

%%
{ID}            { return IDENT; }
{DIGIT}+        { yylval.number = atoi(yytext);
                  return NUMBER; }
"=="            |
"!="            { return COMPARE; }
[-+*/=<>]       return OPERATOR;
"/*"            BEGIN(COMMENT);
<COMMENT>"*/"   BEGIN(INITIAL);
<COMMENT>.|\n   ;
[ \t\n]+        ;
"//"[^\n]*      /* a line comment, discarded */
<<EOF>>         { return END; }
%%

int main() { return yylex(); }
)"};

} // namespace

TEST(Read_flex, Reads_definitions_options_conditions_and_rules_in_order)
{
    const auto file{read_flex(c_like).front()};

    EXPECT_EQ(file.line, 11u);

    ASSERT_EQ(file.definitions.size(), 2u);
    EXPECT_EQ(file.definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(file.definitions.at("ID"), "[a-zA-Z_][a-zA-Z0-9_]*");

    ASSERT_EQ(file.options.size(), 1u);
    EXPECT_EQ(file.options.front(), "noyywrap");

    ASSERT_EQ(file.conditions.size(), 1u);
    EXPECT_EQ(file.conditions.front().name, "COMMENT");
    EXPECT_TRUE(file.conditions.front().exclusive);

    // Ten rules: the <<EOF>> rule is not one, and the code after the second %% is not read.
    ASSERT_EQ(file.rules.size(), 10u);

    EXPECT_EQ(file.rules[0].pattern, "{ID}");
    EXPECT_EQ(file.rules[0].line, 12u);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"IDENT"});

    // The multi-line action is read whole and its return found on its second line.
    EXPECT_EQ(file.rules[1].pattern, "{DIGIT}+");
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"NUMBER"});
    EXPECT_NE(file.rules[1].action.find("atoi"), std::string::npos);

    // A '|' action takes the next rule's token.
    EXPECT_EQ(file.rules[2].pattern, R"("==")");
    EXPECT_EQ(file.rules[2].action, "|");
    EXPECT_EQ(file.rules[2].token, std::optional<std::string>{"COMPARE"});
    EXPECT_EQ(file.rules[3].token, std::optional<std::string>{"COMPARE"});

    // An action without braces, and one that only switches condition.
    EXPECT_EQ(file.rules[4].token, std::optional<std::string>{"OPERATOR"});
    EXPECT_FALSE(file.rules[5].token.has_value());

    // Prefixed rules carry their condition; the bracket with a blank inside is one pattern.
    EXPECT_EQ(file.rules[6].conditions, (std::vector<std::string>{"COMMENT"}));
    EXPECT_EQ(file.rules[7].pattern, R"(.|\n)");
    EXPECT_EQ(file.rules[8].pattern, R"([ \t\n]+)");
    EXPECT_FALSE(file.rules[8].token.has_value());
    EXPECT_EQ(file.rules[9].pattern, R"("//"[^\n]*)");
    EXPECT_FALSE(file.rules[9].token.has_value());
}

TEST(Read_flex, Active_rules_follow_the_start_conditions)
{
    const auto file{read_flex(c_like).front()};

    // INITIAL: every unprefixed rule, none of the COMMENT ones.
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 8, 9}));

    // COMMENT is exclusive: only its own rules.
    EXPECT_EQ(active_rules(file, "COMMENT"), (std::vector<std::size_t>{6, 7}));
}

TEST(Read_flex, The_built_token_set_scans_as_flex_would_and_answers_the_certificates)
{
    const auto file{read_flex(c_like).front()};

    const auto lexer{build(file, "INITIAL")};

    // Longest match, then first rule: "==" is COMPARE, not two OPERATORs; "abc" is IDENT.
    const std::string input{"abc == 42\n"};

    std::vector<std::pair<std::size_t, std::size_t>> tokens;

    const auto consumed{lexer.tokenize_all<std::size_t>(
            input, [&tokens](const std::size_t rule, const std::size_t length) { tokens.emplace_back(rule, length); })};

    EXPECT_EQ(consumed, input.size());
    EXPECT_EQ(
            tokens, (std::vector<std::pair<std::size_t, std::size_t>>{{0, 3}, {8, 1}, {2, 2}, {8, 1}, {1, 2}, {8, 1}}));

    // The conventional C-like shape: the line comment admits every byte but the newline and the whitespace run
    // folds the newline in, so nothing certifies exactly, and newline certifies once the discarded rules, the
    // ones returning nothing, are deleted.
    for (int value{0}; value < 256; ++value)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(value))) << value;
    }

    EXPECT_TRUE(lexer.is_split_point_ignoring('\n'));
}

TEST(Read_flex, Start_condition_scopes_and_code_in_actions_read_as_flex_reads_them)
{
    // The shapes PostgreSQL's and sudo's scanners use: %top with its brace on the line after a blank, a scope whose
    // rules are indented and whose close carries a comment, comments on lines of their own inside and outside a
    // scope, an action whose literals and comments hold braces, and one whose brace opens after code on its line.
    constexpr std::string_view source{R"(%top{
#include "first.h"
}
%x xc

%%
/* the comment start
 * is its own token */
"/*"            { BEGIN(xc); }
<xc>{
    /*
     * inside the scope too
     */
    "*/"        { BEGIN(INITIAL); /* } not a close */ }
    .           { if (c == '{') { depth++; } puts("}"); }
} /* <xc> */
[a-z]+          if (keyword(yytext)) {
                    return KEYWORD;
                } else return WORD;
%%
)"};

    const auto file{read_flex(source).front()};

    ASSERT_EQ(file.rules.size(), 4u);

    EXPECT_TRUE(file.rules[0].conditions.empty());
    EXPECT_EQ(file.rules[0].line, 9u);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[1].pattern, R"("*/")");
    EXPECT_EQ(file.rules[2].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[2].action, R"({ if (c == '{') { depth++; } puts("}"); })");
    EXPECT_TRUE(file.rules[3].conditions.empty());
    EXPECT_EQ(file.rules[3].token, std::optional<std::string>{"KEYWORD"});
    EXPECT_EQ(file.rules[3].action.substr(0, 22), "if (keyword(yytext)) {");

    EXPECT_EQ(active_rules(file, "xc"), (std::vector<std::size_t>{1, 2}));
}

TEST(Read_flex, A_start_condition_prefix_alone_on_its_line_opens_the_next)
{
    // Bison's scanners put the scope's brace on the line after the prefix, and flex, whose newline there yields no
    // token, reads a rule after such a prefix the same way.
    constexpr std::string_view source{R"(%x a b
%%
<INITIAL,a,b>
{
  ","    { return COMMA; }
}
<b>

  "x"    return X;
%%
)"};

    const auto file{read_flex(source).front()};

    ASSERT_EQ(file.rules.size(), 2u);

    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"INITIAL", "a", "b"}));
    EXPECT_EQ(file.rules[0].pattern, R"(",")");
    EXPECT_EQ(file.rules[0].line, 5u);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"b"}));
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"X"});
    EXPECT_EQ(file.rules[1].line, 9u);
}

TEST(Read_flex, Case_insensitive_scanners_fold_every_letter_of_every_pattern)
{
    // PostgreSQL's scanner: the option folds keywords, definitions expanded into patterns, and classes alike.
    constexpr std::string_view source{R"(%option case-insensitive
ident  [a-z_][a-z0-9_]*
%%
select      return SELECT;
{ident}     return IDENT;
[ \t\n]+    ;
%%
)"};

    const auto lexer{build(read_flex(source).front(), "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"SeLeCt"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"SELECTs"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"Foo_1 "}).length, 5u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\t x"}).token, std::optional<std::size_t>{2});
}

TEST(Read_flex, Refusals_name_the_line)
{
    const auto line_of{[](const std::string_view source) {
        try
        {
            std::ignore = read_flex(source);
        }
        catch (const Spec_error& error)
        {
            return static_cast<long>(error.line());
        }

        return -1L;
    }};

    EXPECT_EQ(line_of("DIGIT [0-9]\n"), 2);
    EXPECT_EQ(line_of("DIGIT\n%%\n"), 1);
    EXPECT_EQ(line_of("%{\ncode\n%%\n"), 1);
    EXPECT_EQ(line_of("%%\n[abc   return X;\n"), 2);
    EXPECT_EQ(line_of("%%\nabc   { return X;\n"), 2);
    EXPECT_EQ(line_of("%%\n<S abc  ;\n"), 2);

    // A pattern the regex parser refuses is refused when the token set is built, with the rule's line.
    const auto file{read_flex("%%\n^abc   return X;\n").front()};

    try
    {
        std::ignore = build(file, "INITIAL");

        FAIL() << "an anchored pattern must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 2u);
        EXPECT_NE(std::string_view{error.what()}.find("anchor"), std::string_view::npos);
    }
}

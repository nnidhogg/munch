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
"=="            | /* shares COMPARE, the comment notwithstanding */
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

    // Ten rules of the file's and flex's default rule after them: the <<EOF>> rule is not one, and the code after
    // the second %% is not read.
    ASSERT_EQ(file.rules.size(), 11u);
    EXPECT_EQ(file.rules[10].pattern, R"(.|\n)");
    EXPECT_EQ(file.rules[10].line, 24u);

    EXPECT_EQ(file.rules[0].pattern, "{ID}");
    EXPECT_EQ(file.rules[0].line, 12u);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"IDENT"});

    // The multi-line action is read whole and its return found on its second line.
    EXPECT_EQ(file.rules[1].pattern, "{DIGIT}+");
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"NUMBER"});
    EXPECT_NE(file.rules[1].action.find("atoi"), std::string::npos);

    // A '|' action takes the next rule's token.
    EXPECT_EQ(file.rules[2].pattern, R"("==")");
    EXPECT_EQ(file.rules[2].action, "| /* shares COMPARE, the comment notwithstanding */");
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

    // INITIAL: every unprefixed rule and the default rule, none of the COMMENT ones.
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 8, 9, 10}));

    // COMMENT is exclusive: only its own rules, and the default rule, which stands in every condition.
    EXPECT_EQ(active_rules(file, "COMMENT"), (std::vector<std::size_t>{6, 7, 10}));
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
    // The shapes PostgreSQL's, sudo's, flex's own and OpenSCAD's scanners use: %top with its brace on the line after
    // a blank, a scope whose opener carries a comment, whose rules are indented and whose close carries a comment,
    // indented comments on lines of their own inside and outside a scope, as flex's manual asks for them, code blocks
    // closed mid-line and on their own line, an action whose literals and comments hold braces, and one whose brace
    // opens after code on its line. flex 2.6.4 builds this file.
    constexpr std::string_view source{R"(%top{
#include "first.h"
}
%x xc

%%
  /* the comment start
   * is its own token */
"/*"            { BEGIN(xc); }
<xc>{ /* the comment flex copies out */
    /*
     * inside the scope too
     */
%{/* a code block
   closed mid-line */%}
    "*/"        { BEGIN(INITIAL); /* } not a close */ }
%{ int one_line; %}
    .           { if (c == '{') { depth++; } puts("}"); }
} /* <xc> */
[a-z]+          if (keyword(yytext)) {
                    return KEYWORD;
                } else return WORD;
%%
)"};

    const auto file{read_flex(source).front()};

    ASSERT_EQ(file.rules.size(), 5u);

    EXPECT_TRUE(file.rules[0].conditions.empty());
    EXPECT_EQ(file.rules[0].line, 9u);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[1].pattern, R"("*/")");
    EXPECT_EQ(file.rules[2].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[2].action, R"({ if (c == '{') { depth++; } puts("}"); })");
    EXPECT_TRUE(file.rules[3].conditions.empty());
    EXPECT_EQ(file.rules[3].token, std::optional<std::string>{"KEYWORD"});
    EXPECT_EQ(file.rules[3].action.substr(0, 22), "if (keyword(yytext)) {");

    EXPECT_EQ(active_rules(file, "xc"), (std::vector<std::size_t>{1, 2, 4}));
}

TEST(Read_flex, A_scope_opener_s_line_is_code_flex_copies_out_however_far_its_comment_runs)
{
    // flex 2.6.4, run on this file, copies the comment out whole, its second line included, and scans "a" as 1 in S
    // and "b" as 2 in INITIAL, neither in the other condition: nothing on the comment's lines is a rule.
    constexpr std::string_view comment{R"(%option noyywrap
%x S
%%
<S>{ /* a comment
        that continues */
a   return 1;
}
b   return 2;
%%
)"};

    const auto file{read_flex(comment).front()};

    ASSERT_EQ(file.rules.size(), 3u);
    EXPECT_EQ(file.rules[0].pattern, "a");
    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(file.rules[0].line, 6u);
    EXPECT_EQ(file.rules[1].pattern, "b");
    EXPECT_TRUE(file.rules[1].conditions.empty());
    EXPECT_EQ(active_rules(file, "S"), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{1, 2}));

    // Code other than a comment after the brace is copied out the same way, whether or not it compiles later: flex
    // 2.6.4 writes `x return 3;` into the scanner as code and builds no rule from it.
    const auto code{read_flex("%x S\n%%\n<S>{ /* closed */ x return 3;\na   return 1;\n}\nb   return 2;\n").front()};

    ASSERT_EQ(code.rules.size(), 3u);
    EXPECT_EQ(code.rules[0].pattern, "a");
    EXPECT_EQ(code.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(code.rules[1].pattern, "b");
}

TEST(Read_flex, A_scope_s_close_line_is_code_flex_copies_out_however_far_its_comment_or_brace_block_runs)
{
    // flex 2.6.4, run on this file, copies the comment out whole and returns 2 for b while echoing w and x: nothing
    // on the comment's lines is a rule.
    constexpr std::string_view comment{R"(%option noyywrap
%x S
%%
<S>{
a   return 1;
} /* comment starts
word here
   and ends */
b   return 2;
%%
)"};

    const auto file{read_flex(comment).front()};

    ASSERT_EQ(file.rules.size(), 3u);
    EXPECT_EQ(file.rules[0].pattern, "a");
    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(file.rules[1].pattern, "b");
    EXPECT_EQ(file.rules[1].line, 9u);
    EXPECT_TRUE(file.rules[1].conditions.empty());
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{1, 2}));

    // A brace block after the close runs on the same way: flex 2.6.4 returns 2 for b and echoes c.
    const auto block{read_flex("%x S\n%%\n<S>{\na   return 1;\n} { int\nc   = 3; }\nb   return 2;\n").front()};

    ASSERT_EQ(block.rules.size(), 3u);
    EXPECT_EQ(block.rules[1].pattern, "b");
    EXPECT_EQ(block.rules[1].line, 7u);

    // A comment closed on the close's line, or an indented close, is what the scanners in the wild write.
    EXPECT_EQ(read_flex("%x S\n%%\n<S>{\na   return 1;\n} /* <S> */\nb   return 2;\n").front().rules.size(), 3u);
    EXPECT_EQ(read_flex("%x S\n%%\n<S>{\n  a   return 1;\n  } /* done */\nb   return 2;\n").front().rules.size(), 3u);

    // A second close on the close's line is code, so the outer scope stays open, which flex refuses as it does the
    // reading.
    EXPECT_THROW(std::ignore = read_flex("%x S T\n%%\n<S>{\n<T>{\na   return 1;\n} }\nb   return 2;\n"), Spec_error);

    // A comment at the margin of the rules section, after the first %% or between rules, in a scope or out of one,
    // is refused: flex 2.6.4 reads its slash as the start of a rule and stops with "unrecognized rule", where its
    // manual asks for the comment to be indented.
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

    EXPECT_EQ(line_of("%%\n/* the comment start\n * is its own token */\n\"/*\"   return 1;\n"), 2);
    EXPECT_EQ(line_of("%%\na   return 1;\n/* between\nword here */\nb   return 2;\n"), 3);
    EXPECT_EQ(line_of("%x S\n%%\n<S>{\n/* margin comment */\na   return 1;\n}\nb   return 2;\n"), 4);
    EXPECT_EQ(line_of("%x S\n%%\n<S>{\n}\n/* comment at the margin\nword here\n   and ends */\nb   return 2;\n"), 5);
    EXPECT_EQ(line_of("%%\na   return 1;\n  /* between\nword here */\nb   return 2;\n"), -1);

    // An indented comment is code read to the action's end, so a brace after its close on the line counts, and one
    // never closed is refused as flex refuses it.
    EXPECT_EQ(read_flex("%%\na   return 1;\n  /* c */ {\nb   return 2;\n}\nc   return 3;\n").front().rules.size(), 3u);
    EXPECT_EQ(line_of("%%\na   return 1;\n  /* open\nb   return 2;\n"), 3);
}

TEST(Read_flex, An_action_ends_where_flex_s_action_scanner_ends_it)
{
    // The patterns of the rules read, or the refusal, for a rules section given whole; flex 2.6.4 was run on each
    // with `%option noyywrap` and a driver returning the tokens, and the assertions say what it built.
    const auto patterns{[](const std::string_view rules) {
        const std::string source{"%option noyywrap\n%%\n" + std::string{rules}};

        std::string joined;

        try
        {
            const auto file{read_flex(source).front()};

            for (const auto& rule : file.rules)
            {
                joined += (joined.empty() ? "" : " ") + rule.pattern;
            }
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return joined;
    }};

    // flex's action scanner has no state for a `//` comment, so a brace after one counts: an open one runs the action
    // on to the next close, making `b return 2;` code, and a close one ends the action at its line, so that b and c
    // are rules; flex builds both scanners, whether or not the C it emits compiles.
    EXPECT_EQ(patterns("a   { return 1; } // {\nb   return 2;\n}\nc   return 3;\n"), R"(a c .|\n)");
    EXPECT_EQ(patterns("a   { // }\nb   return 2;\nc   return 3;\n"), R"(a b c .|\n)");

    // A quote after a `//` opens a literal, which the line's end closes where the braces balance: flex ends the
    // action inside the literal and its m4 stops with "end of file in string", so the file is refused by name.
    EXPECT_EQ(
            patterns("a   { return 1; } // it's\nb   return 2;\n"),
            "line 3: a quote is left open at the end of the action's line, where flex ends the action inside the "
            "literal and never closes the code it emits for it, so that the m4 it runs stops with an end of file in "
            "string");
    EXPECT_EQ(patterns("a   puts(\"x\nb   return 2;\n").substr(0, 8), "line 3: ");

    // A brace inside a literal or a block comment does not count, and a comment runs over lines.
    EXPECT_EQ(patterns("a   { puts(\"}\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { if (c == '\\'' || c == '}') return 1; return 4; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { /* }\n      */ return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // A literal ends at its line's end where a brace is open, and the action goes on: here the quote on the next
    // line opens another literal that swallows the close, so flex meets the end of the file inside the action.
    EXPECT_EQ(
            patterns("a   { puts(\"x\ny\"); return 1; }\nb   return 2;\n"), "line 3: the action's braces never close");
    EXPECT_EQ(patterns("a   { puts(\"x\n}\"); return 1; }\nb   return 2;\n").substr(0, 8), "line 3: ");

    // A backslash before the newline carries the literal on to the next line, as C splices lines, so a brace-less
    // action spans both lines and `c return 3;` is inside the literal, no rule; an escape may span a splice too.
    EXPECT_EQ(patterns("a   puts(\"x\\\ny\"); return 1;\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   puts(\"x\\\nc   return 3;\"); return 1;\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { puts(\"x\\\n}\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { puts(\"\\\\\nn\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // Outside a literal a backslash before the newline is no splice: the action ends there.
    EXPECT_EQ(patterns("a   return 1; \\\nb   return 2;\n"), R"(a b .|\n)");

    // A stray close counts below zero and the action still ends at its line; flex builds the scanner, though the C
    // it emits does not compile.
    EXPECT_EQ(patterns("a   return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // A `|` line is taken unread, a brace after the bar counting for nothing.
    EXPECT_EQ(patterns("a   | {\nb   return 2;\nc   return 3;\n"), R"(a b c .|\n)");

    // An action opening with `%{` runs to the end of the first line holding `%}`, comments and literals unread, so
    // the rule-looking line inside is code and a brace after the `%}` counts for nothing; one never closed is
    // refused, as flex refuses it.
    EXPECT_EQ(patterns("a   %{\nc   return 3;\n    return 1; %}\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   %{ return 1; %} {\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(
            patterns("a   %{ return 1;\nb   return 2;\n"),
            "line 3: the action's %{ block is never closed, which flex refuses");

    // A comment left open at the end of the file is refused as flex refuses it.
    EXPECT_EQ(
            patterns("a   { /* open\nb   return 2;\n"),
            "line 3: the action's comment is never closed, which flex refuses as an end of file inside an action");
}

TEST(Read_flex, Scopes_nest_and_a_prefixed_rule_inside_one_is_active_in_both)
{
    // flex's own scanner nests scopes and prefixes rules inside them; flex 2.6.4, run on this grammar, fires x in A
    // and B, y in A, B and C, and z in A alone.
    constexpr std::string_view source{R"(%x A B C
%%
<A>{
  <B>"x"   return X;
  <B,C>{
    "y"    return Y;
  }
  "z"      return Z;
}
"w"        return W;
%%
)"};

    const auto file{read_flex(source).front()};

    ASSERT_EQ(file.rules.size(), 5u);

    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"B", "A"}));
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"A", "B", "C"}));
    EXPECT_EQ(file.rules[2].conditions, (std::vector<std::string>{"A"}));
    EXPECT_TRUE(file.rules[3].conditions.empty());
    EXPECT_EQ(active_rules(file, "A"), (std::vector<std::size_t>{0, 1, 2, 4}));
    EXPECT_EQ(active_rules(file, "B"), (std::vector<std::size_t>{0, 1, 4}));
    EXPECT_EQ(active_rules(file, "C"), (std::vector<std::size_t>{1, 4}));
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{3, 4}));
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

    ASSERT_EQ(file.rules.size(), 3u);

    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"INITIAL", "a", "b"}));
    EXPECT_EQ(file.rules[0].pattern, R"(",")");
    EXPECT_EQ(file.rules[0].line, 5u);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"b"}));
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"X"});
    EXPECT_EQ(file.rules[1].line, 9u);
}

TEST(Read_flex, A_section_delimiter_is_a_margin_percent_pair_whatever_follows_it_and_an_indented_line_is_a_rule)
{
    // The patterns read from a file, or its refusal: flex 2.6.4 lexes a delimiter as `%%` at the start of a line and
    // drops the rest of the line, so a comment or text after either delimiter changes nothing, and it builds each
    // file here as the assertion says.
    const auto patterns{[](const std::string_view source) {
        std::string joined;

        try
        {
            const auto file{read_flex(source).front()};

            for (const auto& rule : file.rules)
            {
                joined += (joined.empty() ? "" : " ") + rule.pattern;
            }
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return joined;
    }};

    EXPECT_EQ(patterns("%option noyywrap\n%% /* rules */\na   return 1;\n%%\n"), R"(a .|\n)");
    EXPECT_EQ(
            patterns("%option noyywrap\n%%\na   return 1;\n%% /* user code */\nint helper(void) { return 0; }\nword   "
                     "return 9;\n"),
            R"(a .|\n)");
    EXPECT_EQ(patterns("%option noyywrap\n%%x y z\na   return 1;\n%%more text\nword   return 9;\n"), R"(a .|\n)");
    EXPECT_EQ(patterns("%option noyywrap\n%%   \na   return 1;\n%%\t\nint f(void) { return 0; }\n"), R"(a .|\n)");
    EXPECT_EQ(patterns("%option noyywrap\r\n%%\r\na   return 1;\r\n%%\r\n"), R"(a .|\n)");

    // The scanner's line is the first delimiter's, whatever follows it.
    EXPECT_EQ(read_flex("%option noyywrap\n%% /* rules */\na   return 1;\n").front().line, 2u);

    // An indented `%%` is no delimiter: in the definitions section it is code flex copies out, so the delimiter is
    // the margin's `%%` below, and in the rules section it is an indented rule matching the two bytes, discarded,
    // which flex 2.6.4 builds and which scans "a%%b" as 1, the two bytes, then 2.
    EXPECT_EQ(patterns("%option noyywrap\n  %%\n%%\na   return 1;\n%%\n"), R"(a .|\n)");
    EXPECT_EQ(patterns("%option noyywrap\n%%\na   return 1;\n  %%\nb   return 2;\n%%\n"), R"(a %% b .|\n)");
    EXPECT_EQ(patterns("%option noyywrap\n%%\n"), R"(.|\n)");

    // After the first rule flex reads an indented line as a rule, in a scope or out of one; only the indented lines
    // before the first rule, the section's prologue, are code: flex 2.6.4 returns 2 for b from the first file and
    // builds the second with a as its one rule.
    const auto indented{read_flex("%option noyywrap\n%%\na   return 1;\n  b   return 2;\n%%\n").front()};

    ASSERT_EQ(indented.rules.size(), 3u);
    EXPECT_EQ(indented.rules[1].pattern, "b");
    EXPECT_EQ(indented.rules[1].token, std::optional<std::string>{"2"});
    EXPECT_EQ(indented.rules[1].line, 4u);
    EXPECT_EQ(
            patterns("%option noyywrap\n%%\n  int n = 0;\n  /* prologue comment */\n\na   return 1;\n%%\n"),
            R"(a .|\n)");

    // A file with no delimiter at the margin has no rules section.
    EXPECT_EQ(
            patterns("%option noyywrap\n  %%\na   return 1;\n"),
            "line 4: the file has no rules section: no line begins with %%");
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

TEST(Read_flex, The_case_option_is_what_the_last_word_naming_it_left_standing)
{
    // flex sets its options before it parses a rule, so order decides and every spelling counts: run on each of
    // these, flex 2.6.4 matches 'A' against the rule `a` exactly where the assertion says it does, and elsewhere
    // leaves it to the default rule.
    const auto folds{[](const std::string_view options) {
        const std::string source{"%option " + std::string{options} + "\n%%\na   return A;\n"};

        const auto lexer{build(read_flex(source).front(), "INITIAL")};

        return lexer.tokenize<std::size_t>(std::string_view{"A"}).token == std::optional<std::size_t>{0};
    }};

    EXPECT_FALSE(folds("noyywrap"));

    // The four spellings that turn it on, and the four that turn it off.
    EXPECT_TRUE(folds("caseless"));
    EXPECT_TRUE(folds("case-insensitive"));
    EXPECT_TRUE(folds("nocaseful"));
    EXPECT_TRUE(folds("nocase-sensitive"));
    EXPECT_FALSE(folds("caseless caseful"));
    EXPECT_FALSE(folds("caseless case-sensitive"));
    EXPECT_FALSE(folds("caseless nocaseless"));
    EXPECT_FALSE(folds("case-insensitive nocase-insensitive"));

    // The last word naming it wins, whichever way round, and a line of its own is no different.
    EXPECT_TRUE(folds("caseful caseless"));
    EXPECT_TRUE(folds("nocaseless caseless"));
    EXPECT_FALSE(folds("nocaseless\n%option caseless\n%option nocaseless"));
    EXPECT_TRUE(folds("caseless\n%option nocaseless\n%option caseless"));

    // Each `no` flips the sense, since flex lexes one as a token of its own: run on these, flex 2.6.4 folds under
    // `nonocaseless` and not under `nononocaseless` or `nonocaseful`. A `no` standing as a word of its own reaches
    // no name, so the sense begins afresh at the next word.
    EXPECT_TRUE(folds("nonocaseless"));
    EXPECT_FALSE(folds("nononocaseless"));
    EXPECT_FALSE(folds("nonocaseful"));
    EXPECT_TRUE(folds("no caseless"));
    EXPECT_TRUE(folds("no\n%option caseless"));

    // `%option i` is no flex option, so it folds nothing; flex refuses the file that names it.
    EXPECT_FALSE(folds("i"));
}

TEST(Read_flex, A_quoted_option_value_is_one_word_and_no_option_of_its_own)
{
    // flex 2.6.4 lexes the quoted value as one token, so the file below scans case-sensitively: "A" is the second
    // rule's token, whatever the file name says.
    constexpr std::string_view quoted{R"(%option noyywrap
%option header-file="generated caseless scanner.h"
%%
a          return 1;
(.|\n)     return 2;
%%
)"};

    const auto file{read_flex(quoted).front()};

    EXPECT_EQ(file.options, (std::vector<std::string>{"noyywrap", R"(header-file="generated caseless scanner.h")"}));
    EXPECT_FALSE(file.parse.caseless);
    EXPECT_EQ(build(file, "INITIAL").tokenize<std::size_t>(std::string{"A"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(build(file, "INITIAL").tokenize<std::size_t>(std::string{"a"}).token, std::optional<std::size_t>{0});

    // Nor is a refused option's name inside the quotes a refusal: flex builds this scanner, and so does the reading.
    EXPECT_NO_THROW(
            std::ignore = read_flex("%option header-file=\"generated lex-compat scanner.h\"\n%%\nab{3}   return A;\n"));

    // A word after the value is an option again, as flex lexes it, with or without a blank after the closing quote:
    // flex 2.6.4 folds "A" under either line.
    EXPECT_TRUE(read_flex("%option header-file=\"a b.h\" caseless\n%%\na   return A;\n").front().parse.caseless);
    EXPECT_TRUE(read_flex("%option header-file=\"a b.h\"caseless\n%%\na   return A;\n").front().parse.caseless);

    // A quote left open is an option flex does not recognize, so the line is refused.
    try
    {
        std::ignore = read_flex("%option noyywrap\n%option header-file=\"a b.h caseless\n%%\na   return A;\n");

        FAIL() << "the open quote was read";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 2u);
    }
}

TEST(Read_flex, The_default_rule_stands_after_the_file_s_rules_unless_nodefault_drops_it)
{
    // flex 2.6.4, run on this file, returns 1 for a, echoes b, newline and c one byte at a time returning nothing,
    // and in S echoes x the same way: its default rule, added after the file's own, matches one byte wherever no
    // rule does, in every start condition.
    constexpr std::string_view plain{R"(%option noyywrap
%x S
%%
a       return 1;
s       BEGIN(S); return 2;
<S>t    BEGIN(INITIAL); return 3;
%%
)"};

    const auto file{read_flex(plain).front()};

    ASSERT_EQ(file.rules.size(), 4u);
    EXPECT_EQ(file.rules[3].pattern, R"(.|\n)");
    EXPECT_EQ(file.rules[3].conditions, (std::vector<std::string>{"*"}));
    EXPECT_EQ(file.rules[3].action, "ECHO;");
    EXPECT_FALSE(file.rules[3].token.has_value());
    EXPECT_EQ(file.rules[3].line, 7u);
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{0, 1, 3}));
    EXPECT_EQ(active_rules(file, "S"), (std::vector<std::size_t>{2, 3}));

    const auto tokens{[](const munch::core::Lexer& lexer, const std::string_view input) {
        std::vector<std::pair<std::size_t, std::size_t>> found;

        std::ignore = lexer.tokenize_all<std::size_t>(
                input,
                [&found](const std::size_t rule, const std::size_t length) { found.emplace_back(rule, length); });

        return found;
    }};

    // Every byte the rules leave out is one token of the default rule, discarded; the rules keep their own.
    const auto initial{build(file, "INITIAL")};

    EXPECT_EQ(
            tokens(initial, "ab\nca"),
            (std::vector<std::pair<std::size_t, std::size_t>>{{0, 1}, {3, 1}, {3, 1}, {3, 1}, {0, 1}}));
    EXPECT_EQ(tokens(build(file, "S"), "xt"), (std::vector<std::pair<std::size_t, std::size_t>>{{3, 1}, {2, 1}}));

    // So every byte begins a token, and a byte no rule consumes past its first position certifies.
    EXPECT_TRUE(initial.is_split_point('\n'));
    EXPECT_TRUE(initial.is_split_point('a'));

    // The default rule ranks below every rule of the file's, so a rule matching the same one byte wins, and a longer
    // rule that fails midway leaves flex the one byte: on "abd" against `abc`, flex 2.6.4 echoes a, b and d.
    const auto longer{build(read_flex("%%\nabc   return 1;\n").front(), "INITIAL")};

    EXPECT_EQ(tokens(longer, "abd"), (std::vector<std::pair<std::size_t, std::size_t>>{{1, 1}, {1, 1}, {1, 1}}));
    EXPECT_EQ(tokens(longer, "abc"), (std::vector<std::pair<std::size_t, std::size_t>>{{0, 3}}));

    // Without a second %% the rule takes the file's last line.
    EXPECT_EQ(read_flex("%%\na   return 1;\n").front().rules.back().line, 2u);

    // Under `%option nodefault` flex 2.6.4 stops with "flex scanner jammed" at b, in INITIAL and in S alike, which
    // is what the token set answers of itself: no rule is added, and b begins no token.
    const auto without{[](const std::string_view options) {
        const std::string source{"%option " + std::string{options} + "\n%%\na   return 1;\n"};

        const auto read{read_flex(source).front()};

        return read.rules.size() == 1 && !build(read, "INITIAL").tokenize<std::size_t>(std::string_view{"b"}).token;
    }};

    EXPECT_FALSE(without("noyywrap"));
    EXPECT_TRUE(without("nodefault"));
    EXPECT_FALSE(without("default"));

    // Each `no` flips the sense and the last word naming it decides, as flex 2.6.4 builds each of these.
    EXPECT_FALSE(without("nonodefault"));
    EXPECT_TRUE(without("nononodefault"));
    EXPECT_FALSE(without("nodefault default"));
    EXPECT_TRUE(without("default nodefault"));
    EXPECT_TRUE(without("nodefault\n%option noyywrap"));
}

TEST(Read_flex, The_options_that_narrow_flex_s_alphabet_are_refused_by_name)
{
    // flex resolves the alphabet's width as its own check_options() does: a width named outright decides, and
    // otherwise a `full` or `fast` table with the equivalence classes off leaves it at 128 bytes. Given these same
    // option lines and a rule naming the byte \x80, flex 2.6.4 refuses to build a scanner for exactly the ones
    // asserted refused here, with "scanner requires -8 flag to use the character \200".
    const auto refused{[](const std::string_view options) {
        const std::string source{"%option " + std::string{options} + "\n%%\n[^a]+   return A;\n"};

        try
        {
            std::ignore = read_flex(source);
        }
        catch (const Spec_error& error)
        {
            return std::string_view{error.what()}.find("128 bytes of ASCII") != std::string_view::npos;
        }

        return false;
    }};

    EXPECT_FALSE(refused("noyywrap"));
    EXPECT_TRUE(refused("7bit"));
    EXPECT_FALSE(refused("8bit"));
    EXPECT_FALSE(refused("no7bit"));
    EXPECT_TRUE(refused("no8bit"));
    EXPECT_FALSE(refused("7bit 8bit"));
    EXPECT_TRUE(refused("8bit 7bit"));

    // A full or fast table drops the equivalence classes, whatever the word's sense, and the default width goes
    // with them; naming the width outright, or asking the classes back, leaves the 256 bytes standing.
    EXPECT_TRUE(refused("full"));
    EXPECT_TRUE(refused("fast"));
    EXPECT_TRUE(refused("nofull"));
    EXPECT_TRUE(refused("nofast"));
    EXPECT_TRUE(refused("full noecs"));
    EXPECT_TRUE(refused("ecs full"));
    EXPECT_FALSE(refused("full ecs"));
    EXPECT_FALSE(refused("full 8bit"));
    EXPECT_FALSE(refused("8bit full"));
    EXPECT_FALSE(refused("noecs"));
    EXPECT_FALSE(refused("align"));

    // Each `no` flips the sense here too: flex refuses the high byte under `nono7bit` and takes it under `no7bit`,
    // `nonono7bit` and `nono8bit`, while `nonofull` asks for a full table as `full` does.
    EXPECT_TRUE(refused("nono7bit"));
    EXPECT_FALSE(refused("nonono7bit"));
    EXPECT_TRUE(refused("nononono7bit"));
    EXPECT_FALSE(refused("nono8bit"));
    EXPECT_TRUE(refused("nonofull"));
    EXPECT_FALSE(refused("full nonoecs"));

    // The refusal names the word that narrowed it, and the line it stands on.
    try
    {
        std::ignore = read_flex("%option noyywrap\n%option full\n%%\n[^a]+   return A;\n");

        FAIL() << "a narrowed alphabet must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 2u);
        EXPECT_NE(std::string_view{error.what()}.find("%option full"), std::string_view::npos);
    }
}

TEST(Read_flex, The_actions_that_move_a_match_s_bounds_or_rerun_it_are_refused_by_name_with_their_line)
{
    // What each does in flex 2.6.4, run on a scanner of the rule shown before `b return 8;` and `.|\n return 9;`
    // under nodefault: `a yymore();` on "ab" returns one token 7 spanning both bytes, where the rules alone give a
    // discarded a and then b; `abc REJECT;` before a second `abc return 7;` returns 7 for "abc", the rule the
    // rules alone would never reach, and before `ab return 8;` alone returns 8 for "ab" and 9 for "c"; `ab {
    // yyless(1); return 7; }` on "ab" returns 7 for a and 8 for b; `a { unput('b'); return 7; }` on "a" returns 7
    // and then 8 for a b the input never held; `a { input(); return 7; }` on "ab" returns 7 and nothing for b.
    const auto refusal{[](const std::string_view source) {
        try
        {
            std::ignore = read_flex(source);
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    const auto rules{[](const std::string_view first) {
        return "%option noyywrap\n%%\n" + std::string{first} + "\nb      return 8;\n.|\\n   return 9;\n";
    }};

    EXPECT_EQ(
            refusal(rules("a    yymore();")),
            "line 3: the action calls yymore(), which appends the next match to this one, so the next token begins "
            "where this match did");
    EXPECT_EQ(
            refusal(rules("abc    REJECT;\nabc    return 7;")),
            "line 3: the action uses REJECT, which drops the match for the next rule's, so which rule matches is not "
            "the rules' longest match and first rule");
    EXPECT_EQ(
            refusal(rules("ab     { yyless(1); return 7; }")),
            "line 3: the action calls yyless(), which gives the end of the match back to be matched again, so the "
            "next token begins inside this match");
    EXPECT_EQ(
            refusal(rules("a      { unput('b'); return 7; }")),
            "line 3: the action calls unput(), which pushes a byte onto the input, so the next token is matched "
            "against bytes the input may not hold");
    EXPECT_EQ(
            refusal(rules("a      { input(); return 7; }")),
            "line 3: the action calls input(), which consumes bytes no rule matched, so the next token begins past "
            "them");
    EXPECT_NE(refusal(rules("a      { c = yyinput (); return 7; }")).find("calls yyinput()"), std::string::npos);

    // The line is the call's rule's, wherever in a multi-line action the call stands.
    EXPECT_EQ(refusal(rules("a      {\n    return 7;\n}\nab     {\n    yyless(1);\n}")).substr(0, 8), "line 6: ");

    // Read as C reads the action: a comment or a literal holds no call, a word inside a longer one is none, a name
    // of the file's own without a parenthesis or reached as a member is none, and flex takes `reject` and `Reject`
    // as names of the file's own, building the scanner without the REJECT machinery.
    EXPECT_EQ(refusal(rules("abc    { /* REJECT */ puts(\"REJECT\"); return 7; }")), "");
    EXPECT_EQ(refusal(rules("abc    { return 7; // REJECT\n       }")), "");
    EXPECT_EQ(refusal(rules("abc    { reject++; Reject++; REJECTED++; return 7; }")), "");
    EXPECT_EQ(refusal(rules("abc    { yylval.input = 1; myinput(); s.input(); p->input(); return 7; }")), "");
    EXPECT_EQ(refusal(rules("abc    { if (c == '\\'') input(); return 7; }")).substr(0, 8), "line 3: ");

    // A `|` line is taken unread, as flex takes it, so a call written after the bar is no call.
    EXPECT_EQ(refusal(rules("abc    | REJECT\nabd    return 7;")), "");

    // A condition change is not a call of these: flex 2.6.4 returns 7, 8 and 6 for "abb" under this scanner, the
    // tokens where the rules put them, and the reading takes the actions as any other, the caveat over every report
    // covering the condition.
    constexpr std::string_view states{R"(%option noyywrap nodefault stack
%x S
%%
a      { yy_push_state(S); return 7; }
<S>b   { yy_pop_state(); return 8; }
b      { BEGIN(INITIAL); return 6; }
.|\n   return 9;
%%
)"};

    const auto file{read_flex(states).front()};

    ASSERT_EQ(file.rules.size(), 4u);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"7"});
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"8"});
    EXPECT_EQ(file.rules[2].token, std::optional<std::string>{"6"});

    // `%option reject` and `%option yymore` declare a use flex cannot see: given `#define MORE yymore()` and the
    // rule `a MORE;`, flex 2.6.4 joins the matches as it does for the call written out, so the option is refused
    // where it stands; turned off, `noreject` and `noyymore`, the options say nothing.
    EXPECT_EQ(
            refusal("%option noyywrap\n%option yymore\n%{\n#define MORE yymore()\n%}\n%%\na    MORE;\nb    return "
                    "7;\n"),
            "line 2: %option yymore declares that an action calls yymore() where flex cannot see it, which appends "
            "the next match to this one, so the next token begins where this match did");
    EXPECT_EQ(refusal(rules("abc    return 7;")), "");
    EXPECT_EQ(refusal("%option reject\n%%\nabc    return 7;\n").substr(0, 22), "line 1: %option reject");
    EXPECT_EQ(refusal("%option reject noreject\n%%\nabc    return 7;\n"), "");
    EXPECT_EQ(refusal("%option noreject noyymore\n%%\nabc    return 7;\n"), "");
    EXPECT_EQ(refusal("%option nonoyymore\n%%\nabc    return 7;\n").substr(0, 8), "line 1: ");
}

TEST(Read_flex, Lex_compat_is_refused_by_name_rather_than_recorded_and_ignored)
{
    // posix-compat changes the same precedence and is refused the same way.
    try
    {
        std::ignore = read_flex("%option posix-compat\n%%\nab{3}   return A;\n");

        FAIL() << "posix-compat must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 1u);
        EXPECT_NE(std::string_view{error.what()}.find("posix-compat"), std::string_view::npos);
    }

    EXPECT_NO_THROW(std::ignore = read_flex("%option posix-compat noposix-compat\n%%\nab{3}   return A;\n"));

    // The two are flags of their own and flex takes that binding while either stands: run on `ab{3}`, flex 2.6.4
    // matches "ababab" under `lex-compat posix-compat nolex-compat` and under `posix-compat lex-compat
    // noposix-compat`, and only "abbb" once both are off.
    EXPECT_THROW(
            std::ignore = read_flex("%option lex-compat posix-compat nolex-compat\n%%\nab{3}   return A;\n"),
            Spec_error);
    EXPECT_THROW(
            std::ignore = read_flex("%option posix-compat lex-compat noposix-compat\n%%\nab{3}   return A;\n"),
            Spec_error);
    EXPECT_NO_THROW(
            std::ignore =
                    read_flex("%option lex-compat posix-compat nolex-compat noposix-compat\n%%\nab{3}   return A;\n"));

    // yylineno changes no language: flex matches "abbb" against `ab{3}` under it, as the reader does.
    EXPECT_NO_THROW(std::ignore = read_flex("%option yylineno\n%%\nab{3}   return A;\n"));

    // Under `%option lex-compat` flex 2.6.4 matches "ababab" against `ab{3}` and refuses "abbb", the repetition
    // binding the whole expression before it; the pattern parser reads the other precedence, so the option is
    // refused where it stands rather than read as if it said nothing.
    try
    {
        std::ignore = read_flex("%option noyywrap\n%option lex-compat\n%%\nab{3}   return A;\n");

        FAIL() << "lex-compat must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 2u);
        EXPECT_NE(std::string_view{error.what()}.find("lex-compat"), std::string_view::npos);
        EXPECT_NE(std::string_view{error.what()}.find("ab{3}"), std::string_view::npos);
    }

    // Turned back off it says nothing, and flex then matches "abbb" as the reader does, leaving "ababab" to the
    // default rule one byte at a time.
    const auto file{read_flex("%option lex-compat nolex-compat\n%%\nab{3}   return A;\n").front()};

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string_view{"abbb"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string_view{"abbb"}).length, 4u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string_view{"ababab"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string_view{"ababab"}).length, 1u);
}

TEST(Read_flex, A_definition_runs_to_the_end_of_its_line_and_carries_a_comment_standing_there)
{
    // flex takes a definition to the end of its line, so a comment there is part of the pattern: flex 2.6.4 builds
    // this file while the definition is unused, and refuses the file that expands it with "unrecognized rule".
    constexpr std::string_view unused{R"(D  [0-9]  /* digits */
%%
[0-9]+   return NUMBER;
%%
)"};

    const auto file{read_flex(unused).front()};

    EXPECT_EQ(file.definitions.at("D"), "[0-9]  /* digits */");

    std::ignore = build(file, "INITIAL");

    const auto expanded{read_flex("D  [0-9]  /* digits */\n%%\n{D}   return NUMBER;\n").front()};

    try
    {
        std::ignore = build(expanded, "INITIAL");

        FAIL() << "a definition carrying a comment must be refused where it is expanded";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 3u);
        EXPECT_NE(std::string_view{error.what()}.find("in definition 'D'"), std::string_view::npos);
    }
}

TEST(Read_flex, A_bracket_expression_ends_the_pattern_only_at_its_own_close)
{
    // flex 2.6.4, run on each of these as a rule of its own, matches the lengths asserted: a POSIX class's `]` is
    // no close, so the blank of `[[:alpha:] ]+` is a member; a `]` first in the bracket is a member and the caret
    // negating one is not a member, so `[^^]` admits every byte but the caret; and a `[:` in any other shape is the
    // `[` and the `:` as members, which leaves `[[:al]pha:]` the bracket `[[:al]` and the text `pha:]`.
    const auto matched{[](const std::string_view pattern, const std::string_view input) {
        const std::string source{"%%\n" + std::string{pattern} + "   return A;\n"};

        const auto file{read_flex(source).front()};

        // The file's one rule and the default rule after it, which takes what the rule does not match.
        if (file.rules.size() != 2 || file.rules.front().pattern != pattern)
        {
            return -2L;
        }

        const auto result{build(file, "INITIAL").tokenize<std::size_t>(input)};

        return result.token == 0 ? static_cast<long>(result.length) : -1L;
    }};

    EXPECT_EQ(matched("[[:alpha:] ]+", "abc def"), 7);
    EXPECT_EQ(matched(R"([[:alpha:]\t ]+)", "abc def"), 7);
    EXPECT_EQ(matched("[[:alpha:][:digit:] ]+", "a1 b"), 4);
    EXPECT_EQ(matched("[^^]+", "abc"), 3);
    EXPECT_EQ(matched("[^^]+", "^"), -1);
    EXPECT_EQ(matched("[]a]+", "a]b"), 2);
    EXPECT_EQ(matched("[^]a]+", "xy]"), 2);
    EXPECT_EQ(matched("[[:alpha:][]+", "a[b"), 3);
    EXPECT_EQ(matched("[a b]+", "a b"), 3);
    EXPECT_EQ(matched("[[:al]pha:]", "apha:]"), 6);
    EXPECT_EQ(matched("[[:alpha]]", "a]"), 2);
    EXPECT_EQ(matched("[[::]]", ":]"), 2);
    EXPECT_EQ(matched("[[:al1pha:]]", "a]"), 2);
    EXPECT_EQ(matched(R"(["a]+)", R"(a"b)"), 2);
    EXPECT_EQ(matched(R"([a\]]+)", "a]b"), 2);
    EXPECT_EQ(matched(R"("[ ]")", "[ ]"), 3);

    // `[^]+` leaves the bracket unclosed, which flex refuses as a bad character class.
    EXPECT_THROW(std::ignore = read_flex("%%\n[^]+   return A;\n"), Spec_error);
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

    // A start-condition scope left open is a parse error in flex, and is refused at the line it opened on.
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n"), 3);
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n%%\n"), 3);
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n}\n"), -1);

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

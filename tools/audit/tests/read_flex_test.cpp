#include "munch/tools/audit/read_flex.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/expression.hpp"

using namespace munch::tools::audit;

namespace
{
/**
 * @brief What one match attempt of a token set answers: the token, if any, and the length.
 */
using Match_t = munch::core::Lexer::Match<std::size_t>;

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

/**
 * @brief An include reader under which every quoted include is an empty header, for the fixtures that include one as
 *        real files do.
 */
const Include_reader_t empty_headers{[](const std::string_view name, std::string_view, Include_form) {
    return std::optional<Included>{{.text = "", .path = std::string{name}}};
}};

/**
 * @brief The tokens a scan of an input emits, each a rule and a length, in order.
 */
using Tokens_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief What a scan of an input left: the tokens it emitted and how much of the input it consumed.
 */
struct Scan
{
    /**
     * @brief The tokens emitted.
     */
    Tokens_t tokens{};

    /**
     * @brief How many bytes the scan consumed.
     */
    std::size_t consumed{};
};

/**
 * @brief Where a file is refused and in what words.
 */
struct Refusal
{
    /**
     * @brief The line the refusal names, none when the file is read.
     */
    std::optional<std::size_t> line{};

    /**
     * @brief The refusal's words, empty when the file is read.
     */
    std::string message{};
};

/**
 * @brief Scans an input with a token set, keeping every token it emits.
 * @param lexer The token set.
 * @param input The input.
 * @return The tokens and how much was consumed.
 */
Scan scan_of(const munch::core::Lexer& lexer, const std::string_view input)
{
    Tokens_t tokens{};

    const auto keep{[&tokens](const std::size_t rule, const std::size_t length) { tokens.emplace_back(rule, length); }};

    const auto consumed{lexer.tokenize_all<std::size_t>(input, keep)};

    return {.tokens = std::move(tokens), .consumed = consumed};
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

/**
 * @brief Returns the refusal of a file read under the command line's case flag and an include reader.
 * @param source The file's text.
 * @param case_insensitive Whether `-i` is given.
 * @param includes How a file the code includes by a quoted name is reached.
 * @return The refusal, no line and no words when the file is read.
 */
Refusal refusal_at(
        const std::string_view source, const bool case_insensitive = false, const Include_reader_t& includes = {})
{
    try
    {
        std::ignore = read_flex(source, {}, includes, case_insensitive);
    }
    catch (const Spec_error& error)
    {
        return {.line = error.line(), .message = error.what()};
    }

    return {.line = std::nullopt, .message = {}};
}

/**
 * @brief Returns the refusal of a scanner's INITIAL token set when it is built.
 * @param file The scanner.
 * @return The refusal, no line and no words when the token set is built.
 */
Refusal build_refusal(const Lexer_spec& file)
{
    try
    {
        std::ignore = build(file, "INITIAL");
    }
    catch (const Spec_error& error)
    {
        return {.line = error.line(), .message = error.what()};
    }

    return {.line = std::nullopt, .message = {}};
}

/**
 * @brief Returns the line a file is refused at.
 * @param source The file's text.
 * @return The line, none when the file is read.
 */
std::optional<std::size_t> line_of(const std::string_view source)
{
    return refusal_at(source).line;
}

/**
 * @brief Returns what the refusal of a file says.
 * @param source The file's text.
 * @return The refusal, empty when the file is read.
 */
std::string refusal_of(const std::string_view source)
{
    return refusal_at(source).message;
}

/**
 * @brief Returns the patterns of a file's rules, or its refusal.
 * @param source The file's text.
 * @return The patterns joined by blanks, or the refusal.
 */
std::string patterns_of(const std::string_view source)
{
    if (const auto [line, message]{refusal_at(source)}; line)
    {
        return message;
    }

    const auto file{read_flex(source).front()};

    const auto written{file.rules | std::views::transform(&Lexer_spec::Rule::pattern)};

    std::string joined{};

    std::ranges::copy(written | std::views::join_with(' '), std::back_inserter(joined));

    return joined;
}

} // namespace

TEST(Read_flex_test, Reads_definitions_options_conditions_and_rules_in_order)
{
    const auto file{read_flex(c_like, {}, empty_headers).front()};

    EXPECT_EQ(file.line, 11U);

    ASSERT_EQ(file.definitions.size(), 2U);
    EXPECT_EQ(file.definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(file.definitions.at("ID"), "[a-zA-Z_][a-zA-Z0-9_]*");

    ASSERT_EQ(file.options.size(), 1U);
    EXPECT_EQ(file.options.front(), "noyywrap");

    ASSERT_EQ(file.conditions.size(), 1U);
    EXPECT_EQ(file.conditions.front().name, "COMMENT");
    EXPECT_TRUE(file.conditions.front().exclusive);

    // Ten rules of the file's and flex's default rule after them: the <<EOF>> rule is not one, and the code after the
    // second %% is not read.
    ASSERT_EQ(file.rules.size(), 11U);
    EXPECT_EQ(file.rules[10].pattern, R"(.|\n)");
    EXPECT_EQ(file.rules[10].line, 24U);

    EXPECT_EQ(file.rules[0].pattern, "{ID}");
    EXPECT_EQ(file.rules[0].line, 12U);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"IDENT"});

    // The multi-line action is read whole and its return found on its second line.
    EXPECT_EQ(file.rules[1].pattern, "{DIGIT}+");
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"NUMBER"});
    EXPECT_TRUE(file.rules[1].action.contains("atoi"));

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

TEST(Read_flex_test, Active_rules_follow_the_start_conditions)
{
    const auto file{read_flex(c_like, {}, empty_headers).front()};

    // INITIAL: every unprefixed rule and the default rule, none of the COMMENT ones.
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 8, 9, 10}));

    // COMMENT is exclusive: only its own rules, and the default rule, which stands in every condition.
    EXPECT_EQ(active_rules(file, "COMMENT"), (std::vector<std::size_t>{6, 7, 10}));
}

TEST(Read_flex_test, The_built_token_set_scans_as_flex_would_and_answers_the_certificates)
{
    const auto file{read_flex(c_like, {}, empty_headers).front()};

    const auto lexer{build(file, "INITIAL")};

    // Longest match, then first rule: "==" is COMPARE, not two OPERATORs; "abc" is IDENT.
    const std::string input{"abc == 42\n"};

    const auto [tokens, consumed]{scan_of(lexer, input)};

    EXPECT_EQ(consumed, input.size());
    EXPECT_EQ(tokens, (Tokens_t{{0, 3}, {8, 1}, {2, 2}, {8, 1}, {1, 2}, {8, 1}}));

    // The conventional C-like shape: the line comment admits every byte but the newline and the whitespace run folds
    // the newline in, so nothing certifies exactly, and newline certifies once the discarded rules, the ones returning
    // nothing, are deleted.
    for (std::size_t value{0}; value < byte_values; ++value)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(value))) << value;
    }

    EXPECT_TRUE(lexer.is_split_point_ignoring('\n'));
}

TEST(Read_flex_test, Start_condition_scopes_and_code_in_actions_read_as_flex_reads_them)
{
    // The shapes PostgreSQL's, sudo's, flex's own and OpenSCAD's scanners use: %top with its brace on the line after a
    // blank, a scope whose opener carries a comment, whose rules are indented and whose close carries a comment,
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
                } else return KEYWORD;
%%
)"};

    const auto file{read_flex(source, {}, empty_headers).front()};

    ASSERT_EQ(file.rules.size(), 5U);

    EXPECT_TRUE(file.rules[0].conditions.empty());
    EXPECT_EQ(file.rules[0].line, 9U);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[1].pattern, R"("*/")");
    EXPECT_EQ(file.rules[2].conditions, (std::vector<std::string>{"xc"}));
    EXPECT_EQ(file.rules[2].action, R"({ if (c == '{') { depth++; } puts("}"); })");
    EXPECT_TRUE(file.rules[3].conditions.empty());
    EXPECT_EQ(file.rules[3].token, std::optional<std::string>{"KEYWORD"});
    EXPECT_TRUE(file.rules[3].action.starts_with("if (keyword(yytext)) {"));

    EXPECT_EQ(active_rules(file, "xc"), (std::vector<std::size_t>{1, 2, 4}));
}

TEST(Read_flex_test, A_scope_opener_s_line_is_code_flex_copies_out_however_far_its_comment_runs)
{
    // flex 2.6.4, run on this file, copies the comment out whole, its second line included, and scans "a" as 1 in S and
    // "b" as 2 in INITIAL, neither in the other condition: nothing on the comment's lines is a rule.
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

    ASSERT_EQ(file.rules.size(), 3U);
    EXPECT_EQ(file.rules[0].pattern, "a");
    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(file.rules[0].line, 6U);
    EXPECT_EQ(file.rules[1].pattern, "b");
    EXPECT_TRUE(file.rules[1].conditions.empty());
    EXPECT_EQ(active_rules(file, "S"), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{1, 2}));

    // Code other than a comment after the brace is copied out the same way, whether or not it compiles later: flex
    // 2.6.4 writes `x return 3;` into the scanner as code and builds no rule from it.
    const auto code{read_flex("%x S\n%%\n<S>{ /* closed */ x return 3;\na   return 1;\n}\nb   return 2;\n").front()};

    ASSERT_EQ(code.rules.size(), 3U);
    EXPECT_EQ(code.rules[0].pattern, "a");
    EXPECT_EQ(code.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(code.rules[1].pattern, "b");
}

TEST(Read_flex_test, A_scope_s_close_line_is_code_flex_copies_out_however_far_its_comment_or_brace_block_runs)
{
    // flex 2.6.4, run on this file, copies the comment out whole and returns 2 for b while echoing w and x: nothing on
    // the comment's lines is a rule.
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

    ASSERT_EQ(file.rules.size(), 3U);
    EXPECT_EQ(file.rules[0].pattern, "a");
    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"S"}));
    EXPECT_EQ(file.rules[1].pattern, "b");
    EXPECT_EQ(file.rules[1].line, 9U);
    EXPECT_TRUE(file.rules[1].conditions.empty());
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{1, 2}));

    // A brace block after the close runs on the same way: flex 2.6.4 returns 2 for b and echoes c.
    const auto block{read_flex("%x S\n%%\n<S>{\na   return 1;\n} { int\nc   = 3; }\nb   return 2;\n").front()};

    ASSERT_EQ(block.rules.size(), 3U);
    EXPECT_EQ(block.rules[1].pattern, "b");
    EXPECT_EQ(block.rules[1].line, 7U);

    // A comment closed on the close's line, or an indented close, is what the scanners in the wild write.
    for (const std::string_view source :
         {"%x S\n%%\n<S>{\na   return 1;\n} /* <S> */\nb   return 2;\n",
          "%x S\n%%\n<S>{\n  a   return 1;\n  } /* done */\nb   return 2;\n"})
    {
        const auto file{read_flex(source).front()};

        EXPECT_EQ(file.rules.size(), 3U) << source;
    }

    // A second close on the close's line is code, so the outer scope stays open, which flex refuses as it does the
    // reading.
    EXPECT_NE(line_of("%x S T\n%%\n<S>{\n<T>{\na   return 1;\n} }\nb   return 2;\n"), std::nullopt);

    // A comment at the margin of the rules section, after the first %% or between rules, in a scope or out of one, is
    // refused: flex 2.6.4 reads its slash as the start of a rule and stops with "unrecognized rule", where its manual
    // asks for the comment to be indented.
    EXPECT_EQ(line_of("%%\n/* the comment start\n * is its own token */\n\"/*\"   return 1;\n"), 2);
    EXPECT_EQ(line_of("%%\na   return 1;\n/* between\nword here */\nb   return 2;\n"), 3);
    EXPECT_EQ(line_of("%x S\n%%\n<S>{\n/* margin comment */\na   return 1;\n}\nb   return 2;\n"), 4);
    EXPECT_EQ(line_of("%x S\n%%\n<S>{\n}\n/* comment at the margin\nword here\n   and ends */\nb   return 2;\n"), 5);
    EXPECT_EQ(line_of("%%\na   return 1;\n  /* between\nword here */\nb   return 2;\n"), std::nullopt);

    // An indented comment is code read to the action's end, so a brace after its close on the line counts, and one
    // never closed is refused as flex refuses it.
    const auto braced{read_flex("%%\na   return 1;\n  /* c */ {\nb   return 2;\n}\nc   return 3;\n").front()};

    EXPECT_EQ(braced.rules.size(), 3U);
    EXPECT_EQ(line_of("%%\na   return 1;\n  /* open\nb   return 2;\n"), 3);
}

TEST(Read_flex_test, A_negated_POSIX_class_is_refused_by_name_and_a_positive_one_is_read)
{
    // flex fills `[:alpha:]` with its ASCII members whatever locale it runs under, and `[:^alpha:]` under that locale,
    // flex 2.6.4 under fr_FR.ISO8859-1 leaving `\xE9` out of it where the C locale keeps it: the negation is refused by
    // name, in a rule and in a definition, and the class itself is read.
    EXPECT_EQ(
            refusal_of("%%\n[[:^alpha:]]   return 7;\n\\xE9   return 8;\n"),
            "line 2: the pattern holds [:^alpha:], which flex fills under the locale it runs under, dropping the "
            "bytes that locale counts in the class beside the ASCII ones, which the file does not decide");
    EXPECT_EQ(
            refusal_of("OTHER [[:^digit:]]\n%%\n{OTHER}   return 7;\n"),
            "line 1: the definition 'OTHER' holds [:^digit:], which flex fills under the locale it runs under, "
            "dropping the bytes that locale counts in the class beside the ASCII ones, which the file does not "
            "decide");
    EXPECT_TRUE(refusal_of("%%\n[[:alpha:]]+   return 7;\n\\xE9   return 8;\n").empty());

    // A quote inside a bracket is a member, so the class after it is read, in a rule and in a definition.
    EXPECT_TRUE(refusal_of("%%\n[^\"[:^print:]]+   return 7;\n").contains("holds [:^print:]"));
    EXPECT_TRUE(refusal_of("%%\n[\"'][[:^alpha:]]   return 7;\n").contains("holds [:^alpha:]"));
    EXPECT_TRUE(refusal_of("DEF [^\"[:^print:]]\n%%\n{DEF}+   return 7;\n").contains("'DEF' holds [:^print:]"));

    // Under the case option flex folds a byte beyond ASCII under its locale, flex 2.6.4 under fr_FR.ISO8859-1 giving
    // `\xE9` its other case `\xC9` where the C locale gives none: such a byte is refused by name under the option,
    // written out, as `\xHH` or as an octal escape, in a rule and in a definition, and read otherwise.
    EXPECT_EQ(
            refusal_of("%option case-insensitive\n%%\n\\xE9   return 7;\n\\xC9   return 8;\n"),
            "line 3: the pattern spells the byte \\xE9 under the case option, which flex folds under the locale it "
            "runs under, giving the byte its other case there and not under the C locale, which the file does not "
            "decide");
    EXPECT_TRUE(refusal_of("%option caseless\n%%\n[a\\xC9]   return 7;\n").contains(R"(spells the byte \xC9)"));
    EXPECT_TRUE(refusal_of("%option caseless\n%%\na\\351   return 7;\n").contains(R"(spells the byte \351)"));
    EXPECT_TRUE(refusal_of("%option caseless\n%%\na\xE9   return 7;\n").contains(R"(spells the byte \xE9 under)"));
    EXPECT_TRUE(refusal_of("%option caseless\nE [\\xE9]\n%%\n{E}   return 7;\n")
                        .contains(R"(the definition 'E' spells the byte \xE9)"));
    EXPECT_TRUE(refusal_of("%option caseless\n%%\n\\x41\\101\\n[a-z]   return 7;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n\\xE9   return 7;\n\\xC9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%option caseless\n%option nocaseless\n%%\n\\xE9   return 7;\n").empty());

    // A `(?i:` group folds case inside itself, under the same locale, flex 2.6.4 under fr_FR.ISO8859-1 taking
    // `(?i:a|\xE9)+` over `a\xC9` as one token where the C locale takes two: once the file folds case anywhere, a byte
    // beyond ASCII in it is refused, in the group, outside it or in a definition, naming the group; a group turning the
    // option off, `(?-i:`, or setting another flag, folds nothing.
    EXPECT_EQ(
            refusal_of("%%\n(?i:a|\\xE9)+   return 7;\n\\xC9   return 8;\n"),
            "line 2: the pattern spells the byte \\xE9 beside the group (?i: that folds case, which flex folds under "
            "the locale it runs under, giving the byte its other case there and not under the C locale, which the "
            "file does not decide");
    EXPECT_TRUE(refusal_of("E [\\xE9]\n%%\n(?is:a{E})+   return 7;\n")
                        .contains(R"('E' spells the byte \xE9 beside the group (?is:)"));
    EXPECT_TRUE(refusal_of("%%\n(?i:a)+   return 7;\n\\xC9   return 8;\n").contains("beside the group (?i:"));
    EXPECT_TRUE(refusal_of("%%\n(?-i:a)+   return 7;\n\\xC9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n(?i-i:a)   return 7;\n\\xE9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n(?-ii:a)   return 7;\n\\xE9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n(?-si:a)   return 7;\n\\xE9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n(?is-i:a)   return 7;\n\\xE9   return 8;\n").empty());

    // flex's `-i` is the case option on before the file's own words, which override it: the reader records it as the
    // first option word, so that `%option caseful` in the file stands last and turns it off again, as flex 2.6.4 with
    // `-i` and that file scans case-sensitively; a byte beyond ASCII is refused under it as under the word.
    const auto folded{read_flex("%%\na+   return 7;\n", {}, {}, true).front()};

    const auto unfolded{read_flex("%option caseful\n%%\na+   return 7;\n", {}, {}, true).front()};

    const auto plain{read_flex("%%\na+   return 7;\n").front()};

    EXPECT_EQ(folded.options, (std::vector<std::string>{"case-insensitive"}));
    EXPECT_EQ(unfolded.options, (std::vector<std::string>{"case-insensitive", "caseful"}));
    EXPECT_TRUE(plain.options.empty());
    EXPECT_TRUE(refusal_of("%%\n\\xE9   return 8;\n").empty());

    const auto [folded_line, folded_message]{refusal_at("%%\n\\xE9   return 8;\n", true)};

    EXPECT_TRUE(folded_message.contains("under the case option"));

    EXPECT_TRUE(refusal_of("%%\n(?s:a)+   return 7;\n\\xC9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n(?i:a)+   return 7;\nb   return 8;\n").empty());

    // A group opens outside a quote and a bracket alone, `"(?i:"` being text and `[(?i:]` members; and an escape before
    // a raw byte beyond ASCII, `\\\xE9` in an ISO-8859-1 file, spells that byte, flex 2.6.4 under fr_FR.ISO8859-1
    // taking `(a|\\\xE9)+` under `caseless` over `a\xC9` as one token where C takes two.
    EXPECT_TRUE(refusal_of("%%\n\"(?i:\"   return 7;\n\\xE9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n[(?i:]   return 7;\n\\xE9   return 8;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n\"(?i:\"(?i:a)   return 7;\n\\xE9   return 8;\n").contains("beside the group (?i:"));
    EXPECT_TRUE(refusal_of("%option caseless\n%%\n(a|\\\xE9)+   return 7;\n")
                        .contains(R"(spells the byte \xE9 under the case option)"));
    EXPECT_TRUE(refusal_of("%%\n(a|\\\xE9)+   return 7;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n\"[:^alpha:]\"   return 7;\n").empty());
    EXPECT_TRUE(refusal_of("%%\n[[:^]   return 7;\n").empty());
}

TEST(Read_flex_test, An_action_ends_where_flex_s_action_scanner_ends_it)
{
    const auto patterns{[](const std::string_view rules) {
        const auto source{std::format("%option noyywrap\n%%\n{}", rules)};

        return patterns_of(source);
    }};

    // flex 2.6.4 was run on each file below with `%option noyywrap` and a driver returning the tokens, and the
    // assertions say what it built. flex's action scanner has no state for a `//` comment, so a brace after one counts:
    // an open one runs the action on to the next close, making `b return 2;` code, which the action reading then
    // refuses at a's line as an action returning 1 in one statement and ending in another, and a close one ends the
    // action at its line, so that b and c are rules; flex builds both scanners, whether or not the C it emits compiles.
    EXPECT_TRUE(patterns("a   { return 1; } // {\nb   return 1;\n}\nc   return 3;\n").starts_with("line 3: "));
    EXPECT_EQ(patterns("a   { // }\nb   return 2;\nc   return 3;\n"), R"(a b c .|\n)");

    // A quote after a `//` opens a literal, which the line's end closes where the braces balance: flex ends the action
    // inside the literal and its m4 stops with "end of file in string", so the file is refused by name.
    EXPECT_EQ(
            patterns("a   { return 1; } // it's\nb   return 1;\n"),
            "line 3: a quote is left open at the end of the action's line, where flex ends the action inside the "
            "literal and never closes the code it emits for it, so that the m4 it runs stops with an end of file in "
            "string");
    EXPECT_TRUE(patterns("a   puts(\"x\nb   return 2;\n").starts_with("line 3: "));

    // A brace inside a literal or a block comment does not count, and a comment runs over lines.
    EXPECT_EQ(patterns("a   { puts(\"}\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { if (c == '\\'' || c == '}') return 1; return 1; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { /* }\n      */ return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // A literal ends at its line's end where a brace is open, and the action goes on: here the quote on the next line
    // opens another literal that swallows the close, so flex meets the end of the file inside the action.
    EXPECT_EQ(
            patterns("a   { puts(\"x\ny\"); return 1; }\nb   return 2;\n"), "line 3: the action's braces never close");
    EXPECT_TRUE(patterns("a   { puts(\"x\n}\"); return 1; }\nb   return 2;\n").starts_with("line 3: "));

    // A backslash before the newline carries the literal on to the next line, as C splices lines, so a brace-less
    // action spans both lines and `c return 3;` is inside the literal, no rule; an escape may span a splice too.
    EXPECT_EQ(patterns("a   puts(\"x\\\ny\"); return 1;\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   puts(\"x\\\nc   return 3;\"); return 1;\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { puts(\"x\\\n}\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   { puts(\"\\\\\nn\"); return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // Outside a literal a backslash before the newline is no splice: the action ends there.
    EXPECT_EQ(patterns("a   return 1; \\\nb   return 2;\n"), R"(a b .|\n)");

    // A stray close counts below zero and the action still ends at its line; flex builds the scanner, though the C it
    // emits does not compile.
    EXPECT_EQ(patterns("a   return 1; }\nb   return 2;\n"), R"(a b .|\n)");

    // A `|` line is taken unread, a brace after the bar counting for nothing.
    EXPECT_EQ(patterns("a   | {\nb   return 2;\nc   return 3;\n"), R"(a b c .|\n)");

    // An action opening with `%{` runs to the end of the first line holding `%}`, comments and literals unread, so the
    // rule-looking line inside is code and a brace after the `%}` counts for nothing; one never closed is refused, as
    // flex refuses it.
    EXPECT_EQ(patterns("a   %{\nc   return 1;\n    return 1; %}\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(patterns("a   %{ return 1; %} {\nb   return 2;\n"), R"(a b .|\n)");
    EXPECT_EQ(
            patterns("a   %{ return 1;\nb   return 2;\n"),
            "line 3: the action's %{ block is never closed, which flex refuses");

    // A comment left open at the end of the file is refused as flex refuses it.
    EXPECT_EQ(
            patterns("a   { /* open\nb   return 2;\n"),
            "line 3: the action's comment is never closed, which flex refuses as an end of file inside an action");
}

TEST(Read_flex_test, Scopes_nest_and_a_prefixed_rule_inside_one_is_active_in_both)
{
    // flex's own scanner nests scopes and prefixes rules inside them; flex 2.6.4, run on this grammar, fires x in A and
    // B, y in A, B and C, and z in A alone.
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

    ASSERT_EQ(file.rules.size(), 5U);

    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"B", "A"}));
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"A", "B", "C"}));
    EXPECT_EQ(file.rules[2].conditions, (std::vector<std::string>{"A"}));
    EXPECT_TRUE(file.rules[3].conditions.empty());
    EXPECT_EQ(active_rules(file, "A"), (std::vector<std::size_t>{0, 1, 2, 4}));
    EXPECT_EQ(active_rules(file, "B"), (std::vector<std::size_t>{0, 1, 4}));
    EXPECT_EQ(active_rules(file, "C"), (std::vector<std::size_t>{1, 4}));
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{3, 4}));
}

TEST(Read_flex_test, A_start_condition_prefix_alone_on_its_line_opens_the_next)
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

    ASSERT_EQ(file.rules.size(), 3U);

    EXPECT_EQ(file.rules[0].conditions, (std::vector<std::string>{"INITIAL", "a", "b"}));
    EXPECT_EQ(file.rules[0].pattern, R"(",")");
    EXPECT_EQ(file.rules[0].line, 5U);
    EXPECT_EQ(file.rules[1].conditions, (std::vector<std::string>{"b"}));
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"X"});
    EXPECT_EQ(file.rules[1].line, 9U);
}

TEST(Read_flex_test, A_section_delimiter_is_a_margin_percent_pair_whatever_follows_it_and_an_indented_line_is_a_rule)
{
    // The patterns read from a file, or its refusal: flex 2.6.4 lexes a delimiter as `%%` at the start of a line and
    // drops the rest of the line, so a comment or text after either delimiter changes nothing, and it builds each file
    // here as the assertion says.
    EXPECT_EQ(patterns_of("%option noyywrap\n%% /* rules */\na   return 1;\n%%\n"), R"(a .|\n)");
    EXPECT_EQ(
            patterns_of("%option noyywrap\n%%\na   return 1;\n%% /* user code */\nint helper(void) { return 0; }\n"
                        "word   return 9;\n"),
            R"(a .|\n)");
    EXPECT_EQ(patterns_of("%option noyywrap\n%%x y z\na   return 1;\n%%more text\nword   return 9;\n"), R"(a .|\n)");
    EXPECT_EQ(patterns_of("%option noyywrap\n%%   \na   return 1;\n%%\t\nint f(void) { return 0; }\n"), R"(a .|\n)");
    EXPECT_EQ(patterns_of("%option noyywrap\r\n%%\r\na   return 1;\r\n%%\r\n"), R"(a .|\n)");

    // The scanner's line is the first delimiter's, whatever follows it.
    const auto delimited{read_flex("%option noyywrap\n%% /* rules */\na   return 1;\n").front()};

    EXPECT_EQ(delimited.line, 2U);

    // An indented `%%` is no delimiter: in the definitions section it is code flex copies out, so the delimiter is the
    // margin's `%%` below, and in the rules section it is an indented rule matching the two bytes, discarded, which
    // flex 2.6.4 builds and which scans "a%%b" as 1, the two bytes, then 2.
    EXPECT_EQ(patterns_of("%option noyywrap\n  %%\n%%\na   return 1;\n%%\n"), R"(a .|\n)");
    EXPECT_EQ(patterns_of("%option noyywrap\n%%\na   return 1;\n  %%\nb   return 2;\n%%\n"), R"(a %% b .|\n)");
    EXPECT_EQ(patterns_of("%option noyywrap\n%%\n"), R"(.|\n)");

    // After the first rule flex reads an indented line as a rule, in a scope or out of one; only the indented lines
    // before the first rule, the section's prologue, are code: flex 2.6.4 returns 2 for b from the first file and
    // builds the second with a as its one rule.
    const auto indented{read_flex("%option noyywrap\n%%\na   return 1;\n  b   return 2;\n%%\n").front()};

    ASSERT_EQ(indented.rules.size(), 3U);
    EXPECT_EQ(indented.rules[1].pattern, "b");
    EXPECT_EQ(indented.rules[1].token, std::optional<std::string>{"2"});
    EXPECT_EQ(indented.rules[1].line, 4U);
    EXPECT_EQ(
            patterns_of("%option noyywrap\n%%\n  int n = 0;\n  /* prologue comment */\n\na   return 1;\n%%\n"),
            R"(a .|\n)");

    // A file with no delimiter at the margin has no rules section.
    EXPECT_EQ(
            patterns_of("%option noyywrap\n  %%\na   return 1;\n"),
            "line 4: the file has no rules section: no line begins with %%");
}

TEST(Read_flex_test, Case_insensitive_scanners_fold_every_letter_of_every_pattern)
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

    const auto file{read_flex(source).front()};

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(token_at(lexer, "SeLeCt"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(lexer, "SELECTs"), std::optional<std::size_t>{1});
    EXPECT_EQ(length_at(lexer, "Foo_1 "), 5U);
    EXPECT_EQ(token_at(lexer, "\t x"), std::optional<std::size_t>{2});
}

TEST(Read_flex_test, The_case_option_is_what_the_last_word_naming_it_left_standing)
{
    // flex sets its options before it parses a rule, so order decides and every spelling counts: run on each of these,
    // flex 2.6.4 matches 'A' against the rule `a` exactly where the assertion says it does, and elsewhere leaves it to
    // the default rule.
    const auto folds{[](const std::string_view options) {
        const auto source{std::format("%option {}\n%%\na   return A;\n", options)};

        const auto file{read_flex(source).front()};

        const auto lexer{build(file, "INITIAL")};

        return token_at(lexer, "A") == std::optional<std::size_t>{0};
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
    // `nonocaseless` and not under `nononocaseless` or `nonocaseful`. A `no` standing as a word of its own reaches no
    // name, so the sense begins afresh at the next word.
    EXPECT_TRUE(folds("nonocaseless"));
    EXPECT_FALSE(folds("nononocaseless"));
    EXPECT_FALSE(folds("nonocaseful"));
    EXPECT_TRUE(folds("no caseless"));
    EXPECT_TRUE(folds("no\n%option caseless"));

    // `%option i` is no flex option, so it folds nothing; flex refuses the file that names it.
    EXPECT_FALSE(folds("i"));
}

TEST(Read_flex_test, A_quoted_option_value_is_one_word_and_no_option_of_its_own)
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

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(token_at(lexer, "A"), std::optional<std::size_t>{1});
    EXPECT_EQ(token_at(lexer, "a"), std::optional<std::size_t>{0});

    // Nor is a refused option's name inside the quotes a refusal: flex builds this scanner, and so does the reading.
    EXPECT_EQ(line_of("%option header-file=\"generated lex-compat scanner.h\"\n%%\nab{3}   return A;\n"), std::nullopt);

    // A word after the value is an option again, as flex lexes it, with or without a blank after the closing quote:
    // flex 2.6.4 folds "A" under either line.
    const auto spaced{read_flex("%option header-file=\"a b.h\" caseless\n%%\na   return A;\n").front()};

    const auto joined{read_flex("%option header-file=\"a b.h\"caseless\n%%\na   return A;\n").front()};

    EXPECT_TRUE(spaced.parse.caseless);
    EXPECT_TRUE(joined.parse.caseless);

    // A quote left open is an option flex does not recognize, so the line is refused.
    EXPECT_EQ(line_of("%option noyywrap\n%option header-file=\"a b.h caseless\n%%\na   return A;\n"), 2U);
}

TEST(Read_flex_test, The_default_rule_stands_after_the_file_s_rules_unless_nodefault_drops_it)
{
    // flex 2.6.4, run on this file, returns 1 for a, echoes b, newline and c one byte at a time returning nothing, and
    // in S echoes x the same way: its default rule, added after the file's own, matches one byte wherever no rule does,
    // in every start condition.
    constexpr std::string_view plain{R"(%option noyywrap
%x S
%%
a       return 1;
s       BEGIN(S); return 2;
<S>t    BEGIN(INITIAL); return 3;
%%
)"};

    const auto file{read_flex(plain).front()};

    ASSERT_EQ(file.rules.size(), 4U);
    EXPECT_EQ(file.rules[3].pattern, R"(.|\n)");
    EXPECT_EQ(file.rules[3].conditions, (std::vector<std::string>{"*"}));
    EXPECT_EQ(file.rules[3].action, "ECHO;");
    EXPECT_FALSE(file.rules[3].token.has_value());
    EXPECT_EQ(file.rules[3].line, 7U);
    EXPECT_EQ(active_rules(file, "INITIAL"), (std::vector<std::size_t>{0, 1, 3}));
    EXPECT_EQ(active_rules(file, "S"), (std::vector<std::size_t>{2, 3}));

    // Every byte the rules leave out is one token of the default rule, discarded; the rules keep their own.
    const auto initial{build(file, "INITIAL")};

    const auto in_s{build(file, "S")};

    const auto [initial_tokens, initial_consumed]{scan_of(initial, "ab\nca")};

    const auto [s_tokens, s_consumed]{scan_of(in_s, "xt")};

    EXPECT_EQ(initial_tokens, (Tokens_t{{0, 1}, {3, 1}, {3, 1}, {3, 1}, {0, 1}}));
    EXPECT_EQ(s_tokens, (Tokens_t{{3, 1}, {2, 1}}));

    // So every byte begins a token, and a byte no rule consumes past its first position certifies.
    EXPECT_TRUE(initial.is_split_point('\n'));
    EXPECT_TRUE(initial.is_split_point('a'));

    // The default rule ranks below every rule of the file's, so a rule matching the same one byte wins, and a longer
    // rule that fails midway leaves flex the one byte: on "abd" against `abc`, flex 2.6.4 echoes a, b and d.
    const auto longer_file{read_flex("%%\nabc   return 1;\n").front()};

    const auto longer{build(longer_file, "INITIAL")};

    const auto [abd_tokens, abd_consumed]{scan_of(longer, "abd")};

    const auto [abc_tokens, abc_consumed]{scan_of(longer, "abc")};

    EXPECT_EQ(abd_tokens, (Tokens_t{{1, 1}, {1, 1}, {1, 1}}));
    EXPECT_EQ(abc_tokens, (Tokens_t{{0, 3}}));

    // Without a second %% the rule takes the file's last line.
    const auto unended{read_flex("%%\na   return 1;\n").front()};

    EXPECT_EQ(unended.rules.back().line, 2U);

    // Under `%option nodefault` flex 2.6.4 stops with "flex scanner jammed" at b, in INITIAL and in S alike, which is
    // what the token set answers of itself: no rule is added, and b begins no token.
    const auto without{[](const std::string_view options) {
        const auto source{std::format("%option {}\n%%\na   return 1;\n", options)};

        const auto read{read_flex(source).front()};

        const auto lexer{build(read, "INITIAL")};

        return read.rules.size() == 1 && !token_at(lexer, "b");
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

TEST(Read_flex_test, The_options_that_narrow_flex_s_alphabet_are_refused_by_name)
{
    // flex resolves the alphabet's width as its own check_options() does: a width named outright decides, and otherwise
    // a `full` or `fast` table with the equivalence classes off leaves it at 128 bytes. Given these same option lines
    // and a rule naming the byte \x80, flex 2.6.4 refuses to build a scanner for exactly the ones asserted refused
    // here, with "scanner requires -8 flag to use the character \200".
    const auto refused{[](const std::string_view options) {
        const auto source{std::format("%option {}\n%%\n[^a]+   return A;\n", options)};

        return refusal_of(source).contains("128 bytes of ASCII");
    }};

    EXPECT_FALSE(refused("noyywrap"));
    EXPECT_TRUE(refused("7bit"));
    EXPECT_FALSE(refused("8bit"));
    EXPECT_FALSE(refused("no7bit"));
    EXPECT_TRUE(refused("no8bit"));
    EXPECT_FALSE(refused("7bit 8bit"));
    EXPECT_TRUE(refused("8bit 7bit"));

    // A full or fast table drops the equivalence classes, whatever the word's sense, and the default width goes with
    // them; naming the width outright, or asking the classes back, leaves the 256 bytes standing.
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
    const auto [line, message]{refusal_at("%option noyywrap\n%option full\n%%\n[^a]+   return A;\n")};

    EXPECT_EQ(line, 2U);
    EXPECT_TRUE(message.contains("%option full"));
}

TEST(Read_flex_test, The_actions_that_move_a_match_s_bounds_or_rerun_it_are_refused_by_name_with_their_line)
{
    // What each does in flex 2.6.4, run on a scanner of the rule shown before `b return 8;` and `.|\n return 9;` under
    // nodefault: `a yymore();` on "ab" returns one token 7 spanning both bytes, where the rules alone give a discarded
    // a and then b; `abc REJECT;` before a second `abc return 7;` returns 7 for "abc", the rule the rules alone would
    // never reach, and before `ab return 8;` alone returns 8 for "ab" and 9 for "c"; `ab { yyless(1); return 7; }` on
    // "ab" returns 7 for a and 8 for b; `a { unput('b'); return 7; }` on "a" returns 7 and then 8 for a b the input
    // never held; `a { input(); return 7; }` on "ab" returns 7 and nothing for b.
    const auto rules{[](const std::string_view first) {
        return std::format("%option noyywrap\n%%\n{}\nb      return 8;\n.|\\n   return 9;\n", first);
    }};

    EXPECT_EQ(
            refusal_of(rules("a    yymore();")),
            "line 3: the action calls yymore(), which appends the next match to this one, so the next token begins "
            "where this match did");
    EXPECT_EQ(
            refusal_of(rules("abc    REJECT;\nabc    return 7;")),
            "line 3: the action uses REJECT, which drops the match for the next rule's, so which rule matches is not "
            "the rules' longest match and first rule");
    EXPECT_EQ(
            refusal_of(rules("ab     { yyless(1); return 7; }")),
            "line 3: the action calls yyless(), which gives the end of the match back to be matched again, so the "
            "next token begins inside this match");
    EXPECT_EQ(
            refusal_of(rules("a      { unput('b'); return 7; }")),
            "line 3: the action calls unput(), which pushes a byte onto the input, so the next token is matched "
            "against bytes the input may not hold");
    EXPECT_EQ(
            refusal_of(rules("a      { input(); return 7; }")),
            "line 3: the action calls input(), which consumes bytes no rule matched, so the next token begins past "
            "them");
    EXPECT_TRUE(refusal_of(rules("a      { c = yyinput (); return 7; }")).contains("calls yyinput()"));

    // An `<<EOF>>` action runs where no match is, so it is read unchecked; a `|` rule above it runs it on that rule's
    // match, `a |` over `<<EOF>> { yymore(); }` returning one token 8 spanning "ab" and over `<<EOF>> { yyless(1);
    // return 7; }` returning 7 for a and 8 for b, so the shared action is checked at the sharing rule's line, and
    // unshared it is not.
    EXPECT_TRUE(refusal_of(rules("a |\n<<EOF>> { yymore(); }")).starts_with("line 3: the action calls yymore()"));
    EXPECT_TRUE(refusal_of(rules("a |\n<<EOF>> { yyless(1); return 7; }"))
                        .starts_with("line 3: the action calls yyless()"));
    EXPECT_EQ(refusal_of(rules("a |\n<<EOF>> { return 7; }")), "");
    EXPECT_EQ(refusal_of(rules("<<EOF>> { yymore(); }")), "");

    // The line is the call's rule's, wherever in a multi-line action the call stands.
    EXPECT_TRUE(refusal_of(rules("a      {\n    return 7;\n}\nab     {\n    yyless(1);\n}")).starts_with("line 6: "));

    // Read as C reads the action: a comment or a literal holds no call, a word inside a longer one is none, a name of
    // the file's own without a parenthesis or reached as a member is none, and flex takes `reject` and `Reject` as
    // names of the file's own, building the scanner without the REJECT machinery.
    EXPECT_EQ(refusal_of(rules(R"(abc    { /* REJECT */ puts("REJECT"); return 7; })")), "");
    EXPECT_EQ(refusal_of(rules("abc    { return 7; // REJECT\n       }")), "");
    EXPECT_EQ(refusal_of(rules("abc    { reject++; Reject++; REJECTED++; return 7; }")), "");
    EXPECT_EQ(refusal_of(rules("abc    { yylval.input = 1; myinput(); s.inputs(); return 7; }")), "");
    EXPECT_TRUE(refusal_of(rules(R"(abc    { if (c == '\'') input(); return 7; })")).starts_with("line 3: "));

    // A `|` line is taken unread, as flex takes it, so a call written after the bar is no call.
    EXPECT_EQ(refusal_of(rules("abc    | REJECT\nabd    return 7;")), "");

    // A condition change is not a call of these: flex 2.6.4 returns 7, 8 and 6 for "abb" under this scanner, the tokens
    // where the rules put them, and the reading takes the actions as any other, the caveat over every report covering
    // the condition.
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

    ASSERT_EQ(file.rules.size(), 4U);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"7"});
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"8"});
    EXPECT_EQ(file.rules[2].token, std::optional<std::string>{"6"});

    // `%option reject` and `%option yymore` declare a use flex cannot see: given `#define MORE yymore()` and the rule
    // `a MORE;`, flex 2.6.4 joins the matches as it does for the call written out, so the option is refused where it
    // stands; turned off, `noreject` and `noyymore`, the options say nothing.
    EXPECT_EQ(
            refusal_of("%option noyywrap\n%option yymore\n%{\n#define MORE yymore()\n%}\n%%\na    MORE;\n"
                       "b    return 7;\n"),
            "line 2: %option yymore declares that an action calls yymore() where flex cannot see it, which appends "
            "the next match to this one, so the next token begins where this match did");
    EXPECT_EQ(refusal_of(rules("abc    return 7;")), "");
    EXPECT_TRUE(refusal_of("%option reject\n%%\nabc    return 7;\n").starts_with("line 1: %option reject"));
    EXPECT_EQ(refusal_of("%option reject noreject\n%%\nabc    return 7;\n"), "");
    EXPECT_EQ(refusal_of("%option noreject noyymore\n%%\nabc    return 7;\n"), "");
    EXPECT_TRUE(refusal_of("%option nonoyymore\n%%\nabc    return 7;\n").starts_with("line 1: "));
}

TEST(Read_flex_test, A_macro_or_a_conditional_that_hides_what_an_action_does_is_refused)
{
    const auto rules{[](const std::string_view first) {
        return std::format("%option noyywrap\n%%\n{}\nb      return 8;\n.|\\n   return 9;\n", first);
    }};

    // A macro the file defines is expanded by nothing here. One whose replacements hold a word the reading gives
    // meaning to, a call that moves the match or a `return`, is opaque, and calling it is refused, wherever and however
    // often it is defined, through another macro, and in an argument as much as in a name: gcc makes `CALL(input)` the
    // call `input()` under `#define CALL(f) f()`. flex copies the sections' code into the scanner, so a macro defined
    // there is defined for every action: the hook and the action each reach input() through one, and gcc expands both
    // to the same call, which consumes the b after the a+ match. A macro whose replacements hold no such word, `#define
    // MAX 10`, changes nothing the reading looks for and its uses are read as ordinary text. What a macro makes of an
    // argument is not decided, so `WRAP(input)` is refused under `#define WRAP(f) 0` too, an over-refusal taken rather
    // than an expansion guessed.
    EXPECT_TRUE(
            refusal_of("%{\n#define ADVANCE() input();\n#define YY_USER_ACTION ADVANCE()\n%}\n%option noyywrap\n%%\n"
                       "a+     return 7;\n")
                    .starts_with("line 3: "));
    EXPECT_TRUE(
            refusal_of(
                    "%{\n#define YY_USER_ACTION ADVANCE()\n%}\n%{\n#define ADVANCE() input();\n%}\n%option noyywrap\n"
                    "%%\na+     return 7;\n")
                    .starts_with("line 2: "));
    EXPECT_TRUE(
            refusal_of(
                    "%{\n#define YY_USER_ACTION ADVANCE()\n%}\n%option noyywrap\n%%\n    #define ADVANCE() input();\n"
                    "a+     return 7;\n")
                    .starts_with("line 2: "));
    EXPECT_TRUE(
            refusal_of("%{\n#define ADVANCE() input();\n%}\n%option noyywrap\n%%\na+     { ADVANCE(); return 7; }\n")
                    .starts_with("line 6: "));
    EXPECT_TRUE(refusal_of("%{\n#define CALL(f) f()\n%}\n%option noyywrap\n%%\na+     { CALL(input); return 7; }\n")
                        .starts_with("line 6: "));
    EXPECT_TRUE(refusal_of("%{\n#define EXEC(...) __VA_ARGS__\n%}\n%option noyywrap\n%%\n"
                           "a+     { EXEC(input()); return 7; }\n")
                        .starts_with("line 6: "));
    EXPECT_TRUE(
            refusal_of("%{\n#define CAT(a,b) a ## b\n%}\n%option noyywrap\n%%\na+     { CAT(in,put)(); return 7; }\n")
                    .starts_with("line 6: "));
    EXPECT_TRUE(refusal_of("%{\n#define ADVANCE() /* tracing\n */ input();\n#define YY_USER_ACTION ADVANCE()\n%}\n"
                           "%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 4: "));
    EXPECT_TRUE(refusal_of("%{\n#define WRAP(f) 0\n%}\n%option noyywrap\n%%\na+     { WRAP(input); return 7; }\n")
                        .starts_with("line 6: "));
    EXPECT_EQ(
            refusal_of(
                    "%{\n#define MAX 10\n%}\n%option noyywrap\n%%\na+     { if (yyleng < MAX) return 7; return 7; }\n"),
            "");

    // Which arm of a conditional is live is the build's to decide, so a stretch holding one is refused rather than read
    // with an arm guessed, in an action and in the copied code alike.
    EXPECT_TRUE(refusal_of("%option noyywrap\n%%\na+ {\n#if 0\n (void)0;\n#elif 1\n return 7;\n#endif\n}\n")
                        .starts_with("line 3: "));
    EXPECT_TRUE(
            refusal_of(
                    "%{\n#if 0\n#define YY_USER_ACTION input();\n#endif\n%}\n%option noyywrap\n%%\na+     return 7;\n")
                    .starts_with("line 1: "));

    // flex writes `YY_BREAK` after every action too, so a definition of the file's own runs after every action; a
    // directive inside an action defines or conditions code the reading does not follow; and an action that returns
    // different tokens from different places emits one the text alone does not decide, where returning the same token
    // from each is read as that token. The compiled scanners: YY_BREAK as `return 9;` returns 9 over "aa" from an
    // action of `;`, the action-defined macro leaves `a+` returning 7 and `b` returning 9, and the conditional return
    // gives 7 over "aa" and 8 over "a".
    EXPECT_TRUE(refusal_of("%{\n#define YY_BREAK return 9;\n%}\n%option noyywrap\n%%\na+     ;\nb      return 8;\n")
                        .starts_with("line 2: "));
    EXPECT_TRUE(refusal_of(rules("a      {\n#define TAG() return 9\n return 7; }\nb      { TAG(); }"))
                        .starts_with("line 3: "));
    EXPECT_TRUE(refusal_of(rules("a+     { if (yyleng >= 2) return 7; return 8; }")).starts_with("line 3: "));
    EXPECT_EQ(refusal_of(rules("a+     { if (yyleng >= 2) return 7; return 7; }")), "");

    // Only a plain value is transparent: a replacement holding any name or operator may change what the action does
    // whatever stands beside it, `#define SELF this` making `SELF->yyinput()` the call the action does not spell, so
    // `count++` is refused where `10` is read as text.
    EXPECT_TRUE(refusal_of("%{\n#define STEP() count++;\n%}\n%option noyywrap\n%%\na+     { STEP(); return 7; }\n")
                        .starts_with("line 6: "));
    EXPECT_TRUE(refusal_of("%{\n#define SELF this\n%}\n%option noyywrap\n%%\na+     { SELF->yyinput(); return 7; }\n")
                        .starts_with("line 6: "));

    // A raw string's newlines end no directive, so the call after the literal is part of the replacement.
    EXPECT_TRUE(refusal_of("%{\n#define ADVANCE() (void)R\"(text\n)\"; yyinput()\n%}\n%option noyywrap\n%%\n"
                           "a+     { ADVANCE(); return 7; }\n")
                        .starts_with("line 7: "));
}

TEST(Read_flex_test, A_call_and_a_member_are_read_past_the_blanks_and_comments_parting_their_tokens)
{
    // What flex 2.6.4 does on "ab" under nodefault with `b return 8;` and `.|\n return 9;` after the rule shown: `a
    // yymore /* c */ ();` returns one token 7 spanning both bytes, as `a yymore();` does, and `ab { yyless /* c */ (1);
    // return 7; }` and the same with a newline before the `(1)` return 7 for a and 8 for b, as the call written plainly
    // does.
    const auto rules{[](const std::string_view first) {
        return std::format("%option noyywrap nodefault\n%%\n{}\nb      return 8;\n.|\\n   return 9;\n", first);
    }};

    EXPECT_EQ(
            refusal_of(rules("a      yymore /* include the following match */ ();")),
            "line 3: the action calls yymore(), which appends the next match to this one, so the next token begins "
            "where this match did");
    EXPECT_TRUE(refusal_of(rules("ab     { yyless /* retain only the prefix */ (1); return 7; }"))
                        .starts_with("line 3: the action calls yyless()"));
    EXPECT_TRUE(refusal_of(rules("ab     { yyless\n         (1); return 7; }"))
                        .starts_with("line 3: the action calls yyless()"));
    EXPECT_TRUE(refusal_of(rules("a      { unput\t/* b */\n\t('b'); return 7; }"))
                        .starts_with("line 3: the action calls unput()"));

    // A name apart from a call is a use the reading cannot follow: `a { (input)(); return 7; }` consumes the b as
    // `input()` does, flex 2.6.4 returning 7 alone on "ab", and a pointer taken to it may be called anywhere.
    EXPECT_EQ(
            refusal_of(rules("a      { (input)(); return 7; }")),
            "line 3: the action names input apart from a call, so what it does with it is out of sight; called, it "
            "consumes bytes no rule matched, so the next token begins past them");
    EXPECT_TRUE(refusal_of(rules("a      { int (*next)(void) = yyinput; next(); return 7; }"))
                        .starts_with("line 3: the action names yyinput apart from a call"));
    EXPECT_TRUE(refusal_of(rules("a      { this->yyinput(); return 7; }"))
                        .starts_with("line 3: the action calls yyinput()"));
    EXPECT_TRUE(refusal_of(rules("a      { (*this).yyinput(); return 7; }"))
                        .starts_with("line 3: the action calls yyinput()"));

    // A raw string's prefix is a token of its own: the `R` that ends "ERROR" opens no raw string, so the splice after
    // the literal is read as gcc reads it and the call it joins is seen.
    EXPECT_TRUE(refusal_of(rules("a+     { const char* e = \"ERROR\"; (void)e; yyin\\ \nput(); return 7; }"))
                        .starts_with("line 3: "));

    EXPECT_TRUE(refusal_of(rules("a      { yyFlexLexer::yyinput(); return 7; }"))
                        .starts_with("line 3: the action calls yyinput()"));

    // A member call of the name on anything is the call, since an alias of `this` is a name the text does not resolve:
    // a C++ scanner's `auto* self = this; self->yyinput();` consumes the b of "aab" as `this->yyinput()` does, so `h .
    // input()` on a holder of the file's own is refused by name past the blanks and comments too, although flex 2.6.4
    // returns 7 for a and 8 for b under it; a member of another name is read, both rules returning over one byte, and a
    // and b certify, each a token of its own.
    EXPECT_TRUE(refusal_of("%option noyywrap nodefault\n%{\nstruct holder { int (*input)(void); };\n%}\n%%\n"
                           "a      { struct holder h = {0}; h . input(); return 7; }\nb      return 8;\n"
                           ".|\\n   return 9;\n")
                        .starts_with("line 6: the action calls input()"));

    constexpr std::string_view members{R"(%option noyywrap nodefault
%{
static int custom(void) { return 9; }
struct holder { int (*input)(void); int (*feed)(void); };
%}
%%
a      { struct holder h = {custom, custom}; h . feed(); return 7; }
b      { struct holder h = {custom, custom}, *p = &h; p /* the holder */ -> feed(); return 8; }
.|\n   return 9;
%%
)"};

    const auto file{read_flex(members).front()};

    ASSERT_EQ(file.rules.size(), 3U);
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"7"});
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"8"});

    const auto lexer{build(file, "INITIAL")};

    const std::string input{"ab"};

    const auto [tokens, consumed]{scan_of(lexer, input)};

    EXPECT_EQ(consumed, input.size());
    EXPECT_EQ(tokens, (Tokens_t{{0, 1}, {1, 1}}));
    EXPECT_TRUE(lexer.is_split_point('a'));
    EXPECT_TRUE(lexer.is_split_point('b'));
}

TEST(Read_flex_test, A_return_inside_a_lambda_or_a_local_class_is_no_return_of_the_action)
{
    // A return inside a lambda returns from the lambda, so the action returns nothing by it; a subscript's bracket
    // opens no lambda, and a return after a lambda is the action's.
    EXPECT_EQ(returned("{ auto f = []{ return 7; }; f(); }"), std::nullopt);
    EXPECT_EQ(returned("{ auto f = [&](int x) -> int { return x; }; return 8; }"), std::optional<std::string>{"8"});
    EXPECT_EQ(returned("{ h[i](x); return 7; }"), std::optional<std::string>{"7"});

    // An attribute's brackets open no capture list, since nothing a lambda takes follows their close: flex 2.6.4
    // returns 7 for a and 8 for b on the first action below. A return of a lambda's own call is the action's.
    EXPECT_EQ(returned("{ [[maybe_unused]] int q = 0; { return 7; } }"), std::optional<std::string>{"7"});
    EXPECT_NE(returned("{ return [](){ return 7; }(); }"), std::nullopt);

    // The body is the brace outside every group after the capture list, so a template list, a specifier and a braced
    // default argument are stepped over: flex 2.6.4 with gcc 13 as C++23 discards a and returns 8 for b on "ab" for
    // each of the three below.
    for (const std::string_view action :
         {"{ auto f = []<class T>(T x) { return x; }; (void)f(7); }",
          "{ auto f = [](int x = int{7}) { return x; }; (void)f(); }",
          "{ auto f = [] constexpr { return 7; }; (void)f(); }"})
    {
        EXPECT_EQ(returned(action), std::nullopt) << action;
    }

    // A template-id ends with `>`, so the name a braced initializer initializes stands before its argument list and not
    // beside the brace: flex 2.6.4 with gcc 13 as C++23 returns 7 over "aa" for the first action below. The list's own
    // default argument may compare, which is a comparison and not a second opener, so the second action's lambda is
    // passed over whole and the rule discards as the compiled scanner does.
    EXPECT_EQ(returned("{ if (std::array<bool, 1>{true}[0]) { return 7; } }"), std::optional<std::string>{"7"});
    EXPECT_EQ(returned("{ auto f = []<bool b = (1 < 2)>() { return b; }; (void)f(); }"), std::nullopt);
    EXPECT_EQ(
            returned("{ auto f = []<bool b = (1 < 2)>() { return b; }; (void)f(); return 8; }"),
            std::optional<std::string>{"8"});

    // An argument of the template-id may compare too, and the comparison stands in a group of its own, so the list's
    // own brackets are the ones outside every group; a type may be written as a specifier in parentheses, whose `)`
    // stands where a name would. flex 2.6.4 with gcc 13 as C++23 returns 7 over "aa" for each below.
    EXPECT_EQ(returned("{ if (std::array<bool, (2 > 1)>{true}[0]) { return 7; } }"), std::optional<std::string>{"7"});
    EXPECT_EQ(
            returned("{ std::array<bool, 1> flags; if (decltype(flags){true}[0]) { return 7; } }"),
            std::optional<std::string>{"7"});
    EXPECT_EQ(returned("{ if (decltype(std::array{true}){true}[0]) { return 7; } }"), std::optional<std::string>{"7"});

    // A requires clause stands between the parameter list and the body, and its constraint may be a requires
    // expression, whose braces are not the body's: the lambda below is never called and the action returns nothing.
    EXPECT_EQ(
            returned("{ auto f = []<typename T>() requires requires { typename T::value_type; } { return 7; }; "
                     "(void)f; }"),
            std::nullopt);
    EXPECT_EQ(returned("{ auto f = [](auto x) requires requires { 1; } { return 7; }; (void)f; }"), std::nullopt);
    EXPECT_EQ(
            returned("{ auto f = []<typename T>() requires requires { 1; } { return 7; }; (void)f; return 8; }"),
            std::optional<std::string>{"8"});

    // A class keyword opens a definition where a statement begins, after a `typedef` and after a label as well as after
    // a `;` or a brace, and the body is left out of the return search wherever the head's brace stands.
    EXPECT_EQ(
            returned("{ typedef struct Local { int f() { return 7; } } Local; Local x; (void)x.f(); }"), std::nullopt);
    EXPECT_EQ(returned("{ struct Local final { int f() { return 7; } }; Local x; (void)x.f(); }"), std::nullopt);
    EXPECT_EQ(returned("{ if constexpr (true) {} [] { return 7; }(); }"), std::nullopt);
    EXPECT_EQ(returned("{ if consteval {} [] { return 7; }(); }"), std::nullopt);
    EXPECT_EQ(returned("{ if !consteval {} [] { return; }(); return 7; }"), std::optional<std::string>{"7"});
    EXPECT_EQ(returned("{ if not consteval {} [] { return; }(); return 7; }"), std::optional<std::string>{"7"});

    // The first `requires` opens the clause and every one after it opens a requires expression of the constraint, so a
    // constraint joined by `&&` reaches the lambda's own body and not a requirement's braces.
    EXPECT_EQ(
            returned("{ auto f = []<class T>() requires true && requires { typename T::value_type; } { return 7; }; "
                     "(void)f; }"),
            std::nullopt);
    EXPECT_EQ(
            returned("{ auto f = []<class T>() requires requires { typename T::value_type; } && requires { typename "
                     "T::size_type; } { return 7; }; (void)f; }"),
            std::nullopt);
    EXPECT_EQ(returned("{ auto f = []<class T>() requires (sizeof(T) > 0) { return 7; }; (void)f; }"), std::nullopt);
}

TEST(Read_flex_test, A_bar_rule_above_an_EOF_rule_shares_its_action)
{
    // flex 2.6.4 gives `a |` the action of the `<<EOF>>` rule under it, as of any rule: on "ab" it returns 7 for a and
    // 8 for b. The `<<EOF>>` rule itself matches no byte and is no rule of the token set.
    constexpr std::string_view shared{R"(%option noyywrap nodefault
%%
a       |
<<EOF>> return 7;
b       return 8;
.|\n    return 9;
%%
)"};

    const auto file{read_flex(shared).front()};

    ASSERT_EQ(file.rules.size(), 3U);
    EXPECT_EQ(file.rules[0].pattern, "a");
    EXPECT_EQ(file.rules[0].token, std::optional<std::string>{"7"});
    EXPECT_EQ(file.rules[1].pattern, "b");

    const auto set{token_set(file, "INITIAL")};

    EXPECT_FALSE(set.rules[0].discarded);
}

TEST(Read_flex_test, A_percent_brace_action_is_its_block_alone_and_the_rest_of_the_closing_line_is_dropped)
{
    // flex 2.6.4 copies a `%{` action out from after its `%{` to its `%}` and drops the rest of the `%}` line, so `a %{
    // int y = 2;\n int x = 1;\n%} return 7;` emits the two declarations and no return: on "ab" it returns 8 alone, the
    // a discarded. A `yyless(0)` after the `%}` is dropped with the rest and refuses nothing.
    constexpr std::string_view dropped{R"(%option noyywrap nodefault
%%
a %{ int y = 2;
 int x = 1;
%} return 7;
b return 8;
.|\n return 9;
%%
)"};

    const auto file{read_flex(dropped).front()};

    ASSERT_EQ(file.rules.size(), 3U);
    EXPECT_EQ(file.rules[0].action, "int y = 2;\n int x = 1;\n");
    EXPECT_FALSE(file.rules[0].token.has_value());
    EXPECT_EQ(file.rules[1].token, std::optional<std::string>{"8"});
    EXPECT_EQ(file.rules[1].line, 6U);

    const auto set{token_set(file, "INITIAL")};

    EXPECT_TRUE(set.rules[0].discarded);

    constexpr std::string_view kept{R"(%option noyywrap nodefault
%%
a %{ return 7; %} yyless(0);
b return 8;
.|\n return 9;
%%
)"};

    const auto with_call{read_flex(kept).front()};

    ASSERT_EQ(with_call.rules.size(), 3U);
    EXPECT_EQ(with_call.rules[0].token, std::optional<std::string>{"7"});
}

TEST(Read_flex_test, An_action_is_read_with_its_lines_spliced_as_C_splices_them)
{
    // C deletes a backslash and the newline after it before it reads a token, so a word may be written over two lines.
    // flex 2.6.4 on "a \n b" with `[ \t\n]+ { ret\<newline>urn 2; }`, `[ab] return 1;` and `.|\n return 9;` returns 1,
    // 2 and 1, the whitespace a visible token of length 3. The same splicing continues a `//` comment onto the next
    // line, where it hides the `return` there, and joins a parted `yyless`, which the guard must then see.
    const auto rules{[](const std::string_view first) {
        return std::format("%option noyywrap nodefault\n%%\n{}\n[ab]   return 1;\n.|\\n   return 9;\n", first);
    }};

    const auto spliced{read_flex(rules("[ \\t\\n]+ { ret\\\nurn 2; }")).front()};

    ASSERT_EQ(spliced.rules.size(), 3U);
    EXPECT_EQ(spliced.rules[0].token, std::optional<std::string>{"2"});

    const auto spliced_set{token_set(spliced, "INITIAL")};

    const auto spliced_lexer{build(spliced, "INITIAL")};

    EXPECT_FALSE(spliced_set.rules[0].discarded);
    EXPECT_FALSE(spliced_lexer.is_split_point_ignoring('\n'));

    const auto commented{read_flex(rules("[ \\t\\n]+ { // ret\\\nreturn 2; }")).front()};

    ASSERT_EQ(commented.rules.size(), 3U);
    EXPECT_FALSE(commented.rules[0].token.has_value());

    const auto commented_set{token_set(commented, "INITIAL")};

    EXPECT_TRUE(commented_set.rules[0].discarded);

    EXPECT_TRUE(
            refusal_of(rules("ab     { yyl\\\ness(1); return 7; }")).starts_with("line 3: the action calls yyless()"));

    // The tokens keep their places in the action as written, the splice counted in a token's extent, and the expression
    // returned is spliced as they were.
    const auto tokens{c_tokens("re\\\nturn T_\\\r\nA;")};

    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].text, "return");
    EXPECT_EQ(tokens[0].at, 0U);
    EXPECT_EQ(tokens[0].end, 8U);
    EXPECT_EQ(tokens[1].text, "T_A");
    EXPECT_EQ(tokens[1].at, 9U);
    EXPECT_EQ(tokens[1].end, 15U);
    EXPECT_EQ(tokens[2].text, ";");
    EXPECT_EQ(returned("{ return T_\\\nA; }"), std::optional<std::string>{"T_A"});

    // A C++ raw string is one literal to its closing delimiter, a quote inside it closing nothing: flex 2.6.4 ends the
    // action's first line with that quote's literal open, so the action runs to the brace on the next line, and g++
    // compiles the raw string and the `return 7` after it. A universal character name is part of the identifier around
    // it, so `return\u03B1` is a name of the file's own, and flex on "ab" returns 8 alone.
    const auto raw{c_tokens("{ const char* s = R\"(x\")\"; return 7;\n}")};

    ASSERT_EQ(raw.size(), 12U);
    EXPECT_EQ(raw[6].text, "R\"(x\")\"");
    EXPECT_EQ(raw[8].text, "return");
    EXPECT_EQ(returned("{ const char* s = R\"(x\")\"; return 7;\n}"), std::optional<std::string>{"7"});
    EXPECT_EQ(returned(R"({ puts(u8R"d("; return 9; )d"); })"), std::nullopt);

    const auto universal{c_tokens(R"(return\u03B1 = 7; \u00E9t\U000000E9 = 8;)")};

    ASSERT_EQ(universal.size(), 8U);
    EXPECT_EQ(universal[0].text, R"(return\u03B1)");
    EXPECT_EQ(universal[4].text, R"(\u00E9t\U000000E9)");
    EXPECT_EQ(returned(R"({ return\u03B1 = 7; })"), std::nullopt);
    EXPECT_EQ(returned("{ int caf\xC3\xA9 = 1; return caf\xC3\xA9; }"), std::optional<std::string>{"caf\xC3\xA9"});
}

TEST(Read_flex_test, An_action_emits_a_token_only_where_every_path_through_it_returns_that_token)
{
    // flex 2.6.4 with gcc 13, each scanner before `b return 8;`: `a+ { if (yyleng >= 2) return 7; }` returns 8 alone on
    // "ab", the single a discarded as the action falls through, and 7 then 8 on "aab", so the rule emits on one path
    // and discards on another and is refused; a `static struct` declared in the action, `a { static struct Helper { int
    // f() const { return 7; } } h; }`, returns nothing from the action, 8 alone on "ab", so the rule is discarded and
    // the method's return is no return of the action's; and `a { (void)"R"; ret\` spliced to `urn 7; }` returns 7 on
    // "ab", the `R` inside an ordinary literal opening no raw string, so the splice joins the `return`. A path is
    // followed through a block, an `if` with an `else` and a label; a loop, a `switch` or an `if` alone may fall
    // through and is refused rather than followed.
    const auto rules{[](const std::string_view first) {
        return std::format("%option noyywrap\n%%\n{}\nb      return 8;\n", first);
    }};

    const auto token_of{[&rules](const std::string_view first) {
        const auto source{rules(first)};

        const auto [line, message]{refusal_at(source)};

        if (line)
        {
            return std::format("line {}: ", *line);
        }

        const auto file{read_flex(source).front()};

        return file.rules.front().token.value_or("discarded");
    }};

    EXPECT_EQ(token_of("a+     { if (yyleng >= 2) return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a      { while (yyleng) return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a      { switch (yyleng) { default: return 7; } }"), "line 3: ");
    EXPECT_EQ(token_of("a      { if (yyleng) { return 7; } }"), "line 3: ");

    EXPECT_EQ(token_of("a      return 7;"), "7");
    EXPECT_EQ(token_of("a      { return 7; }"), "7");
    EXPECT_EQ(token_of("a      { { return 7; } }"), "7");
    EXPECT_EQ(token_of("a      { if (yyleng) { return 7; } else { return 7; } }"), "7");
    EXPECT_EQ(token_of("a      { if (yyleng) return 7; else return 7; }"), "7");
    EXPECT_EQ(token_of("a      { if (yyleng) return 7; else if (yytext[0]) return 7; else return 7; }"), "7");
    EXPECT_EQ(token_of("a      { done: return 7; }"), "7");
    EXPECT_EQ(token_of("a      { done: return 7; return 7; }"), "7");

    // A jump anywhere but after the last return leaves the action on its path with the match discarded: flex 2.6.4 with
    // `a+ { if (yyleng == 1) break; return 7; }` before `b return 8;` returns 8 alone on "ab", and so with `continue`
    // in place of `break`; a `goto` leaves for somewhere out of sight.
    EXPECT_EQ(token_of("a+     { if (yyleng == 1) break; return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { if (yyleng == 1) continue; return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a      { if (yyleng) goto done; return 7; done: return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a      { if (yyleng == 1) break; }"), "discarded");

    // flex defines `YY_BREAK` as `break;`, so an action spelling it leaves as a `break` does: flex 2.6.4 with `a+ { if
    // (yyleng == 1) YY_BREAK; return 7; }` returns 8 alone on "ab".
    EXPECT_EQ(token_of("a+     { if (yyleng == 1) YY_BREAK; return 7; }"), "line 3: ");

    // The one jump allowed after a stored token is a `break` or a `continue`, never a `goto`, whose label is out of
    // sight: flex 2.6.4 returns 8 and 8 for "aab" under `a+ { (void)yyleng; goto emit_token; }` before `b { emit_token:
    // return 8; }`.
    EXPECT_EQ(token_of("a+     { (void)yyleng; goto emit_token; }\nc      { emit_token: return 8; }"), "line 3: ");

    // A member of the name parenthesised to be called is the call too: the C++ scanner's `(self->yyinput)()` consumes
    // the b of "aab" as `self->yyinput()` does.
    EXPECT_EQ(token_of("a+     { auto* self = this; (self->yyinput)(); return 7; }"), "line 3: ");

    // A change of buffer changes what is scanned next: flex 2.6.4 with `a+ { yy_scan_string(""); return 7; }` before `b
    // return 8;` returns 7 alone on "aab", the b never scanned, and so with `YY_FLUSH_BUFFER;`; the calls that switch,
    // push, pop, flush or restart a buffer are refused by name, as a hook holding one is.
    EXPECT_EQ(token_of(R"(a+     { yy_scan_string(""); return 7; })"), "line 3: ");
    EXPECT_EQ(token_of("a+     { YY_FLUSH_BUFFER; return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { yyrestart(stdin); return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { yy_switch_to_buffer(other); return 7; }"), "line 3: ");

    // Reinitializing the current buffer flushes it as yy_flush_buffer() does: flex 2.6.4 with `a+ {
    // yy_init_buffer(YY_CURRENT_BUFFER, yyin); return 7; }` before `b return 8;` returns 7 alone on "aab"; and deleting
    // the current buffer leaves the scan without it, so both are refused by name too.
    EXPECT_EQ(token_of("a+     { yy_init_buffer(YY_CURRENT_BUFFER, yyin); return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { yy_delete_buffer(YY_CURRENT_BUFFER); return 7; }"), "line 3: ");

    // A `goto` in an action returning nowhere leaves for a label out of sight, `a+ { goto emit_token; }` reaching the
    // `return 8` of `b { emit_token: return 8; }`: flex 2.6.4 returns 8 and 8 for "aab".
    EXPECT_EQ(token_of("a+     { goto emit_token; }\nc      { emit_token: return 8; }"), "line 3: ");

    // `yyterminate()` ends the scan with no token: flex 2.6.4 with `a+ { if (yyleng == 1) yyterminate(); return 7; }`
    // prints nothing for "ab", and so with the call in the hook. A call through a member of anything is the call, an
    // alias of `this` being a name the text does not resolve: a C++ scanner's `auto* self = this; self->yyinput();`
    // consumes the b of "aab" as `this->yyinput()` does.
    EXPECT_EQ(token_of("a+     { if (yyleng == 1) yyterminate(); return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { auto* self = this; self->yyinput(); return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { s.input(); return 7; }"), "line 3: ");
    EXPECT_EQ(token_of("a+     { yylval.input = 1; myinput(); return 7; }"), "7");

    EXPECT_EQ(token_of("a      { static struct Helper { int f() const { return 7; } } h; }"), "discarded");
    EXPECT_EQ(token_of("a      { const struct Helper { int f() const { return 7; } } h{}; }"), "discarded");
    EXPECT_EQ(token_of("a      { static struct Helper { int f() const { return 9; } } h; return 7; }"), "7");

    EXPECT_EQ(token_of("a      { (void)\"R\"; ret\\\nurn 7; }"), "7");
    EXPECT_EQ(token_of("a      { (void)'R'; ret\\\nurn 7; }"), "7");
    EXPECT_EQ(token_of("a      { /* R\"( */ ret\\\nurn 7; }"), "7");
    EXPECT_EQ(token_of("a      { (void)R\"(x\\\n)\"; ret\\\nurn 7; }"), "7");

    // The lines are joined before any escape is read, so a backslash before a backslash ending the line escapes the
    // next line's first byte and the literal goes on: gcc 13 takes `"x\\` and `y"` on the next line as "x\y", and the
    // action returns 7.
    EXPECT_EQ(token_of("a      { (void)\"x\\\\\ny\"; ret\\\nurn 7; }"), "7");

    // The token is named by the expression returned, as written: a rule is one token to the certificates whatever its
    // action computes, and `a+ { return yyleng == 1 ? 7 : 8; }` returns 7 for "a" and 8 for "aa" under flex.
    EXPECT_EQ(token_of("a+     { return yyleng == 1 ? 7 : 8; }"), "yyleng == 1 ? 7 : 8");
}

TEST(Read_flex_test, A_YY_USER_ACTION_is_read_as_code_flex_runs_before_every_action)
{
    // The hook's replacement runs past a raw string's newline as the macro collector reads it: flex 2.6.4 under
    // `#define YY_USER_ACTION (void)R"(text` then `)"; yyinput();` takes "abab" through `a return 7; b return 8;` as 7
    // and 7, each b consumed by the hook, so the hook is refused at its line.
    const auto hook{[](const std::string_view definition) {
        const auto source{std::format(
                "%{{\n#define YY_USER_ACTION {}\n%}}\n%option noyywrap\n%%\na+ return 7;\nb return 8;\n", definition)};

        return refusal_of(source);
    }};

    const auto raw{hook("(void)R\"(text\n)\"; yyinput();")};

    EXPECT_TRUE(raw.starts_with("line 2: "));
    EXPECT_TRUE(raw.contains("yyinput()"));

    // The same joining inside the hook: `"x\\` and `y"; yyinput();` on the next line is one literal and the call, which
    // flex 2.6.4 runs before every action, taking "aab" as the one token 7 with the b consumed.
    EXPECT_TRUE(hook("(void)\"x\\\\\ny\"; yyinput();").contains("yyinput()"));

    // The hook runs before the rule's own action, so a return in it returns first, a `break` or a `continue` ends the
    // rule's case without the action, and a `goto` leaves for somewhere out of sight: flex 2.6.4 under `#define
    // YY_USER_ACTION return 9;` returns 9 and 9 for "aab", and under `if (yyleng == 1) return 9;` 7 and 9.
    EXPECT_TRUE(hook("return 9;").contains("holds `return`"));
    EXPECT_TRUE(hook("if (yyleng == 1) return 9;").contains("holds `return`"));
    EXPECT_TRUE(hook("if (yyleng == 1) break;").contains("holds `break`"));
    EXPECT_TRUE(hook("if (yyleng == 1) yyterminate();").contains("yyterminate()"));
    EXPECT_TRUE(hook("counter += 1'000; yyinput();").contains("yyinput()"));
    EXPECT_TRUE(hook("if (yyleng == 1) YY_BREAK;").contains("holds `YY_BREAK`"));
    EXPECT_TRUE(hook("yy_flush_buffer(YY_CURRENT_BUFFER);").contains("yy_flush_buffer()"));
    EXPECT_EQ(hook("(void)yyleng;"), "");

    // YY_USER_ACTION runs before every action, so a call in it is a call in every action: flex 2.6.4 with `#define
    // YY_USER_ACTION input();` in a `%{` block takes "aab" through `a+ { return 7; }` and `b return 8;` as the one
    // token 7, the b consumed by the hook. A comment naming the macro defines nothing.
    EXPECT_TRUE(refusal_of("%{\n#define YY_USER_ACTION input();\n%}\n%option noyywrap\n%%\na+     return 7;\n"
                           "b      return 8;\n")
                        .starts_with("line 2: the YY_USER_ACTION the definitions define, run before every action"));
    EXPECT_EQ(refusal_of("%{\n/* YY_USER_ACTION input() */\n%}\n%option noyywrap\n%%\na+     return 7;\n"), "");

    // The directive is read as C reads it, so a comment between its words and a splice inside one hide nothing.
    EXPECT_TRUE(refusal_of("%{\n# /* c */ define YY_USER_ACTION input();\n%}\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 2: "));
    EXPECT_TRUE(refusal_of("%{\n#def\\\nine YY_USER_ACTION input();\n%}\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 2: "));

    // A comment holding a newline is one blank to the preprocessor and does not end the directive, so the replacement
    // runs past it: gcc 13 compiles the definition below and flex takes "aab" through `a+` and `b` as the one token 7.
    EXPECT_TRUE(refusal_of("%{\n#define YY_USER_ACTION /* a\n  comment */ input();\n%}\n%option noyywrap\n%%\n"
                           "a+     return 7;\n")
                        .starts_with("line 2: "));
    EXPECT_EQ(refusal_of("%{\n#define YY_USER_ACTION /* plain */ ;\n%}\n%option noyywrap\n%%\na+     return 7;\n"), "");
    EXPECT_TRUE(refusal_of("  #define YY_USER_ACTION unput('x');\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 1: "));

    // A directive at the margin may be spliced over several lines as one inside a block may: flex copies the lines
    // through as they stand and the compiler joins them before any of it means anything, so the hook is the same hook
    // whichever way it is written.
    EXPECT_TRUE(refusal_of("    #define YY_USER_ACTION \\\n        input();\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 1: "));
    EXPECT_EQ(refusal_of("    #define YY_USER_ACTION \\\n        ;\n%option noyywrap\n%%\na+     return 7;\n"), "");

    // A line ends in a newline of its own or in a carriage return and a newline, and the compiler joins a spliced line
    // either way: flex 2.6.4 with gcc 13 takes "aab" through the scanners below as the one token 7 whichever ending the
    // file carries.
    EXPECT_TRUE(refusal_of("%{\r\n#define YY_USER_ACTION \\\r\n input();\r\n%}\r\n%option noyywrap\r\n%%\r\n"
                           "a+     return 7;\r\n")
                        .starts_with("line 2: "));
    EXPECT_TRUE(refusal_of("%option noyywrap\r\n    #define YY_USER_ACTION \\\r\n        input();\r\n%%\r\n"
                           "a+     return 7;\r\n")
                        .starts_with("line 2: "));

    // The indented code before the first rule is the rules section's prologue, which flex copies into the scanner ahead
    // of every action, so a hook defined there runs before every action as one defined above `%%` does: the compiled
    // scanner takes "aab" as the one token 7, the b consumed by the hook.
    EXPECT_TRUE(refusal_of("%option noyywrap\n%%\n    #define YY_USER_ACTION input();\na+     return 7;\n"
                           "b      return 8;\n")
                        .starts_with("line 3: "));
    EXPECT_TRUE(refusal_of("%option noyywrap\n%%\n    #define YY_USER_ACTION \\\n        input();\na+     return 7;\n")
                        .starts_with("line 3: "));
    EXPECT_EQ(refusal_of("%option noyywrap\n%%\n    int seen = 0;\na+     return 7;\n"), "");

    // gcc joins the lines where blanks stand between the backslash and the line's end, warning as it does it, and the
    // scanner is compiled by the compiler: the first below takes "aab" as the one token 7. A code block in the rules
    // section is copied into the scanner as the definitions' blocks are. Code after the second `%%` is copied after the
    // scanner, too late to define the hook the actions expanded, and the same definition there leaves both tokens,
    // which is why that one is read and not refused.
    EXPECT_TRUE(refusal_of("%{\n#define YY_USER_ACTION \\ \n input();\n%}\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 2: "));
    EXPECT_TRUE(refusal_of("%option noyywrap\n%%\n%{\n#define YY_USER_ACTION input();\n%}\na+     return 7;\n")
                        .starts_with("line 4: "));
    EXPECT_TRUE(refusal_of("%top{\n#define YY_USER_ACTION input();\n}\n%option noyywrap\n%%\na+     return 7;\n")
                        .starts_with("line 2: "));
    EXPECT_EQ(
            refusal_of("%option noyywrap\n%%\na+     return 7;\nb      return 8;\n%%\n"
                       "#define YY_USER_ACTION input();\n"),
            "");
}

TEST(Read_flex_test, A_file_the_copied_code_includes_defines_what_the_file_s_own_code_would)
{
    // A file the copied code includes by a quoted name defines what the file's own code would: flex 2.6.4 under a
    // `#include "hook.h"` holding `#define YY_USER_ACTION yyinput();` takes "aab" as the one token 7. The reader given
    // reaches the file, and the hook it defines is refused with the file's name; a reader that finds no such file, or
    // no reader at all, has the include refused by name, where an include in angle brackets names a system header and
    // is read past.
    const auto including{[](const std::string_view definitions, const Include_reader_t& reader) {
        const auto source{std::format("%{{\n{}\n%}}\n%option noyywrap\n%%\na+ return 7;\nb return 8;\n", definitions)};

        const auto [line, message]{refusal_at(source, false, reader)};

        return message;
    }};

    // Reaches `hook.h` alone, a header defining a hook that moves the match.
    const Include_reader_t hooks{
            [](const std::string_view name, std::string_view, Include_form) -> std::optional<Included> {
                if (name == "hook.h")
                {
                    return Included{.text = "#define YY_USER_ACTION yyinput();\n", .path = std::string{name}};
                }

                return std::nullopt;
            }};

    EXPECT_TRUE(including(R"(#include "hook.h")", hooks)
                        .contains(R"(line 2: the YY_USER_ACTION the included file "hook.h" defines, run before)"));

    const auto missing{including(R"(#include "other.h")", hooks)};

    EXPECT_TRUE(missing.contains(R"(line 2: the code includes "other.h", a file)"));
    EXPECT_TRUE(missing.contains("not found beside the file including it"));
    EXPECT_TRUE(including(R"(#include "hook.h")", {}).contains("the reading does not reach"));
    EXPECT_EQ(including("#include <stdio.h>", {}), "");

    // A file named by a macro, `#include HOOK_FILE`, is out of sight, since the reading expands no macro; an
    // angle-bracket include the reader finds is read as a quoted one is, since a project's own header may be reached
    // through the compiler's include path, and one it does not find is a system header's: gcc 13 reads `<hook.h>`
    // beside the file under `-I.` and flex takes "aab" as the one token 7 under both spellings.
    EXPECT_TRUE(including("#define HOOK_FILE \"hook.h\"\n#include HOOK_FILE", hooks)
                        .contains("named by the macro HOOK_FILE"));
    EXPECT_TRUE(including("#include <hook.h>", hooks).contains(R"(the included file "hook.h" defines)"));
    EXPECT_EQ(including("#include <other.h>", hooks), "");

    // The reader is told how the directive names the file, so that the command line looks for a quoted name beside the
    // including file and for an angle-bracket name on its `--include` directories alone, as gcc resolves them:
    // `<hook.h>` under `-Iinc` reaches inc/hook.h from a file whose own directory holds another.
    std::vector<std::string> forms{};

    const Include_reader_t noting{[&forms](const std::string_view name, std::string_view, const Include_form form) {
        const std::string_view spelled{form == Include_form::angled ? " angled" : " quoted"};

        forms.push_back(std::format("{}{}", name, spelled));

        return std::optional<Included>{{.text = "", .path = std::string{name}}};
    }};

    EXPECT_EQ(including("#include <one.h>\n#include \"two.h\"", noting), "");
    EXPECT_EQ(forms, (std::vector<std::string>{"one.h angled", "two.h quoted"}));

    // An angle-bracket include ending the header without a newline names its file whole. `outer.h` includes `<hook.h>`
    // with no newline after it, and `hook.h` defines the hook.
    const Include_reader_t bare{
            [](const std::string_view name, std::string_view, Include_form) -> std::optional<Included> {
                if (name == "outer.h")
                {
                    return Included{.text = "#include <hook.h>", .path = "outer.h"};
                }

                if (name == "hook.h")
                {
                    return Included{.text = "#define YY_USER_ACTION yyinput();\n", .path = "hook.h"};
                }

                return std::nullopt;
            }};

    EXPECT_TRUE(including(R"(#include "outer.h")", bare).contains(R"(the included file "hook.h" defines)"));

    // A nested include is resolved beside the file including it, as a compiler resolves a quoted name: `sub/hooks.h`
    // including `"inner.h"` reaches `sub/inner.h`, so the reader is handed the including file's path back and the hook
    // is found where gcc finds it.
    std::vector<std::string> asked{};

    // Reaches every file beside the file including it.
    const Include_reader_t nested{
            [&asked](
                    const std::string_view name, const std::string_view from, Include_form) -> std::optional<Included> {
                asked.push_back(std::format("{} > {}", from, name));

                const auto directory{from.substr(0, from.rfind('/') + 1)};

                const auto path{std::format("{}{}", directory, name)};

                if (path == "sub/hooks.h")
                {
                    return Included{.text = "#include \"inner.h\"\n", .path = path};
                }

                if (path == "sub/inner.h")
                {
                    return Included{.text = "#define YY_USER_ACTION yyinput();\n", .path = path};
                }

                return Included{.text = "", .path = path};
            }};

    EXPECT_TRUE(including(R"(#include "sub/hooks.h")", nested).contains(R"(the included file "inner.h" defines)"));
    EXPECT_EQ(asked, (std::vector<std::string>{" > sub/hooks.h", "sub/hooks.h > inner.h"}));
}

TEST(Read_flex_test, Many_stretches_of_copied_code_are_no_include)
{
    // A definitions section of 65 declarations, each in a `%{ %}` block of its own, is read.
    std::string many{};

    constexpr auto declarations{65};

    for (auto declared{0}; declared < declarations; ++declared)
    {
        many += std::format("%{{\nstatic int g{};\n%}}\n", declared);
    }

    EXPECT_EQ(line_of(many + "%option noyywrap\n%%\na+ return 7;\n"), std::nullopt);
}

TEST(Read_flex_test, Whether_an_action_returns_is_read_past_its_comments_and_literals)
{
    // flex 2.6.4 on "a\nb" returns 1, 2 and 1 for the first scanner, the whitespace rule returning WHITESPACE whatever
    // its comment says, and 1 and 1 for the second, the literal returning nothing; so the first scanner has no
    // discarded token and the newline certifies neither exactly nor modulo discarded tokens, while the second discards
    // its whitespace and the newline certifies modulo the discarded tokens.
    constexpr std::string_view commented{R"(%option noyywrap nodefault
%%
[ \t\n]+  { /* return; */ return WHITESPACE; }
[ab]      return 1;
%%
)"};

    const auto with_comment{read_flex(commented).front()};

    ASSERT_EQ(with_comment.rules.size(), 2U);
    EXPECT_EQ(with_comment.rules[0].token, std::optional<std::string>{"WHITESPACE"});

    const auto commented_set{token_set(with_comment, "INITIAL")};

    EXPECT_FALSE(commented_set.rules[0].discarded);

    const auto commented_lexer{build(with_comment, "INITIAL")};

    EXPECT_FALSE(commented_lexer.is_split_point('\n'));
    EXPECT_FALSE(commented_lexer.is_split_point_ignoring('\n'));

    constexpr std::string_view quoted{R"(%option noyywrap nodefault
%%
[ \t\n]+  { puts("return TOKEN;"); }
[ab]      return 1;
%%
)"};

    const auto with_literal{read_flex(quoted).front()};

    ASSERT_EQ(with_literal.rules.size(), 2U);
    EXPECT_FALSE(with_literal.rules[0].token.has_value());

    const auto quoted_set{token_set(with_literal, "INITIAL")};

    EXPECT_TRUE(quoted_set.rules[0].discarded);

    const auto quoted_lexer{build(with_literal, "INITIAL")};

    EXPECT_FALSE(quoted_lexer.is_split_point('\n'));
    EXPECT_TRUE(quoted_lexer.is_split_point_ignoring('\n'));

    // The expression returned is read from its first token through its last, a comment before it no part of it.
    EXPECT_EQ(
            returned("{ /* the token */ return /* which */ T_EXIT /* it is */ ; }"),
            std::optional<std::string>{"T_EXIT"});
    EXPECT_EQ(returned(R"({ return "return X;"; })"), std::optional<std::string>{R"("return X;")"});
    EXPECT_EQ(returned("{ return; // return X;\n }"), std::nullopt);
    EXPECT_EQ(
            returned("{ RETURN_TOKEN /* wrapped */ (T_EXIT, 2); }", {"RETURN_TOKEN"}),
            std::optional<std::string>{"T_EXIT"});
    EXPECT_EQ(returned("{ token /* stored */ = BUILD; }", {"token"}), std::optional<std::string>{"BUILD"});
    EXPECT_EQ(returned(R"({ puts("token = X;"); })", {"token"}), std::nullopt);
}

TEST(Read_flex_test, Lex_compat_is_refused_by_name_rather_than_recorded_and_ignored)
{
    // Under `%option lex-compat` flex 2.6.4 matches "ababab" against `ab{3}` and refuses "abbb", the repetition binding
    // the whole expression before it; the pattern parser reads the other precedence, so the option is refused where it
    // stands.
    const auto [lex_line, lex_message]{refusal_at("%option noyywrap\n%option lex-compat\n%%\nab{3}   return A;\n")};

    EXPECT_EQ(lex_line, 2U);
    EXPECT_TRUE(lex_message.contains("lex-compat"));
    EXPECT_TRUE(lex_message.contains("ab{3}"));

    // posix-compat changes the same precedence and is refused the same way.
    const auto [posix_line, posix_message]{refusal_at("%option posix-compat\n%%\nab{3}   return A;\n")};

    EXPECT_EQ(posix_line, 1U);
    EXPECT_TRUE(posix_message.contains("posix-compat"));

    EXPECT_EQ(line_of("%option posix-compat noposix-compat\n%%\nab{3}   return A;\n"), std::nullopt);

    // The two are flags of their own and flex takes that binding while either stands: run on `ab{3}`, flex 2.6.4
    // matches "ababab" under `lex-compat posix-compat nolex-compat` and under `posix-compat lex-compat noposix-compat`,
    // and only "abbb" once both are off.
    EXPECT_NE(line_of("%option lex-compat posix-compat nolex-compat\n%%\nab{3}   return A;\n"), std::nullopt);
    EXPECT_NE(line_of("%option posix-compat lex-compat noposix-compat\n%%\nab{3}   return A;\n"), std::nullopt);
    EXPECT_EQ(
            line_of("%option lex-compat posix-compat nolex-compat noposix-compat\n%%\nab{3}   return A;\n"),
            std::nullopt);

    // yylineno changes no language: flex matches "abbb" against `ab{3}` under it, as the reader does.
    EXPECT_EQ(line_of("%option yylineno\n%%\nab{3}   return A;\n"), std::nullopt);

    // Turned back off it says nothing, and flex then matches "abbb" as the reader does, leaving "ababab" to the default
    // rule one byte at a time.
    const auto file{read_flex("%option lex-compat nolex-compat\n%%\nab{3}   return A;\n").front()};

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(
            lexer.tokenize<std::size_t>(std::string_view{"abbb"}),
            (Match_t{.token = std::optional<std::size_t>{0}, .length = 4U}));
    EXPECT_EQ(
            lexer.tokenize<std::size_t>(std::string_view{"ababab"}),
            (Match_t{.token = std::optional<std::size_t>{1}, .length = 1U}));
}

TEST(Read_flex_test, A_definition_runs_to_the_end_of_its_line_and_carries_a_comment_standing_there)
{
    // flex takes a definition to the end of its line, so a comment there is part of the pattern: flex 2.6.4 builds this
    // file while the definition is unused, and refuses the file that expands it with "unrecognized rule".
    constexpr std::string_view unused{R"(D  [0-9]  /* digits */
%%
[0-9]+   return NUMBER;
%%
)"};

    const auto file{read_flex(unused).front()};

    EXPECT_EQ(file.definitions.at("D"), "[0-9]  /* digits */");

    std::ignore = build(file, "INITIAL");

    const auto expanded{read_flex("D  [0-9]  /* digits */\n%%\n{D}   return NUMBER;\n").front()};

    const auto [line, message]{build_refusal(expanded)};

    EXPECT_EQ(line, 3U);
    EXPECT_TRUE(message.contains("in definition 'D'"));
}

TEST(Read_flex_test, A_bracket_expression_ends_the_pattern_only_at_its_own_close)
{
    // flex 2.6.4, run on each of these as a rule of its own, matches the lengths asserted: a POSIX class's `]` is no
    // close, so the blank of `[[:alpha:] ]+` is a member; a `]` first in the bracket is a member and the caret negating
    // one is not a member, so `[^^]` admits every byte but the caret; and a `[:` in any other shape is the `[` and the
    // `:` as members, which leaves `[[:al]pha:]` the bracket `[[:al]` and the text `pha:]`.
    const auto matched{[](const std::string_view pattern, const std::string_view input) -> std::optional<std::size_t> {
        const auto source{std::format("%%\n{}   return A;\n", pattern)};

        const auto file{read_flex(source).front()};

        // The file's one rule and the default rule after it, which takes what the rule does not match.
        if (file.rules.size() != 2 || file.rules.front().pattern != pattern)
        {
            ADD_FAILURE() << "the file of " << pattern << " holds other rules";

            return std::nullopt;
        }

        const auto lexer{build(file, "INITIAL")};

        const auto [token, length]{lexer.tokenize<std::size_t>(input)};

        if (token != 0)
        {
            return std::nullopt;
        }

        return length;
    }};

    EXPECT_EQ(matched("[[:alpha:] ]+", "abc def"), 7);
    EXPECT_EQ(matched(R"([[:alpha:]\t ]+)", "abc def"), 7);
    EXPECT_EQ(matched("[[:alpha:][:digit:] ]+", "a1 b"), 4);
    EXPECT_EQ(matched("[^^]+", "abc"), 3);
    EXPECT_EQ(matched("[^^]+", "^"), std::nullopt);
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
    EXPECT_NE(line_of("%%\n[^]+   return A;\n"), std::nullopt);
}

TEST(Read_flex_test, Refusals_name_the_line)
{
    EXPECT_EQ(line_of("DIGIT [0-9]\n"), 2);
    EXPECT_EQ(line_of("DIGIT\n%%\n"), 1);
    EXPECT_EQ(line_of("%{\ncode\n%%\n"), 1);
    EXPECT_EQ(line_of("%%\n[abc   return X;\n"), 2);
    EXPECT_EQ(line_of("%%\nabc   { return X;\n"), 2);
    EXPECT_EQ(line_of("%%\n<S abc  ;\n"), 2);

    // A start-condition scope left open is a parse error in flex, and is refused at the line it opened on.
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n"), 3);
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n%%\n"), 3);
    EXPECT_EQ(line_of("%x X\n%%\n<X>{\na   return A;\n}\n"), std::nullopt);

    // A pattern the regex parser refuses is refused when the token set is built, with the rule's line.
    const auto file{read_flex("%%\n^abc   return X;\n").front()};

    const auto [line, message]{build_refusal(file)};

    EXPECT_EQ(line, 2U);
    EXPECT_TRUE(message.contains("anchor"));
}

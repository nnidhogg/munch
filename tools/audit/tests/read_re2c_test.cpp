#include "munch/tools/audit/read_re2c.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace munch::tools::audit;

namespace
{
std::string grammar(const std::string_view name)
{
    std::ifstream stream{std::string{SOURCE_DIR} + "/tools/audit/grammars/" + std::string{name}};

    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/**
 * @brief The re2c idioms in one block: configurations, definitions referring to each other by bare name, a
 *        case-insensitive literal, conditions with a transition, a `:=` action, comments, and the special rules. A
 *        block is a C comment, so a comment closer inside it is spelled `"*" "/"`, as re2c users spell it.
 */
constexpr std::string_view idioms{R"(int lex() {
    /*!re2c
        re2c:define:YYCTYPE = char;  // a configuration
        digit  = [0-9];
        number = digit+ ("." digit+)?;   // a definition using another, as a block may only comment this way

        <INITIAL> 'true' | 'false'   { return BOOLEAN; }
        <INITIAL> number             := return NUMBER;
        <INITIAL> "/*"               :=> COMMENT
        <COMMENT> "*" "/"            => INITIAL { continue; }
        <COMMENT> [^]                { continue; }
        <*> [ \t\n]+                 { continue; }
        <!INITIAL>                   { setup(); }
        <INITIAL> *                  { return ERROR; }
        <INITIAL> $                  { return END; }
    */
    /*!max:re2c*/
})"};

} // namespace

TEST(Read_re2c, Reads_configurations_definitions_and_rules_with_their_dialect_rewritten)
{
    const auto scanners{read_re2c(idioms)};

    // One block with rules, so one scanner, named by the line its block opens on.
    ASSERT_EQ(scanners.size(), 1u);

    const auto& spec{scanners.front()};

    EXPECT_EQ(spec.line, 2u);

    ASSERT_EQ(spec.options.size(), 1u);
    EXPECT_EQ(spec.options.front(), "define:YYCTYPE=char");

    ASSERT_EQ(spec.definitions.size(), 2u);
    EXPECT_EQ(spec.definitions.at("digit"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("number"), R"({digit}+("."{digit}+)?)");

    // Six rules: the setup rule, the default rule and the end rule are not tokens.
    ASSERT_EQ(spec.rules.size(), 6u);

    EXPECT_EQ(spec.rules[0].pattern, "'true' | 'false'");
    EXPECT_EQ(spec.rules[0].expression, "[tT][rR][uU][eE]|[fF][aA][lL][sS][eE]");
    EXPECT_EQ(spec.rules[0].conditions, (std::vector<std::string>{"INITIAL"}));
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"BOOLEAN"});
    EXPECT_EQ(spec.rules[0].line, 7u);

    EXPECT_EQ(spec.rules[1].pattern, "number");
    EXPECT_EQ(spec.rules[1].expression, "{number}");
    EXPECT_EQ(spec.rules[1].token, std::optional<std::string>{"NUMBER"});

    // Transitions are kept as text and return nothing.
    EXPECT_EQ(spec.rules[2].pattern, R"("/*")");
    EXPECT_FALSE(spec.rules[2].token.has_value());
    EXPECT_EQ(spec.rules[3].conditions, (std::vector<std::string>{"COMMENT"}));
    EXPECT_EQ(spec.rules[3].pattern, R"("*" "/")");
    EXPECT_EQ(spec.rules[3].expression, R"("*""/")");
    EXPECT_FALSE(spec.rules[3].token.has_value());
    EXPECT_EQ(spec.rules[4].pattern, "[^]");
    EXPECT_EQ(spec.rules[4].expression, R"([\x00-\xff])");
    EXPECT_EQ(spec.rules[5].conditions, (std::vector<std::string>{"*"}));

    // The conditions are the ones the rules name, INITIAL and `*` aside, each exclusive.
    ASSERT_EQ(spec.conditions.size(), 1u);
    EXPECT_EQ(spec.conditions.front().name, "COMMENT");
    EXPECT_TRUE(spec.conditions.front().exclusive);
    EXPECT_EQ(active_rules(spec, "COMMENT"), (std::vector<std::size_t>{3, 4, 5}));
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 5}));
}

TEST(Read_re2c, The_flags_choose_the_case_insensitive_quote_and_the_flex_syntax)
{
    // PHP's scanner shape: flex-style definitions with a bare underscore, `{name}` references, a setup rule with
    // a `:=` action, and an action whose character literal holds a brace.
    constexpr std::string_view source{R"(/*!re2c
LNUM [0-9]+(_[0-9]+)*

<!*> := yyleng = YYCURSOR - SCNG(yy_text);

<INITIAL>{LNUM} { return NUMBER; }
<INITIAL>"exit" {
    enter_nesting('{');
    return EXIT;
}
<INITIAL>'a\x41\n' { return A; }
*/
)"};

    const auto plain{read_re2c(source).front()};

    ASSERT_EQ(plain.rules.size(), 3u);
    EXPECT_EQ(plain.definitions.at("LNUM"), "[0-9]+(_[0-9]+)*");
    EXPECT_EQ(plain.rules[0].expression, "{LNUM}");
    EXPECT_EQ(plain.rules[1].expression, R"("exit")");
    EXPECT_EQ(plain.rules[1].token, std::optional<std::string>{"EXIT"});
    EXPECT_EQ(plain.rules[2].expression, R"([aA][aA][\n])");

    const auto inverted{read_re2c(source, {.case_inverted = true}).front()};

    EXPECT_EQ(inverted.rules[1].expression, "[eE][xX][iI][tT]");
    EXPECT_EQ(inverted.rules[2].expression, R"([a][\x41][\n])");

    const auto insensitive{read_re2c(source, {.case_insensitive = true}).front()};

    EXPECT_EQ(insensitive.rules[1].expression, "[eE][xX][iI][tT]");
    EXPECT_EQ(insensitive.rules[2].expression, R"([aA][aA][\n])");

    // Set in the file, a flag holds from there on, the next block included, and a block without rules is no
    // scanner but hands its configurations on.
    const auto in_file{read_re2c("/*!re2c\nre2c:flags:case-inverted = 1;\n*/\n/*!re2c\n\"ab\" { return X; }\n*/\n")};

    ASSERT_EQ(in_file.size(), 1u);
    ASSERT_EQ(in_file.front().rules.size(), 1u);
    EXPECT_EQ(in_file.front().line, 4u);
    EXPECT_EQ(in_file.front().rules[0].expression, "[aA][bB]");
    EXPECT_EQ(in_file.front().options, (std::vector<std::string>{"flags:case-inverted=1"}));
}

TEST(Read_re2c, A_defined_name_opening_a_line_is_a_rule_and_an_undefined_one_a_flex_definition)
{
    // Normal syntax: a rule may open with a defined name and a blank, and stays a rule.
    const auto normal{
            read_re2c("/*!re2c\ndigit = [0-9];\ndigit+ { return N; }\ndigit \"x\" { return X; }\n*/\n").front()};

    ASSERT_EQ(normal.rules.size(), 2u);
    EXPECT_EQ(normal.rules[0].expression, "{digit}+");
    EXPECT_EQ(normal.rules[1].expression, R"({digit}"x")");
    EXPECT_EQ(normal.definitions.size(), 1u);

    // Flex syntax inferred: an undefined name opening a line with a blank and no action is a definition, a comment
    // closing on the line included in it, and a later rule opening with a bare literal is still a rule.
    const auto flex{read_re2c("/*!re2c\nD [0-9] /* digits */ +\nab { return AB; }\n{D} { return N; }\n*/\n").front()};

    ASSERT_EQ(flex.rules.size(), 2u);
    EXPECT_EQ(flex.definitions.at("D"), "[0-9]+");
    EXPECT_EQ(flex.rules[0].expression, "ab");
    EXPECT_EQ(flex.rules[1].expression, "{D}");
}

TEST(Read_re2c, The_forms_the_caller_names_return_tokens_as_return_does)
{
    // PHP wraps its returns in macros and ninja stores the token and breaks out; neither is a `return`, so both read
    // as discarding until the caller names the form.
    constexpr std::string_view source{R"(/*!re2c
        "exit"   { RETURN_TOKEN_WITH_IDENT(T_EXIT); }
        "{"      { enter_nesting('{'); RETURN_TOKEN('{'); }
        "?>"     { RETURN_END_TOKEN; }
        [ \t]+   { RETURN_OR_SKIP_TOKEN(T_WHITESPACE); }
        "build"  { token = BUILD; break; }
        [^]      { continue; }
    */
)"};

    const auto unnamed{read_re2c(source).front()};

    ASSERT_EQ(unnamed.rules.size(), 6u);

    for (const auto& rule : unnamed.rules)
    {
        EXPECT_FALSE(rule.token.has_value()) << rule.pattern;
    }

    const auto named{
            read_re2c(source, {}, {"RETURN_TOKEN", "RETURN_TOKEN_WITH_IDENT", "RETURN_END_TOKEN", "token"}).front()};

    EXPECT_EQ(named.rules[0].token, std::optional<std::string>{"T_EXIT"});
    EXPECT_EQ(named.rules[1].token, std::optional<std::string>{"'{'"});
    EXPECT_EQ(named.rules[2].token, std::optional<std::string>{"RETURN_END_TOKEN"});
    EXPECT_FALSE(named.rules[3].token.has_value()); // the form that may skip was not named, the caller's call
    EXPECT_EQ(named.rules[4].token, std::optional<std::string>{"BUILD"});
    EXPECT_FALSE(named.rules[5].token.has_value());
}

TEST(Read_re2c, Each_block_with_rules_is_a_scanner_of_its_own)
{
    // ninja's shape: one function per block, the definitions declared once and used by the later blocks.
    constexpr std::string_view source{R"(/*!re2c
    re2c:define:YYCTYPE = char;
    name = [a-z]+;
*/
Token ReadToken() {
    /*!re2c
        name        { return IDENT; }
        [ ]+        { continue; }
    */
}
bool ReadIdent() {
    /*!local:re2c
        digits = [0-9]+;
        name        { return true; }
        *           { return false; }
    */
}
bool ReadNumber() {
    /*!re2c
        digits      { return true; }
        *           { return false; }
    */
}
)"};

    // The local block reads the definitions so far and passes none of its own on, so the last block's `digits` is a
    // bare name, no definition.
    const auto scanners{read_re2c(source)};

    ASSERT_EQ(scanners.size(), 3u);

    EXPECT_EQ(scanners[2].line, 19u);
    EXPECT_FALSE(scanners[2].definitions.contains("digits"));

    EXPECT_EQ(scanners[0].line, 6u);
    EXPECT_EQ(scanners[0].rules.size(), 2u);
    EXPECT_EQ(scanners[0].definitions.at("name"), "[a-z]+");
    EXPECT_EQ(scanners[0].options, (std::vector<std::string>{"define:YYCTYPE=char"}));

    EXPECT_EQ(scanners[1].line, 12u);
    EXPECT_EQ(scanners[1].rules.size(), 1u);
    EXPECT_EQ(scanners[1].rules[0].expression, "{name}");
    EXPECT_EQ(scanners[1].definitions.at("name"), "[a-z]+");
    EXPECT_EQ(scanners[1].definitions.at("digits"), "[0-9]+");
}

TEST(Read_re2c, Case_insensitive_keywords_tokenize_either_way)
{
    // The conventional twin's case-insensitive keywords tokenize either way, and lose to nothing shorter.
    const auto conventional{build(read_re2c(grammar("c-like-conventional.re")).front(), "INITIAL")};

    EXPECT_EQ(conventional.tokenize<std::size_t>(std::string{"WHILE"}).length, 5u);
    EXPECT_EQ(conventional.tokenize<std::size_t>(std::string{"while"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(conventional.tokenize<std::size_t>(std::string{"whilex"}).token, std::optional<std::size_t>{1});
}

TEST(Read_re2c, A_class_difference_becomes_the_bracket_of_the_bytes_left)
{
    // The forms re2c's own lexer, PHP's and yasm's use: a definition minus a bracket, [^] minus an alternation of
    // classes and one-byte literals, a parenthesised difference, and a chain; the pattern keeps the operator.
    constexpr std::string_view source{R"(/*!re2c
    any = [\000-\377];
    eol = [\n];
    naked_char = [^] \ ("\000" | eol | [ \t]);
    naked = (naked_char \ ['"]) naked_char*;
    ";" (any \ [\000])*  { return COMMENT; }
    naked                { return NAKED; }
    [a-z] \ [aeiou] \ [x-z] { return CONSONANT; }
    re2c:define:YYFILL = 'if (!fill()) return error("no; input");';
    [ \t\n]+ { continue; }
*/
)"};

    const auto spec{read_re2c(source).front()};

    EXPECT_EQ(spec.definitions.at("naked_char"), R"([\x01-\x08\x0b-\x1f!-\xff])");
    EXPECT_EQ(spec.definitions.at("naked"), R"(([\x01-\x08\x0b-\x1f!#-&(-\xff]){naked_char}*)");

    ASSERT_EQ(spec.rules.size(), 4u);
    EXPECT_EQ(spec.rules[0].pattern, R"(";" (any \ [\000])*)");
    EXPECT_EQ(spec.rules[0].expression, R"(";"([\x01-\xff])*)");
    EXPECT_EQ(spec.rules[2].pattern, R"([a-z] \ [aeiou] \ [x-z])");
    EXPECT_EQ(spec.rules[2].expression, "[b-df-hj-np-tvw]");

    // The configuration's value carries a ';' inside its quotes without ending the configuration there.
    ASSERT_EQ(spec.options.size(), 1u);
    EXPECT_EQ(spec.rules[3].pattern, R"([ \t\n]+)");
}

TEST(Read_re2c, Rules_blocks_are_libraries_the_use_directive_merges_and_tags_and_empty_rules_are_no_tokens)
{
    // re2c's own lexer: a named rules block holding a definition, local blocks using it, a use block, a tag in a
    // pattern, and the empty rule with and without trailing context.
    constexpr std::string_view source{R"(/*!rules:re2c:char_lit
    re2c:flags:utf-8 = 1;
    esc = [\\];
    char_lit = esc [x] [0-9a-f]{2} | [^];
*/
/*!local:re2c
    !use:char_lit;
    char_lit [']  { return Ret::OK; }
    ""            { return Ret::OK; }
*/
/*!re2c
    ":"? "=>" @p [a-z]+ #q  { return ARROW; }
    "" / [ ]                { return NOTHING; }
    [ ]+                    { return SPACE; }
*/
/*!use:re2c:char_lit
    char_lit [`]  { return Ret::OK; }
*/
)"};

    const auto scanners{read_re2c(source)};

    ASSERT_EQ(scanners.size(), 3u);

    EXPECT_EQ(scanners[0].line, 6u);
    ASSERT_EQ(scanners[0].rules.size(), 1u);
    EXPECT_EQ(scanners[0].rules[0].expression, "{char_lit}[']");
    EXPECT_EQ(scanners[0].definitions.at("char_lit"), R"({esc}[x][0-9a-f]{2}|[\x00-\xff])");
    EXPECT_EQ(scanners[0].options, (std::vector<std::string>{"flags:utf-8=1"}));

    EXPECT_EQ(scanners[1].line, 11u);
    ASSERT_EQ(scanners[1].rules.size(), 2u);
    EXPECT_EQ(scanners[1].rules[0].pattern, R"(":"? "=>" @p [a-z]+ #q)");
    EXPECT_EQ(scanners[1].rules[0].expression, R"(":"?"=>"[a-z]+)");
    EXPECT_EQ(scanners[1].rules[1].token, std::optional<std::string>{"SPACE"});
    EXPECT_FALSE(scanners[1].definitions.contains("esc"));

    EXPECT_EQ(scanners[2].line, 16u);
    ASSERT_EQ(scanners[2].rules.size(), 1u);
    EXPECT_EQ(scanners[2].rules[0].expression, "{char_lit}[`]");
    EXPECT_EQ(scanners[2].definitions.at("esc"), R"([\\])");
}

TEST(Read_re2c, Refusals_name_the_line)
{
    const auto line_of{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source);
        }
        catch (const Spec_error& error)
        {
            return static_cast<long>(error.line());
        }

        return -1L;
    }};

    EXPECT_EQ(line_of("/*!re2c\n [a-z]+ { return X;\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n digit = [0-9]\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] \\ [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] \\ \"ab\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \\ [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n !use:missing;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n !include \"other.re\";\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\u00e9\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\u00e9] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\\\u\" [0-9a-fA-F]{4} { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n [\\\\u] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n '\\X00e9' { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a-z]+\n*/"), 3);
}

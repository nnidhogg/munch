#include "munch/tools/audit/read_re2c.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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
 *        case-insensitive literal, conditions with a transition, a `:=` action over two lines, comments, and the
 *        special rules. A block is a C comment, so a comment closer inside it is spelled `"*" "/"`, as re2c users
 *        spell it, and the rule after the `:=` action opens its line, which is where re2c ends such an action; re2c
 *        itself reads this block as the test expects.
 */
constexpr std::string_view idioms{R"(int lex() {
    /*!re2c
        re2c:define:YYCTYPE = char;  // a configuration
        digit  = [0-9];
        number = digit+ ("." digit+)?;   // a definition using another, as a block may only comment this way

        <INITIAL> 'true' | 'false'   { return BOOLEAN; }
        <INITIAL> number             := digits = 1;
                                        return NUMBER;
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

    // Seven rules: the setup rule and the end rule are not tokens, and the default rule is one, placed last.
    ASSERT_EQ(spec.rules.size(), 7u);

    EXPECT_EQ(spec.rules[0].pattern, "'true' | 'false'");
    EXPECT_EQ(spec.rules[0].expression, "[tT][rR][uU][eE]|[fF][aA][lL][sS][eE]");
    EXPECT_EQ(spec.rules[0].conditions, (std::vector<std::string>{"INITIAL"}));
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"BOOLEAN"});
    EXPECT_EQ(spec.rules[0].line, 7u);

    // The `:=` action runs to the line that opens with the next rule, so both its lines are the one action.
    EXPECT_EQ(spec.rules[1].pattern, "number");
    EXPECT_EQ(spec.rules[1].expression, "{number}");
    EXPECT_EQ(spec.rules[1].token, std::optional<std::string>{"NUMBER"});
    EXPECT_TRUE(spec.rules[1].action.starts_with(":= digits = 1;"));
    EXPECT_TRUE(spec.rules[1].action.ends_with("return NUMBER;"));

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

    // The default rule is the token its action returns, over one byte, and stands last whatever its line.
    EXPECT_EQ(spec.rules[6].pattern, "*");
    EXPECT_EQ(spec.rules[6].expression, R"([\x00-\xff])");
    EXPECT_EQ(spec.rules[6].conditions, (std::vector<std::string>{"INITIAL"}));
    EXPECT_EQ(spec.rules[6].token, std::optional<std::string>{"ERROR"});
    EXPECT_EQ(spec.rules[6].line, 15u);

    // The conditions are the ones the rules name, INITIAL and `*` aside, each exclusive.
    ASSERT_EQ(spec.conditions.size(), 1u);
    EXPECT_EQ(spec.conditions.front().name, "COMMENT");
    EXPECT_TRUE(spec.conditions.front().exclusive);
    EXPECT_EQ(active_rules(spec, "COMMENT"), (std::vector<std::size_t>{3, 4, 5}));
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 5, 6}));
}

TEST(Read_re2c, The_default_rule_is_a_token_of_one_byte_at_the_lowest_priority_wherever_it_stands)
{
    // re2c 3.1, run on this block: "a" is the default rule's token, since "ab" needs two bytes and nothing else
    // takes one "a"; "b" is the third rule's, though the default rule stands above it; "ab" is the second rule's.
    const auto ascii{read_re2c("/*!re2c\n*     { return 2; }\n\"ab\"  { return 1; }\n[b]   { return 3; }\n*/\n")};

    ASSERT_EQ(ascii.size(), 1u);
    ASSERT_EQ(ascii.front().rules.size(), 3u);
    EXPECT_EQ(ascii.front().rules.back().pattern, "*");
    EXPECT_EQ(ascii.front().rules.back().line, 2u);

    const auto lexer{build(ascii.front(), "INITIAL")};

    // A token's id is its rule's index once the default rule stands last: "ab" is 0, [b] is 1 and `*` is 2.
    const auto token{[&lexer](const std::string_view input) {
        const auto match{lexer.tokenize<std::size_t>(std::string{input})};

        return std::pair<std::size_t, std::size_t>{match.token.value_or(99), match.length};
    }};

    EXPECT_EQ(token("a"), (std::pair<std::size_t, std::size_t>{2, 1}));
    EXPECT_EQ(token("ab"), (std::pair<std::size_t, std::size_t>{0, 2}));
    EXPECT_EQ(token("b"), (std::pair<std::size_t, std::size_t>{1, 1}));
    EXPECT_EQ(token("c"), (std::pair<std::size_t, std::size_t>{2, 1}));

    // Under UTF-8 the default rule takes one byte where `[^]` takes a whole code point: re2c 3.1 scans the two
    // bytes of "\xc3\xa9" as two default-rule matches under the first block and as one `[^]` match under the second.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 1; }\n* { return 2; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 1; }\n[^] { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 2u);
    EXPECT_EQ(build(utf8[0], "INITIAL").tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 1u);
    EXPECT_EQ(build(utf8[1], "INITIAL").tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 2u);
}

TEST(Read_re2c, A_second_default_rule_for_a_condition_is_refused_and_a_used_one_yields_to_the_block_s_own)
{
    // re2c 3.1 answers each of these with "code to default rule in condition ... is already defined at line N": two
    // default rules with no condition, two in `<*>`, and `<a, b>` after `<a>` gave a its own.
    const auto refused_at{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source);
        }
        catch (const Spec_error& error)
        {
            return std::pair{static_cast<long>(error.line()), std::string{error.what()}};
        }

        return std::pair{-1L, std::string{}};
    }};

    const auto twice{refused_at("/*!re2c\n*  { return 1; }\n[a] { return 2; }\n*  { return 3; }\n*/\n")};

    EXPECT_EQ(twice.first, 4);
    EXPECT_NE(twice.second.find("already defined at line 2"), std::string::npos);
    EXPECT_EQ(refused_at("/*!re2c\n<*> * { return 1; }\n<*> * { return 2; }\n<a> \"x\" { return 3; }\n*/\n").first, 3);
    EXPECT_EQ(
            refused_at("/*!re2c\n<a> * { return 1; }\n<a, b> * { return 2; }\n<b> \"x\" { return 3; }\n*/\n").first, 3);

    // `<*> *` and `<a> *` are rules of different conditions to re2c, so both stand, and a's own wins in a: re2c 3.1,
    // run with -c, scans "z" in a as 2 with the `<*> *` rule above it.
    const auto both{read_re2c("/*!re2c\n<*> * { return 1; }\n<a> * { return 2; }\n<a> \"x\" { return 3; }\n*/\n")};

    ASSERT_EQ(both.size(), 1u);
    ASSERT_EQ(both.front().rules.size(), 3u);
    EXPECT_EQ(both.front().rules[1].token, std::optional<std::string>{"2"});
    EXPECT_EQ(both.front().rules[2].token, std::optional<std::string>{"1"});
    EXPECT_EQ(build(both.front(), "a").tokenize<std::size_t>(std::string{"z"}).token, std::optional<std::size_t>{1});

    // A used block's default rule yields to the using block's own, wherever the two stand: re2c 3.1 scans "c" as 4
    // with the `!use:` directive above the block's own default rule and below it alike.
    for (const auto own_first : {false, true})
    {
        const std::string block{
                "/*!rules:re2c:base\n* { return 1; }\n\"ab\" { return 2; }\n*/\n"
                "/*!re2c\n" +
                std::string{own_first ? "* { return 4; }\n!use:base;\n" : "!use:base;\n* { return 4; }\n"} +
                "[a] { return 3; }\n*/\n"};

        const auto scanners{read_re2c(block)};

        ASSERT_EQ(scanners.size(), 1u);
        ASSERT_EQ(scanners.front().rules.size(), 3u);
        EXPECT_EQ(scanners.front().rules.back().pattern, "*");
        EXPECT_EQ(scanners.front().rules.back().token, std::optional<std::string>{"4"});
    }

    // With conditions the used rule yields condition by condition: re2c 3.1, run on this file with -c, scans "z" in
    // a as 2, the block's own, and in b as 1, the used rule's, which still stands there.
    const auto conditions{read_re2c(
            "/*!rules:re2c:base\n<a, b> * { return 1; }\n*/\n"
            "/*!re2c\n<a> * { return 2; }\n!use:base;\n<a> \"x\" { return 3; }\n<b> \"x\" { return 4; }\n*/\n")};

    // The default rules stand last in the order they were read, the block's own `<a> *` and then the used one, left
    // with b alone.
    ASSERT_EQ(conditions.size(), 1u);
    ASSERT_EQ(conditions.front().rules.size(), 4u);
    EXPECT_EQ(conditions.front().rules[2].conditions, (std::vector<std::string>{"a"}));
    EXPECT_EQ(conditions.front().rules[2].token, std::optional<std::string>{"2"});
    EXPECT_EQ(conditions.front().rules[3].conditions, (std::vector<std::string>{"b"}));
    EXPECT_EQ(conditions.front().rules[3].token, std::optional<std::string>{"1"});
    EXPECT_EQ(
            build(conditions.front(), "a").tokenize<std::size_t>(std::string{"z"}).token,
            std::optional<std::size_t>{2});
    EXPECT_EQ(
            build(conditions.front(), "b").tokenize<std::size_t>(std::string{"z"}).token,
            std::optional<std::size_t>{3});

    // A use block's own default rule overrides the used block's the same way.
    const auto use_block{read_re2c("/*!rules:re2c:base\n* { return 1; }\n*/\n/*!use:re2c:base\n* { return 2; }\n*/\n")};

    ASSERT_EQ(use_block.size(), 1u);
    ASSERT_EQ(use_block.front().rules.size(), 1u);
    EXPECT_EQ(use_block.front().rules.front().token, std::optional<std::string>{"2"});
}

TEST(Read_re2c, Star_rules_rank_below_a_condition_s_own_wherever_they_stand)
{
    // What the rule taking an input returns, in a condition.
    const auto returned{[](const Lexer_spec& spec, const std::string_view condition, const std::string_view input) {
        const auto match{build(spec, condition).tokenize<std::size_t>(std::string{input})};

        return match.token ? spec.rules[*match.token].token.value_or("none") : std::string{"none"};
    }};

    // re2c 3.1, run with -c on this block, scans "x" in a as 4 and warns that the `<*>` rule above it is shadowed by
    // it, and scans "x" in b as 3; the same with the `<*>` rule written below a's rules.
    for (const auto star_first : {true, false})
    {
        const std::string star{"<*> \"x\" { return 3; }\n"};

        const std::string own{"<a> \"x\" { return 4; }\n<a> [y] { return 5; }\n"};

        const auto scanners{
                read_re2c("/*!re2c\n" + (star_first ? star + own : own + star) + "<b> [y] { return 6; }\n*/\n")};

        ASSERT_EQ(scanners.size(), 1u);
        ASSERT_EQ(scanners.front().rules.size(), 4u);
        EXPECT_EQ(scanners.front().rules[3].pattern, "\"x\"");
        EXPECT_EQ(scanners.front().rules[3].conditions, (std::vector<std::string>{"*"}));
        EXPECT_EQ(returned(scanners.front(), "a", "x"), "4");
        EXPECT_EQ(returned(scanners.front(), "b", "x"), "3");
    }

    // Among several: re2c 3.1 scans, in a, "x" as 2, "q" and "m" as 4 and "xy" as 5, and in b "x" as 6 and "q" and "m"
    // as 1, so a condition's own rules come first in their order and the `<*>` rules after them in theirs.
    const auto ties{read_re2c(
            "/*!re2c\n<*> [a-z] { return 1; }\n<a> [x] { return 2; }\n<*> [q] { return 3; }\n<a> [a-z] { return 4; }\n"
            "<*> \"xy\" { return 5; }\n<b> [x] { return 6; }\n*/\n")};

    ASSERT_EQ(ties.size(), 1u);
    EXPECT_EQ(returned(ties.front(), "a", "x"), "2");
    EXPECT_EQ(returned(ties.front(), "a", "q"), "4");
    EXPECT_EQ(returned(ties.front(), "a", "m"), "4");
    EXPECT_EQ(returned(ties.front(), "a", "xy"), "5");
    EXPECT_EQ(returned(ties.front(), "b", "x"), "6");
    EXPECT_EQ(returned(ties.front(), "b", "q"), "1");
    EXPECT_EQ(returned(ties.front(), "b", "m"), "1");

    // The default rules rank the same way: re2c 3.1 scans "z" in a as 2 and in b as 1 with `<*> *` above `<a> *` and
    // below it alike, while "y" is 5 in both, a `<*>` rule beating any default rule.
    for (const auto star_first : {true, false})
    {
        const std::string star{"<*> * { return 1; }\n"};

        const std::string own{
                "<a> * { return 2; }\n<a> \"x\" { return 3; }\n<b> \"x\" { return 4; }\n<*> \"y\" { return 5; }\n"};

        const auto scanners{read_re2c("/*!re2c\n" + (star_first ? star + own : own + star) + "*/\n")};

        ASSERT_EQ(scanners.size(), 1u);
        ASSERT_EQ(scanners.front().rules.size(), 5u);
        EXPECT_EQ(scanners.front().rules[3].conditions, (std::vector<std::string>{"a"}));
        EXPECT_EQ(scanners.front().rules[4].conditions, (std::vector<std::string>{"*"}));
        EXPECT_EQ(returned(scanners.front(), "a", "z"), "2");
        EXPECT_EQ(returned(scanners.front(), "b", "z"), "1");
        EXPECT_EQ(returned(scanners.front(), "a", "y"), "5");
        EXPECT_EQ(returned(scanners.front(), "b", "y"), "5");
    }

    // A used block's rules stand where the directive does and rank by their conditions like the rest: re2c 3.1 scans,
    // in a, "x" as 11, "y" as 2 and "z" and "m" as 4, and in b "x" as 1, "y" as 12, "z" as 3 and "m" as 13.
    const auto used{read_re2c(
            "/*!rules:re2c:base\n<a> \"x\" { return 11; }\n<*> \"y\" { return 12; }\n<*> [a-z] { return 13; }\n*/\n"
            "/*!re2c\n<*> \"x\" { return 1; }\n!use:base;\n<a> \"y\" { return 2; }\n<b> \"z\" { return 3; }\n"
            "<a> [a-z] { return 4; }\n*/\n")};

    ASSERT_EQ(used.size(), 1u);
    EXPECT_EQ(returned(used.front(), "a", "x"), "11");
    EXPECT_EQ(returned(used.front(), "a", "y"), "2");
    EXPECT_EQ(returned(used.front(), "a", "z"), "4");
    EXPECT_EQ(returned(used.front(), "a", "m"), "4");
    EXPECT_EQ(returned(used.front(), "b", "x"), "1");
    EXPECT_EQ(returned(used.front(), "b", "y"), "12");
    EXPECT_EQ(returned(used.front(), "b", "z"), "3");
    EXPECT_EQ(returned(used.front(), "b", "m"), "13");
}

TEST(Read_re2c, Rules_naming_a_condition_and_rules_naming_none_in_one_scanner_are_refused_as_re2c_refuses_them)
{
    // The line a refusal points at and its words, or no line.
    const auto refused_at{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source);
        }
        catch (const Spec_error& error)
        {
            return std::pair{static_cast<long>(error.line()), std::string{error.what()}};
        }

        return std::pair{-1L, std::string{}};
    }};

    // re2c 3.1, run with -c, answers each of these with "cannot mix conditions with normal rules" at the first rule
    // naming no condition, wherever it stands: after a `<a>` rule, before one, beside a `<*>` rule, the default rule
    // `*` and the empty rule `""` naming none, and a used block's rule naming none beside the using block's `<a>`.
    const std::string mixing{"cannot mix conditions with normal rules"};

    const auto after{refused_at("/*!re2c\n<a> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n")};

    EXPECT_EQ(after.first, 3);
    EXPECT_NE(after.second.find(mixing), std::string::npos);
    EXPECT_EQ(refused_at("/*!re2c\n\"y\" { return 2; }\n<a> \"x\" { return 1; }\n*/\n").first, 2);
    EXPECT_EQ(refused_at("/*!re2c\n<*> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n").first, 3);
    EXPECT_EQ(refused_at("/*!re2c\n<a> \"x\" { return 1; }\n* { return 2; }\n*/\n").first, 3);
    EXPECT_EQ(refused_at("/*!re2c\n<a> \"x\" { return 1; }\n\"\" { return 2; }\n*/\n").first, 3);

    const auto used{refused_at(
            "/*!rules:re2c:base\n\"y\" { return 2; }\n*/\n/*!re2c\n<a> \"x\" { return 1; }\n!use:base;\n*/\n")};

    EXPECT_EQ(used.first, 2);
    EXPECT_NE(used.second.find(mixing), std::string::npos);

    // The end rule `$` is counted otherwise: alone beside `<a>` rules re2c answers "EOF rule without other rules
    // doesn't make sense" at it, and beside another rule naming none the mixing is what is refused, at that rule.
    const auto end{refused_at("/*!re2c\n<a> \"x\" { return 1; }\n$ { return 3; }\n*/\n")};

    EXPECT_EQ(end.first, 3);
    EXPECT_NE(end.second.find("EOF rule without other rules doesn't make sense"), std::string::npos);
    EXPECT_EQ(refused_at("/*!re2c\n<a> \"x\" { return 1; }\n$ { return 3; }\n\"y\" { return 2; }\n*/\n").first, 4);

    // Not refused: a rules block of both kinds that no scanner uses, since re2c compiles it nowhere; a setup rule
    // `<!*>` beside `<a>` rules; and rules of one kind alone.
    EXPECT_EQ(
            refused_at("/*!rules:re2c:base\n<a> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n"
                       "/*!re2c\n<a> \"x\" { return 1; }\n*/\n")
                    .first,
            -1);
    EXPECT_EQ(refused_at("/*!re2c\n<a> \"x\" { return 1; }\n<!*> { setup(); }\n*/\n").first, -1);
    EXPECT_EQ(refused_at("/*!re2c\n\"y\" { return 2; }\n* { return 3; }\n*/\n").first, -1);
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

TEST(Read_re2c, A_flex_style_definition_stands_wherever_its_name_does_and_admits_no_action_on_its_line)
{
    // What the rule taking an input returns, and how many bytes it takes.
    const auto scanned{[](const Lexer_spec& spec, const std::string_view input) {
        const auto match{build(spec, "INITIAL").tokenize<std::size_t>(std::string{input})};

        return std::pair{
                match.token ? spec.rules[*match.token].token.value_or("none") : std::string{"none"}, match.length};
    }};

    constexpr Re2c_flags flex{.flex_syntax = true};

    // re2c 3.1 -F reads a name followed by a blank as a definition wherever the name stands, indented or after a rule
    // on the same line: it scans, under the first block, the byte E9 as 1 and a as 2, and under the second a as 3, b
    // as 1 and c as 4.
    const auto indented{
            read_re2c("/*!re2c\n    accent [\\xe9]\n    {accent} { return 1; }\n    * { return 2; }\n*/\n", flex)};

    ASSERT_EQ(indented.size(), 1u);
    EXPECT_EQ(indented.front().definitions.at("accent"), R"([\xe9])");
    EXPECT_EQ(scanned(indented.front(), "\xe9"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(indented.front(), "a"), (std::pair{std::string{"2"}, std::size_t{1}}));

    const auto mid_line{read_re2c(
            "/*!re2c\nx [a]\n[b] { return 1; } y [c]\n{x} { return 3; }\n{y} { return 4; }\n* { return 2; }\n*/\n",
            flex)};

    ASSERT_EQ(mid_line.size(), 1u);
    EXPECT_EQ(scanned(mid_line.front(), "a"), (std::pair{std::string{"3"}, std::size_t{1}}));
    EXPECT_EQ(scanned(mid_line.front(), "b"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(mid_line.front(), "c"), (std::pair{std::string{"4"}, std::size_t{1}}));

    // A `{` after the blanks opens no definition: `alias   {accent}` is a rule whose regex is the literal alias and
    // the reference, running on to the next line's reference, so re2c 3.1 -F scans "alias" as five matches of the
    // default rule and "alias" followed by E9 twice as 1; and with `{alias}` on the next line the name is a symbol
    // no definition binds, which re2c answers with "undefined symbol", the reading where the scanner is built.
    const auto brace{read_re2c(
            "/*!re2c\naccent [\\xe9]\nalias   {accent}\n{accent} { return 1; }\n* { return 2; }\n*/\n", flex)};

    ASSERT_EQ(brace.size(), 1u);
    ASSERT_EQ(brace.front().rules.size(), 2u);
    EXPECT_EQ(brace.front().rules[0].expression, "alias{accent}{accent}");
    EXPECT_EQ(scanned(brace.front(), "alias"), (std::pair{std::string{"2"}, std::size_t{1}}));
    EXPECT_EQ(scanned(brace.front(), "alias\xe9\xe9"), (std::pair{std::string{"1"}, std::size_t{7}}));

    const auto unbound{
            read_re2c("/*!re2c\naccent [\\xe9]\nalias   {accent}\n{alias} { return 1; }\n* { return 2; }\n*/\n", flex)};

    ASSERT_EQ(unbound.size(), 1u);
    EXPECT_FALSE(unbound.front().definitions.contains("alias"));
    EXPECT_THROW(std::ignore = build(unbound.front(), "INITIAL"), Spec_error);

    // An action on the definition's line is refused, as re2c -F answers it with a syntax error, the name defined
    // before or not, and a bare name after the blank opening a second definition alike; without the flag the name
    // is besides a symbol no definition binds.
    const auto refused{[](const std::string_view source, const Re2c_flags flags) {
        try
        {
            std::ignore = read_re2c(source, flags);
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    EXPECT_NE(refused("/*!re2c\naccent [\\xe9] { return 5; }\n*/\n", flex).find("syntax error"), std::string::npos);
    EXPECT_NE(refused("/*!re2c\nx [a]\nx [b] { return 3; }\n*/\n", flex).find("syntax error"), std::string::npos);
    EXPECT_NE(refused("/*!re2c\na b { return 3; }\n*/\n", flex).find("syntax error"), std::string::npos);
    EXPECT_NE(
            refused("/*!re2c\naccent [\\xe9] { return 5; }\n*/\n", {}).find("no definition binds"), std::string::npos);
    EXPECT_EQ(refused("/*!re2c\nab { return 3; }\n*/\n", flex), "");
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
    EXPECT_EQ(scanners[1].rules.size(), 2u);
    EXPECT_EQ(scanners[1].rules[0].expression, "{name}");
    EXPECT_EQ(scanners[1].rules[1].pattern, "*");
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

TEST(Read_re2c, A_class_difference_subtracts_code_points_and_takes_the_operands_re2c_takes)
{
    // What the rule taking an input returns, and how many bytes it takes.
    const auto scanned{[](const Lexer_spec& spec, const std::string_view input) {
        const auto match{build(spec, "INITIAL").tokenize<std::size_t>(std::string{input})};

        return std::pair{
                match.token ? spec.rules[*match.token].token.value_or("none") : std::string{"none"}, match.length};
    }};

    const std::string e_acute{"\xc3\xa9"};

    const std::string surrogate{"\xed\xa0\x80"};

    // Under UTF-8 the difference is taken over code points and the encoding comes after it: re2c 3.1 scans é, the
    // bytes C3 A9, as 1 over both, a as 2, the euro sign as 1 over three bytes, and ED A0 80 as 1 over three, its
    // default encoding policy encoding a surrogate like any other code point.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\x7f] { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 1u);
    EXPECT_EQ(
            utf8.front().rules[0].expression, R"(([\u{80}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanned(utf8.front(), e_acute), (std::pair{std::string{"1"}, std::size_t{2}}));
    EXPECT_EQ(scanned(utf8.front(), "a"), (std::pair{std::string{"2"}, std::size_t{1}}));
    EXPECT_EQ(scanned(utf8.front(), "\xe2\x82\xac"), (std::pair{std::string{"1"}, std::size_t{3}}));
    EXPECT_EQ(scanned(utf8.front(), surrogate), (std::pair{std::string{"1"}, std::size_t{3}}));

    // The operands re2c takes for char sets, over code points too: a one-character literal in either quote, a
    // negated class, the dot, a group of alternatives that are each one, a name defined as any of these, and a chain
    // of differences through definitions, a used block's among them. re2c 3.1 scans, in the first scanner, é as 3
    // over two bytes, a as 5, ê as 6 over two, "aé b" as 6 over three bytes and then 1 and 6, the byte 01 as 1 and
    // the newline as 4; and in the second é as 7 over two bytes and a as 4.
    const auto operands{read_re2c(
            "/*!rules:re2c:base\nany = [^];\nnonascii = any \\ [\\x00-\\x7f];\nnonascii { return 7; }\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\nany = [^];\nctl = [\\x00-\\x1f];\nprintable = any \\ ctl;\n"
            "word = printable \\ (' ' | \"\\t\");\n\"\\xe9\" \\ 'x' { return 3; }\nany \\ [^a] { return 5; }\n"
            "word+ { return 6; }\n. \\ [a] { return 1; }\n* { return 4; }\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\n!use:base;\n* { return 4; }\n*/\n")};

    ASSERT_EQ(operands.size(), 2u);
    ASSERT_EQ(operands.front().rules.size(), 5u);
    EXPECT_EQ(operands.front().rules[0].expression, R"([\u{e9}])");
    EXPECT_EQ(operands.front().rules[1].expression, "[a]");
    EXPECT_EQ(operands.front().rules[2].expression, "{word}+");
    EXPECT_EQ(scanned(operands.front(), e_acute), (std::pair{std::string{"3"}, std::size_t{2}}));
    EXPECT_EQ(scanned(operands.front(), "a"), (std::pair{std::string{"5"}, std::size_t{1}}));
    EXPECT_EQ(scanned(operands.front(), "\xc3\xaa"), (std::pair{std::string{"6"}, std::size_t{2}}));
    EXPECT_EQ(scanned(operands.front(), "a" + e_acute + " b"), (std::pair{std::string{"6"}, std::size_t{3}}));
    EXPECT_EQ(scanned(operands.front(), " b"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(operands.front(), "b"), (std::pair{std::string{"6"}, std::size_t{1}}));
    EXPECT_EQ(scanned(operands.front(), "\x01"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(operands.front(), "\n"), (std::pair{std::string{"4"}, std::size_t{1}}));
    EXPECT_EQ(scanned(operands.back(), e_acute), (std::pair{std::string{"7"}, std::size_t{2}}));
    EXPECT_EQ(scanned(operands.back(), "a"), (std::pair{std::string{"4"}, std::size_t{1}}));

    // The difference takes the whole term on either side, as re2c's grammar has it, so a concatenation, a
    // repetition, a tag or a literal of two characters on either side is refused as re2c refuses it, "can only
    // difference char sets"; a group around the difference and an alternation beside it stand.
    const auto refused{[](const std::string_view rule) {
        try
        {
            std::ignore = read_re2c("/*!re2c\n" + std::string{rule} + " { return 1; }\n*/\n");
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()}.find("can only difference char sets") != std::string::npos;
        }

        return false;
    }};

    EXPECT_TRUE(refused(R"([a-z] \ [x] [y])"));
    EXPECT_TRUE(refused(R"([y] [a-z] \ [x])"));
    EXPECT_TRUE(refused(R"([a-z] \ [x]*)"));
    EXPECT_TRUE(refused(R"([a-z]* \ [x])"));
    EXPECT_TRUE(refused(R"([a-z] \ [x] @p)"));
    EXPECT_TRUE(refused(R"([a-z] \ "xy")"));
    EXPECT_TRUE(refused(R"([a-z] \ ("ab" | [\n]))"));
    EXPECT_TRUE(refused("d = [a-z][0-9];\nd \\ [b]"));
    EXPECT_FALSE(refused(R"(([a-z] \ [x])+)"));
    EXPECT_FALSE(refused(R"([0-9] | [a-z] \ [x] | [A-Z])"));

    // re2c 3.1 scans, under the last two, "abc" as 1 over three bytes and x as 2, and 1, a and Q as 1 and x as 2.
    const auto shapes{
            read_re2c("/*!re2c\n([a-z] \\ [x])+ { return 1; }\n* { return 2; }\n*/\n"
                      "/*!re2c\n[0-9] | [a-z] \\ [x] | [A-Z] { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(scanned(shapes.front(), "abc"), (std::pair{std::string{"1"}, std::size_t{3}}));
    EXPECT_EQ(scanned(shapes.front(), "x"), (std::pair{std::string{"2"}, std::size_t{1}}));
    EXPECT_EQ(shapes.back().rules[0].expression, "[0-9]|[a-wyz]|[A-Z]");
    EXPECT_EQ(scanned(shapes.back(), "Q"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(shapes.back(), "x"), (std::pair{std::string{"2"}, std::size_t{1}}));
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
    // The used block sets the UTF-8 encoding, so `[^]` is every code point, the surrogates among them, and not
    // every byte; the blocks using it read their own rules under that encoding as well.
    EXPECT_EQ(
            scanners[0].definitions.at("char_lit"),
            R"({esc}[x][0-9a-f]{2}|([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
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

TEST(Read_re2c, Configurations_govern_the_whole_block_under_every_name_re2c_gives_them)
{
    // A configuration affects the whole block, even where it stands at the end of it, so the rule above it is
    // case-insensitive too; re2c compiled from this shape matches WHILE.
    const auto late{read_re2c("/*!re2c\n\"while\" { return W; }\nre2c:flags:case-insensitive = 1;\n*/\n").front()};

    EXPECT_EQ(late.rules.front().expression, "[wW][hH][iI][lL][eE]");

    // The canonical name is as good as the `flags:` alias, both being what the manual's configuration list gives.
    const auto canonical{read_re2c("/*!re2c\nre2c:case-insensitive = 1;\n\"if\" { return I; }\n*/\n").front()};

    EXPECT_EQ(canonical.rules.front().expression, "[iI][fF]");

    const auto inverted{
            read_re2c("/*!re2c\nre2c:case-inverted = 1;\n\"if\" { return I; }\n'do' { return D; }\n*/\n").front()};

    EXPECT_EQ(inverted.rules[0].expression, "[iI][fF]");
    EXPECT_EQ(inverted.rules[1].expression, "[d][o]");

    // Of two assignments to one configuration the last is the one that governs, so the zero standing above the rule
    // holds nothing back: re2c 3.1 compiled from this shape matches ABC, and so does the rule read here.
    const auto repeated{read_re2c("/*!re2c\nre2c:case-insensitive = 0;\n\"abc\" { return A; }\n"
                                  "re2c:case-insensitive = 1;\n*/\n")
                                .front()};

    EXPECT_EQ(repeated.rules.front().expression, "[aA][bB][cC]");

    // The encoding goes the same way: turned on above the rule and off below it, the block reads bytes, so `[^]`
    // admits one byte rather than a whole code point, which is what re2c's own scanner does with it.
    const auto turned{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] { return A; }\nre2c:encoding:utf8 = 0;\n*/\n").front()};

    EXPECT_EQ(turned.rules.front().expression, R"([\x00-\xff])");
}

TEST(Read_re2c, An_unnamed_use_block_takes_the_most_recent_rules_block_with_its_configurations)
{
    // A use block with no name of its own uses the most recent rules block, named or not, and reads its own rules
    // under the configurations it inherits from it; the name in `use:re2c:name` is the block used, not a name of the
    // use block, so a second use of the same rules block gets that block's rules and not the first one's.
    constexpr std::string_view source{R"(/*!rules:re2c:kw
    re2c:case-insensitive = 1;
    "while" { return WHILE; }
*/
/*!use:re2c
    "if" { return IF; }
*/
/*!use:re2c:kw
    "for" { return FOR; }
*/
)"};

    const auto scanners{read_re2c(source)};

    ASSERT_EQ(scanners.size(), 2u);

    ASSERT_EQ(scanners[0].rules.size(), 2u);
    EXPECT_EQ(scanners[0].rules[0].expression, "[wW][hH][iI][lL][eE]");
    EXPECT_EQ(scanners[0].rules[1].expression, "[iI][fF]");

    ASSERT_EQ(scanners[1].rules.size(), 2u);
    EXPECT_EQ(scanners[1].rules[0].token, std::optional<std::string>{"WHILE"});
    EXPECT_EQ(scanners[1].rules[1].token, std::optional<std::string>{"FOR"});
}

TEST(Read_re2c, The_utf8_encoding_matches_code_points_and_the_other_encodings_are_refused)
{
    // Under the UTF-8 encoding a pattern names code points and the scanner reads their encodings: `[^]` is every
    // code point, the surrogates among them, which re2c's default encoding policy encodes like any other; the dot is
    // every one but the newline; an escape beyond ASCII is a code point of two bytes; an ASCII literal stands as it
    // is. A scanner re2c compiled from these rules consumes the same one to four bytes per code point, three for a
    // surrogate's encoding, and none of a stray continuation byte or an overlong one.
    constexpr std::string_view source{R"(/*!re2c
    re2c:encoding:utf8 = 1;
    [^]    { return ANY; }
    .      { return DOT; }
    [\xff] { return HIGH; }
    "ab"   { return AB; }
*/
)"};

    const auto spec{read_re2c(source).front()};

    ASSERT_EQ(spec.rules.size(), 4u);
    EXPECT_EQ(spec.rules[0].expression, R"(([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");

    // A caret among the members of a negated class is a member, not a second negation.
    const auto caret{read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^^] { return X; }\n*/\n").front()};

    EXPECT_EQ(
            caret.rules.front().expression,
            R"(([\u{0}-\u{5d}\u{5f}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(
            spec.rules[1].expression,
            R"(([\u{0}-\u{9}\u{b}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(spec.rules[2].expression, R"([\u{ff}])");
    EXPECT_EQ(spec.rules[3].expression, R"("ab")");

    // Built, one code point is one token however many bytes it takes, and an encoding no code point has is no token.
    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"a"}).length, 1u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 2u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xe2\x82\xac"}).length, 3u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xf0\x9f\x98\x80"}).length, 4u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xed\xa0\x80"}).length, 3u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\x80"}).length, 0u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"\xc0\xaf"}).length, 0u);
}

TEST(Read_re2c, A_used_block_is_read_again_under_the_flags_of_the_block_using_it)
{
    // re2c compiles a rules block's regexes at every point of use, under the configurations in force there, so the
    // using block's own configuration governs the rules it takes as well as the ones it writes: a scanner compiled
    // from this shape matches WHILE and IF alike, whichever side of the directive the configuration stands on.
    constexpr std::string_view directive{R"(/*!rules:re2c:kw
    "while" { return WHILE; }
*/
/*!re2c
    re2c:flags:case-insensitive = 1;
    !use:kw;
    "if" { return IF; }
*/
)"};

    const auto merged{read_re2c(directive).front()};

    ASSERT_EQ(merged.rules.size(), 2u);
    EXPECT_EQ(merged.rules[0].expression, "[wW][hH][iI][lL][eE]");
    EXPECT_EQ(merged.rules[1].expression, "[iI][fF]");

    // One rules block read under two encodings, which is what re2c's own multiple-encoding example does: `[^]` is
    // every code point in the use block that asks for UTF-8 and every byte in the one that asks for nothing.
    constexpr std::string_view encodings{R"(/*!rules:re2c
    [^] { return ANY; }
*/
/*!use:re2c
    re2c:encoding:utf8 = 1;
*/
/*!use:re2c
    "x" { return X; }
*/
)"};

    const auto scanners{read_re2c(encodings)};

    ASSERT_EQ(scanners.size(), 2u);
    EXPECT_EQ(
            scanners[0].rules.front().expression,
            R"(([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanners[1].rules.front().expression, R"([\x00-\xff])");

    // The rules the use block takes and the ones it writes are one compiled block, so of two assignments to one
    // configuration the use block's own is the last and governs both: re2c 3.1 compiled from the first shape
    // matches neither ABC nor DEF, and from the second both, whichever block the setting stands in.
    const auto sensitive{read_re2c("/*!rules:re2c\nre2c:case-insensitive = 1;\n\"abc\" { return A; }\n*/\n"
                                   "/*!use:re2c\nre2c:case-insensitive = 0;\n\"def\" { return D; }\n*/\n")
                                 .front()};

    ASSERT_EQ(sensitive.rules.size(), 2u);
    EXPECT_EQ(sensitive.rules[0].expression, R"("abc")");
    EXPECT_EQ(sensitive.rules[1].expression, R"("def")");

    const auto insensitive{read_re2c("/*!rules:re2c\nre2c:case-insensitive = 0;\n\"abc\" { return A; }\n*/\n"
                                     "/*!use:re2c\nre2c:case-insensitive = 1;\n\"def\" { return D; }\n*/\n")
                                   .front()};

    ASSERT_EQ(insensitive.rules.size(), 2u);
    EXPECT_EQ(insensitive.rules[0].expression, "[aA][bB][cC]");
    EXPECT_EQ(insensitive.rules[1].expression, "[dD][eE][fF]");
}

TEST(Read_re2c, A_block_that_turns_an_encoding_off_reads_under_what_it_left)
{
    // A configuration governs the whole block, so a block that turns the encoding it inherited off reads its
    // patterns under what it left: the raw byte below is one whose code points only an `--input-encoding` could
    // say, refused under UTF-8 and read as bytes without it, and re2c 3.1 compiles this file and matches it.
    const auto scanners{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 0;\n\"\xc3\xa9\" { return B; }\n*/\n")};

    ASSERT_EQ(scanners.size(), 2u);
    EXPECT_EQ(scanners[1].rules.front().expression, "\"\xc3\xa9\"");

    // With the encoding left on, the same byte is refused, at its own line.
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

    EXPECT_EQ(
            line_of("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"
                    "/*!re2c\n\"\xc3\xa9\" { return B; }\n*/\n"),
            6);
}

TEST(Read_re2c, A_definition_another_block_uses_is_translated_again_under_that_blocks_flags)
{
    // re2c compiles a definition's regex at every point of use, so `point = [^];` written where the encoding was
    // ASCII admits a whole code point in the block that turns UTF-8 on: re2c 3.1 consumes both bytes of an e-acute
    // there, and so does the token set read here.
    const auto turned_on{
            read_re2c("/*!re2c\npoint = [^];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\npoint { return P; }\n*/\n")};

    ASSERT_EQ(turned_on.size(), 1u);
    EXPECT_EQ(build(turned_on.front(), "INITIAL").tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 2u);

    // And the other way about: written under UTF-8 and used where a configuration has turned it off, the same
    // definition admits one byte, which is what re2c's own scanner consumes there.
    const auto turned_off{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\npoint = [^];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 0;\npoint { return P; }\n*/\n")};

    ASSERT_EQ(turned_off.size(), 1u);
    EXPECT_EQ(turned_off.front().definitions.at("point"), R"([\x00-\xff])");
    EXPECT_EQ(build(turned_off.front(), "INITIAL").tokenize<std::size_t>(std::string{"\xc3\xa9"}).length, 1u);

    // A flex-style definition is read again under the flex syntax its own line is the evidence of, its regex
    // ending where its line does.
    const auto flex{read_re2c("/*!re2c\nDIGIT [0-9]\n*/\n/*!re2c\n{DIGIT}+ { return N; }\n*/\n")};

    ASSERT_EQ(flex.size(), 1u);
    EXPECT_EQ(flex.front().definitions.at("DIGIT"), "[0-9]");
    EXPECT_EQ(build(flex.front(), "INITIAL").tokenize<std::size_t>(std::string{"42"}).length, 2u);

    // A definition this block's flags cannot read waits for the block to name it, since that is where re2c would
    // compile it: the byte beyond ASCII below stands for whatever code points an `--input-encoding` says, which no
    // file carries, and re2c 3.1 compiles both of these files. Named, it is refused at its own line; unnamed, it is
    // left out and the block reads as re2c reads it.
    const auto unused{
            read_re2c("/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n")};

    ASSERT_EQ(unused.size(), 1u);
    EXPECT_FALSE(unused.front().definitions.contains("accent"));
    EXPECT_EQ(unused.front().rules.size(), 1u);

    const auto named{[] {
        try
        {
            std::ignore = read_re2c(
                    "/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                    "/*!re2c\nre2c:encoding:utf8 = 1;\naccent { return A; }\n*/\n");
        }
        catch (const Spec_error& error)
        {
            return static_cast<long>(error.line());
        }

        return -1L;
    }()};

    EXPECT_EQ(named, 2);
}

TEST(Read_re2c, An_unreadable_definition_is_refused_only_where_a_rule_reaches_it)
{
    // re2c 3.1 compiles this file, an alias of the unreadable definition standing beside it and no rule naming
    // either, and its scanner takes "a" as one byte: the alias is as unused as the definition, so the block reads.
    constexpr std::string_view unused{
            "/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"};

    const auto scanners{read_re2c(unused)};

    ASSERT_EQ(scanners.size(), 1u);
    EXPECT_FALSE(scanners.front().definitions.contains("accent"));
    EXPECT_EQ(scanners.front().definitions.at("alias"), "{accent}");
    EXPECT_EQ(build(scanners.front(), "INITIAL").tokenize<std::size_t>(std::string{"a"}).length, 1u);

    // A rule naming the alias reaches the definition through it, and that is where re2c compiles the definition, so
    // the refusal names the definition's own line; a longer chain reaches it the same way.
    const auto refused_at{[](const std::string_view source) {
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

    EXPECT_EQ(
            refused_at("/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\n*/\n"
                       "/*!re2c\nre2c:encoding:utf8 = 1;\nalias { return A; }\n*/\n"),
            2);
    EXPECT_EQ(
            refused_at("/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\nfurther = alias;\n*/\n"
                       "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] further { return A; }\n*/\n"),
            2);

    // Braces inside a bracket or a quoted literal are text and reach nothing: re2c 3.1 compiles this file and scans
    // "{" and "a" as the first rule's, the eight bytes of "{accent}" as the second's.
    const auto text{
            read_re2c("/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[{accent}] { return 1; }\n\"{accent}\" { return 2; }\n*/\n")};

    ASSERT_EQ(text.size(), 1u);
    EXPECT_EQ(text.front().rules.size(), 2u);
    EXPECT_EQ(
            build(text.front(), "INITIAL").tokenize<std::size_t>(std::string{"{"}).token,
            std::optional<std::size_t>{0});
    EXPECT_EQ(
            build(text.front(), "INITIAL").tokenize<std::size_t>(std::string{"{accent}"}).token,
            std::optional<std::size_t>{1});
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
    EXPECT_EQ(line_of("/*!use:re2c\n [a-z] { return X; }\n*/"), 1);

    // The encodings whose code unit is not a byte, a byte beyond ASCII in the source under UTF-8, and an encoding
    // policy that leaves the surrogates elsewhere.
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf16 = 1;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:flags:unicode = 1;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:ebcdic = 1;\n [a-z] { return X; }\n*/"), 2);

    // EBCDIC has a byte per code point, as ASCII has, so the reason it is refused is the mapping and not the width;
    // and an encoding the caller passes rather than a configuration is refused the same way, at the file's head.
    const auto refusal{[](const std::string_view source, const Re2c_flags& flags) {
        try
        {
            std::ignore = read_re2c(source, flags);
        }
        catch (const Spec_error& error)
        {
            return std::pair{std::string{error.what()}, static_cast<long>(error.line())};
        }

        return std::pair{std::string{}, -1L};
    }};

    EXPECT_NE(
            refusal("/*!re2c\n re2c:encoding:ebcdic = 1;\n [a] { return X; }\n*/", {}).first.find("another code point"),
            std::string::npos);

    const auto passed{refusal("/*!re2c\n [a] { return X; }\n*/", {.encoding = Re2c_encoding::ucs2})};

    EXPECT_NE(passed.first.find("not one byte"), std::string::npos);
    EXPECT_EQ(passed.second, 1);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n \"caf\xc3\xa9\" { return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n [\xc3\xa9] { return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n re2c:encoding-policy = fail;\n [a] { return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n !include \"other.re\";\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\u00e9\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\u00e9] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\\\u\" [0-9a-fA-F]{4} { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n [\\\\u] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n '\\X00e9' { return X; }\n*/"), 2);

    // re2c's hexadecimal escape takes no braces: re2c 3.1 answers `\x{...}` with a syntax error in a hexadecimal
    // escape sequence, in a class and in a literal alike, while two digits after the `x`, and a digit after those,
    // are forms of its own that it compiles.
    EXPECT_EQ(line_of("/*!re2c\n [\\x{100}] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\x{ff}] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\x{41}\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\xff] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n [\\x100] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a-z]+\n*/"), 3);
}

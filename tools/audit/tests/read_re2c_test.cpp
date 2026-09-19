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
        re2c:define:YYCTYPE = char; re2c:eof = 0;  // two configurations
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
        <*> $                        { return END; }
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

    ASSERT_EQ(spec.options.size(), 2u);
    EXPECT_EQ(spec.options.front(), "define:YYCTYPE=char");
    EXPECT_EQ(spec.options.back(), "eof=0");

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
    // A `<*>` rule stands in every condition the rules name, as re2c appends it to each.
    EXPECT_EQ(spec.rules[5].conditions, (std::vector<std::string>{"INITIAL", "COMMENT"}));

    // The default rule is the token its action returns, over one byte, and stands last whatever its line.
    EXPECT_EQ(spec.rules[6].pattern, "*");
    EXPECT_EQ(spec.rules[6].expression, R"([\x00-\xff])");
    EXPECT_EQ(spec.rules[6].conditions, (std::vector<std::string>{"INITIAL"}));
    EXPECT_EQ(spec.rules[6].token, std::optional<std::string>{"ERROR"});
    EXPECT_EQ(spec.rules[6].line, 15u);

    // The conditions are the ones the rules name, `*` aside, INITIAL inclusive as everywhere and the rest exclusive.
    ASSERT_EQ(spec.conditions.size(), 2u);
    EXPECT_EQ(spec.conditions[0].name, "INITIAL");
    EXPECT_FALSE(spec.conditions[0].exclusive);
    EXPECT_EQ(spec.conditions[1].name, "COMMENT");
    EXPECT_TRUE(spec.conditions[1].exclusive);
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

TEST(Read_re2c, A_condition_is_the_scanner_s_when_any_rule_names_it_and_INITIAL_is_none_of_a_conditioned_block_s)
{
    // The empty rule names the only condition, and the `<*>` rule stands in it and nowhere else: re2c compiles the
    // one condition A, so INITIAL, the name the default condition is reported under, is no condition of this scanner.
    const auto scanners{read_re2c("/*!re2c\n<A> \"\" { return 0; }\n<*> \"a\"+ { return 1; }\n*/\n")};

    ASSERT_EQ(scanners.size(), 1u);

    const auto& spec{scanners.front()};

    ASSERT_EQ(spec.conditions.size(), 1u);
    EXPECT_EQ(spec.conditions.front().name, "A");
    ASSERT_EQ(spec.rules.size(), 1u);
    EXPECT_EQ(spec.rules.front().conditions, (std::vector<std::string>{"A"}));
    EXPECT_EQ(active_rules(spec, "A"), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(active_rules(spec, "INITIAL").empty());

    // `<*>` default and end rules stand in every named condition and in no other; the `<*> $` rule is the end rule
    // of each, which re2c:eof asks of each.
    const auto ended{
            read_re2c("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<*> * { return -1; }\n<*> $ { return 0; "
                      "}\n<B> \"b\" { return 2; }\n*/\n")};

    ASSERT_EQ(ended.size(), 1u);
    ASSERT_EQ(ended.front().conditions.size(), 2u);
    EXPECT_EQ(ended.front().conditions[0].name, "A");
    EXPECT_EQ(ended.front().conditions[1].name, "B");
    EXPECT_EQ(active_rules(ended.front(), "A"), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(active_rules(ended.front(), "B"), (std::vector<std::size_t>{1, 2}));
    EXPECT_TRUE(active_rules(ended.front(), "INITIAL").empty());

    // re2c's own checks of the end rule, in its words: an end rule whose name has no other rule, `<*>` counted as
    // a name of its own, an end rule without re2c:eof, and re2c:eof without an end rule in some condition or in a
    // block naming none.
    const auto refusal_of{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source);
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<B> $ { return 0; }\n*/\n"),
            "line 4: EOF rule in condition 'B' without other rules doesn't make sense");
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<*> $ { return 0; }\n*/\n"),
            "line 4: EOF rule in condition '*' without other rules doesn't make sense");
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 0;\n$ { return 0; }\n*/\n"),
            "line 3: EOF rule without other rules doesn't make sense");
    EXPECT_EQ(
            refusal_of("/*!re2c\n<A> \"a\"+ { return 1; }\n<A> $ { return 0; }\n*/\n"),
            "line 3: in condition 'A' $ rule found, but 're2c:eof' configuration is not set");
    EXPECT_EQ(
            refusal_of("/*!re2c\n\"a\"+ { return 1; }\n$ { return 0; }\n*/\n"),
            "line 3: $ rule found, but 're2c:eof' configuration is not set");
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 0;\n\"a\"+ { return 1; }\n*/\n"),
            "line 1: 're2c:eof' configuration is set, but no $ rule found");
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<A> $ { return 0; }\n<B> \"b\" { return 2; "
                       "}\n*/\n"),
            "line 1: in condition 'B' 're2c:eof' configuration is set, but no $ rule found");
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:eof = -1;\n\"a\"+ { return 1; }\n*/\n").empty());

    // The checks hold where re2c holds them: a block whose rules are no tokens is held once it is read, where it
    // declares no scanner; a rules block is held only where a use block takes it up, with the rules and the
    // configuration that block supplies, and an unused one is held to nothing.
    EXPECT_EQ(
            refusal_of("/*!re2c\n\"\" { return 2; }\n$ { return 0; }\n*/\n/*!re2c\n\"a\" { return 1; }\n*/\n"),
            "line 3: $ rule found, but 're2c:eof' configuration is not set");
    const auto reused{
            read_re2c("/*!rules:re2c\n$ { return 0; }\n*/\n/*!use:re2c\nre2c:eof = 0;\n\"a\" { return 1; }\n*/\n")};
    ASSERT_EQ(reused.size(), 1u);
    ASSERT_EQ(reused.front().rules.size(), 1u);
    EXPECT_EQ(reused.front().rules.front().token, std::optional<std::string>{"1"});
    EXPECT_TRUE(read_re2c("/*!rules:re2c\n$ { return 0; }\n*/\n").empty());

    // A rule naming INITIAL names the default condition, which the scanner then has, inclusive, whether or not the
    // rule is a token: `<INITIAL> ""` alone leaves INITIAL a condition with no token rule.
    const auto initial{read_re2c("/*!re2c\n<INITIAL> \"\" { return 0; }\n<A> \"a\" { return 1; }\n*/\n").front()};
    ASSERT_EQ(initial.conditions.size(), 2u);
    EXPECT_EQ(initial.conditions[0].name, "INITIAL");
    EXPECT_FALSE(initial.conditions[0].exclusive);
    EXPECT_TRUE(active_rules(initial, "INITIAL").empty());
    EXPECT_EQ(active_rules(initial, "A"), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(refusal_of("/*!re2c\n<A> \"\" { return 0; }\n<C> \"c\" { return 3; }\n*/\n").empty());

    // Rules naming only `<*>` have no condition to stand in, so the block is refused at the first of them.
    EXPECT_THROW(
            {
                try
                {
                    std::ignore = read_re2c("/*!re2c\n<*> \"a\"+ { return 1; }\n*/\n");
                }
                catch (const Spec_error& error)
                {
                    EXPECT_EQ(error.line(), 2u);
                    EXPECT_TRUE(std::string_view{error.what()}.contains("names `<*>` where no rule"));
                    throw;
                }
            },
            Spec_error);
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
        EXPECT_EQ(scanners.front().rules[3].conditions, (std::vector<std::string>{"a", "b"}));
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
        EXPECT_EQ(scanners.front().rules[4].conditions, (std::vector<std::string>{"a", "b"}));
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

    // The entry rule `<>`, which re2c 3.1 accepts under -c with no regex and runs in the condition it numbers zero,
    // is no token either, blanks inside the brackets or not; a setup or entry rule whose code returns is refused,
    // since re2c runs the code before any rule's own action.
    EXPECT_EQ(refused_at("/*!re2c\n<> { setup(); }\n<a> \"x\" { return 1; }\n*/\n").first, -1);
    EXPECT_EQ(refused_at("/*!re2c\n< > { setup(); }\n<a> \"x\" { return 1; }\n*/\n").first, -1);
    EXPECT_EQ(refused_at("/*!re2c\n<a> \"x\" { return 1; }\n<!a> { return 7; }\n*/\n").first, 3);
    EXPECT_EQ(refused_at("/*!re2c\n<> { return 7; }\n<a> \"x\" { return 1; }\n*/\n").first, 2);
}

TEST(Read_re2c, An_action_that_moves_a_scan_pointer_is_refused_by_the_pointers_name)
{
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

    // An action that moves a scan pointer leaves the next token beginning elsewhere than where its match ended: a
    // scanner re2c 3.1 builds from `"a" { ++YYCURSOR; ... }` reports token 1 spanning two bytes on "abx", so the
    // rule's match is not the scanner's token, and the action is refused by the pointer's name. A pointer only
    // read, the `YYCURSOR - SCNG(yy_text)` of a length, moves nothing and is read as any action is.
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { ++YYCURSOR; return 1; }\n*/\n").first, 2);
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { YYCURSOR = p; return 1; }\n*/\n").first, 2);
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { YYMARKER -= 1; return 1; }\n*/\n").first, 2);
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { n = YYCURSOR - start; return 1; }\n*/\n").first, -1);
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { if (YYCURSOR == end) return 1; return 1; }\n*/\n").first, -1);
    EXPECT_EQ(refused_at("/*!re2c\n\"y\" { return 2; }\n* { return 3; }\n*/\n").first, -1);

    // The names are the block's: `re2c:define:YYCURSOR = cur;` renames the pointer and the actions then move
    // `cur`, which re2c 3.1 compiles to the scanner the unrenamed spelling compiles to, so the rename is followed.
    EXPECT_EQ(refused_at("/*!re2c\nre2c:define:YYCURSOR = cur;\n\"a\" { ++cur; return 1; }\n*/\n").first, 3);
    EXPECT_EQ(refused_at("/*!re2c\nre2c:define:YYCURSOR = cur;\n\"a\" { n = cur - p; return 1; }\n*/\n").first, -1);

    // `++` and `--` are written adjacent; `1 - -YYCURSOR[0]` is a difference of a negation and moves nothing.
    EXPECT_EQ(refused_at("/*!re2c\n\"a\" { n = 1 - -YYCURSOR[0]; return 1; }\n*/\n").first, -1);
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
    // PHP wraps its returns in macros and ninja stores the token and breaks out; neither is a `return`, and an
    // action ending in a call the caller has not named as a return falls into the next rule's action under re2c,
    // so the scanner is refused until the caller names the forms; a whitespace rule leaves by `continue`.
    constexpr std::string_view source{R"(/*!re2c
        "exit"   { RETURN_TOKEN_WITH_IDENT(T_EXIT); }
        "{"      { enter_nesting('{'); RETURN_TOKEN('{'); }
        "?>"     { RETURN_END_TOKEN; }
        [ \t]+   { continue; }
        "build"  { token = BUILD; break; }
        [^]      { continue; }
    */
)"};

    try
    {
        std::ignore = read_re2c(source);

        FAIL() << "an action ending in an unnamed call must be refused";
    }
    catch (const Spec_error& error)
    {
        EXPECT_EQ(error.line(), 2u);
        EXPECT_NE(std::string_view{error.what()}.find("ends without returning or leaving"), std::string_view::npos);
    }

    const auto named{
            read_re2c(source, {}, {"RETURN_TOKEN", "RETURN_TOKEN_WITH_IDENT", "RETURN_END_TOKEN", "token"}).front()};

    EXPECT_EQ(named.rules[0].token, std::optional<std::string>{"T_EXIT"});
    EXPECT_EQ(named.rules[1].token, std::optional<std::string>{"'{'"});
    EXPECT_EQ(named.rules[2].token, std::optional<std::string>{"RETURN_END_TOKEN"});
    EXPECT_FALSE(named.rules[3].token.has_value()); // the whitespace rule leaves by `continue` and discards
    EXPECT_EQ(named.rules[4].token, std::optional<std::string>{"BUILD"});
    EXPECT_FALSE(named.rules[5].token.has_value());
}

TEST(Read_re2c, Whether_an_action_returns_is_read_past_its_comments_and_literals)
{
    // re2c 3.1 on "a\nb" returns 1, 2 and 1 for the first scanner, the whitespace rule returning 2 whatever its
    // comment says, and 1 and 1 for the second, whose whitespace rule holds its return in a literal and restarts;
    // so the first scanner has no discarded token and the newline certifies neither exactly nor modulo discarded
    // tokens, while the second discards its whitespace and the newline certifies modulo the discarded tokens.
    constexpr std::string_view commented{R"(/*!re2c
        [ \t\n]+  { /* return; */ return 2; }
        [ab]      { return 1; }
        *         { return 9; }
    */
)"};

    constexpr std::string_view quoted{R"(restart:
/*!re2c
        [ \t\n]+  { const char *m = "return 2;"; (void)m; goto restart; }
        [ab]      { return 1; }
        *         { return 9; }
    */
)"};

    const auto with_comment{read_re2c(commented).front()};

    ASSERT_EQ(with_comment.rules.size(), 3u);
    EXPECT_EQ(with_comment.rules[0].token, std::optional<std::string>{"2"});
    EXPECT_FALSE(token_set(with_comment, "INITIAL").rules[0].discarded);

    const auto commented_lexer{build(with_comment, "INITIAL")};

    EXPECT_FALSE(commented_lexer.is_split_point('\n'));
    EXPECT_FALSE(commented_lexer.is_split_point_ignoring('\n'));

    const auto with_literal{read_re2c(quoted).front()};

    ASSERT_EQ(with_literal.rules.size(), 3u);
    EXPECT_FALSE(with_literal.rules[0].token.has_value());
    EXPECT_TRUE(token_set(with_literal, "INITIAL").rules[0].discarded);

    const auto quoted_lexer{build(with_literal, "INITIAL")};

    EXPECT_FALSE(quoted_lexer.is_split_point('\n'));
    EXPECT_TRUE(quoted_lexer.is_split_point_ignoring('\n'));
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

TEST(Read_re2c, A_non_capturing_group_is_its_body_and_a_backwards_range_spans_its_members)
{
    // re2c 3.1 reads `(![a-z])+` as `[a-z]+`, the `!` after the opening marking a group that captures nothing, and
    // `[Z-A]` as `[A-Z]`: its generated scanner returns 1 over "a", 2 over "!" and 3 over "ABZ". Read byte by byte
    // the mark would be a member of the group, the rule matching "!a" and not "a", and the range refused.
    constexpr std::string_view source{R"(/*!re2c
    re2c:define:YYCTYPE = char;
    (![a-z])+ { return 1; }
    [!] { return 2; }
    [Z-A]+ { return 3; }
    * { return 9; }
*/
)"};

    const auto file{read_re2c(source).front()};

    ASSERT_EQ(file.rules.size(), 4u);
    EXPECT_EQ(file.rules[0].pattern, "(![a-z])+");

    // The mark may stand after blanks, `( ![a-z])+`, which re2c 3.1 compiles to the same scanner as `(![a-z])+`;
    // read as a member, the `!` had made the rule match a bang.
    for (const std::string_view spelling : {"( ![a-z])+", "( /* c */ ![a-z])+", "(\n![a-z])+"})
    {
        EXPECT_EQ(
                read_re2c("/*!re2c\n" + std::string{spelling} + " { return 1; }\n*/\n").front().rules[0].expression,
                read_re2c("/*!re2c\n(![a-z])+ { return 1; }\n*/\n").front().rules[0].expression)
                << spelling;
    }

    // Inside a case-insensitive literal re2c folds the letter an escape spells as it folds a bare one: '\\Ab',
    // '\\x41b' and '\\101b' compile to scanners matching ab, aB, Ab and AB, where the escaped letter had stayed exact.
    for (const std::string_view spelling : {R"('\Ab')", R"('\x41b')", R"('\101b')"})
    {
        EXPECT_EQ(
                read_re2c("/*!re2c\n" + std::string{spelling} + " { return 1; }\n*/\n").front().rules[0].expression,
                "[aA][bB]")
                << spelling;
    }
    EXPECT_EQ(file.rules[0].expression, "([a-z])+");
    EXPECT_EQ(file.rules[2].expression, "[Z-A]+");

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"a"}).token, std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"a"}).length, 1u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"!a"}).token, std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"!a"}).length, 1u);
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"ABZ"}).token, std::optional<std::size_t>{2});
    EXPECT_EQ(lexer.tokenize<std::size_t>(std::string{"ABZ"}).length, 3u);
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

TEST(Read_re2c, An_empty_class_difference_is_refused_under_the_configuration_the_block_leaves_and_no_other)
{
    // What the rule taking an input returns, and how many bytes it takes.
    const auto scanned{[](const Lexer_spec& spec, const std::string_view input) {
        const auto match{build(spec, "INITIAL").tokenize<std::size_t>(std::string{input})};

        return std::pair{
                match.token ? spec.rules[*match.token].token.value_or("none") : std::string{"none"}, match.length};
    }};

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

    const std::string wide{"\xc4\x80"};

    // Which code points the operands hold is the configuration's to say: re2c 3.1 scans U+0100, the bytes C4 80, as 1
    // over both under `[^] \ [\x00-\xff]` once the block turns UTF-8 on, wherever in the block the configuration
    // stands, since `[^]` is then every code point and the bytes are subtracted from it, and a as 2; reading the rule
    // first under ASCII, where the difference is empty, refused the block for what its configuration undoes.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\xff] { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 1u);
    EXPECT_EQ(
            utf8.front().rules[0].expression,
            R"(([\u{100}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanned(utf8.front(), wide), (std::pair{std::string{"1"}, std::size_t{2}}));
    EXPECT_EQ(scanned(utf8.front(), "a"), (std::pair{std::string{"2"}, std::size_t{1}}));

    const auto below{
            read_re2c("/*!re2c\n[^] \\ [\\x00-\\xff] { return 1; }\nre2c:encoding:utf8 = 1;\n* { return 2; }\n*/\n")};

    ASSERT_EQ(below.size(), 1u);
    EXPECT_EQ(scanned(below.front(), wide), (std::pair{std::string{"1"}, std::size_t{2}}));

    // The case flags the same way: under `re2c:case-inverted = 1` the double-quoted literal folds and the
    // single-quoted one is exact, so `"a" \ 'A'` is the exact a, which re2c 3.1 scans as 1 and A as 2; read under
    // the flags inherited, where the quotes fold the other way about, the difference was empty.
    const auto inverted{
            read_re2c("/*!re2c\nre2c:case-inverted = 1;\n\"a\" \\ 'A' { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(inverted.size(), 1u);
    EXPECT_EQ(inverted.front().rules[0].expression, "[a]");
    EXPECT_EQ(scanned(inverted.front(), "a"), (std::pair{std::string{"1"}, std::size_t{1}}));
    EXPECT_EQ(scanned(inverted.front(), "A"), (std::pair{std::string{"2"}, std::size_t{1}}));

    // A rules block and a block of definitions alone are compiled where they are used, under the flags there, so
    // re2c 3.1 scans U+0100 as 1 through either from a block that turns UTF-8 on, and the difference is refused at
    // its own line where a block under ASCII reaches it.
    const auto library{
            read_re2c("/*!rules:re2c:base\nwide = [^] \\ [\\x00-\\xff];\nwide { return 1; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n!use:base;\n* { return 2; }\n*/\n")};

    ASSERT_EQ(library.size(), 1u);
    EXPECT_EQ(scanned(library.front(), wide), (std::pair{std::string{"1"}, std::size_t{2}}));

    const auto carried{
            read_re2c("/*!re2c\nwide = [^] \\ [\\x00-\\xff];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\nwide { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(carried.size(), 1u);
    EXPECT_EQ(scanned(carried.front(), wide), (std::pair{std::string{"1"}, std::size_t{2}}));

    EXPECT_EQ(
            refused_at("/*!re2c\nwide = [^] \\ [\\x00-\\xff];\n*/\n"
                       "/*!re2c\nwide { return 1; }\n* { return 2; }\n*/\n"),
            2);
    EXPECT_EQ(
            refused_at("/*!rules:re2c:base\nwide = [^] \\ [\\x00-\\xff];\nwide { return 1; }\n*/\n"
                       "/*!re2c\n!use:base;\n* { return 2; }\n*/\n"),
            2);

    // A difference the settled configuration leaves empty is refused at its rule's line, as an empty class is,
    // whether the configuration is the default one, the block's own or one inverted back: re2c 3.1 compiles each
    // into a rule that matches nothing, which the reading has no pattern for.
    EXPECT_EQ(refused_at("/*!re2c\n[^] \\ [\\x00-\\xff] { return 1; }\n* { return 2; }\n*/\n"), 2);
    EXPECT_EQ(refused_at("/*!re2c\nre2c:case-inverted = 1;\n'a' \\ \"A\" { return 1; }\n* { return 2; }\n*/\n"), 3);
    EXPECT_EQ(
            refused_at("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\xff] { return 1; }\nre2c:encoding:utf8 = "
                       "0;\n* { return 2; }\n*/\n"),
            3);
    EXPECT_EQ(
            refused_at("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 3; }\n[^] \\ [\\x00-\\xff] { return 1; }\n"
                       "re2c:encoding:utf8 = 0;\n[b] \\ [b] { return 4; }\n*/\n"),
            4);
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

TEST(Read_re2c, An_action_emits_a_token_only_where_every_path_returns_it_and_a_macro_is_its_value_in_a_pointer)
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

    // re2c 3.1 with gcc 13 on `"a" { if (flag) return 7; return 8; }` returns 7 for "a" under flag and 8 without,
    // and `"a"+ { if (flag) return 7; }` falls through without flag, so neither emits a token the text decides;
    // both branches returning the one token does.
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { if (flag) return 7; return 8; }\n\"b\" { return 9; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\"+ { if (flag) return 7; }\n\"b\" { return 9; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { if (flag) return 7; else return 7; }\n\"b\" { return 9; }\n*/\n"), -1);

    // A transparent macro is its value before the compiler compares anything: with `#define SLOT 0` the scanner
    // re2c 3.1 builds from `re2c:define:YYCURSOR = "cursors[0]";` and `"a" { ++cursors[SLOT]; ... }` leaves the
    // cursor two bytes on from the start of "ab", so the action moves the configured pointer whichever side spells
    // the index through the macro; `#define SLOT 1` names another slot and moves nothing of the scanner's.
    constexpr std::string_view block{
            "/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n"
            "\"a\" { ++cursors[SLOT]; return 7; }\n\"b\" { return 8; }\n*/\n"};

    EXPECT_EQ(line_of("#define SLOT 0\n" + std::string{block}), 5);
    EXPECT_EQ(line_of("#define SLOT 1\n" + std::string{block}), -1);
    EXPECT_EQ(line_of("#define SLOT 0U\n" + std::string{block}), 5);
    EXPECT_EQ(line_of("#define SLOT 0x0\n" + std::string{block}), 5);

    // A macro defined more than once stands for each of its values in turn, since which definition is live at the
    // action is not decided here: with `#define SLOT 1` after the block, gcc 13 still expands the action's SLOT to
    // 0 and the scanner moves its cursor; a function-like macro whose replacement is a value stands for it whatever
    // its arguments; and a pointer named through an opaque macro, `#define CUR cursors[0]` with
    // `re2c:define:YYCURSOR = "CUR";`, is out of sight, the action `++cursors[0]` moving it unseen.
    EXPECT_EQ(line_of("#define SLOT 0\n" + std::string{block} + "#define SLOT 1\n"), 5);
    EXPECT_EQ(line_of("#define SLOT 1\n" + std::string{block} + "#define SLOT 0\n"), 5);
    EXPECT_EQ(
            line_of("#define SLOT() 0\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n"
                    "\"a\" { ++cursors[SLOT()]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);
    EXPECT_EQ(
            line_of("#define CUR cursors[0]\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"CUR\";\n"
                    "\"a\" { ++cursors[0]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);

    // An integer literal is its value whatever its spelling: `0U`, `0x0`, `00` and `0'0` index the slot `0` does,
    // and the scanner built from each moves its cursor past the b of "ab"; `1U` is another slot.
    const auto why_of{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source);
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    for (const std::string_view index : {"0U", "0x0", "00", "0'0", "0'0'0", "0b0", "0ULL", "(0)", "((0U))"})
    {
        EXPECT_NE(
                why_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n\"a\" { "
                       "++cursors[" +
                       std::string{index} + "]; return 7; }\n\"b\" { return 8; }\n*/\n")
                        .find("line 4: the action moves cursors[0]"),
                std::string::npos)
                << index;
    }

    EXPECT_EQ(
            line_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n\"a\" { "
                    "++cursors[1U]; "
                    "return 7; }\n\"b\" { return 8; }\n*/\n"),
            -1);

    // An index into the array a pointer is configured in is read to its value when it is a constant expression,
    // `+0`, `1-1` and `2-1` among them, and is out of sight otherwise, `cursors[i]` standing for any slot and
    // `cursors[SLOT]` under `constexpr unsigned SLOT = 0` or a function-like `#define SLOT() 0` not invoked alike,
    // in the action and in the configured spelling; the scanner built from each of the first three moves its cursor
    // past the b of "ab".
    const auto indexed{
            [](const std::string_view head, const std::string_view configured, const std::string_view index) {
                return std::string{head} + "/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"" +
                       std::string{configured} + "\";\n\"a\" { ++cursors[" + std::string{index} +
                       "]; return 7; }\n\"b\" { return 8; }\n*/\n";
            }};

    EXPECT_NE(why_of(indexed("", "cursors[0]", "+0")).find("moves cursors[0]"), std::string::npos);
    EXPECT_NE(why_of(indexed("", "cursors[0]", "1-1")).find("moves cursors[0]"), std::string::npos);
    EXPECT_NE(
            why_of(indexed("", "cursors[0]", "(1 << 1) - 2")).find("indexes cursors by an expression"),
            std::string::npos);
    EXPECT_EQ(why_of(indexed("", "cursors[0]", "2-1")), "");
    EXPECT_NE(why_of(indexed("", "cursors[0]", "i")).find("indexes cursors by an expression"), std::string::npos);
    EXPECT_NE(
            why_of(indexed("constexpr unsigned SLOT = 0;\n", "cursors[SLOT]", "0"))
                    .find("indexes cursors by an expression"),
            std::string::npos);
    EXPECT_NE(
            why_of(indexed("#define SLOT() 0\nconstexpr unsigned SLOT = 1;\n", "cursors[SLOT]", "1"))
                    .find("indexes cursors by an expression"),
            std::string::npos);

    // The pointer handed on rather than moved here is out of sight: a reference bound to it, its address taken, or
    // the pointer passed to a call, which may take it by reference; re2c 3.1 with `auto& cursor = cursors[0];
    // ++cursor;` moves the cursor past the b of "ab". A cast's parentheses pass nothing.
    const auto acting{[](const std::string_view action) {
        return "/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n\"a\" { " +
               std::string{action} + " }\n\"b\" { return 8; }\n*/\n";
    }};

    EXPECT_NE(
            why_of(acting("auto& cursor = cursors[0]; ++cursor; return 7;")).find("hands on cursors[0]"),
            std::string::npos);
    EXPECT_NE(why_of(acting("auto* p = &cursors[0]; ++*p; return 7;")).find("hands on cursors[0]"), std::string::npos);
    EXPECT_NE(why_of(acting("advance(cursors[0]); return 7;")).find("hands on cursors[0]"), std::string::npos);
    EXPECT_NE(why_of(acting("(*step)(cursors[0]); return 7;")).find("hands on cursors[0]"), std::string::npos);
    EXPECT_EQ(why_of(acting("int n = (int)(cursors[0] - start); return n;")), "");

    // A reference bound with braces, `auto& cursor{cursors[0]}`, and the pointer passed inside a grouping,
    // `std::advance((cursors[0]), 1)`, hand it on as the `=` binding and the bare argument do; re2c 3.1 moves the
    // cursor past the b of "ab" under each. An array reached through a member, `in->cursors` of the configured
    // `in->cursors[0]`, is indexed as `cursors` is: `in->cursors[1-1]` moves the pointer and `in->cursors[i]` is
    // out of sight.
    EXPECT_NE(
            why_of(acting("auto& cursor{cursors[0]}; ++cursor; return 7;")).find("hands on cursors[0]"),
            std::string::npos);
    EXPECT_NE(
            why_of(acting("std::advance((cursors[0]), 1); return 7;")).find("hands on cursors[0]"), std::string::npos);
    EXPECT_NE(
            why_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"in->cursors[0]\";\n\"a\" { "
                   "++in->cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                    .find("moves in->cursors[0]"),
            std::string::npos);
    EXPECT_NE(
            why_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"in->cursors[0]\";\n\"a\" { "
                   "++in->cursors[i]; return 7; }\n\"b\" { return 8; }\n*/\n")
                    .find("indexes in->cursors by an expression"),
            std::string::npos);
    EXPECT_EQ(why_of(acting("long n = (const char*)(cursors[0]) - start; return (int)n;")), "");

    // A `break` in a re2c action leaves the loop the file wrote around the scanner, which is out of sight, where
    // flex's `break` discards; a `continue` restarts the scan and discards, and so does a `goto` to a label the
    // file declares before the block, `loop:`, while a `goto` to any other label is out of sight. re2c 3.1 with the
    // block inside `for (;;)` and `"a" { break; }` leaves the loop on "ab" and never scans the b.
    EXPECT_NE(why_of(acting("break;")).find("leaves by `break` the loop"), std::string::npos);
    EXPECT_EQ(why_of(acting("continue;")), "");
    EXPECT_EQ(why_of("loop:\n" + acting("goto loop;")), "");
    EXPECT_NE(why_of(acting("goto loop;")).find("leaves by `goto`"), std::string::npos);
    EXPECT_NE(
            why_of("loop:\n" + acting("if (n) goto loop; return 7;")).find("leaves by `goto loop`"), std::string::npos);

    // A label after the block is left for, not restarted at: re2c 3.1 with `"a" { goto done; }` and `done: return
    // 0;` past the scanning loop returns 0 on "ab" and never scans the b, where a label anywhere outside the blocks
    // read the rule as discarding.
    EXPECT_NE(why_of(acting("goto done;") + "done:\n").find("leaves by `goto`"), std::string::npos);

    // A label before the block restarts the scan only where nothing between the label and the block's opener
    // leaves: `emit_token: return 9;` returns 9 for "aa" under re2c 3.1 where a rescan would return 9 and 9, and
    // `loop: if (finished) return 0;` returns 0 on "ab" once the action sets the flag; each is refused. A stored
    // token followed by a restarting `goto` is rescanned over and never returned, `token = 7; goto loop;` giving 8
    // alone on "ab" under `--returns token`, so only a `break` may follow a stored token.
    EXPECT_NE(
            why_of("emit_token: return 9;\n" + acting("goto emit_token;")).find("leaves by `goto`"), std::string::npos);
    EXPECT_NE(
            why_of("loop: if (finished) return 0;\n" + acting("finished = 1; goto loop;")).find("leaves by `goto`"),
            std::string::npos);
    EXPECT_EQ(why_of("loop: tok = cursor;\n" + acting("goto loop;")), "");

    const auto stored{[](const std::string_view source) {
        try
        {
            std::ignore = read_re2c(source, {}, {"token"});
        }
        catch (const Spec_error& error)
        {
            return std::string{error.what()};
        }

        return std::string{};
    }};

    EXPECT_NE(stored("loop:\n" + acting("token = 7; goto loop;")).find("leaves by `goto loop`"), std::string::npos);
    EXPECT_EQ(stored("for (;;) {\n" + acting("token = 7; break;") + "}\n"), "");

    // re2c writes the rules' actions one after another and control falls from an action's end into the next, so an
    // action returning nowhere must leave by a jump: `"a"+ { ++count; }` before `"b" { return 8; }` returns 8 for
    // "a" under re2c 3.1, where reading it as a discard certified the b; and a `break` after a statement that is
    // no return leaves the loop as a bare `break` does. A shortcut rule, `:=> COMMENT`, and a transition rule's
    // `=> COMMENT { continue; }` leave as re2c writes them.
    EXPECT_NE(why_of(acting("++count;")).find("ends without returning or leaving"), std::string::npos);
    EXPECT_NE(why_of(acting("++count; break;")).find("leaves by `break` the loop"), std::string::npos);
    EXPECT_EQ(
            why_of("/*!re2c\nre2c:define:YYCTYPE = char;\n<A> \"a\" :=> B\n<B> \"b\" => A { continue; }\n<A> \"c\" { "
                   "return 3; }\n*/\n"),
            "");

    // The evaluator reads decimal literals and `+`, `-`, `*`, `/` and `%` alone; a shift, a bitwise operator or a
    // suffixed literal has a width and a type the reading does not model, `~0U >> 31` being 1 under gcc 13 where a
    // signed reading says 0 and `(1U << 31) << 1` being 0, so each is out of sight. A parenthesised array base,
    // `(cursors)[1-1]`, and a configured `(cursors)[0]` come down to the name before the index is read, and a run
    // with an earlier index of its own, `slots[0].cursors`, is an array too.
    EXPECT_NE(
            why_of(indexed("", "cursors[0]", "~0U >> 31")).find("indexes cursors by an expression"), std::string::npos);
    EXPECT_NE(
            why_of(indexed("", "cursors[0]", "(1U << 31) << 1")).find("indexes cursors by an expression"),
            std::string::npos);
    EXPECT_NE(why_of(acting("++(cursors)[1-1]; return 7;")).find("moves cursors[0]"), std::string::npos);
    EXPECT_NE(
            why_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"(cursors)[0]\";\n\"a\" { "
                   "++cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                    .find("moves cursors[0]"),
            std::string::npos);
    EXPECT_NE(
            why_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"slots[0].cursors[0]\";\n\"a\" { "
                   "++slots[0].cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                    .find("moves slots[0].cursors[0]"),
            std::string::npos);

    // The pointer handed on inside parentheses is handed on: `auto& cursor = (cursors[0]);` and `&(cursors[0])`
    // each moved the cursor past the b of "ab" under re2c 3.1 where the reading looked at the name's neighbours
    // alone.
    EXPECT_NE(
            why_of(acting("auto& cursor = (cursors[0]); ++cursor; return 7;")).find("hands on cursors[0]"),
            std::string::npos);
    EXPECT_NE(
            why_of(acting("auto cursor = &(cursors[0]); ++*cursor; return 7;")).find("hands on cursors[0]"),
            std::string::npos);

    // A file the code includes by a quoted name defines macros as the file's own code does, and is refused where
    // no reader reaches it.
    EXPECT_NE(
            why_of("#include \"slots.h\"\n" + acting("return 7;")).find("includes \"slots.h\", a file of its own"),
            std::string::npos);
    EXPECT_EQ(
            line_of("#define SLOT 0\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[SLOT]\";\n"
                    "\"a\" { ++cursors[0]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);
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

    // The encoding a block's configurations leave is the one it reads under, the last assignment governing, so a
    // block turning UTF-16 on and off again reads as ASCII, which re2c 3.1 compiles; one turning it on after a rule
    // is refused at the assignment, which stands anywhere in the block.
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf16 = 1;\n re2c:encoding:utf16 = 0;\n [a-z] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] { return X; }\n re2c:encoding:utf16 = 1;\n*/"), 3);

    // re2c ends the opener at a blank, a newline, a colon or the close, so `/*!re2cx` is an ill-formed start of a
    // block, which re2c 3.1 refuses; a setup rule's `!` may stand after blanks, `< ! C >` being `<!C>` to re2c.
    EXPECT_EQ(line_of("/*!re2cx\n [a] { return X; }\n*/\n/*!re2c\n [b] { return Y; }\n*/"), 1);
    EXPECT_EQ(line_of("/*!re2c\n < ! C > { setup(); }\n <C> [a] { return X; }\n*/"), -1);
    EXPECT_EQ(read_re2c("/*!re2c\n < ! C > { setup(); }\n <C> [a] { return X; }\n*/").front().rules.size(), 1u);

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

    // A setup rule's code and the entry rule's are written into the actions re2c runs them before, so a scan
    // pointer either one moves is moved before a match is handed on, and both are refused by the pointer's name as
    // a rule's own action is. The generated scanner shows the setup action inside each of its condition's actions.
    EXPECT_EQ(line_of("/*!re2c\n <!C> { ++YYCURSOR; }\n <C> [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n <> { ++YYCURSOR; }\n <C> [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n <!C> { count(); }\n <C> [a] { return X; }\n*/"), -1);

    // The name a `define:` configuration gives a pointer is the name the actions write, quoted or bare: re2c reads
    // `= "cur";` as `= cur;`, so the quotes are no part of the name and `++cur` moves the cursor either way.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = cur;\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n [a] { return cur[0]; }\n*/"), -1);

    // Parentheses around the name change nothing an operator beside them does, and a `++` written across a line
    // splice is the one operator the compiler reads, since it joins the lines before anything means anything. The
    // refusal names the rule's line, which is where the action it refuses begins.
    EXPECT_EQ(line_of("/*!re2c\n [a] { (YYCURSOR)++; return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { ++(YYCURSOR); return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { +\\\n+YYCURSOR; return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { return 1 - -YYCURSOR[0]; }\n*/"), -1);

    // re2c applies a configuration to the whole block it stands in, wherever in the block it is written, and of
    // two assignments to one name the last governs, so the name the actions write is the block's last word on it
    // and not what stood above a rule. The key stays canonical: a renamed pointer is renamed again by another
    // `define:YYCURSOR`, never by one naming the value the first gave it.
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"previous\";\n re2c:define:YYCURSOR = \"cur\";\n"
                    " [a] { ++cur; return X; }\n*/"),
            4);
    EXPECT_EQ(line_of("/*!re2c\n [a] { ++cur; return X; }\n re2c:define:YYCURSOR = \"cur\";\n*/"), 2);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"previous\";\n re2c:define:YYCURSOR = \"cur\";\n"
                    " [a] { ++previous; return X; }\n*/"),
            -1);

    // A configured name is an expression and not always one word, and a `define:` configuration carries from the
    // block it stands in to the blocks after it, so a member the first block names is the member the second
    // block's actions move.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { return in->cur[0]; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n*/\n/*!re2c\n [a] { ++cur; return X; }\n*/"), 5);

    // The reading models re2c's default API, where the scanner advances by stepping a pointer an action can be
    // seen to step too. Under the custom API it advances by calling `YYSKIP`, which names no pointer, so an action
    // calling it moves the match out of this reading's sight: such a block is refused by name. re2c 3.1 accepts
    // all four spellings below, and the default one is read as before.
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = custom;\n [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:flags:input = custom;\n [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = default;\n [a] { return X; }\n*/"), -1);

    // Which API the block reads under is its last word on it, as every configuration is, and the style names the
    // spelling of the API's operations, which decides nothing while the API is the default one: `functions` is
    // what C writes under the default API and refuses nothing.
    EXPECT_EQ(line_of("/*!re2c\n re2c:api:style = functions;\n [a] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = custom;\n re2c:api = default;\n [a] { return X; }\n*/"), -1);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = default;\n re2c:api = custom;\n [a] { return X; }\n*/"), 3);

    // A spelling of the pointer is compared in the reading's own: parentheses around one name are grouping,
    // `(*in).cur` is `in->cur`, and every other parenthesis is a call's or an index's and stays, so a pointer
    // reached through an accessor is moved by the operator after the call and `slots[(i+j)*k]` is not
    // `slots[i+(j*k)]`.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"(in)->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { ++(*in).cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cursor()\";\n [a] { in->cursor()++; return X; }\n*/"), 3);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cursor()\";\n [a] { in->cursor() = in->cursor() + 1; "
                    "}\n*/"),
            3);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"slots[(i+j)*k]\";\n [a] { ++slots[i+(j*k)]; return X; }\n*/"),
            3);

    // An imported default rule this block's own default overrides is gone with its action, and a rules library is
    // judged by the block using it, under that block's API.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n * { ++cur; return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [a] { return Y; }\n * { return Z; }\n*/"),
            -1);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n re2c:api = custom;\n [z] { return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:api = default;\n [a] { return Y; }\n*/"),
            -1);

    // An action calling a macro of the file's whose replacement returns or moves a pointer is refused by the
    // macro's name, as the flex reader refuses one; re2c copies the action as written and the compiler expands it.
    EXPECT_EQ(line_of("#define EMIT() return X\n/*!re2c\n [a] { EMIT(); }\n*/"), 3);
    EXPECT_EQ(line_of("#define STEP() ++YYCURSOR\n/*!re2c\n [a] { STEP(); return X; }\n*/"), 3);
    EXPECT_EQ(line_of("#define MAX 10\n/*!re2c\n [a] { if (n < MAX) return X; return X; }\n*/"), -1);

    // A pointer named by an expression is a run of tokens, and a macro moving it holds that run, in a replacement
    // or in an argument; the API setting carries from a used block into its user and from a block to the next, and
    // is refused where rules read under it, so a configuring block alone refuses nothing and a user that sets it
    // back reads under the default; and an overridden imported default is told from a survivor by its action and
    // not by its line alone.
    EXPECT_EQ(
            line_of("#define STEP() (++in->cur)\n/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { STEP(); return "
                    "X; }\n*/"),
            4);
    EXPECT_EQ(
            line_of("#define ADVANCE(p) ++p\n/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n"
                    " [a] { ADVANCE(in->cur); return X; }\n*/"),
            4);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n re2c:api = custom;\n [z] { return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " [a] { return Y; }\n*/"),
            2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = custom;\n*/\n/*!re2c\n re2c:api = default;\n [a] { return X; }\n*/"), -1);

    // A live imported action is judged with the block's own, wherever in the scanner's rules it stands; a group
    // inside a group comes down to the name; and a macro whose replacement is an operator alone is opaque, since
    // `#define STEP ++` makes `STEP YYCURSOR` a move.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n [a] { ++YYCURSOR; return X; }\n*/\n/*!re2c\n !use:base;\n [b] { return Y; }\n"
                    " * { return Z; }\n*/"),
            2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"((in))->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("#define STEP ++\n/*!re2c\n [a] { STEP YYCURSOR; return X; }\n*/"), 3);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n * { ++cur; return X; } [z] { return W; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [a] { return Y; }\n * { return Z; }\n*/"),
            -1);

    // A block read through a `!use:` directive is compiled where it is used, under the using block's
    // configurations, which re2c lets stand after the directive; and parentheses around the whole configured name
    // name the same pointer the actions write without them.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n [a] { ++cur; return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [b] { return Y; }\n*/"),
            2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"(cur)\";\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"((cur))\";\n [a] { ++cur; return X; }\n*/"), 3);
}

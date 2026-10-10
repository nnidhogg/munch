#include "munch/tools/audit/read_re2c.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <fstream>
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
 * @brief What a scan answers at the start of an input: the token of the rule taking it, by name, and the length.
 */
using Scanned_t = std::pair<std::string, std::size_t>;

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
 * @brief The re2c idioms in one block: configurations, definitions referring to each other by bare name, a
 *        case-insensitive literal, conditions with a transition, a `:=` action over two lines, comments, and the
 *        special rules. A block is a C comment, so a comment closer inside it is spelled `"*" "/"`, as re2c users spell
 *        it, and the rule after the `:=` action opens its line, which is where re2c ends such an action; re2c itself
 *        reads this block as the test expects.
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

/**
 * @brief Returns the text of one of the grammars beside the tests.
 * @param name The file's name.
 * @return Its text.
 */
std::string grammar(const std::string_view name)
{
    const auto path{std::format("{}/tools/audit/grammars/{}", SOURCE_DIR, name)};

    std::ifstream stream{path};

    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns the refusal of a file read under flags.
 * @param source The file's text.
 * @param flags The command line's flags.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The refusal, no line and no words when the file is read.
 */
Refusal refusal_at(const std::string_view source, const Re2c_flags flags = {}, const Returning_t& returning = {})
{
    try
    {
        std::ignore = read_re2c(source, flags, returning);
    }
    catch (const Spec_error& error)
    {
        return {.line = error.line(), .message = error.what()};
    }

    return {.line = std::nullopt, .message = {}};
}

/**
 * @brief Returns the line a file read under flags is refused at.
 * @param source The file's text.
 * @param flags The command line's flags.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The line, none when the file is read.
 */
std::optional<std::size_t> line_of(
        const std::string_view source, const Re2c_flags flags = {}, const Returning_t& returning = {})
{
    return refusal_at(source, flags, returning).line;
}

/**
 * @brief Returns what the refusal of a file read under flags says.
 * @param source The file's text.
 * @param flags The command line's flags.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The refusal, empty when the file is read.
 */
std::string refusal_of(const std::string_view source, const Re2c_flags flags = {}, const Returning_t& returning = {})
{
    return refusal_at(source, flags, returning).message;
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
 * @brief Returns what the rule taking an input returns in a condition, and how many bytes it takes.
 * @param spec The scanner.
 * @param condition The condition.
 * @param input The input.
 * @return The rule's token, `none` for a rule returning nothing or no rule, and the length.
 */
Scanned_t scanned(const Lexer_spec& spec, const std::string_view condition, const std::string_view input)
{
    const auto lexer{build(spec, condition)};

    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    if (!token)
    {
        return {"none", length};
    }

    return {spec.rules[*token].token.value_or("none"), length};
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
 * @brief Returns a block whose scan pointer is configured as `cursors[0]` and whose rule `"a"` takes an action, after
 *        the code given.
 * @param head The code before the block.
 * @param action The action's code.
 * @return The file's text.
 */
std::string acting(const std::string_view head, const std::string_view action)
{
    return std::format(
            "{}/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n\"a\" {{ {} }}\n\"b\" "
            "{{ return 8; }}\n*/\n",
            head, action);
}

} // namespace

TEST(Read_re2c_test, Reads_configurations_definitions_and_rules_with_their_dialect_rewritten)
{
    const auto scanners{read_re2c(idioms)};

    // One block with rules, so one scanner, named by the line its block opens on.
    ASSERT_EQ(scanners.size(), 1U);

    const auto& spec{scanners.front()};

    EXPECT_EQ(spec.line, 2U);

    ASSERT_EQ(spec.options.size(), 2U);
    EXPECT_EQ(spec.options.front(), "define:YYCTYPE=char");
    EXPECT_EQ(spec.options.back(), "eof=0");

    ASSERT_EQ(spec.definitions.size(), 2U);
    EXPECT_EQ(spec.definitions.at("digit"), "[0-9]");
    EXPECT_EQ(spec.definitions.at("number"), R"({digit}+("."{digit}+)?)");

    // Seven rules: the setup rule and the end rule are not tokens, and the default rule is one, placed last.
    ASSERT_EQ(spec.rules.size(), 7U);

    EXPECT_EQ(spec.rules[0].pattern, "'true' | 'false'");
    EXPECT_EQ(spec.rules[0].expression, "[tT][rR][uU][eE]|[fF][aA][lL][sS][eE]");
    EXPECT_EQ(spec.rules[0].conditions, (std::vector<std::string>{"INITIAL"}));
    EXPECT_EQ(spec.rules[0].token, std::optional<std::string>{"BOOLEAN"});
    EXPECT_EQ(spec.rules[0].line, 7U);

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
    EXPECT_EQ(spec.rules[6].line, 15U);

    // The conditions are the ones the rules name, `*` aside, INITIAL inclusive as everywhere and the rest exclusive.
    ASSERT_EQ(spec.conditions.size(), 2U);
    EXPECT_EQ(spec.conditions[0].name, "INITIAL");
    EXPECT_FALSE(spec.conditions[0].exclusive);
    EXPECT_EQ(spec.conditions[1].name, "COMMENT");
    EXPECT_TRUE(spec.conditions[1].exclusive);
    EXPECT_EQ(active_rules(spec, "COMMENT"), (std::vector<std::size_t>{3, 4, 5}));
    EXPECT_EQ(active_rules(spec, "INITIAL"), (std::vector<std::size_t>{0, 1, 2, 5, 6}));
}

TEST(Read_re2c_test, The_default_rule_is_a_token_of_one_byte_at_the_lowest_priority_wherever_it_stands)
{
    // re2c 3.1, run on this block: "a" is the default rule's token, since "ab" needs two bytes and nothing else takes
    // one "a"; "b" is the third rule's, though the default rule stands above it; "ab" is the second rule's.
    const auto ascii{read_re2c("/*!re2c\n*     { return 2; }\n\"ab\"  { return 1; }\n[b]   { return 3; }\n*/\n")};

    ASSERT_EQ(ascii.size(), 1U);

    const auto& spec{ascii.front()};

    ASSERT_EQ(spec.rules.size(), 3U);
    EXPECT_EQ(spec.rules.back().pattern, "*");
    EXPECT_EQ(spec.rules.back().line, 2U);

    // Each rule returns its own token: `*` 2, "ab" 1 and [b] 3.
    EXPECT_EQ(scanned(spec, "INITIAL", "a"), (Scanned_t{"2", 1}));
    EXPECT_EQ(scanned(spec, "INITIAL", "ab"), (Scanned_t{"1", 2}));
    EXPECT_EQ(scanned(spec, "INITIAL", "b"), (Scanned_t{"3", 1}));
    EXPECT_EQ(scanned(spec, "INITIAL", "c"), (Scanned_t{"2", 1}));

    // Under UTF-8 the default rule takes one byte where `[^]` takes a whole code point: re2c 3.1 scans the two bytes of
    // "\xc3\xa9" as two default-rule matches under the first block and as one `[^]` match under the second.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 1; }\n* { return 2; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 1; }\n[^] { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 2U);

    const auto byte_default{build(utf8[0], "INITIAL")};

    const auto whole_point{build(utf8[1], "INITIAL")};

    EXPECT_EQ(length_at(byte_default, "\xc3\xa9"), 1U);
    EXPECT_EQ(length_at(whole_point, "\xc3\xa9"), 2U);
}

TEST(Read_re2c_test, A_second_default_rule_for_a_condition_is_refused_and_a_used_one_yields_to_the_block_s_own)
{
    // re2c 3.1 answers each of these with "code to default rule in condition ... is already defined at line N": two
    // default rules with no condition, two in `<*>`, and `<a, b>` after `<a>` gave a its own.
    const auto [twice_line, twice_message]{
            refusal_at("/*!re2c\n*  { return 1; }\n[a] { return 2; }\n*  { return 3; }\n*/\n")};

    EXPECT_EQ(twice_line, 4);
    EXPECT_TRUE(twice_message.contains("already defined at line 2"));
    EXPECT_EQ(line_of("/*!re2c\n<*> * { return 1; }\n<*> * { return 2; }\n<a> \"x\" { return 3; }\n*/\n"), 3);
    EXPECT_EQ(line_of("/*!re2c\n<a> * { return 1; }\n<a, b> * { return 2; }\n<b> \"x\" { return 3; }\n*/\n"), 3);

    // `<*> *` and `<a> *` are rules of different conditions to re2c, so both stand, and a's own wins in a: re2c 3.1,
    // run with -c, scans "z" in a as 2 with the `<*> *` rule above it.
    const auto both{read_re2c("/*!re2c\n<*> * { return 1; }\n<a> * { return 2; }\n<a> \"x\" { return 3; }\n*/\n")};

    ASSERT_EQ(both.size(), 1U);

    const auto& both_spec{both.front()};

    ASSERT_EQ(both_spec.rules.size(), 3U);
    EXPECT_EQ(both_spec.rules[1].token, std::optional<std::string>{"2"});
    EXPECT_EQ(both_spec.rules[2].token, std::optional<std::string>{"1"});

    const auto in_a{build(both_spec, "a")};

    EXPECT_EQ(token_at(in_a, "z"), std::optional<std::size_t>{1});

    // A used block's default rule yields to the using block's own, wherever the two stand: re2c 3.1 scans "c" as 4 with
    // the `!use:` directive above the block's own default rule and below it alike.
    for (const auto own_first : {false, true})
    {
        const std::string_view own{own_first ? "* { return 4; }\n!use:base;\n" : "!use:base;\n* { return 4; }\n"};

        const auto block{std::format(
                "/*!rules:re2c:base\n* {{ return 1; }}\n\"ab\" {{ return 2; }}\n*/\n/*!re2c\n{}[a] {{ return 3; "
                "}}\n*/\n",
                own)};

        const auto scanners{read_re2c(block)};

        ASSERT_EQ(scanners.size(), 1U);

        const auto& spec{scanners.front()};

        ASSERT_EQ(spec.rules.size(), 3U);
        EXPECT_EQ(spec.rules.back().pattern, "*");
        EXPECT_EQ(spec.rules.back().token, std::optional<std::string>{"4"});
    }

    // With conditions the used rule yields condition by condition: re2c 3.1, run on this file with -c, scans "z" in a
    // as 2, the block's own, and in b as 1, the used rule's, which still stands there.
    const auto conditions{read_re2c(
            "/*!rules:re2c:base\n<a, b> * { return 1; }\n*/\n"
            "/*!re2c\n<a> * { return 2; }\n!use:base;\n<a> \"x\" { return 3; }\n<b> \"x\" { return 4; }\n*/\n")};

    // The default rules stand last in the order they were read, the block's own `<a> *` and then the used one, left
    // with b alone.
    ASSERT_EQ(conditions.size(), 1U);

    const auto& conditioned{conditions.front()};

    ASSERT_EQ(conditioned.rules.size(), 4U);
    EXPECT_EQ(conditioned.rules[2].conditions, (std::vector<std::string>{"a"}));
    EXPECT_EQ(conditioned.rules[2].token, std::optional<std::string>{"2"});
    EXPECT_EQ(conditioned.rules[3].conditions, (std::vector<std::string>{"b"}));
    EXPECT_EQ(conditioned.rules[3].token, std::optional<std::string>{"1"});

    const auto conditioned_a{build(conditioned, "a")};

    const auto conditioned_b{build(conditioned, "b")};

    EXPECT_EQ(token_at(conditioned_a, "z"), std::optional<std::size_t>{2});
    EXPECT_EQ(token_at(conditioned_b, "z"), std::optional<std::size_t>{3});

    // A use block's own default rule overrides the used block's the same way.
    const auto use_block{read_re2c("/*!rules:re2c:base\n* { return 1; }\n*/\n/*!use:re2c:base\n* { return 2; }\n*/\n")};

    ASSERT_EQ(use_block.size(), 1U);
    ASSERT_EQ(use_block.front().rules.size(), 1U);
    EXPECT_EQ(use_block.front().rules.front().token, std::optional<std::string>{"2"});
}

TEST(Read_re2c_test, A_condition_is_the_scanner_s_when_any_rule_names_it_and_INITIAL_is_none_of_a_conditioned_block_s)
{
    // The empty rule names the only condition, and the `<*>` rule stands in it and nowhere else: re2c compiles the one
    // condition A, so INITIAL, the name the default condition is reported under, is no condition of this scanner.
    const auto scanners{read_re2c("/*!re2c\n<A> \"\" { return 0; }\n<*> \"a\"+ { return 1; }\n*/\n")};

    ASSERT_EQ(scanners.size(), 1U);

    const auto& spec{scanners.front()};

    ASSERT_EQ(spec.conditions.size(), 1U);
    EXPECT_EQ(spec.conditions.front().name, "A");
    ASSERT_EQ(spec.rules.size(), 1U);
    EXPECT_EQ(spec.rules.front().conditions, (std::vector<std::string>{"A"}));
    EXPECT_EQ(active_rules(spec, "A"), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(active_rules(spec, "INITIAL").empty());

    // `<*>` default and end rules stand in every named condition and in no other; the `<*> $` rule is the end rule of
    // each, which re2c:eof asks of each.
    const auto ended{
            read_re2c("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<*> * { return -1; }\n"
                      "<*> $ { return 0; }\n<B> \"b\" { return 2; }\n*/\n")};

    ASSERT_EQ(ended.size(), 1U);

    const auto& ended_spec{ended.front()};

    ASSERT_EQ(ended_spec.conditions.size(), 2U);
    EXPECT_EQ(ended_spec.conditions[0].name, "A");
    EXPECT_EQ(ended_spec.conditions[1].name, "B");
    EXPECT_EQ(active_rules(ended_spec, "A"), (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(active_rules(ended_spec, "B"), (std::vector<std::size_t>{1, 2}));
    EXPECT_TRUE(active_rules(ended_spec, "INITIAL").empty());

    // re2c's own checks of the end rule, in its words: an end rule whose name has no other rule, `<*>` counted as a
    // name of its own, an end rule without re2c:eof, and re2c:eof without an end rule in some condition or in a block
    // naming none.
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
            refusal_of("/*!re2c\nre2c:eof = 0;\n<A> \"a\"+ { return 1; }\n<A> $ { return 0; }\n"
                       "<B> \"b\" { return 2; }\n*/\n"),
            "line 1: in condition 'B' 're2c:eof' configuration is set, but no $ rule found");
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:eof = -1;\n\"a\"+ { return 1; }\n*/\n").empty());

    // re2c 3.1 reads the value as a number and leaves end-of-input handling off for every negative one, as for its
    // default -1, while zero and a positive value turn it on; a value that is no number, or overflows, it refuses.
    const auto eof_of{[](const std::string_view value, const std::string_view end_rule) {
        return refusal_of(std::format("/*!re2c\nre2c:eof = {};\n\"a\"+ {{ return 1; }}\n{}*/\n", value, end_rule));
    }};

    EXPECT_TRUE(eof_of("-2", "").empty());
    EXPECT_TRUE(eof_of("-2147483648", "").empty());
    EXPECT_EQ(eof_of("-2", "$ { return 0; }\n"), "line 4: $ rule found, but 're2c:eof' configuration is not set");
    EXPECT_TRUE(eof_of("255", "$ { return 0; }\n").empty());
    EXPECT_EQ(eof_of("255", ""), "line 1: 're2c:eof' configuration is set, but no $ rule found");
    EXPECT_EQ(eof_of("abc", ""), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("-0", ""), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("007", ""), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("2147483648", ""), "line 2: configuration value overflow");

    // re2c reads each setting as a number where it is written, so a blank within the value is refused and a later
    // setting does not take back the refusal of an earlier one; blanks around the value say nothing.
    EXPECT_EQ(eof_of("- 2", ""), "line 2: bad configuration value (expected number)");
    EXPECT_TRUE(eof_of("\t-2 ", "").empty());
    EXPECT_EQ(eof_of("abc;\nre2c:eof = -1", ""), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("2147483648;\nre2c:eof = -1", ""), "line 2: configuration value overflow");
    EXPECT_TRUE(eof_of("0;\nre2c:eof = -1", "").empty());
    EXPECT_TRUE(eof_of("-1;\nre2c:eof = 0", "$ { return 0; }\n").empty());

    // The last value stands and is held to the code unit, a byte, whether or not an end rule is there; an earlier value
    // past it is taken back by a later one.
    EXPECT_EQ(eof_of("256", "$ { return 0; }\n"), "line 1: EOF exceeds maximum code unit value for given encoding");
    EXPECT_EQ(eof_of("256", ""), "line 1: EOF exceeds maximum code unit value for given encoding");
    EXPECT_TRUE(eof_of("256;\nre2c:eof = -1", "").empty());
    EXPECT_TRUE(eof_of("300;\nre2c:eof = 5", "$ { return 0; }\n").empty());

    // Around the value re2c lets a space or a tab stand, never a newline or a carriage return; and it holds a block
    // without rules to the code unit too.
    EXPECT_EQ(eof_of("-1\n", ""), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("\n 0", "$ { return 0; }\n"), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(eof_of("0\r", "$ { return 0; }\n"), "line 2: bad configuration value (expected number)");
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:eof = 256;\n*/\n/*!re2c\nre2c:eof = -1;\n\"a\"+ { return 1; }\n*/\n"),
            "line 1: EOF exceeds maximum code unit value for given encoding");

    // The checks hold where re2c holds them: a block whose rules are no tokens is held once it is read, where it
    // declares no scanner; a rules block is held only where a use block takes it up, with the rules and the
    // configuration that block supplies, and an unused one is held to nothing.
    EXPECT_EQ(
            refusal_of("/*!re2c\n\"\" { return 2; }\n$ { return 0; }\n*/\n/*!re2c\n\"a\" { return 1; }\n*/\n"),
            "line 3: $ rule found, but 're2c:eof' configuration is not set");

    const auto reused{
            read_re2c("/*!rules:re2c\n$ { return 0; }\n*/\n/*!use:re2c\nre2c:eof = 0;\n\"a\" { return 1; }\n*/\n")};

    ASSERT_EQ(reused.size(), 1U);
    ASSERT_EQ(reused.front().rules.size(), 1U);
    EXPECT_EQ(reused.front().rules.front().token, std::optional<std::string>{"1"});
    EXPECT_TRUE(read_re2c("/*!rules:re2c\n$ { return 0; }\n*/\n").empty());
    EXPECT_EQ(
            read_re2c("/*!rules:re2c\n* { return 1; }\n*/\n/*!use:re2c\nre2c:eof = 0;\n$ { return 0; }\n*/\n").size(),
            1U);
    EXPECT_EQ(
            refusal_of("/*!rules:re2c\n<B> $ { return 0; }\n*/\n/*!use:re2c\nre2c:eof = 0;\n"
                       "<*> \"a\" { return 1; }\n*/\n"),
            "line 2: EOF rule in condition 'B' without other rules doesn't make sense");

    // The same brought in by a `!use:` directive is the reading's own restriction: re2c 3.1 compiles that form.
    EXPECT_EQ(
            refusal_of("/*!rules:re2c:base\n<B> $ { return 0; }\n*/\n/*!re2c\nre2c:eof = 0;\n!use:base;\n"
                       "<*> \"a\" { return 1; }\n*/\n"),
            "line 2: EOF rule in condition 'B' without other rules doesn't make sense");

    // A rule naming INITIAL names the default condition, which the scanner then has, inclusive, whether or not the rule
    // is a token: `<INITIAL> ""` alone leaves INITIAL a condition with no token rule.
    const auto initial{read_re2c("/*!re2c\n<INITIAL> \"\" { return 0; }\n<A> \"a\" { return 1; }\n*/\n").front()};

    ASSERT_EQ(initial.conditions.size(), 2U);
    EXPECT_EQ(initial.conditions[0].name, "INITIAL");
    EXPECT_FALSE(initial.conditions[0].exclusive);
    EXPECT_TRUE(active_rules(initial, "INITIAL").empty());
    EXPECT_EQ(active_rules(initial, "A"), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(refusal_of("/*!re2c\n<A> \"\" { return 0; }\n<C> \"c\" { return 3; }\n*/\n").empty());

    // Rules naming only `<*>` have no condition to stand in, so the block is refused at the first of them.
    const auto [star_line, star_message]{refusal_at("/*!re2c\n<*> \"a\"+ { return 1; }\n*/\n")};

    EXPECT_EQ(star_line, 2U);
    EXPECT_TRUE(star_message.contains("names `<*>` where no rule"));
}

TEST(Read_re2c_test, Star_rules_rank_below_a_condition_s_own_wherever_they_stand)
{
    const auto returned{[](const Lexer_spec& spec, const std::string_view condition, const std::string_view input) {
        const auto [token, length]{scanned(spec, condition, input)};

        return token;
    }};

    // re2c 3.1, run with -c on this block, scans "x" in a as 4 and warns that the `<*>` rule above it is shadowed by
    // it, and scans "x" in b as 3; the same with the `<*>` rule written below a's rules.
    for (const auto star_first : {true, false})
    {
        const std::string star{"<*> \"x\" { return 3; }\n"};

        const std::string own{"<a> \"x\" { return 4; }\n<a> [y] { return 5; }\n"};

        const auto& first{star_first ? star : own};

        const auto& second{star_first ? own : star};

        const auto block{std::format("/*!re2c\n{}{}<b> [y] {{ return 6; }}\n*/\n", first, second)};

        const auto scanners{read_re2c(block)};

        ASSERT_EQ(scanners.size(), 1U);

        const auto& spec{scanners.front()};

        ASSERT_EQ(spec.rules.size(), 4U);
        EXPECT_EQ(spec.rules[3].pattern, R"("x")");
        EXPECT_EQ(spec.rules[3].conditions, (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(returned(spec, "a", "x"), "4");
        EXPECT_EQ(returned(spec, "b", "x"), "3");
    }

    // Among several: re2c 3.1 scans, in a, "x" as 2, "q" and "m" as 4 and "xy" as 5, and in b "x" as 6 and "q" and "m"
    // as 1, so a condition's own rules come first in their order and the `<*>` rules after them in theirs.
    const auto ties{read_re2c(
            "/*!re2c\n<*> [a-z] { return 1; }\n<a> [x] { return 2; }\n<*> [q] { return 3; }\n<a> [a-z] { return 4; }\n"
            "<*> \"xy\" { return 5; }\n<b> [x] { return 6; }\n*/\n")};

    ASSERT_EQ(ties.size(), 1U);

    const auto& ties_spec{ties.front()};

    EXPECT_EQ(returned(ties_spec, "a", "x"), "2");
    EXPECT_EQ(returned(ties_spec, "a", "q"), "4");
    EXPECT_EQ(returned(ties_spec, "a", "m"), "4");
    EXPECT_EQ(returned(ties_spec, "a", "xy"), "5");
    EXPECT_EQ(returned(ties_spec, "b", "x"), "6");
    EXPECT_EQ(returned(ties_spec, "b", "q"), "1");
    EXPECT_EQ(returned(ties_spec, "b", "m"), "1");

    // The default rules rank the same way: re2c 3.1 scans "z" in a as 2 and in b as 1 with `<*> *` above `<a> *` and
    // below it alike, while "y" is 5 in both, a `<*>` rule beating any default rule.
    for (const auto star_first : {true, false})
    {
        const std::string star{"<*> * { return 1; }\n"};

        const std::string own{
                "<a> * { return 2; }\n<a> \"x\" { return 3; }\n<b> \"x\" { return 4; }\n<*> \"y\" { return 5; }\n"};

        const auto& first{star_first ? star : own};

        const auto& second{star_first ? own : star};

        const auto block{std::format("/*!re2c\n{}{}*/\n", first, second)};

        const auto scanners{read_re2c(block)};

        ASSERT_EQ(scanners.size(), 1U);

        const auto& spec{scanners.front()};

        ASSERT_EQ(spec.rules.size(), 5U);
        EXPECT_EQ(spec.rules[3].conditions, (std::vector<std::string>{"a"}));
        EXPECT_EQ(spec.rules[4].conditions, (std::vector<std::string>{"a", "b"}));
        EXPECT_EQ(returned(spec, "a", "z"), "2");
        EXPECT_EQ(returned(spec, "b", "z"), "1");
        EXPECT_EQ(returned(spec, "a", "y"), "5");
        EXPECT_EQ(returned(spec, "b", "y"), "5");
    }

    // A used block's rules stand where the directive does and rank by their conditions like the rest: re2c 3.1 scans,
    // in a, "x" as 11, "y" as 2 and "z" and "m" as 4, and in b "x" as 1, "y" as 12, "z" as 3 and "m" as 13.
    const auto used{read_re2c(
            "/*!rules:re2c:base\n<a> \"x\" { return 11; }\n<*> \"y\" { return 12; }\n<*> [a-z] { return 13; }\n*/\n"
            "/*!re2c\n<*> \"x\" { return 1; }\n!use:base;\n<a> \"y\" { return 2; }\n<b> \"z\" { return 3; }\n"
            "<a> [a-z] { return 4; }\n*/\n")};

    ASSERT_EQ(used.size(), 1U);

    const auto& used_spec{used.front()};

    EXPECT_EQ(returned(used_spec, "a", "x"), "11");
    EXPECT_EQ(returned(used_spec, "a", "y"), "2");
    EXPECT_EQ(returned(used_spec, "a", "z"), "4");
    EXPECT_EQ(returned(used_spec, "a", "m"), "4");
    EXPECT_EQ(returned(used_spec, "b", "x"), "1");
    EXPECT_EQ(returned(used_spec, "b", "y"), "12");
    EXPECT_EQ(returned(used_spec, "b", "z"), "3");
    EXPECT_EQ(returned(used_spec, "b", "m"), "13");
}

TEST(Read_re2c_test, Rules_naming_a_condition_and_rules_naming_none_in_one_scanner_are_refused_as_re2c_refuses_them)
{
    // The line a refusal points at and its words, or no line. re2c 3.1, run with -c, answers each of these with "cannot
    // mix conditions with normal rules" at the first rule naming no condition, wherever it stands: after a `<a>` rule,
    // before one, beside a `<*>` rule, the default rule `*` and the empty rule `""` naming none, and a used block's
    // rule naming none beside the using block's `<a>`.
    const auto [after_line, after_message]{refusal_at("/*!re2c\n<a> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n")};

    const std::string mixing{"cannot mix conditions with normal rules"};

    EXPECT_EQ(after_line, 3);
    EXPECT_TRUE(after_message.contains(mixing));
    EXPECT_EQ(line_of("/*!re2c\n\"y\" { return 2; }\n<a> \"x\" { return 1; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n<*> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n"), 3);
    EXPECT_EQ(line_of("/*!re2c\n<a> \"x\" { return 1; }\n* { return 2; }\n*/\n"), 3);
    EXPECT_EQ(line_of("/*!re2c\n<a> \"x\" { return 1; }\n\"\" { return 2; }\n*/\n"), 3);

    const auto [used_line, used_message]{refusal_at(
            "/*!rules:re2c:base\n\"y\" { return 2; }\n*/\n/*!re2c\n<a> \"x\" { return 1; }\n!use:base;\n*/\n")};

    EXPECT_EQ(used_line, 2);
    EXPECT_TRUE(used_message.contains(mixing));

    // The end rule `$` is counted otherwise: alone beside `<a>` rules re2c answers "EOF rule without other rules
    // doesn't make sense" at it, and beside another rule naming none the mixing is what is refused, at that rule.
    const auto [end_line, end_message]{refusal_at("/*!re2c\n<a> \"x\" { return 1; }\n$ { return 3; }\n*/\n")};

    EXPECT_EQ(end_line, 3);
    EXPECT_TRUE(end_message.contains("EOF rule without other rules doesn't make sense"));
    EXPECT_EQ(line_of("/*!re2c\n<a> \"x\" { return 1; }\n$ { return 3; }\n\"y\" { return 2; }\n*/\n"), 4);

    // Not refused: a rules block of both kinds that no scanner uses, since re2c compiles it nowhere; a setup rule
    // `<!*>` beside `<a>` rules; and rules of one kind alone.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n<a> \"x\" { return 1; }\n\"y\" { return 2; }\n*/\n"
                    "/*!re2c\n<a> \"x\" { return 1; }\n*/\n"),
            std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n<a> \"x\" { return 1; }\n<!*> { setup(); }\n*/\n"), std::nullopt);

    // The entry rule `<>`, which re2c 3.1 accepts under -c with no regex and runs in the condition it numbers zero, is
    // no token either, blanks inside the brackets or not; a setup or entry rule whose code returns is refused, since
    // re2c runs the code before any rule's own action.
    EXPECT_EQ(line_of("/*!re2c\n<> { setup(); }\n<a> \"x\" { return 1; }\n*/\n"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n< > { setup(); }\n<a> \"x\" { return 1; }\n*/\n"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n<a> \"x\" { return 1; }\n<!a> { return 7; }\n*/\n"), 3);
    EXPECT_EQ(line_of("/*!re2c\n<> { return 7; }\n<a> \"x\" { return 1; }\n*/\n"), 2);
}

TEST(Read_re2c_test, An_action_that_moves_a_scan_pointer_is_refused_by_the_pointers_name)
{
    // An action that moves a scan pointer leaves the next token beginning elsewhere than where its match ended: a
    // scanner re2c 3.1 builds from `"a" { ++YYCURSOR; ... }` reports token 1 spanning two bytes on "abx", so the rule's
    // match is not the scanner's token, and the action is refused by the pointer's name. A pointer only read, the
    // `YYCURSOR - SCNG(yy_text)` of a length, moves nothing and is read as any action is.
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { ++YYCURSOR; return 1; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { YYCURSOR = p; return 1; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { YYMARKER -= 1; return 1; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { n = YYCURSOR - start; return 1; }\n*/\n"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { if (YYCURSOR == end) return 1; return 1; }\n*/\n"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n\"y\" { return 2; }\n* { return 3; }\n*/\n"), std::nullopt);

    // The names are the block's: `re2c:define:YYCURSOR = cur;` renames the pointer and the actions then move `cur`,
    // which re2c 3.1 compiles to the scanner the unrenamed spelling compiles to, so the rename is followed.
    EXPECT_EQ(line_of("/*!re2c\nre2c:define:YYCURSOR = cur;\n\"a\" { ++cur; return 1; }\n*/\n"), 3);
    EXPECT_EQ(line_of("/*!re2c\nre2c:define:YYCURSOR = cur;\n\"a\" { n = cur - p; return 1; }\n*/\n"), std::nullopt);

    // `++` and `--` are written adjacent; `1 - -YYCURSOR[0]` is a difference of a negation and moves nothing.
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { n = 1 - -YYCURSOR[0]; return 1; }\n*/\n"), std::nullopt);
}

TEST(Read_re2c_test, The_flags_choose_the_case_insensitive_quote_and_the_flex_syntax)
{
    // PHP's scanner shape: flex-style definitions with a bare underscore, `{name}` references, a setup rule with a `:=`
    // action, and an action whose character literal holds a brace.
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

    ASSERT_EQ(plain.rules.size(), 3U);
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

    // Set in the file, a flag holds from there on, the next block included, and a block without rules is no scanner but
    // hands its configurations on.
    const auto in_file{read_re2c("/*!re2c\nre2c:flags:case-inverted = 1;\n*/\n/*!re2c\n\"ab\" { return X; }\n*/\n")};

    ASSERT_EQ(in_file.size(), 1U);
    ASSERT_EQ(in_file.front().rules.size(), 1U);
    EXPECT_EQ(in_file.front().line, 4U);
    EXPECT_EQ(in_file.front().rules[0].expression, "[aA][bB]");
    EXPECT_EQ(in_file.front().options, (std::vector<std::string>{"flags:case-inverted=1"}));
}

TEST(Read_re2c_test, A_defined_name_opening_a_line_is_a_rule_and_an_undefined_one_a_flex_definition)
{
    // Normal syntax: a rule may open with a defined name and a blank, and stays a rule.
    const auto normal{
            read_re2c("/*!re2c\ndigit = [0-9];\ndigit+ { return N; }\ndigit \"x\" { return X; }\n*/\n").front()};

    ASSERT_EQ(normal.rules.size(), 2U);
    EXPECT_EQ(normal.rules[0].expression, "{digit}+");
    EXPECT_EQ(normal.rules[1].expression, R"({digit}"x")");
    EXPECT_EQ(normal.definitions.size(), 1U);

    // Flex syntax inferred: an undefined name opening a line with a blank and no action is a definition, a comment
    // closing on the line included in it, and a later rule opening with a bare literal is still a rule.
    const auto flex{read_re2c("/*!re2c\nD [0-9] /* digits */ +\nab { return AB; }\n{D} { return N; }\n*/\n").front()};

    ASSERT_EQ(flex.rules.size(), 2U);
    EXPECT_EQ(flex.definitions.at("D"), "[0-9]+");
    EXPECT_EQ(flex.rules[0].expression, "ab");
    EXPECT_EQ(flex.rules[1].expression, "{D}");
}

TEST(Read_re2c_test, A_flex_style_definition_stands_wherever_its_name_does_and_admits_no_action_on_its_line)
{
    constexpr Re2c_flags flex{.flex_syntax = true};

    // re2c 3.1 -F reads a name followed by a blank as a definition wherever the name stands, indented or after a rule
    // on the same line: it scans, under the first block, the byte E9 as 1 and a as 2, and under the second a as 3, b as
    // 1 and c as 4.
    const auto indented{
            read_re2c("/*!re2c\n    accent [\\xe9]\n    {accent} { return 1; }\n    * { return 2; }\n*/\n", flex)};

    ASSERT_EQ(indented.size(), 1U);

    const auto& indented_spec{indented.front()};

    EXPECT_EQ(indented_spec.definitions.at("accent"), R"([\xe9])");
    EXPECT_EQ(scanned(indented_spec, "INITIAL", "\xe9"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(indented_spec, "INITIAL", "a"), (Scanned_t{"2", 1}));

    const auto mid_line{read_re2c(
            "/*!re2c\nx [a]\n[b] { return 1; } y [c]\n{x} { return 3; }\n{y} { return 4; }\n* { return 2; }\n*/\n",
            flex)};

    ASSERT_EQ(mid_line.size(), 1U);

    const auto& mid_line_spec{mid_line.front()};

    EXPECT_EQ(scanned(mid_line_spec, "INITIAL", "a"), (Scanned_t{"3", 1}));
    EXPECT_EQ(scanned(mid_line_spec, "INITIAL", "b"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(mid_line_spec, "INITIAL", "c"), (Scanned_t{"4", 1}));

    // A `{` after the blanks opens no definition: `alias {accent}` is a rule whose regex is the literal alias and the
    // reference, running on to the next line's reference, so re2c 3.1 -F scans "alias" as five matches of the default
    // rule and "alias" followed by E9 twice as 1; and with `{alias}` on the next line the name is a symbol no
    // definition binds, which re2c answers with "undefined symbol", the reading where the scanner is built.
    const auto brace{read_re2c(
            "/*!re2c\naccent [\\xe9]\nalias   {accent}\n{accent} { return 1; }\n* { return 2; }\n*/\n", flex)};

    ASSERT_EQ(brace.size(), 1U);

    const auto& brace_spec{brace.front()};

    ASSERT_EQ(brace_spec.rules.size(), 2U);
    EXPECT_EQ(brace_spec.rules[0].expression, "alias{accent}{accent}");
    EXPECT_EQ(scanned(brace_spec, "INITIAL", "alias"), (Scanned_t{"2", 1}));
    EXPECT_EQ(scanned(brace_spec, "INITIAL", "alias\xe9\xe9"), (Scanned_t{"1", 7}));

    const auto unbound{
            read_re2c("/*!re2c\naccent [\\xe9]\nalias   {accent}\n{alias} { return 1; }\n* { return 2; }\n*/\n", flex)};

    ASSERT_EQ(unbound.size(), 1U);
    EXPECT_FALSE(unbound.front().definitions.contains("alias"));

    const auto [line, message]{build_refusal(unbound.front())};

    EXPECT_EQ(line, 3U);
    EXPECT_TRUE(message.contains("unknown definition 'alias'"));

    // An action on the definition's line is refused, as re2c -F answers it with a syntax error, the name defined before
    // or not, and a bare name after the blank opening a second definition alike; without the flag the name is besides a
    // symbol no definition binds.
    EXPECT_TRUE(refusal_of("/*!re2c\naccent [\\xe9] { return 5; }\n*/\n", flex).contains("syntax error"));
    EXPECT_TRUE(refusal_of("/*!re2c\nx [a]\nx [b] { return 3; }\n*/\n", flex).contains("syntax error"));
    EXPECT_TRUE(refusal_of("/*!re2c\na b { return 3; }\n*/\n", flex).contains("syntax error"));
    EXPECT_TRUE(refusal_of("/*!re2c\naccent [\\xe9] { return 5; }\n*/\n", {}).contains("no definition binds"));
    EXPECT_EQ(refusal_of("/*!re2c\nab { return 3; }\n*/\n", flex), "");
}

TEST(Read_re2c_test, The_forms_the_caller_names_return_tokens_as_return_does)
{
    // PHP wraps its returns in macros and ninja stores the token and breaks out; neither is a `return`, and an action
    // ending in a call the caller has not named as a return falls into the next rule's action under re2c, so the
    // scanner is refused until the caller names the forms; a whitespace rule leaves by `continue`.
    constexpr std::string_view source{R"(/*!re2c
        "exit"   { RETURN_TOKEN_WITH_IDENT(T_EXIT); }
        "{"      { enter_nesting('{'); RETURN_TOKEN('{'); }
        "?>"     { RETURN_END_TOKEN; }
        [ \t]+   { continue; }
        "build"  { token = BUILD; break; }
        [^]      { continue; }
    */
)"};

    const auto [unnamed_line, unnamed_message]{refusal_at(source)};

    EXPECT_EQ(unnamed_line, 2U);
    EXPECT_TRUE(unnamed_message.contains("ends without returning or leaving"));

    const auto named{
            read_re2c(source, {}, {"RETURN_TOKEN", "RETURN_TOKEN_WITH_IDENT", "RETURN_END_TOKEN", "token"}).front()};

    EXPECT_EQ(named.rules[0].token, std::optional<std::string>{"T_EXIT"});
    EXPECT_EQ(named.rules[1].token, std::optional<std::string>{"'{'"});
    EXPECT_EQ(named.rules[2].token, std::optional<std::string>{"RETURN_END_TOKEN"});

    // The whitespace rule leaves by `continue` and discards.
    EXPECT_FALSE(named.rules[3].token.has_value());
    EXPECT_EQ(named.rules[4].token, std::optional<std::string>{"BUILD"});
    EXPECT_FALSE(named.rules[5].token.has_value());
}

TEST(Read_re2c_test, Whether_an_action_returns_is_read_past_its_comments_and_literals)
{
    // re2c 3.1 on "a\nb" returns 1, 2 and 1 for the first scanner, the whitespace rule returning 2 whatever its comment
    // says, and 1 and 1 for the second, whose whitespace rule holds its return in a literal and restarts; so the first
    // scanner has no discarded token and the newline certifies neither exactly nor modulo discarded tokens, while the
    // second discards its whitespace and the newline certifies modulo the discarded tokens.
    constexpr std::string_view commented{R"(/*!re2c
        [ \t\n]+  { /* return; */ return 2; }
        [ab]      { return 1; }
        *         { return 9; }
    */
)"};

    const auto with_comment{read_re2c(commented).front()};

    ASSERT_EQ(with_comment.rules.size(), 3U);
    EXPECT_EQ(with_comment.rules[0].token, std::optional<std::string>{"2"});

    const auto commented_set{token_set(with_comment, "INITIAL")};

    EXPECT_FALSE(commented_set.rules[0].discarded);

    const auto commented_lexer{build(with_comment, "INITIAL")};

    EXPECT_FALSE(commented_lexer.is_split_point('\n'));
    EXPECT_FALSE(commented_lexer.is_split_point_ignoring('\n'));

    constexpr std::string_view quoted{R"(restart:
/*!re2c
        [ \t\n]+  { const char *m = "return 2;"; (void)m; goto restart; }
        [ab]      { return 1; }
        *         { return 9; }
    */
)"};

    const auto with_literal{read_re2c(quoted).front()};

    ASSERT_EQ(with_literal.rules.size(), 3U);
    EXPECT_FALSE(with_literal.rules[0].token.has_value());

    const auto quoted_set{token_set(with_literal, "INITIAL")};

    EXPECT_TRUE(quoted_set.rules[0].discarded);

    const auto quoted_lexer{build(with_literal, "INITIAL")};

    EXPECT_FALSE(quoted_lexer.is_split_point('\n'));
    EXPECT_TRUE(quoted_lexer.is_split_point_ignoring('\n'));
}

TEST(Read_re2c_test, Each_block_with_rules_is_a_scanner_of_its_own)
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

    ASSERT_EQ(scanners.size(), 3U);

    EXPECT_EQ(scanners[2].line, 19U);
    EXPECT_FALSE(scanners[2].definitions.contains("digits"));

    EXPECT_EQ(scanners[0].line, 6U);
    EXPECT_EQ(scanners[0].rules.size(), 2U);
    EXPECT_EQ(scanners[0].definitions.at("name"), "[a-z]+");
    EXPECT_EQ(scanners[0].options, (std::vector<std::string>{"define:YYCTYPE=char"}));

    EXPECT_EQ(scanners[1].line, 12U);
    EXPECT_EQ(scanners[1].rules.size(), 2U);
    EXPECT_EQ(scanners[1].rules[0].expression, "{name}");
    EXPECT_EQ(scanners[1].rules[1].pattern, "*");
    EXPECT_EQ(scanners[1].definitions.at("name"), "[a-z]+");
    EXPECT_EQ(scanners[1].definitions.at("digits"), "[0-9]+");
}

TEST(Read_re2c_test, Case_insensitive_keywords_tokenize_either_way)
{
    // The conventional twin's case-insensitive keywords tokenize either way, and lose to nothing shorter.
    const auto text{grammar("c-like-conventional.re")};

    const auto scanner{read_re2c(text).front()};

    const auto conventional{build(scanner, "INITIAL")};

    EXPECT_EQ(length_at(conventional, "WHILE"), 5U);
    EXPECT_EQ(token_at(conventional, "while"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(conventional, "whilex"), std::optional<std::size_t>{1});
}

TEST(Read_re2c_test, A_non_capturing_group_is_its_body_and_a_backwards_range_spans_its_members)
{
    // re2c 3.1 reads `(![a-z])+` as `[a-z]+`, the `!` after the opening marking a group that captures nothing, and
    // `[Z-A]` as `[A-Z]`: its generated scanner returns 1 over "a", 2 over "!" and 3 over "ABZ".
    constexpr std::string_view source{R"(/*!re2c
    re2c:define:YYCTYPE = char;
    (![a-z])+ { return 1; }
    [!] { return 2; }
    [Z-A]+ { return 3; }
    * { return 9; }
*/
)"};

    const auto file{read_re2c(source).front()};

    ASSERT_EQ(file.rules.size(), 4U);
    EXPECT_EQ(file.rules[0].pattern, "(![a-z])+");
    EXPECT_EQ(file.rules[0].expression, "([a-z])+");
    EXPECT_EQ(file.rules[2].expression, "[Z-A]+");

    // The mark may stand after blanks, `( ![a-z])+`, which re2c 3.1 compiles to the same scanner as `(![a-z])+`.
    const auto marked{read_re2c("/*!re2c\n(![a-z])+ { return 1; }\n*/\n").front()};

    for (const std::string_view spelling : {"( ![a-z])+", "( /* c */ ![a-z])+", "(\n![a-z])+"})
    {
        const auto block{std::format("/*!re2c\n{} {{ return 1; }}\n*/\n", spelling)};

        const auto spaced{read_re2c(block).front()};

        EXPECT_EQ(spaced.rules[0].expression, marked.rules[0].expression) << spelling;
    }

    // Inside a case-insensitive literal re2c folds the letter an escape spells as it folds a bare one: '\\Ab', '\\x41b'
    // and '\\101b' compile to scanners matching ab, aB, Ab and AB.
    for (const std::string_view spelling : {R"('\Ab')", R"('\x41b')", R"('\101b')"})
    {
        const auto block{std::format("/*!re2c\n{} {{ return 1; }}\n*/\n", spelling)};

        const auto folded{read_re2c(block).front()};

        EXPECT_EQ(folded.rules[0].expression, "[aA][bB]") << spelling;
    }

    const auto lexer{build(file, "INITIAL")};

    EXPECT_EQ(
            lexer.tokenize<std::size_t>(std::string_view{"a"}),
            (Match_t{.token = std::optional<std::size_t>{0}, .length = 1U}));
    EXPECT_EQ(
            lexer.tokenize<std::size_t>(std::string_view{"!a"}),
            (Match_t{.token = std::optional<std::size_t>{1}, .length = 1U}));
    EXPECT_EQ(
            lexer.tokenize<std::size_t>(std::string_view{"ABZ"}),
            (Match_t{.token = std::optional<std::size_t>{2}, .length = 3U}));
}

TEST(Read_re2c_test, A_class_difference_becomes_the_bracket_of_the_bytes_left)
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

    ASSERT_EQ(spec.rules.size(), 4U);
    EXPECT_EQ(spec.rules[0].pattern, R"(";" (any \ [\000])*)");
    EXPECT_EQ(spec.rules[0].expression, R"(";"([\x01-\xff])*)");
    EXPECT_EQ(spec.rules[2].pattern, R"([a-z] \ [aeiou] \ [x-z])");
    EXPECT_EQ(spec.rules[2].expression, "[b-df-hj-np-tvw]");

    // The configuration's value carries a ';' inside its quotes without ending the configuration there.
    ASSERT_EQ(spec.options.size(), 1U);
    EXPECT_EQ(spec.rules[3].pattern, R"([ \t\n]+)");
}

TEST(Read_re2c_test, A_class_difference_subtracts_code_points_and_takes_the_operands_re2c_takes)
{
    // Under UTF-8 the difference is taken over code points and the encoding comes after it: re2c 3.1 scans é, the bytes
    // C3 A9, as 1 over both, a as 2, the euro sign as 1 over three bytes, and ED A0 80 as 1 over three, its default
    // encoding policy encoding a surrogate like any other code point.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\x7f] { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 1U);

    const auto& utf8_spec{utf8.front()};

    const std::string e_acute{"\xc3\xa9"};

    const std::string surrogate{"\xed\xa0\x80"};

    EXPECT_EQ(utf8_spec.rules[0].expression, R"(([\u{80}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", e_acute), (Scanned_t{"1", 2}));
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", "a"), (Scanned_t{"2", 1}));
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", "\xe2\x82\xac"), (Scanned_t{"1", 3}));
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", surrogate), (Scanned_t{"1", 3}));

    // The operands re2c takes for char sets, over code points too: a one-character literal in either quote, a negated
    // class, the dot, a group of alternatives that are each one, a name defined as any of these, and a chain of
    // differences through definitions, a used block's among them. re2c 3.1 scans, in the first scanner, é as 3 over two
    // bytes, a as 5, ê as 6 over two, "aé b" as 6 over three bytes and then 1 and 6, the byte 01 as 1 and the newline
    // as 4; and in the second é as 7 over two bytes and a as 4.
    const auto operands{read_re2c(
            "/*!rules:re2c:base\nany = [^];\nnonascii = any \\ [\\x00-\\x7f];\nnonascii { return 7; }\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\nany = [^];\nctl = [\\x00-\\x1f];\nprintable = any \\ ctl;\n"
            "word = printable \\ (' ' | \"\\t\");\n\"\\xe9\" \\ 'x' { return 3; }\nany \\ [^a] { return 5; }\n"
            "word+ { return 6; }\n. \\ [a] { return 1; }\n* { return 4; }\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\n!use:base;\n* { return 4; }\n*/\n")};

    ASSERT_EQ(operands.size(), 2U);

    const auto& operands_first{operands.front()};

    const auto& operands_second{operands.back()};

    ASSERT_EQ(operands_first.rules.size(), 5U);
    EXPECT_EQ(operands_first.rules[0].expression, R"([\u{e9}])");
    EXPECT_EQ(operands_first.rules[1].expression, "[a]");
    EXPECT_EQ(operands_first.rules[2].expression, "{word}+");
    EXPECT_EQ(scanned(operands_first, "INITIAL", e_acute), (Scanned_t{"3", 2}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", "a"), (Scanned_t{"5", 1}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", "\xc3\xaa"), (Scanned_t{"6", 2}));

    const auto spaced{std::format("a{} b", e_acute)};

    EXPECT_EQ(scanned(operands_first, "INITIAL", spaced), (Scanned_t{"6", 3}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", " b"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", "b"), (Scanned_t{"6", 1}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", "\x01"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(operands_first, "INITIAL", "\n"), (Scanned_t{"4", 1}));
    EXPECT_EQ(scanned(operands_second, "INITIAL", e_acute), (Scanned_t{"7", 2}));
    EXPECT_EQ(scanned(operands_second, "INITIAL", "a"), (Scanned_t{"4", 1}));

    // The difference takes the whole term on either side, as re2c's grammar has it, so a concatenation, a repetition, a
    // tag or a literal of two characters on either side is refused as re2c refuses it, "can only difference char sets";
    // a group around the difference and an alternation beside it stand.
    const auto refused{[](const std::string_view rule) {
        const auto block{std::format("/*!re2c\n{} {{ return 1; }}\n*/\n", rule)};

        return refusal_of(block).contains("can only difference char sets");
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

    ASSERT_EQ(shapes.size(), 2U);

    const auto& shapes_first{shapes.front()};

    const auto& shapes_second{shapes.back()};

    EXPECT_EQ(scanned(shapes_first, "INITIAL", "abc"), (Scanned_t{"1", 3}));
    EXPECT_EQ(scanned(shapes_first, "INITIAL", "x"), (Scanned_t{"2", 1}));
    EXPECT_EQ(shapes_second.rules[0].expression, "[0-9]|[a-wyz]|[A-Z]");
    EXPECT_EQ(scanned(shapes_second, "INITIAL", "Q"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(shapes_second, "INITIAL", "x"), (Scanned_t{"2", 1}));
}

TEST(Read_re2c_test, An_empty_class_difference_is_refused_under_the_configuration_the_block_leaves_and_no_other)
{
    // Which code points the operands hold is the configuration's to say: re2c 3.1 scans U+0100, the bytes C4 80, as 1
    // over both under `[^] \ [\x00-\xff]` once the block turns UTF-8 on, wherever in the block the configuration
    // stands, since `[^]` is then every code point and the bytes are subtracted from it, and a as 2.
    const auto utf8{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\xff] { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(utf8.size(), 1U);

    const auto& utf8_spec{utf8.front()};

    const std::string wide{"\xc4\x80"};

    EXPECT_EQ(utf8_spec.rules[0].expression, R"(([\u{100}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", wide), (Scanned_t{"1", 2}));
    EXPECT_EQ(scanned(utf8_spec, "INITIAL", "a"), (Scanned_t{"2", 1}));

    const auto below{
            read_re2c("/*!re2c\n[^] \\ [\\x00-\\xff] { return 1; }\nre2c:encoding:utf8 = 1;\n* { return 2; }\n*/\n")};

    ASSERT_EQ(below.size(), 1U);
    EXPECT_EQ(scanned(below.front(), "INITIAL", wide), (Scanned_t{"1", 2}));

    // The case flags the same way: under `re2c:case-inverted = 1` the double-quoted literal folds and the single-quoted
    // one is exact, so `"a" \ 'A'` is the exact a, which re2c 3.1 scans as 1 and A as 2.
    const auto inverted{
            read_re2c("/*!re2c\nre2c:case-inverted = 1;\n\"a\" \\ 'A' { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(inverted.size(), 1U);

    const auto& inverted_spec{inverted.front()};

    EXPECT_EQ(inverted_spec.rules[0].expression, "[a]");
    EXPECT_EQ(scanned(inverted_spec, "INITIAL", "a"), (Scanned_t{"1", 1}));
    EXPECT_EQ(scanned(inverted_spec, "INITIAL", "A"), (Scanned_t{"2", 1}));

    // A rules block and a block of definitions alone are compiled where they are used, under the flags there, so re2c
    // 3.1 scans U+0100 as 1 through either from a block that turns UTF-8 on, and the difference is refused at its own
    // line where a block under ASCII reaches it.
    const auto library{
            read_re2c("/*!rules:re2c:base\nwide = [^] \\ [\\x00-\\xff];\nwide { return 1; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n!use:base;\n* { return 2; }\n*/\n")};

    ASSERT_EQ(library.size(), 1U);
    EXPECT_EQ(scanned(library.front(), "INITIAL", wide), (Scanned_t{"1", 2}));

    const auto carried{
            read_re2c("/*!re2c\nwide = [^] \\ [\\x00-\\xff];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\nwide { return 1; }\n* { return 2; }\n*/\n")};

    ASSERT_EQ(carried.size(), 1U);
    EXPECT_EQ(scanned(carried.front(), "INITIAL", wide), (Scanned_t{"1", 2}));

    EXPECT_EQ(
            line_of("/*!re2c\nwide = [^] \\ [\\x00-\\xff];\n*/\n"
                    "/*!re2c\nwide { return 1; }\n* { return 2; }\n*/\n"),
            2);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\nwide = [^] \\ [\\x00-\\xff];\nwide { return 1; }\n*/\n"
                    "/*!re2c\n!use:base;\n* { return 2; }\n*/\n"),
            2);

    // A difference the settled configuration leaves empty is refused at its rule's line, as an empty class is, whether
    // the configuration is the default one, the block's own or one inverted back: re2c 3.1 compiles each into a rule
    // that matches nothing, which the reading has no pattern for.
    EXPECT_EQ(line_of("/*!re2c\n[^] \\ [\\x00-\\xff] { return 1; }\n* { return 2; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\nre2c:case-inverted = 1;\n'a' \\ \"A\" { return 1; }\n* { return 2; }\n*/\n"), 3);
    EXPECT_EQ(
            line_of("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] \\ [\\x00-\\xff] { return 1; }\n"
                    "re2c:encoding:utf8 = 0;\n* { return 2; }\n*/\n"),
            3);
    EXPECT_EQ(
            line_of("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return 3; }\n[^] \\ [\\x00-\\xff] { return 1; }\n"
                    "re2c:encoding:utf8 = 0;\n[b] \\ [b] { return 4; }\n*/\n"),
            4);
}

TEST(Read_re2c_test, Rules_blocks_are_libraries_the_use_directive_merges_and_tags_and_empty_rules_are_no_tokens)
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

    ASSERT_EQ(scanners.size(), 3U);

    EXPECT_EQ(scanners[0].line, 6U);
    ASSERT_EQ(scanners[0].rules.size(), 1U);
    EXPECT_EQ(scanners[0].rules[0].expression, "{char_lit}[']");

    // The used block sets the UTF-8 encoding, so `[^]` is every code point, the surrogates among them, and not every
    // byte; the blocks using it read their own rules under that encoding as well.
    EXPECT_EQ(
            scanners[0].definitions.at("char_lit"),
            R"({esc}[x][0-9a-f]{2}|([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanners[0].options, (std::vector<std::string>{"flags:utf-8=1"}));

    EXPECT_EQ(scanners[1].line, 11U);
    ASSERT_EQ(scanners[1].rules.size(), 2U);
    EXPECT_EQ(scanners[1].rules[0].pattern, R"(":"? "=>" @p [a-z]+ #q)");
    EXPECT_EQ(scanners[1].rules[0].expression, R"(":"?"=>"[a-z]+)");
    EXPECT_EQ(scanners[1].rules[1].token, std::optional<std::string>{"SPACE"});
    EXPECT_FALSE(scanners[1].definitions.contains("esc"));

    EXPECT_EQ(scanners[2].line, 16U);
    ASSERT_EQ(scanners[2].rules.size(), 1U);
    EXPECT_EQ(scanners[2].rules[0].expression, "{char_lit}[`]");
    EXPECT_EQ(scanners[2].definitions.at("esc"), R"([\\])");
}

TEST(Read_re2c_test, Configurations_govern_the_whole_block_under_every_name_re2c_gives_them)
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
    constexpr std::string_view repeated_source{
            "/*!re2c\nre2c:case-insensitive = 0;\n\"abc\" { return A; }\nre2c:case-insensitive = 1;\n*/\n"};

    const auto repeated{read_re2c(repeated_source).front()};

    EXPECT_EQ(repeated.rules.front().expression, "[aA][bB][cC]");

    // The encoding goes the same way: turned on above the rule and off below it, the block reads bytes, so `[^]` admits
    // one byte rather than a whole code point, which is what re2c's own scanner does with it.
    const auto turned{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^] { return A; }\nre2c:encoding:utf8 = 0;\n*/\n").front()};

    EXPECT_EQ(turned.rules.front().expression, R"([\x00-\xff])");
}

TEST(Read_re2c_test, An_unnamed_use_block_takes_the_most_recent_rules_block_with_its_configurations)
{
    // A use block with no name of its own uses the most recent rules block, named or not, and reads its own rules under
    // the configurations it inherits from it; the name in `use:re2c:name` is the block used, not a name of the use
    // block, so a second use of the same rules block gets that block's rules and not the first one's.
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

    ASSERT_EQ(scanners.size(), 2U);

    ASSERT_EQ(scanners[0].rules.size(), 2U);
    EXPECT_EQ(scanners[0].rules[0].expression, "[wW][hH][iI][lL][eE]");
    EXPECT_EQ(scanners[0].rules[1].expression, "[iI][fF]");

    ASSERT_EQ(scanners[1].rules.size(), 2U);
    EXPECT_EQ(scanners[1].rules[0].token, std::optional<std::string>{"WHILE"});
    EXPECT_EQ(scanners[1].rules[1].token, std::optional<std::string>{"FOR"});
}

TEST(Read_re2c_test, The_utf8_encoding_matches_code_points_and_the_other_encodings_are_refused)
{
    // Under the UTF-8 encoding a pattern names code points and the scanner reads their encodings: `[^]` is every code
    // point, the surrogates among them, which re2c's default encoding policy encodes like any other; the dot is every
    // one but the newline; an escape beyond ASCII is a code point of two bytes; an ASCII literal stands as it is. A
    // scanner re2c compiled from these rules consumes the same one to four bytes per code point, three for a
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

    ASSERT_EQ(spec.rules.size(), 4U);
    EXPECT_EQ(spec.rules[0].expression, R"(([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(
            spec.rules[1].expression,
            R"(([\u{0}-\u{9}\u{b}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(spec.rules[2].expression, R"([\u{ff}])");
    EXPECT_EQ(spec.rules[3].expression, R"("ab")");

    // A caret among the members of a negated class is a member, not a second negation.
    const auto caret{read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[^^] { return X; }\n*/\n").front()};

    EXPECT_EQ(
            caret.rules.front().expression,
            R"(([\u{0}-\u{5d}\u{5f}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");

    // Built, one code point is one token however many bytes it takes, and an encoding no code point has is no token.
    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(length_at(lexer, "a"), 1U);
    EXPECT_EQ(length_at(lexer, "\xc3\xa9"), 2U);
    EXPECT_EQ(length_at(lexer, "\xe2\x82\xac"), 3U);
    EXPECT_EQ(length_at(lexer, "\xf0\x9f\x98\x80"), 4U);
    EXPECT_EQ(length_at(lexer, "\xed\xa0\x80"), 3U);
    EXPECT_EQ(length_at(lexer, "\x80"), 0U);
    EXPECT_EQ(length_at(lexer, "\xc0\xaf"), 0U);
}

TEST(Read_re2c_test, A_used_block_is_read_again_under_the_flags_of_the_block_using_it)
{
    // re2c compiles a rules block's regexes at every point of use, under the configurations in force there, so the
    // using block's own configuration governs the rules it takes as well as the ones it writes: a scanner compiled from
    // this shape matches WHILE and IF alike, whichever side of the directive the configuration stands on.
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

    ASSERT_EQ(merged.rules.size(), 2U);
    EXPECT_EQ(merged.rules[0].expression, "[wW][hH][iI][lL][eE]");
    EXPECT_EQ(merged.rules[1].expression, "[iI][fF]");

    // One rules block read under two encodings, which is what re2c's own multiple-encoding example does: `[^]` is every
    // code point in the use block that asks for UTF-8 and every byte in the one that asks for nothing.
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

    ASSERT_EQ(scanners.size(), 2U);
    EXPECT_EQ(
            scanners[0].rules.front().expression,
            R"(([\u{0}-\u{d7ff}\u{e000}-\u{10ffff}]|"\xed"[\xa0-\xbf][\x80-\xbf]))");
    EXPECT_EQ(scanners[1].rules.front().expression, R"([\x00-\xff])");

    // The rules the use block takes and the ones it writes are one compiled block, so of two assignments to one
    // configuration the use block's own is the last and governs both: re2c 3.1 compiled from the first shape matches
    // neither ABC nor DEF, and from the second both, whichever block the setting stands in.
    constexpr std::string_view sensitive_source{
            "/*!rules:re2c\nre2c:case-insensitive = 1;\n\"abc\" { return A; }\n*/\n"
            "/*!use:re2c\nre2c:case-insensitive = 0;\n\"def\" { return D; }\n*/\n"};

    const auto sensitive{read_re2c(sensitive_source).front()};

    ASSERT_EQ(sensitive.rules.size(), 2U);
    EXPECT_EQ(sensitive.rules[0].expression, R"("abc")");
    EXPECT_EQ(sensitive.rules[1].expression, R"("def")");

    constexpr std::string_view insensitive_source{
            "/*!rules:re2c\nre2c:case-insensitive = 0;\n\"abc\" { return A; }\n*/\n"
            "/*!use:re2c\nre2c:case-insensitive = 1;\n\"def\" { return D; }\n*/\n"};

    const auto insensitive{read_re2c(insensitive_source).front()};

    ASSERT_EQ(insensitive.rules.size(), 2U);
    EXPECT_EQ(insensitive.rules[0].expression, "[aA][bB][cC]");
    EXPECT_EQ(insensitive.rules[1].expression, "[dD][eE][fF]");
}

TEST(Read_re2c_test, A_block_that_turns_an_encoding_off_reads_under_what_it_left)
{
    // A configuration governs the whole block, so a block that turns the encoding it inherited off reads its patterns
    // under what it left: the raw byte below is one whose code points only an `--input-encoding` could say, refused
    // under UTF-8 and read as bytes without it, and re2c 3.1 compiles this file and matches it.
    const auto scanners{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 0;\n\"\xc3\xa9\" { return B; }\n*/\n")};

    ASSERT_EQ(scanners.size(), 2U);
    EXPECT_EQ(scanners[1].rules.front().expression, "\"\xc3\xa9\"");

    // With the encoding left on, the same byte is refused, at its own line.
    EXPECT_EQ(
            line_of("/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"
                    "/*!re2c\n\"\xc3\xa9\" { return B; }\n*/\n"),
            6);
}

TEST(Read_re2c_test, A_definition_another_block_uses_is_translated_again_under_that_blocks_flags)
{
    // re2c compiles a definition's regex at every point of use, so `point = [^];` written where the encoding was ASCII
    // admits a whole code point in the block that turns UTF-8 on: re2c 3.1 consumes both bytes of an e-acute there, and
    // so does the token set read here.
    const auto turned_on{
            read_re2c("/*!re2c\npoint = [^];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\npoint { return P; }\n*/\n")};

    ASSERT_EQ(turned_on.size(), 1U);

    const auto turned_on_lexer{build(turned_on.front(), "INITIAL")};

    EXPECT_EQ(length_at(turned_on_lexer, "\xc3\xa9"), 2U);

    // And the other way about: written under UTF-8 and used where a configuration has turned it off, the same
    // definition admits one byte, which is what re2c's own scanner consumes there.
    const auto turned_off{
            read_re2c("/*!re2c\nre2c:encoding:utf8 = 1;\npoint = [^];\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 0;\npoint { return P; }\n*/\n")};

    ASSERT_EQ(turned_off.size(), 1U);
    EXPECT_EQ(turned_off.front().definitions.at("point"), R"([\x00-\xff])");

    const auto turned_off_lexer{build(turned_off.front(), "INITIAL")};

    EXPECT_EQ(length_at(turned_off_lexer, "\xc3\xa9"), 1U);

    // A flex-style definition is read again under the flex syntax its own line is the evidence of, its regex ending
    // where its line does.
    const auto flex{read_re2c("/*!re2c\nDIGIT [0-9]\n*/\n/*!re2c\n{DIGIT}+ { return N; }\n*/\n")};

    ASSERT_EQ(flex.size(), 1U);
    EXPECT_EQ(flex.front().definitions.at("DIGIT"), "[0-9]");

    const auto flex_lexer{build(flex.front(), "INITIAL")};

    EXPECT_EQ(length_at(flex_lexer, "42"), 2U);

    // A definition this block's flags cannot read waits for the block to name it, since that is where re2c would
    // compile it: the byte beyond ASCII below stands for whatever code points an `--input-encoding` says, which no file
    // carries, and re2c 3.1 compiles both of these files. Named, it is refused at its own line; unnamed, it is left out
    // and the block reads as re2c reads it.
    const auto unused{
            read_re2c("/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n")};

    ASSERT_EQ(unused.size(), 1U);
    EXPECT_FALSE(unused.front().definitions.contains("accent"));
    EXPECT_EQ(unused.front().rules.size(), 1U);

    EXPECT_EQ(
            line_of("/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                    "/*!re2c\nre2c:encoding:utf8 = 1;\naccent { return A; }\n*/\n"),
            2U);
}

TEST(Read_re2c_test, An_unreadable_definition_is_refused_only_where_a_rule_reaches_it)
{
    // re2c 3.1 compiles this file, an alias of the unreadable definition standing beside it and no rule naming either,
    // and its scanner takes "a" as one byte: the alias is as unused as the definition, so the block reads.
    constexpr std::string_view unused{
            "/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\n*/\n"
            "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] { return A; }\n*/\n"};

    const auto scanners{read_re2c(unused)};

    ASSERT_EQ(scanners.size(), 1U);

    const auto& spec{scanners.front()};

    EXPECT_FALSE(spec.definitions.contains("accent"));
    EXPECT_EQ(spec.definitions.at("alias"), "{accent}");

    const auto lexer{build(spec, "INITIAL")};

    EXPECT_EQ(length_at(lexer, "a"), 1U);

    // A rule naming the alias reaches the definition through it, and that is where re2c compiles the definition, so the
    // refusal names the definition's own line; a longer chain reaches it the same way.
    EXPECT_EQ(
            line_of("/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\n*/\n"
                    "/*!re2c\nre2c:encoding:utf8 = 1;\nalias { return A; }\n*/\n"),
            2);
    EXPECT_EQ(
            line_of("/*!re2c\naccent = \"\xc3\xa9\";\nalias = accent;\nfurther = alias;\n*/\n"
                    "/*!re2c\nre2c:encoding:utf8 = 1;\n[a] further { return A; }\n*/\n"),
            2);

    // Braces inside a bracket or a quoted literal are text and reach nothing: re2c 3.1 compiles this file and scans "{"
    // and "a" as the first rule's, the eight bytes of "{accent}" as the second's.
    const auto text{
            read_re2c("/*!re2c\naccent = \"\xc3\xa9\";\n*/\n"
                      "/*!re2c\nre2c:encoding:utf8 = 1;\n[{accent}] { return 1; }\n\"{accent}\" { return 2; }\n*/\n")};

    ASSERT_EQ(text.size(), 1U);
    EXPECT_EQ(text.front().rules.size(), 2U);

    const auto text_lexer{build(text.front(), "INITIAL")};

    EXPECT_EQ(token_at(text_lexer, "{"), std::optional<std::size_t>{0});
    EXPECT_EQ(token_at(text_lexer, "{accent}"), std::optional<std::size_t>{1});
}

TEST(Read_re2c_test, An_action_emits_a_token_only_where_every_path_returns_it)
{
    // re2c 3.1 with gcc 13 on `"a" { if (flag) return 7; return 8; }` returns 7 for "a" under flag and 8 without, and
    // `"a"+ { if (flag) return 7; }` falls through without flag, so neither emits a token the text decides; both
    // branches returning the one token does.
    EXPECT_EQ(line_of("/*!re2c\n\"a\" { if (flag) return 7; return 8; }\n\"b\" { return 9; }\n*/\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n\"a\"+ { if (flag) return 7; }\n\"b\" { return 9; }\n*/\n"), 2);
    EXPECT_EQ(
            line_of("/*!re2c\n\"a\" { if (flag) return 7; else return 7; }\n\"b\" { return 9; }\n*/\n"), std::nullopt);
}

TEST(Read_re2c_test, A_transparent_macro_is_its_value_in_a_pointer_s_index)
{
    // A transparent macro is its value before the compiler compares anything: with `#define SLOT 0` the scanner re2c
    // 3.1 builds from `re2c:define:YYCURSOR = "cursors[0]";` and `"a" { ++cursors[SLOT]; ... }` leaves the cursor two
    // bytes on from the start of "ab", so the action moves the configured pointer whichever side spells the index
    // through the macro; `#define SLOT 1` names another slot and moves nothing of the scanner's.
    constexpr std::string_view block{
            "/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n"
            "\"a\" { ++cursors[SLOT]; return 7; }\n\"b\" { return 8; }\n*/\n"};

    const auto line_around{[block](const std::string_view before, const std::string_view after = "") {
        const auto source{std::format("{}{}{}", before, block, after)};

        return line_of(source);
    }};

    EXPECT_EQ(line_around("#define SLOT 0\n"), 5);
    EXPECT_EQ(line_around("#define SLOT 1\n"), std::nullopt);
    EXPECT_EQ(line_around("#define SLOT 0U\n"), 5);
    EXPECT_EQ(line_around("#define SLOT 0x0\n"), 5);

    // A macro defined more than once stands for each of its values in turn, since which definition is live at the
    // action is not decided here: with `#define SLOT 1` after the block, gcc 13 still expands the action's SLOT to 0
    // and the scanner moves its cursor; a function-like macro whose replacement is a value stands for it whatever its
    // arguments; and a pointer named through an opaque macro, `#define CUR cursors[0]` with `re2c:define:YYCURSOR =
    // "CUR";`, is out of sight, the action `++cursors[0]` moving it unseen.
    EXPECT_EQ(line_around("#define SLOT 0\n", "#define SLOT 1\n"), 5);
    EXPECT_EQ(line_around("#define SLOT 1\n", "#define SLOT 0\n"), 5);
    EXPECT_EQ(
            line_of("#define SLOT() 0\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n"
                    "\"a\" { ++cursors[SLOT()]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);
    EXPECT_EQ(
            line_of("#define CUR cursors[0]\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"CUR\";\n"
                    "\"a\" { ++cursors[0]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);

    // An integer literal is its value whatever its spelling: `0U`, `0x0`, `00` and `0'0` index the slot `0` does, and
    // the scanner built from each moves its cursor past the b of "ab"; `1U` is another slot.
    for (const std::string_view index : {"0U", "0x0", "00", "0'0", "0'0'0", "0b0", "0ULL", "(0)", "((0U))"})
    {
        const auto spelled{std::format(
                "/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n\"a\" {{ ++cursors[{}]; "
                "return 7; }}\n\"b\" {{ return 8; }}\n*/\n",
                index)};

        EXPECT_TRUE(refusal_of(spelled).contains("line 4: the action moves cursors[0]")) << index;
    }

    EXPECT_EQ(
            line_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[0]\";\n"
                    "\"a\" { ++cursors[1U]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            std::nullopt);
}

TEST(Read_re2c_test, A_constant_index_is_read_to_its_value_and_any_other_is_out_of_sight)
{
    // An index into the array a pointer is configured in is read to its value when it is a constant expression, `+0`,
    // `1-1` and `2-1` among them, and is out of sight otherwise, `cursors[i]` standing for any slot and `cursors[SLOT]`
    // under `constexpr unsigned SLOT = 0` or a function-like `#define SLOT() 0` not invoked alike, in the action and in
    // the configured spelling; the scanner built from each of the first three moves its cursor past the b of "ab".
    const auto indexed{
            [](const std::string_view head, const std::string_view configured, const std::string_view index) {
                return std::format(
                        "{}/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"{}\";\n\"a\" {{ "
                        "++cursors[{}]; return 7; }}\n\"b\" {{ return 8; }}\n*/\n",
                        head, configured, index);
            }};

    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "+0")).contains("moves cursors[0]"));
    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "1-1")).contains("moves cursors[0]"));
    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "(1 << 1) - 2")).contains("indexes cursors by an expression"));
    EXPECT_EQ(refusal_of(indexed("", "cursors[0]", "2-1")), "");
    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "i")).contains("indexes cursors by an expression"));
    EXPECT_TRUE(refusal_of(indexed("constexpr unsigned SLOT = 0;\n", "cursors[SLOT]", "0"))
                        .contains("indexes cursors by an expression"));
    EXPECT_TRUE(refusal_of(indexed("#define SLOT() 0\nconstexpr unsigned SLOT = 1;\n", "cursors[SLOT]", "1"))
                        .contains("indexes cursors by an expression"));

    // The evaluator reads decimal literals and `+`, `-`, `*`, `/` and `%` alone; a shift, a bitwise operator or a
    // suffixed literal has a width and a type the reading does not model, `~0U >> 31` being 1 under gcc 13 where a
    // signed reading says 0 and `(1U << 31) << 1` being 0, so each is out of sight. A parenthesised array base,
    // `(cursors)[1-1]`, and a configured `(cursors)[0]` come down to the name before the index is read, and a run with
    // an earlier index of its own, `slots[0].cursors`, is an array too.
    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "~0U >> 31")).contains("indexes cursors by an expression"));
    EXPECT_TRUE(refusal_of(indexed("", "cursors[0]", "(1U << 31) << 1")).contains("indexes cursors by an expression"));
    EXPECT_TRUE(refusal_of(acting("", "++(cursors)[1-1]; return 7;")).contains("moves cursors[0]"));
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"(cursors)[0]\";\n"
                           "\"a\" { ++cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                        .contains("moves cursors[0]"));
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"slots[0].cursors[0]\";\n"
                           "\"a\" { ++slots[0].cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                        .contains("moves slots[0].cursors[0]"));
}

TEST(Read_re2c_test, A_pointer_handed_on_rather_than_moved_is_out_of_sight)
{
    // The pointer handed on rather than moved here is out of sight: a reference bound to it, its address taken, or the
    // pointer passed to a call, which may take it by reference; re2c 3.1 with `auto& cursor = cursors[0]; ++cursor;`
    // moves the cursor past the b of "ab". A cast's parentheses pass nothing.
    EXPECT_TRUE(
            refusal_of(acting("", "auto& cursor = cursors[0]; ++cursor; return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of(acting("", "auto* p = &cursors[0]; ++*p; return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of(acting("", "advance(cursors[0]); return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of(acting("", "(*step)(cursors[0]); return 7;")).contains("hands on cursors[0]"));
    EXPECT_EQ(refusal_of(acting("", "int n = (int)(cursors[0] - start); return n;")), "");

    // A reference bound with braces, `auto& cursor{cursors[0]}`, and the pointer passed inside a grouping,
    // `std::advance((cursors[0]), 1)`, hand it on as the `=` binding and the bare argument do; re2c 3.1 moves the
    // cursor past the b of "ab" under each. An array reached through a member, `in->cursors` of the configured
    // `in->cursors[0]`, is indexed as `cursors` is: `in->cursors[1-1]` moves the pointer and `in->cursors[i]` is out of
    // sight.
    EXPECT_TRUE(
            refusal_of(acting("", "auto& cursor{cursors[0]}; ++cursor; return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of(acting("", "std::advance((cursors[0]), 1); return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"in->cursors[0]\";\n"
                           "\"a\" { ++in->cursors[1-1]; return 7; }\n\"b\" { return 8; }\n*/\n")
                        .contains("moves in->cursors[0]"));
    EXPECT_TRUE(refusal_of("/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"in->cursors[0]\";\n"
                           "\"a\" { ++in->cursors[i]; return 7; }\n\"b\" { return 8; }\n*/\n")
                        .contains("indexes in->cursors by an expression"));
    EXPECT_EQ(refusal_of(acting("", "long n = (const char*)(cursors[0]) - start; return (int)n;")), "");

    // The pointer handed on inside parentheses is handed on: `auto& cursor = (cursors[0]);` and `&(cursors[0])` each
    // moves the cursor past the b of "ab" under re2c 3.1.
    EXPECT_TRUE(
            refusal_of(acting("", "auto& cursor = (cursors[0]); ++cursor; return 7;")).contains("hands on cursors[0]"));
    EXPECT_TRUE(refusal_of(acting("", "auto cursor = &(cursors[0]); ++*cursor; return 7;"))
                        .contains("hands on cursors[0]"));
}

TEST(Read_re2c_test, A_break_a_continue_and_a_goto_leave_or_restart_the_scan_as_re2c_has_them)
{
    // A `break` in a re2c action leaves the loop the file wrote around the scanner, which is out of sight, where flex's
    // `break` discards; a `continue` restarts the scan and discards, and so does a `goto` to a label the file declares
    // before the block, `loop:`, while a `goto` to any other label is out of sight. re2c 3.1 with the block inside `for
    // (;;)` and `"a" { break; }` leaves the loop on "ab" and never scans the b.
    EXPECT_TRUE(refusal_of(acting("", "break;")).contains("leaves by `break` the loop"));
    EXPECT_EQ(refusal_of(acting("", "continue;")), "");
    EXPECT_EQ(refusal_of(acting("loop:\n", "goto loop;")), "");
    EXPECT_TRUE(refusal_of(acting("", "goto loop;")).contains("leaves by `goto`"));
    EXPECT_TRUE(refusal_of(acting("loop:\n", "if (n) goto loop; return 7;")).contains("leaves by `goto loop`"));

    // A label after the block is left for, not restarted at: re2c 3.1 with `"a" { goto done; }` and `done: return 0;`
    // past the scanning loop returns 0 on "ab" and never scans the b.
    const auto label_after{acting("", "goto done;") + "done:\n"};

    EXPECT_TRUE(refusal_of(label_after).contains("leaves by `goto`"));

    // A label before the block restarts the scan only where nothing between the label and the block's opener leaves:
    // `emit_token: return 9;` returns 9 for "aa" under re2c 3.1 where a rescan would return 9 and 9, and `loop: if
    // (finished) return 0;` returns 0 on "ab" once the action sets the flag; each is refused. A stored token followed
    // by a restarting `goto` is rescanned over and never returned, `token = 7; goto loop;` giving 8 alone on "ab" under
    // `--returns token`, so only a `break` may follow a stored token.
    EXPECT_TRUE(refusal_of(acting("emit_token: return 9;\n", "goto emit_token;")).contains("leaves by `goto`"));
    EXPECT_TRUE(refusal_of(acting("loop: if (finished) return 0;\n", "finished = 1; goto loop;"))
                        .contains("leaves by `goto`"));
    EXPECT_EQ(refusal_of(acting("loop: tok = cursor;\n", "goto loop;")), "");

    EXPECT_TRUE(
            refusal_of(acting("loop:\n", "token = 7; goto loop;"), {}, {"token"}).contains("leaves by `goto loop`"));

    const auto in_loop{acting("for (;;) {\n", "token = 7; break;") + "}\n"};

    EXPECT_EQ(refusal_of(in_loop, {}, {"token"}), "");
}

TEST(Read_re2c_test, An_action_returning_nowhere_leaves_by_a_jump)
{
    // re2c writes the rules' actions one after another and control falls from an action's end into the next, so an
    // action returning nowhere must leave by a jump: `"a"+ { ++count; }` before `"b" { return 8; }` returns 8 for "a"
    // under re2c 3.1; and a `break` after a statement that is no return leaves the loop as a bare `break` does. A
    // shortcut rule, `:=> COMMENT`, and a transition rule's `=> COMMENT { continue; }` leave as re2c writes them.
    EXPECT_TRUE(refusal_of(acting("", "++count;")).contains("ends without returning or leaving"));
    EXPECT_TRUE(refusal_of(acting("", "++count; break;")).contains("leaves by `break` the loop"));
    EXPECT_EQ(
            refusal_of("/*!re2c\nre2c:define:YYCTYPE = char;\n<A> \"a\" :=> B\n<B> \"b\" => A { continue; }\n"
                       "<A> \"c\" { return 3; }\n*/\n"),
            "");
}

TEST(Read_re2c_test, An_included_file_defines_macros_and_is_refused_where_no_reader_reaches_it)
{
    // A file the code includes by a quoted name defines macros as the file's own code does, and is refused where no
    // reader reaches it.
    EXPECT_TRUE(refusal_of(acting("#include \"slots.h\"\n", "return 7;"))
                        .contains(R"(includes "slots.h", a file of its own)"));
    EXPECT_EQ(
            line_of("#define SLOT 0\n/*!re2c\nre2c:define:YYCTYPE = char;\nre2c:define:YYCURSOR = \"cursors[SLOT]\";\n"
                    "\"a\" { ++cursors[0]; return 7; }\n\"b\" { return 8; }\n*/\n"),
            5);
}

TEST(Read_re2c_test, A_malformed_block_is_refused_at_its_line)
{
    EXPECT_EQ(line_of("/*!re2c\n [a-z]+ { return X;\n"), 2);
    EXPECT_EQ(line_of("/*!re2c\n digit = [0-9]\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] \\ [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] \\ \"ab\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \\ [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n !use:missing;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!use:re2c\n [a-z] { return X; }\n*/"), 1);
    EXPECT_EQ(line_of("/*!re2c\n { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a-z]+\n*/"), 3);
}

TEST(Read_re2c_test, An_encoding_the_byte_reading_cannot_follow_is_refused_at_its_line)
{
    // The encodings whose code unit is not a byte, a byte beyond ASCII in the source under UTF-8, and an encoding
    // policy that leaves the surrogates elsewhere.
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf16 = 1;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:flags:unicode = 1;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:ebcdic = 1;\n [a-z] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n \"caf\xc3\xa9\" { return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n [\xc3\xa9] { return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:encoding:utf8 = 1;\n re2c:encoding-policy = fail;\n [a] { return X; }\n*/"), 3);

    // The encoding a block's configurations leave is the one it reads under, the last assignment governing, so a block
    // turning UTF-16 on and off again reads as ASCII, which re2c 3.1 compiles; one turning it on after a rule is
    // refused at the assignment, which stands anywhere in the block.
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:encoding:utf16 = 1;\n re2c:encoding:utf16 = 0;\n [a-z] { return X; }\n*/"),
            std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n [a-z] { return X; }\n re2c:encoding:utf16 = 1;\n*/"), 3);
}

TEST(Read_re2c_test, A_block_opener_and_a_setup_rule_are_read_as_re2c_ends_them)
{
    // re2c ends the opener at a blank, a newline, a colon or the close, so `/*!re2cx` is an ill-formed start of a
    // block, which re2c 3.1 refuses; a setup rule's `!` may stand after blanks, `< ! C >` being `<!C>` to re2c.
    EXPECT_EQ(line_of("/*!re2cx\n [a] { return X; }\n*/\n/*!re2c\n [b] { return Y; }\n*/"), 1);
    EXPECT_EQ(line_of("/*!re2c\n < ! C > { setup(); }\n <C> [a] { return X; }\n*/"), std::nullopt);

    const auto spaced{read_re2c("/*!re2c\n < ! C > { setup(); }\n <C> [a] { return X; }\n*/").front()};

    EXPECT_EQ(spaced.rules.size(), 1U);
}

TEST(Read_re2c_test, An_encoding_is_refused_for_its_own_reason_whichever_side_names_it)
{
    // EBCDIC has a byte per code point, as ASCII has, so the reason it is refused is the mapping and not the width; and
    // an encoding the caller passes rather than a configuration is refused the same way, at the file's head.
    const auto [ebcdic_line, ebcdic_message]{
            refusal_at("/*!re2c\n re2c:encoding:ebcdic = 1;\n [a] { return X; }\n*/", {})};

    EXPECT_TRUE(ebcdic_message.contains("another code point"));

    const auto [passed_line, passed_message]{
            refusal_at("/*!re2c\n [a] { return X; }\n*/", {.encoding = Re2c_encoding::ucs2})};

    EXPECT_TRUE(passed_message.contains("not one byte"));
    EXPECT_EQ(passed_line, 1);
}

TEST(Read_re2c_test, An_include_and_a_Unicode_escape_are_refused_at_their_line)
{
    // An include brings in what the reading does not see, and a Unicode escape needs an encoding the byte reading has
    // not got; a backslash escaping the backslash before a `u` is no Unicode escape.
    EXPECT_EQ(line_of("/*!re2c\n !include \"other.re\";\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\u00e9\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\u00e9] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\\\u\" [0-9a-fA-F]{4} { return X; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n [\\\\u] { return X; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n '\\X00e9' { return X; }\n*/"), 2);

    // The directive outside a block includes another file's blocks where it stands, and what they configure holds for
    // the blocks after it, so it is refused at its line as the one inside a block is.
    EXPECT_EQ(line_of("/*!include:re2c \"flags.re\" */\n/*!re2c\n \"ab\" { return X; }\n*/"), 1);
    EXPECT_EQ(line_of("/*!re2c\n \"ab\" { return X; }\n*/\n/*!include:re2c \"more.re\" */\n"), 4);
    EXPECT_EQ(line_of("/*!max:re2c*/\n/*!re2c\n \"ab\" { return X; }\n*/"), std::nullopt);
}

TEST(Read_re2c_test, A_braced_hexadecimal_escape_is_refused_as_re2c_refuses_it)
{
    // re2c's hexadecimal escape takes no braces: re2c 3.1 answers `\x{...}` with a syntax error in a hexadecimal escape
    // sequence, in a class and in a literal alike, while two digits after the `x`, and a digit after those, are forms
    // of its own that it compiles.
    EXPECT_EQ(line_of("/*!re2c\n [\\x{100}] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\x{ff}] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n \"\\x{41}\" { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [\\xff] { return X; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n [\\x100] { return X; }\n*/"), std::nullopt);
}

TEST(Read_re2c_test, A_scan_pointer_an_action_moves_is_refused_at_the_rule_s_line)
{
    // A setup rule's code and the entry rule's are written into the actions re2c runs them before, so a scan pointer
    // either one moves is moved before a match is handed on, and both are refused by the pointer's name as a rule's own
    // action is. The generated scanner shows the setup action inside each of its condition's actions.
    EXPECT_EQ(line_of("/*!re2c\n <!C> { ++YYCURSOR; }\n <C> [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n <> { ++YYCURSOR; }\n <C> [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n <!C> { count(); }\n <C> [a] { return X; }\n*/"), std::nullopt);

    // The name a `define:` configuration gives a pointer is the name the actions write, quoted or bare: re2c reads `=
    // "cur";` as `= cur;`, so the quotes are no part of the name and `++cur` moves the cursor either way.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = cur;\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n [a] { return cur[0]; }\n*/"), std::nullopt);

    // Parentheses around the name change nothing an operator beside them does, and a `++` written across a line splice
    // is the one operator the compiler reads, since it joins the lines before anything means anything. The refusal
    // names the rule's line, which is where the action it refuses begins.
    EXPECT_EQ(line_of("/*!re2c\n [a] { (YYCURSOR)++; return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { ++(YYCURSOR); return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { +\\\n+YYCURSOR; return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n [a] { return 1 - -YYCURSOR[0]; }\n*/"), std::nullopt);

    // re2c applies a configuration to the whole block it stands in, wherever in the block it is written, and of two
    // assignments to one name the last governs, so the name the actions write is the block's last word on it and not
    // what stood above a rule. The key stays canonical: a renamed pointer is renamed again by another
    // `define:YYCURSOR`, never by one naming the value the first gave it.
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"previous\";\n re2c:define:YYCURSOR = \"cur\";\n"
                    " [a] { ++cur; return X; }\n*/"),
            4);
    EXPECT_EQ(line_of("/*!re2c\n [a] { ++cur; return X; }\n re2c:define:YYCURSOR = \"cur\";\n*/"), 2);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"previous\";\n re2c:define:YYCURSOR = \"cur\";\n"
                    " [a] { ++previous; return X; }\n*/"),
            std::nullopt);

    // A configured name is an expression and not always one word, and a `define:` configuration carries from the block
    // it stands in to the blocks after it, so a member the first block names is the member the second block's actions
    // move.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { return in->cur[0]; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"cur\";\n*/\n/*!re2c\n [a] { ++cur; return X; }\n*/"), 5);

    // Parentheses around the whole configured name, or a group inside a group, name the same pointer the actions write
    // without them.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"((in))->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"(cur)\";\n [a] { ++cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"((cur))\";\n [a] { ++cur; return X; }\n*/"), 3);
}

TEST(Read_re2c_test, The_custom_API_is_refused_where_rules_read_under_it)
{
    // The reading models re2c's default API, where the scanner advances by stepping a pointer an action can be seen to
    // step too. Under the custom API it advances by calling `YYSKIP`, which names no pointer, so an action calling it
    // moves the match out of this reading's sight: such a block is refused by name. re2c 3.1 accepts all four spellings
    // below, and the default one is read.
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = custom;\n [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:flags:input = custom;\n [a] { return X; }\n*/"), 2);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = default;\n [a] { return X; }\n*/"), std::nullopt);

    // Which API the block reads under is its last word on it, as every configuration is, and the style names the
    // spelling of the API's operations, which decides nothing while the API is the default one: `functions` is what C
    // writes under the default API and refuses nothing.
    EXPECT_EQ(line_of("/*!re2c\n re2c:api:style = functions;\n [a] { return X; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = custom;\n re2c:api = default;\n [a] { return X; }\n*/"), std::nullopt);
    EXPECT_EQ(line_of("/*!re2c\n re2c:api = default;\n re2c:api = custom;\n [a] { return X; }\n*/"), 3);

    // A spelling of the pointer is compared in the reading's own: parentheses around one name are grouping, `(*in).cur`
    // is `in->cur`, and every other parenthesis is a call's or an index's and stays, so a pointer reached through an
    // accessor is moved by the operator after the call and `slots[(i+j)*k]` is not `slots[i+(j*k)]`.
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"(in)->cur\";\n [a] { ++in->cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n [a] { ++(*in).cur; return X; }\n*/"), 3);
    EXPECT_EQ(line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cursor()\";\n [a] { in->cursor()++; return X; }\n*/"), 3);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"in->cursor()\";\n"
                    " [a] { in->cursor() = in->cursor() + 1; }\n*/"),
            3);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:define:YYCURSOR = \"slots[(i+j)*k]\";\n [a] { ++slots[i+(j*k)]; return X; }\n*/"),
            3);
}

TEST(Read_re2c_test, A_used_block_is_judged_where_the_block_using_it_reads_it)
{
    // An imported default rule this block's own default overrides is gone with its action, and is told from a survivor
    // by its action and not by its line alone; a live imported action is judged with the block's own, wherever in the
    // scanner's rules it stands.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n * { ++cur; return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [a] { return Y; }\n * { return Z; }\n*/"),
            std::nullopt);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n * { ++cur; return X; } [z] { return W; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [a] { return Y; }\n * { return Z; }\n*/"),
            std::nullopt);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n [a] { ++YYCURSOR; return X; }\n*/\n/*!re2c\n !use:base;\n [b] { return Y; }\n"
                    " * { return Z; }\n*/"),
            2);

    // A rules library is compiled where it is used, under the using block's API and configurations, which re2c lets
    // stand after the `!use:` directive. The API setting carries from a used block into its user and from a block to
    // the next, and is refused where rules read under it, so a configuring block alone refuses nothing and a user that
    // sets it back reads under the default.
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n re2c:api = custom;\n [z] { return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:api = default;\n [a] { return Y; }\n*/"),
            std::nullopt);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n re2c:api = custom;\n [z] { return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " [a] { return Y; }\n*/"),
            2);
    EXPECT_EQ(
            line_of("/*!re2c\n re2c:api = custom;\n*/\n/*!re2c\n re2c:api = default;\n [a] { return X; }\n*/"),
            std::nullopt);
    EXPECT_EQ(
            line_of("/*!rules:re2c:base\n [a] { ++cur; return X; }\n*/\n/*!re2c\n !use:base;\n"
                    " re2c:define:YYCURSOR = \"cur\";\n [b] { return Y; }\n*/"),
            2);
}

TEST(Read_re2c_test, A_macro_an_action_calls_is_judged_by_what_its_replacement_holds)
{
    // An action calling a macro of the file's whose replacement returns or moves a pointer is refused by the macro's
    // name, as the flex reader refuses one; re2c copies the action as written and the compiler expands it.
    EXPECT_EQ(line_of("#define EMIT() return X\n/*!re2c\n [a] { EMIT(); }\n*/"), 3);
    EXPECT_EQ(line_of("#define STEP() ++YYCURSOR\n/*!re2c\n [a] { STEP(); return X; }\n*/"), 3);
    EXPECT_EQ(line_of("#define MAX 10\n/*!re2c\n [a] { if (n < MAX) return X; return X; }\n*/"), std::nullopt);

    // A pointer named by an expression is a run of tokens, and a macro moving it holds that run, in a replacement or in
    // an argument; and a macro whose replacement is an operator alone is opaque, since `#define STEP ++` makes `STEP
    // YYCURSOR` a move.
    EXPECT_EQ(
            line_of("#define STEP() (++in->cur)\n/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n"
                    " [a] { STEP(); return X; }\n*/"),
            4);
    EXPECT_EQ(
            line_of("#define ADVANCE(p) ++p\n/*!re2c\n re2c:define:YYCURSOR = \"in->cur\";\n"
                    " [a] { ADVANCE(in->cur); return X; }\n*/"),
            4);
    EXPECT_EQ(line_of("#define STEP ++\n/*!re2c\n [a] { STEP YYCURSOR; return X; }\n*/"), 3);
}

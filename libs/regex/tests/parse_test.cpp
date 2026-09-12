#include "munch/regex/parse.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "munch/nfa/nfa.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/regex/patterns.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::regex;

namespace
{
/**
 * @brief The length a regex matches at the start of an input, or -1 when it matches nothing there.
 */
long matched(const Regex& regex, const std::string_view input)
{
    const auto nfa{to_nfa(regex).set_accept_token(Token{1, 1}).build()};

    const auto [token, length]{Simulator::run(nfa, input)};

    return token ? static_cast<long>(length) : -1;
}

/**
 * @brief Whether a regex matches the whole input and nothing shorter is preferred, the token-set reading.
 */
bool accepts(const Regex& regex, const std::string_view input)
{
    return matched(regex, input) == static_cast<long>(input.size());
}

/**
 * @brief The offset a refused pattern is refused at, or -1 when it is accepted.
 */
long refused_at(const std::string_view pattern, const Definitions_t& definitions = {})
{
    try
    {
        std::ignore = parse(pattern, definitions);
    }
    catch (const Syntax_error& error)
    {
        return static_cast<long>(error.offset());
    }

    return -1;
}

} // namespace

TEST(Parse, Literals_merge_into_one_text_and_match_as_written)
{
    const auto regex{parse("hello")};

    ASSERT_TRUE(std::holds_alternative<Text>(regex.node));
    EXPECT_EQ(std::get<Text>(regex.node).text, "hello");
    EXPECT_TRUE(accepts(regex, "hello"));
    EXPECT_EQ(matched(regex, "help"), -1);
}

TEST(Parse, Operators_bind_to_the_last_atom_and_alternation_binds_loosest)
{
    // ab* is a followed by any number of b; ab|cd is two words, not a(b|c)d.
    EXPECT_TRUE(accepts(parse("ab*"), "a"));
    EXPECT_TRUE(accepts(parse("ab*"), "abbb"));
    EXPECT_EQ(matched(parse("ab*"), "b"), -1);

    EXPECT_TRUE(accepts(parse("ab|cd"), "ab"));
    EXPECT_TRUE(accepts(parse("ab|cd"), "cd"));
    EXPECT_EQ(matched(parse("ab|cd"), "acd"), -1);

    EXPECT_TRUE(accepts(parse("(ab)+"), "abab"));
    EXPECT_TRUE(accepts(parse("colou?r"), "color"));
    EXPECT_TRUE(accepts(parse("colou?r"), "colour"));
}

TEST(Parse, Counts_are_exact_at_least_and_bounded)
{
    EXPECT_TRUE(accepts(parse("a{3}"), "aaa"));
    EXPECT_EQ(matched(parse("a{3}"), "aa"), -1);
    EXPECT_EQ(matched(parse("a{3}"), "aaaa"), 3);

    EXPECT_TRUE(accepts(parse("a{2,}"), "aaaaa"));
    EXPECT_EQ(matched(parse("a{2,}"), "a"), -1);

    EXPECT_TRUE(accepts(parse("a{2,4}"), "aaa"));
    EXPECT_EQ(matched(parse("a{2,4}"), "aaaaa"), 4);
}

TEST(Parse, The_dot_takes_any_byte_but_the_newline)
{
    const auto regex{parse(".+")};

    EXPECT_TRUE(accepts(regex, "any \t bytes \xff here"));
    EXPECT_EQ(matched(regex, "one\ntwo"), 3);
}

TEST(Parse, Brackets_take_ranges_negation_classes_and_the_literal_edges)
{
    EXPECT_TRUE(accepts(parse("[a-cx]+"), "abcx"));
    EXPECT_EQ(matched(parse("[a-cx]+"), "d"), -1);

    // Negation is over every byte, the newline included, as flex has it.
    EXPECT_TRUE(accepts(parse("[^a]+"), "b\n\xff"));
    EXPECT_EQ(matched(parse("[^a]+"), "a"), -1);

    EXPECT_TRUE(accepts(parse("[[:alpha:]_][[:alnum:]_]*"), "under_score1"));
    EXPECT_TRUE(accepts(parse("[[:xdigit:]]+"), "0aF9"));
    EXPECT_TRUE(accepts(parse("[[:space:]]+"), " \t\n\r"));

    // A leading ']' and an edge '-' are members.
    EXPECT_TRUE(accepts(parse("[]a]+"), "]a]"));
    EXPECT_TRUE(accepts(parse("[a-]+"), "-a-"));
    EXPECT_TRUE(accepts(parse("[-a]+"), "-a-"));

    // Escapes inside brackets decode.
    EXPECT_TRUE(accepts(parse(R"([\n\t]+)"), "\n\t"));
}

TEST(Parse, Escapes_decode_named_octal_hex_and_the_byte_itself)
{
    EXPECT_TRUE(accepts(parse(R"(\n\t\r\f\v\a\b)"), "\n\t\r\f\v\a\b"));
    EXPECT_TRUE(accepts(parse(R"(\101\x42\x4)"), "AB\x04"));
    EXPECT_TRUE(accepts(parse(R"(\.\*\\\/)"), R"(.*\/)"));
    EXPECT_TRUE(accepts(parse(R"(\0)"), std::string_view{"\0", 1}));
}

TEST(Parse, Quoted_text_is_literal_with_its_escapes_decoded)
{
    EXPECT_TRUE(accepts(parse(R"("a.b*")"), "a.b*"));
    EXPECT_EQ(matched(parse(R"("a.b*")"), "axb"), -1);
    EXPECT_TRUE(accepts(parse(R"("say \"hi\"\n")"), "say \"hi\"\n"));
    EXPECT_TRUE(accepts(parse(R"("ab"+)"), "abab"));
}

TEST(Parse, Code_points_encode_as_utf8_in_literals_and_read_brackets_as_scalars)
{
    // In a literal, in quoted text and on its own: the encoding's bytes.
    EXPECT_TRUE(
            accepts(parse(R"(a\u{e9}b)"),
                    "a\xc3\xa9"
                    "b"));
    EXPECT_TRUE(accepts(parse(R"("caf\u{E9}")"), "caf\xc3\xa9"));
    EXPECT_TRUE(accepts(parse(R"(\u{1F600})"), "\xf0\x9f\x98\x80"));
    EXPECT_TRUE(accepts(parse(R"(\u{41})"), "A"));

    // A bracket with a code point reads every member as a scalar: one encoding, not one byte.
    const auto latin{parse(R"([a-z\u{c0}-\u{ff}])")};

    EXPECT_TRUE(accepts(latin, "q"));
    EXPECT_TRUE(accepts(latin, "\xc3\xa9"));
    EXPECT_FALSE(accepts(latin, "\xc3"));
    EXPECT_FALSE(accepts(latin, "\xe2\x82\xac"));

    // Any scalar, and negation over the scalars: the surrogates never encode, so they are left out.
    const auto any{parse(R"([\u{0}-\u{10FFFF}])")};

    EXPECT_TRUE(accepts(any, "\n"));
    EXPECT_TRUE(accepts(any, "\xe2\x82\xac"));
    EXPECT_TRUE(accepts(any, "\xf4\x8f\xbf\xbf"));
    EXPECT_FALSE(accepts(any, "\xed\xa0\x80"));
    EXPECT_FALSE(accepts(any, "\xc0\x80"));
    EXPECT_FALSE(accepts(any, "\x80"));

    const auto not_newline{parse(R"([^\n\u{e9}])")};

    EXPECT_TRUE(accepts(not_newline, "a"));
    EXPECT_TRUE(accepts(not_newline, "\xc3\xa8"));
    EXPECT_FALSE(accepts(not_newline, "\n"));
    EXPECT_FALSE(accepts(not_newline, "\xc3\xa9"));

    // Without a code point the bracket stays bytes, as flex reads it.
    EXPECT_TRUE(accepts(parse("[^\n]"), "\xc3"));

    // Refused: no digits, beyond U+10FFFF, a surrogate, and a byte beyond ASCII beside a code point.
    EXPECT_EQ(refused_at(R"(\u{})"), 0);
    EXPECT_EQ(refused_at(R"(\u{110000})"), 0);
    EXPECT_EQ(refused_at(R"(\u{d800})"), 0);
    EXPECT_EQ(refused_at(R"([\xe9\u{e9}])"), 0);
    EXPECT_EQ(refused_at(R"([\u{d800}-\u{dfff}])"), 1);
}

TEST(Parse, The_caseless_option_folds_letters_in_texts_brackets_and_definitions)
{
    constexpr Parse_options caseless{.caseless = true};

    EXPECT_EQ(matched(parse("select", {}, caseless), "SeLeCt"), 6);
    EXPECT_EQ(matched(parse(R"("a_1b")", {}, caseless), "A_1B"), 4);
    EXPECT_FALSE(accepts(parse(R"("a_1b")", {}, caseless), "A-1B"));
    EXPECT_EQ(matched(parse("(ab|cd)*e?", {}, caseless), "ABcDCdE"), 7);
    EXPECT_EQ(matched(parse("x{2,3}", {}, caseless), "xXxX"), 3);
    EXPECT_EQ(matched(parse(R"(\x41+)", {}, caseless), "aAa"), 3);

    // Both cases are members before the negation, and the case classes name every letter.
    EXPECT_EQ(matched(parse("[a-c]+", {}, caseless), "AbCx"), 3);
    EXPECT_EQ(matched(parse("[^a-c]+", {}, caseless), "xyzA"), 3);
    EXPECT_EQ(matched(parse("[[:upper:]]+", {}, caseless), "Hello"), 5);
    EXPECT_EQ(matched(parse("[0-9_]+", {}, caseless), "0_9a"), 3);
    EXPECT_EQ(matched(parse(R"([^\u{e9}a])", {}, caseless), "A"), -1);

    const Definitions_t definitions{{"ident", "[a-z_][a-z0-9_]*"}};

    EXPECT_EQ(matched(parse("{ident}", definitions, caseless), "Foo_1 "), 5);

    // Without the option the same patterns are exact, and flex's group flag turns it on or off inside the group.
    EXPECT_FALSE(accepts(parse("select"), "SELECT"));
    EXPECT_EQ(matched(parse("[^a-c]+"), "xyzA"), 4);
    EXPECT_EQ(matched(parse(R"((?i:"false"|"true")x)"), "TRUEx"), 5);
    EXPECT_FALSE(accepts(parse(R"((?i:"false"|"true")x)"), "TRUEX"));
    EXPECT_EQ(matched(parse("(?-i:ab)c", {}, caseless), "abC"), 3);
    EXPECT_FALSE(accepts(parse("(?-i:ab)c", {}, caseless), "ABC"));
    EXPECT_EQ(refused_at("(?s:.)"), 2);
}

TEST(Parse, Definitions_expand_and_nest)
{
    const Definitions_t definitions{
            {"DIGIT", "[0-9]"},
            {"ID", "[a-zA-Z_][a-zA-Z0-9_]*"},
            {"NUMBER", "{DIGIT}+(\\.{DIGIT}+)?"},
    };

    EXPECT_TRUE(accepts(parse("{ID}", definitions), "snake_case2"));
    EXPECT_TRUE(accepts(parse("{NUMBER}", definitions), "3.14"));
    EXPECT_TRUE(accepts(parse("{NUMBER}", definitions), "42"));
    EXPECT_EQ(matched(parse("{NUMBER}", definitions), ".5"), -1);

    // A definition under an operator repeats as a group would.
    EXPECT_TRUE(accepts(parse("{DIGIT}{2}", definitions), "12"));
    EXPECT_EQ(matched(parse("{DIGIT}{2}", definitions), "123"), 2);
}

TEST(Parse, A_parsed_pattern_builds_what_the_combinators_build)
{
    // The identifier pattern, written both ways, agrees on a battery of inputs, matched length included.
    const auto parsed{parse("[a-zA-Z_][a-zA-Z0-9_]*")};

    const auto built{patterns::identifier()};

    for (const std::string_view input : {"abc", "_x9", "9abc", "", "a b", "Zz_", "é", "a-b"})
    {
        EXPECT_EQ(matched(parsed, input), matched(built, input)) << input;
    }
}

TEST(Parse, Refusals_name_the_offset_and_the_reason)
{
    // Anchors, trailing context and start conditions are context, not language; a '$' inside a pattern and a '^'
    // past its start are bytes, as flex reads them.
    EXPECT_EQ(refused_at("^abc"), 0);
    EXPECT_EQ(refused_at("abc$"), 3);
    EXPECT_TRUE(accepts(parse("$[0-9a-f]+"), "$ff"));
    EXPECT_TRUE(accepts(parse("a^b"), "a^b"));
    EXPECT_EQ(refused_at("ab/c"), 2);
    EXPECT_EQ(refused_at("<S>ab"), 0);

    // Structure.
    EXPECT_EQ(refused_at(""), 0);
    EXPECT_EQ(refused_at("a|"), 2);
    EXPECT_EQ(refused_at("()"), 1);
    EXPECT_EQ(refused_at("(ab"), 3);
    EXPECT_EQ(refused_at("ab)"), 2);
    EXPECT_EQ(refused_at("*a"), 0);
    EXPECT_EQ(refused_at("[abc"), 4);
    EXPECT_EQ(refused_at("[z-a]"), 1);
    EXPECT_EQ(refused_at("[[:nope:]]"), 3);
    EXPECT_EQ(refused_at(R"("open)"), 5);
    EXPECT_EQ(refused_at(R"("")"), 2);
    EXPECT_EQ(refused_at("a{3,1}"), 5);
    EXPECT_EQ(refused_at("a{,3}"), 1);
    EXPECT_EQ(refused_at(R"(\x)"), 2);

    // Definitions.
    EXPECT_EQ(refused_at("{NOPE}"), 0);
    EXPECT_EQ(refused_at("a{LOOP}", {{"LOOP", "x{LOOP}"}}), 1);
    EXPECT_EQ(refused_at("{BAD}", {{"BAD", "(a"}}), 0);

    try
    {
        std::ignore = parse("{BAD}", {{"BAD", "(a"}});

        FAIL() << "a faulty definition must be refused";
    }
    catch (const Syntax_error& error)
    {
        EXPECT_NE(std::string_view{error.what()}.find("in definition 'BAD'"), std::string_view::npos);
    }
}

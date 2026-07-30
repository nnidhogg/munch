#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/dfa/tools/graphviz.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"

using namespace munch;
using namespace munch::core;
using namespace munch::regex;

namespace
{
// Set to 1 to enable exporting NFA and DFA graphs in Graphviz DOT format for debugging.
#define DEBUG_DOT 0

class Builder_dbg : public Builder
{
public:
    using Builder::dfa;
    using Builder::nfa;
};

} // namespace

class Lexer_test : public testing::Test
{
protected:
    static auto identifier_regex()
    {
        const auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};

        return identifier;
    }

    static auto integer_literal_regex()
    {
        const auto integer_literal{plus(any_of(Set::digits()))};

        return integer_literal;
    }

    static auto string_literal_regex()
    {
        const auto string_literal{concat(text("\""), kleene(any_of(Set::printable())), text("\""))};

        return string_literal;
    }

    static auto fixed_point_literal_regex()
    {
        const auto fixed_point_literal{concat(plus(any_of(Set::digits())), text("."), plus(any_of(Set::digits())))};

        return fixed_point_literal;
    }

    static auto floating_point_literal_regex()
    {
        const auto any_digit{any_of(Set::digits())};

        const auto sign_part{choice(text("+"), text("-"))};

        const auto exponent_part{concat(choice(text("e"), text("E")), optional(sign_part), plus(any_digit))};

        const auto leading_digits{concat(plus(any_digit), text("."), kleene(any_digit), optional(exponent_part))};

        const auto leading_decimal{concat(text("."), plus(any_digit), optional(exponent_part))};

        const auto forced_exponent{concat(plus(any_digit), exponent_part)};

        const auto fraction_part{choice(leading_digits, leading_decimal, forced_exponent)};

        const auto floating_point_literal{concat(optional(sign_part), fraction_part)};

        return floating_point_literal;
    }

    static auto wide_string_literal_regex()
    {
        const auto wide_string_literal{
                concat(text("L\""), kleene(any_of(Set::printable() + Set::escape())), text("\""))};

        return wide_string_literal;
    }

    static auto character_literal_regex()
    {
        const auto character_literal{concat(text("'"), any_of(Set::printable() + Set::escape()), text("'"))};

        return character_literal;
    }

    static auto wide_character_literal_regex()
    {
        const auto wide_character_literal{concat(text("L'"), any_of(Set::printable() + Set::escape()), text("'"))};

        return wide_character_literal;
    }

    static auto single_line_comment_regex()
    {
        const auto single_line_comment{
                concat(text("//"), kleene(any_of(Set::printable() + Set::escape() - Set::newline())))};

        return single_line_comment;
    }

    static auto multi_line_comment_regex()
    {
        const auto multi_line_comment{concat(text("/*"), kleene(any_of(Set::printable() + Set::escape())), text("*/"))};

        return multi_line_comment;
    }

    void write_dot(const nfa::Nfa& nfa, const std::string& name) const
    {
        const std::filesystem::path dot_path{debug_path_ / (name + ".dot")};

        nfa::tools::Graphviz::to_file(nfa, dot_path);
    }

    void write_dot(const dfa::Dfa& dfa, const std::string& name) const
    {
        const std::filesystem::path dot_path{debug_path_ / (name + ".dot")};

        dfa::tools::Graphviz::to_file(dfa, dot_path);
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Lexer_test, Test_empty)
{
    const Builder builder;

    const auto lexer{builder.build()};

    constexpr std::vector<char> input;

    EXPECT_EQ(lexer.tokenize<int>(input), Lexer::Match<int>(std::nullopt, 0));
}

TEST_F(Lexer_test, Test_keywords)
{
    enum class Token_kind : uint8_t
    {
        Boolean,
        Char,
        String,
        Int8,
        Uint8,
        Int16,
        Uint16,
        Int32,
        Uint32,
        Int64,
        Uint64,
    };

    Builder_dbg builder;

    builder.add_token(text("boolean"), Token_kind::Boolean, 1);
    builder.add_token(text("char"), Token_kind::Char, 1);
    builder.add_token(text("string"), Token_kind::String, 1);
    builder.add_token(text("int8"), Token_kind::Int8, 1);
    builder.add_token(text("uint8"), Token_kind::Uint8, 1);
    builder.add_token(text("int16"), Token_kind::Int16, 1);
    builder.add_token(text("uint16"), Token_kind::Uint16, 1);
    builder.add_token(text("int32"), Token_kind::Int32, 1);
    builder.add_token(text("uint32"), Token_kind::Uint32, 1);
    builder.add_token(text("int64"), Token_kind::Int64, 1);
    builder.add_token(text("uint64"), Token_kind::Uint64, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "keywords_nfa");

    write_dot(builder.dfa(), "keywords_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), Match(Token_kind::Boolean, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), Match(Token_kind::Char, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), Match(Token_kind::String, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), Match(Token_kind::Int8, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), Match(Token_kind::Uint8, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), Match(Token_kind::Int16, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), Match(Token_kind::Uint16, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), Match(Token_kind::Int32, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), Match(Token_kind::Uint32, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), Match(Token_kind::Int64, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), Match(Token_kind::Uint64, 6));
}

TEST_F(Lexer_test, Test_identifier)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "identifier_nfa");

    write_dot(builder.dfa(), "identifier_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name"), Match(Token_kind::Identifier, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("_someVar"), Match(Token_kind::Identifier, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("MyVariable123"), Match(Token_kind::Identifier, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("__Another_var__99"), Match(Token_kind::Identifier, 17));
    EXPECT_EQ(lexer.tokenize<Token_kind>("camelCase"), Match(Token_kind::Identifier, 9));
    EXPECT_EQ(lexer.tokenize<Token_kind>("___"), Match(Token_kind::Identifier, 3));
}

TEST_F(Lexer_test, Test_integer_literal)
{
    enum class Token_kind : uint8_t
    {
        Integer_literal,
    };

    Builder_dbg builder;

    builder.add_token(integer_literal_regex(), Token_kind::Integer_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "integer_literal_nfa");

    write_dot(builder.dfa(), "integer_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("123"), Match(Token_kind::Integer_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("007"), Match(Token_kind::Integer_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1234567890"), Match(Token_kind::Integer_literal, 10));
    EXPECT_EQ(lexer.tokenize<Token_kind>("5"), Match(Token_kind::Integer_literal, 1));
    EXPECT_EQ(lexer.tokenize<Token_kind>("0"), Match(Token_kind::Integer_literal, 1));
}

TEST_F(Lexer_test, Test_string_literal)
{
    enum class Token_kind : uint8_t
    {
        String_literal,
    };

    Builder_dbg builder;

    builder.add_token(string_literal_regex(), Token_kind::String_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "string_literal_nfa");

    write_dot(builder.dfa(), "string_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("\"Hello\""), Match(Token_kind::String_literal, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"\""), Match(Token_kind::String_literal, 2));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"Hello world\""), Match(Token_kind::String_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"\\\"Quote\\\"\""), Match(Token_kind::String_literal, 11));
}

TEST_F(Lexer_test, Test_fixed_point_literal)
{
    enum class Token_kind : uint8_t
    {
        Fixed_point_literal,
    };

    Builder_dbg builder;

    builder.add_token(fixed_point_literal_regex(), Token_kind::Fixed_point_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "fixed_point_literal_nfa");

    write_dot(builder.dfa(), "fixed_point_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("1.2"), Match(Token_kind::Fixed_point_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14"), Match(Token_kind::Fixed_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("123.456"), Match(Token_kind::Fixed_point_literal, 7));

    EXPECT_EQ(lexer.tokenize<Token_kind>("."), Match(std::nullopt, 0));
    EXPECT_EQ(lexer.tokenize<Token_kind>(".1"), Match(std::nullopt, 0));
    EXPECT_EQ(lexer.tokenize<Token_kind>("58."), Match(std::nullopt, 0));
}

TEST_F(Lexer_test, Test_floating_point_literal)
{
    enum class Token_kind : uint8_t
    {
        Floating_point_literal,
    };

    Builder_dbg builder;

    builder.add_token(floating_point_literal_regex(), Token_kind::Floating_point_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "floating_point_literal_nfa");
    write_dot(builder.dfa(), "floating_point_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14159"), Match(Token_kind::Floating_point_literal, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("2e10"), Match(Token_kind::Floating_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("-1.23E-4"), Match(Token_kind::Floating_point_literal, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("+0.5"), Match(Token_kind::Floating_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1e-10"), Match(Token_kind::Floating_point_literal, 5));
}

TEST_F(Lexer_test, Test_wide_string_literals)
{
    enum class Token_kind : uint8_t
    {
        Wide_string_literal,
    };

    Builder_dbg builder;

    builder.add_token(wide_string_literal_regex(), Token_kind::Wide_string_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "wide_string_literal_nfa");

    write_dot(builder.dfa(), "wide_string_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"Hello\""), Match(Token_kind::Wide_string_literal, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"\""), Match(Token_kind::Wide_string_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"Wide world\""), Match(Token_kind::Wide_string_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"\\\"Escaped\\\"\""), Match(Token_kind::Wide_string_literal, 14));
}

TEST_F(Lexer_test, Test_character_literals)
{
    enum class Token_kind : uint8_t
    {
        Character_literal,
    };

    Builder_dbg builder;

    builder.add_token(character_literal_regex(), Token_kind::Character_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "character_literal_nfa");

    write_dot(builder.dfa(), "character_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), Match(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("' '"), Match(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'\n'"), Match(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'\''"), Match(Token_kind::Character_literal, 3));
}

TEST_F(Lexer_test, Test_wide_character_literals)
{
    enum class Token_kind : uint8_t
    {
        Wide_character_literal,
    };

    Builder_dbg builder;

    builder.add_token(wide_character_literal_regex(), Token_kind::Wide_character_literal, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "wide_character_literal_nfa");

    write_dot(builder.dfa(), "wide_character_literal_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), Match(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L' '"), Match(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'\n'"), Match(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'\''"), Match(Token_kind::Wide_character_literal, 4));
}

TEST_F(Lexer_test, Test_single_line_comments)
{
    enum class Token_kind : uint8_t
    {
        Single_line_comment,
    };

    Builder_dbg builder;

    builder.add_token(single_line_comment_regex(), Token_kind::Single_line_comment, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "single_line_comment_nfa");

    write_dot(builder.dfa(), "single_line_comment_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("// This is a comment"), Match(Token_kind::Single_line_comment, 20));
    EXPECT_EQ(lexer.tokenize<Token_kind>("//"), Match(Token_kind::Single_line_comment, 2));
    EXPECT_EQ(lexer.tokenize<Token_kind>("// @#$%^&*()"), Match(Token_kind::Single_line_comment, 12));
}

TEST_F(Lexer_test, Test_multi_line_comments)
{
    enum class Token_kind : uint8_t
    {
        Multi_line_comment,
    };

    Builder_dbg builder;

    builder.add_token(multi_line_comment_regex(), Token_kind::Multi_line_comment, 1);

#if DEBUG_DOT
    write_dot(builder.nfa(), "multi_line_comment_nfa");

    write_dot(builder.dfa(), "multi_line_comment_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("/* comment */"), Match(Token_kind::Multi_line_comment, 13));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* multi\n   line\n   comment */"), Match(Token_kind::Multi_line_comment, 30));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/* start /* nested */ end */"), Match(Token_kind::Multi_line_comment, 28));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/**/"), Match(Token_kind::Multi_line_comment, 4));
}

TEST_F(Lexer_test, Test_combined)
{
    enum class Token_kind : uint8_t
    {
        // Keywords
        Boolean,
        Char,
        String,
        Int8,
        Uint8,
        Int16,
        Uint16,
        Int32,
        Uint32,
        Int64,
        Uint64,

        // Identifier
        Identifier,

        // Literals
        Integer_literal,
        String_literal,
        Wide_string_literal,
        Character_literal,
        Wide_character_literal,
        Fixed_point_literal,
        Floating_point_literal,

        // Comments
        Single_line_comment,
        Multi_line_comment,
    };

    Builder_dbg builder;

    builder.add_token(text("boolean"), Token_kind::Boolean, 1);
    builder.add_token(text("char"), Token_kind::Char, 1);
    builder.add_token(text("string"), Token_kind::String, 1);
    builder.add_token(text("int8"), Token_kind::Int8, 1);
    builder.add_token(text("uint8"), Token_kind::Uint8, 1);
    builder.add_token(text("int16"), Token_kind::Int16, 1);
    builder.add_token(text("uint16"), Token_kind::Uint16, 1);
    builder.add_token(text("int32"), Token_kind::Int32, 1);
    builder.add_token(text("uint32"), Token_kind::Uint32, 1);
    builder.add_token(text("int64"), Token_kind::Int64, 1);
    builder.add_token(text("uint64"), Token_kind::Uint64, 1);

    builder.add_token(identifier_regex(), Token_kind::Identifier, 4);

    builder.add_token(integer_literal_regex(), Token_kind::Integer_literal, 2);
    builder.add_token(string_literal_regex(), Token_kind::String_literal, 2);
    builder.add_token(character_literal_regex(), Token_kind::Character_literal, 2);
    builder.add_token(wide_string_literal_regex(), Token_kind::Wide_string_literal, 2);
    builder.add_token(wide_character_literal_regex(), Token_kind::Wide_character_literal, 2);

    builder.add_token(fixed_point_literal_regex(), Token_kind::Fixed_point_literal, 2);
    builder.add_token(floating_point_literal_regex(), Token_kind::Floating_point_literal, 3);

    builder.add_token(single_line_comment_regex(), Token_kind::Single_line_comment, 0);
    builder.add_token(multi_line_comment_regex(), Token_kind::Multi_line_comment, 0);

#if DEBUG_DOT
    write_dot(builder.nfa(), "combined_nfa");

    write_dot(builder.dfa(), "combined_dfa");
#endif

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), Match(Token_kind::Boolean, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), Match(Token_kind::Char, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), Match(Token_kind::String, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), Match(Token_kind::Int8, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), Match(Token_kind::Uint8, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), Match(Token_kind::Int16, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), Match(Token_kind::Uint16, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), Match(Token_kind::Int32, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), Match(Token_kind::Uint32, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), Match(Token_kind::Int64, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), Match(Token_kind::Uint64, 6));

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name_1"), Match(Token_kind::Identifier, 15));

    EXPECT_EQ(lexer.tokenize<Token_kind>("1234"), Match(Token_kind::Integer_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"hello world\""), Match(Token_kind::String_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), Match(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"wide string\""), Match(Token_kind::Wide_string_literal, 14));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), Match(Token_kind::Wide_character_literal, 4));

    EXPECT_EQ(lexer.tokenize<Token_kind>("123.45"), Match(Token_kind::Fixed_point_literal, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14159e+2"), Match(Token_kind::Floating_point_literal, 10));

    EXPECT_EQ(lexer.tokenize<Token_kind>("// a comment"), Match(Token_kind::Single_line_comment, 12));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/* a comment */"), Match(Token_kind::Multi_line_comment, 15));
}

TEST_F(Lexer_test, Tokenize_all_matches_sequential_tokenization)
{
    enum class Token_kind : uint8_t
    {
        Keyword,
        Identifier,
        Integer_literal,
        Whitespace,
    };

    Builder_dbg builder;

    builder.add_token(text("boolean"), Token_kind::Keyword, 1);
    builder.add_token(identifier_regex(), Token_kind::Identifier, 4);
    builder.add_token(integer_literal_regex(), Token_kind::Integer_literal, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 2);

    const auto lexer{builder.build()};

    const std::string input{"boolean x 1234 boolean_ish  42 y7"};

    using Match_t = std::pair<Token_kind, std::size_t>;

    std::vector<Match_t> batch;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&batch](const Token_kind token, const std::size_t length) { batch.emplace_back(token, length); })};

    EXPECT_EQ(consumed, input.size());

    std::vector<Match_t> sequential;

    for (std::size_t offset{0}; offset < input.size();)
    {
        const auto [token, length]{lexer.tokenize<Token_kind>(input.cbegin() + offset, input.cend())};

        ASSERT_TRUE(token.has_value());
        ASSERT_GT(length, 0u);

        sequential.emplace_back(*token, length);

        offset += length;
    }

    EXPECT_EQ(batch, sequential);
}

TEST_F(Lexer_test, Tokenize_all_stops_at_unmatched_input)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"abc def @rest"};

    std::size_t tokens{0};

    const auto consumed{
            lexer.tokenize_all<Token_kind>(input, [&tokens](const Token_kind, const std::size_t) { ++tokens; })};

    EXPECT_EQ(consumed, 8u);
    EXPECT_EQ(tokens, 4u);

    EXPECT_EQ(lexer.tokenize_all<Token_kind>(std::string{}, [](const Token_kind, const std::size_t) {}), 0u);
}

TEST_F(Lexer_test, Tokenize_all_stops_when_the_sink_returns_false)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"one two three"};

    std::size_t tokens{0};

    // Stop after the second token; the stopping token still counts as tokenized.
    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&tokens](const Token_kind, const std::size_t) { return ++tokens < 2; })};

    EXPECT_EQ(tokens, 2u);
    EXPECT_EQ(consumed, 4u);
}

TEST_F(Lexer_test, Split_points_depend_on_the_token_set)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Newline,
    };

    // A single-character newline token: '\n' is consumed only at a token start, so it certifies as a split point.
    Builder_dbg single;

    single.add_token(identifier_regex(), Token_kind::Identifier, 1);
    single.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    single.add_token(text("\n"), Token_kind::Newline, 1);

    const auto single_lexer{single.build()};

    EXPECT_TRUE(single_lexer.is_split_point('\n'));
    EXPECT_FALSE(single_lexer.is_split_point('a'));
    EXPECT_FALSE(single_lexer.is_split_point(' '));

    // A newline-run token: "\n\n" is one token, so splitting at the second '\n' would change the tokenization,
    // and the analysis correctly refuses what a split-at-newline heuristic would wrongly allow.
    Builder_dbg run;

    run.add_token(identifier_regex(), Token_kind::Identifier, 1);
    run.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    run.add_token(plus(any_of(Set::newline())), Token_kind::Newline, 1);

    const auto run_lexer{run.build()};

    EXPECT_FALSE(run_lexer.is_split_point('\n'));
}

TEST_F(Lexer_test, Long_runs_tokenize_identically_to_the_per_token_scan)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Number,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 1);

    const auto lexer{builder.build()};

    const std::string input{
            std::string(30, ' ') + "an_identifier_well_past_the_probe_distance " + std::string(25, '7') + " x 9 " +
            std::string(18, 'y')};

    std::vector<std::pair<Token_kind, std::size_t>> batch;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&batch](const Token_kind token, const std::size_t length) { batch.emplace_back(token, length); })};

    EXPECT_EQ(consumed, input.size());

    std::vector<std::pair<Token_kind, std::size_t>> single;

    for (std::size_t offset{0}; offset < input.size();)
    {
        const auto [token, length]{lexer.tokenize<Token_kind>(input.cbegin() + offset, input.cend())};

        ASSERT_TRUE(token.has_value());
        ASSERT_GT(length, 0U);

        single.emplace_back(*token, length);

        offset += length;
    }

    EXPECT_EQ(batch, single);
}

TEST_F(Lexer_test, Chunk_boundaries_land_on_certified_split_points)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    std::string input;

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_GT(boundaries[index], boundaries[index - 1]);
        EXPECT_TRUE(lexer.is_split_point(input[boundaries[index]]));
    }
}

TEST_F(Lexer_test, Chunk_boundaries_degenerate_when_nothing_certifies)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    // Identifier characters continue identifiers and whitespace continues runs, so no byte certifies and the
    // plan is one chunk: the serial scan.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"alpha beta gamma delta epsilon zeta eta theta"};

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());
}

TEST_F(Lexer_test, Parallel_tokenization_matches_the_serial_stream)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    std::string input;

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    ASSERT_EQ(
            lexer.tokenize_all<Token_kind>(
                    input,
                    [&serial](const Token_kind token, const std::size_t length) {
                        serial.emplace_back(token, length);
                    }),
            input.size());

    constexpr std::size_t chunks{4};

    std::array<std::vector<std::pair<Token_kind, std::size_t>>, chunks> streams;

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(
            input, chunks, [&streams](const std::size_t chunk, const Token_kind token, const std::size_t length) {
                streams[chunk].emplace_back(token, length);
            })};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    ASSERT_EQ(consumed.size(), boundaries.size() - 1);

    std::vector<std::pair<Token_kind, std::size_t>> spliced;

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        EXPECT_EQ(consumed[chunk], boundaries[chunk + 1] - boundaries[chunk]);

        spliced.insert(spliced.end(), streams[chunk].cbegin(), streams[chunk].cend());
    }

    EXPECT_EQ(spliced, serial);
}

TEST_F(Lexer_test, Parallel_tokenization_reports_a_rejected_chunk)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    std::string input;

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    // A byte no token accepts, planted in the last quarter: only that chunk stops short, at exactly its offset.
    const auto poison{(input.size() * 7) / 8};

    input[poison] = '@';

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(
            input, 4, [](const std::size_t, const Token_kind, const std::size_t) {})};

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(consumed.size(), boundaries.size() - 1);

    for (std::size_t chunk{0}; chunk + 1 < consumed.size(); ++chunk)
    {
        EXPECT_EQ(consumed[chunk], boundaries[chunk + 1] - boundaries[chunk]);
    }

    EXPECT_EQ(boundaries[consumed.size() - 1] + consumed.back(), poison);
}

TEST_F(Lexer_test, Diagnose_reports_shadowed_keywords_as_dead)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword_if,
        Whitespace,
    };

    Builder_dbg builder;

    // The identifier pattern outranks the keyword, so "if" always tokenizes as an identifier and the keyword can
    // never win any input.
    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(text("if"), Token_kind::Keyword_if, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto diagnostics{builder.diagnose()};

    EXPECT_EQ(diagnostics.dead_tokens, (std::vector<std::size_t>{static_cast<std::size_t>(Token_kind::Keyword_if)}));
    EXPECT_TRUE(diagnostics.equal_priority_ties.empty());
}

TEST_F(Lexer_test, Diagnose_passes_a_healthy_grammar)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword_if,
        Number,
        Whitespace,
    };

    Builder_dbg builder;

    builder.add_token(text("if"), Token_kind::Keyword_if, 1);
    builder.add_token(identifier_regex(), Token_kind::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(diagnostics.dead_tokens.empty());
    EXPECT_TRUE(diagnostics.equal_priority_ties.empty());
}

TEST_F(Lexer_test, Diagnose_reports_equal_priority_ties)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword_if,
    };

    Builder_dbg builder;

    // Both accept the spelling "if" at the same priority, so the build breaks the tie by registered value: the
    // identifier wins, which also leaves the keyword dead.
    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(text("if"), Token_kind::Keyword_if, 1);

    const auto diagnostics{builder.diagnose()};

    EXPECT_EQ(
            diagnostics.equal_priority_ties, (std::vector<std::pair<std::size_t, std::size_t>>{
                                                     {static_cast<std::size_t>(Token_kind::Identifier),
                                                      static_cast<std::size_t>(Token_kind::Keyword_if)}}));
    EXPECT_EQ(diagnostics.dead_tokens, (std::vector<std::size_t>{static_cast<std::size_t>(Token_kind::Keyword_if)}));
}

TEST_F(Lexer_test, Diagnose_reports_a_tie_that_leaves_both_tokens_alive)
{
    enum class Token_kind : uint8_t
    {
        Keyword_if,
        Identifier,
    };

    Builder_dbg builder;

    // Reversed registered values: the keyword wins its own spelling by the tie break, every other identifier
    // spelling still belongs to the identifier, so both stay alive but the tie is still an accident to report.
    builder.add_token(text("if"), Token_kind::Keyword_if, 1);
    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);

    const auto diagnostics{builder.diagnose()};

    EXPECT_EQ(
            diagnostics.equal_priority_ties, (std::vector<std::pair<std::size_t, std::size_t>>{
                                                     {static_cast<std::size_t>(Token_kind::Keyword_if),
                                                      static_cast<std::size_t>(Token_kind::Identifier)}}));
    EXPECT_TRUE(diagnostics.dead_tokens.empty());
}

TEST_F(Lexer_test, Diagnose_ignores_equal_priorities_over_disjoint_languages)
{
    enum class Token_kind : uint8_t
    {
        Number,
        Semicolon,
    };

    Builder_dbg builder;

    // Equal priorities are only a problem when some input could go either way; disjoint token languages never
    // meet in an accepting state set.
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(diagnostics.dead_tokens.empty());
    EXPECT_TRUE(diagnostics.equal_priority_ties.empty());
}

TEST_F(Lexer_test, Diagnose_accepts_an_empty_builder)
{
    const Builder_dbg builder;

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(diagnostics.dead_tokens.empty());
    EXPECT_TRUE(diagnostics.equal_priority_ties.empty());
}

TEST_F(Lexer_test, Diagnose_treats_liveness_per_token_not_per_pattern)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword,
    };

    Builder_dbg builder;

    // The keyword is registered twice: the "if" pattern is fully shadowed by the identifier, the ";" pattern is
    // not. Liveness belongs to the token value, so one winning pattern keeps the token alive.
    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(text("if"), Token_kind::Keyword, 2);
    builder.add_token(text(";"), Token_kind::Keyword, 2);

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(diagnostics.dead_tokens.empty());
    EXPECT_TRUE(diagnostics.equal_priority_ties.empty());
}

TEST_F(Lexer_test, Diagnose_reports_every_pair_of_a_three_way_tie)
{
    enum class Token_kind : uint8_t
    {
        First,
        Second,
        Third,
    };

    Builder_dbg builder;

    // All three accept exactly ";" at the same priority, so every pair of the three collides.
    builder.add_token(text(";"), Token_kind::First, 1);
    builder.add_token(text(";"), Token_kind::Second, 1);
    builder.add_token(text(";"), Token_kind::Third, 1);

    const auto diagnostics{builder.diagnose()};

    const std::vector<std::pair<std::size_t, std::size_t>> expected{
            {static_cast<std::size_t>(Token_kind::First), static_cast<std::size_t>(Token_kind::Second)},
            {static_cast<std::size_t>(Token_kind::First), static_cast<std::size_t>(Token_kind::Third)},
            {static_cast<std::size_t>(Token_kind::Second), static_cast<std::size_t>(Token_kind::Third)}};

    EXPECT_EQ(diagnostics.equal_priority_ties, expected);
    EXPECT_EQ(
            diagnostics.dead_tokens,
            (std::vector<std::size_t>{
                    static_cast<std::size_t>(Token_kind::Second), static_cast<std::size_t>(Token_kind::Third)}));
}

TEST_F(Lexer_test, Split_points_refuse_a_reentrant_start_state)
{
    enum class Token_kind : uint8_t
    {
        Run,
    };

    // kleene minimizes to an accepting start state with a self-loop, so the start state is reachable again after
    // consuming input: 'a' can continue a token mid-scan even though only the start state consumes it, and
    // splitting "aa" would turn one length-two token into two length-one tokens.
    Builder_dbg builder;

    builder.add_token(kleene(text("a")), Token_kind::Run, 1);

    const auto lexer{builder.build()};

    EXPECT_FALSE(lexer.is_split_point('a'));

    const auto boundaries{lexer.chunk_boundaries(std::string{"aa"}, 2)};

    ASSERT_EQ(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.back(), 2U);
}

TEST_F(Lexer_test, Split_points_refuse_reentry_through_a_cycle)
{
    enum class Token_kind : uint8_t
    {
        Item,
    };

    // (ab)*c returns to the start state after every "ab": minimization merges the post-"ab" state with the start
    // state, so the re-entry is a cycle rather than a self-loop, and 'c' must not certify even though only the
    // start state consumes it. Splitting "abc" before the 'c' would orphan an unmatchable "ab" chunk.
    Builder_dbg builder;

    builder.add_token(concat(kleene(text("ab")), text("c")), Token_kind::Item, 1);

    const auto lexer{builder.build()};

    EXPECT_FALSE(lexer.is_split_point('c'));
    EXPECT_FALSE(lexer.is_split_point('a'));
}

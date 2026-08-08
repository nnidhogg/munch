#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/tools/graphviz.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/unicode.hpp"

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

TEST_F(Lexer_test, Split_windows_generalize_the_byte_certificate)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword,
        Number,
        Operator,
    };

    // The refutation grammar from the certified-windows study: the byte certificate reaches only "a", which
    // cannot cut inside the window, while the three-byte window "abx" pins the covering token of its final byte
    // to origin 1, the occurrence tokenizing as a|bx.
    Builder_dbg builder;

    builder.add_token(text("a"), Token_kind::Identifier, 2);
    builder.add_token(text("abc"), Token_kind::Keyword, 1);
    builder.add_token(text("bx"), Token_kind::Number, 2);
    builder.add_token(text("x"), Token_kind::Operator, 2);

    const auto lexer{builder.build()};

    const auto abx{lexer.is_split_window("abx")};

    ASSERT_TRUE(abx.has_value());
    EXPECT_EQ(*abx, 1U);

    // The specialization theorem, executable: at length one the window decision coincides with the shipped byte
    // predicate on every byte value, certificates and refusals alike.
    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        EXPECT_EQ(lexer.is_split_window({&byte, 1}).has_value(), lexer.is_split_point(byte));
    }
}

TEST_F(Lexer_test, Split_windows_stay_conservative_and_scoped)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Keyword,
        Operator,
    };

    // Strictness: over {a, ab, b} the input "ab" always tokenizes as the single token ab, making it a true
    // certificate at origin 0, and the conservative model still refuses it: the accepting a seeds a competing
    // origin for b and unanimity is lost. Refusal is model-relative, never proof that no certificate exists.
    Builder_dbg strict;

    strict.add_token(text("a"), Token_kind::Identifier, 1);
    strict.add_token(text("ab"), Token_kind::Keyword, 1);
    strict.add_token(text("b"), Token_kind::Operator, 1);

    EXPECT_FALSE(strict.build().is_split_window("ab").has_value());

    // Vacuity: over {0, 00, 01} the model certifies "1001" at origin 2, and no completely tokenizable input
    // contains that window. The call answers the model; the certificate is conditional on occurrence, and a
    // caller holds an occurrence only by finding the window in input at hand.
    Builder_dbg vacuous;

    vacuous.add_token(text("0"), Token_kind::Identifier, 1);
    vacuous.add_token(text("00"), Token_kind::Keyword, 1);
    vacuous.add_token(text("01"), Token_kind::Operator, 1);

    const auto model{vacuous.build().is_split_window("1001")};

    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(*model, 2U);

    // A grammar whose search space exhausts: a+ accepts after every byte, every offset stays in play, and no
    // window of any length certifies.
    Builder_dbg unbounded;

    unbounded.add_token(plus(text("a")), Token_kind::Identifier, 1);

    const auto plus_lexer{unbounded.build()};

    EXPECT_FALSE(plus_lexer.is_split_window("a").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aa").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aaaaa").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aaaaaa").has_value());

    // Nullable token sets sit outside the window soundness proof and are refused outright, matching the byte
    // predicate's withdrawal of its initial-state exemption. The empty window certifies nothing either.
    Builder_dbg nullable;

    nullable.add_token(kleene(text("a")), Token_kind::Identifier, 1);

    EXPECT_FALSE(nullable.build().is_split_window("a").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("").has_value());
}

TEST_F(Lexer_test, Window_fallback_plans_parallel_cuts_where_no_byte_certifies)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
        Operator,
        String,
    };

    // The study's motivating shape: whitespace runs swallow the newline, string bodies swallow every printable
    // byte, and no single byte certifies; the two-byte window "\n!" still pins a token start at the '!'.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::Whitespace, 1);
    builder.add_token(text("!"), Token_kind::Operator, 1);
    builder.add_token(concat(text("\""), kleene(any_of(Set::printable())), text("\"")), Token_kind::String, 2);

    const auto lexer{builder.build()};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(symbol)));
    }

    const auto window{lexer.is_split_window("\n!")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    std::string input;

    for (int block{0}; block < 64; ++block)
    {
        input += "alpha beta \"quoted text!\" gamma\n!delta\n!";
    }

    // The default planner keeps the unconditional byte contract and degenerates here; window recovery is the
    // explicit sibling with its documented completely-tokenizable condition.
    ASSERT_EQ(lexer.chunk_boundaries(input, 8), (std::vector<std::size_t>{0, input.size()}));

    const auto boundaries{lexer.chunk_boundaries_with_windows(input.begin(), input.end(), 8)};

    ASSERT_GT(boundaries.size(), 2U);

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        EXPECT_GT(boundaries[index], boundaries[index - 1]);
    }

    // The semantic assertion: the concatenated chunk-local streams equal the serial stream, token for token.
    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    ASSERT_EQ(consumed, input.size());

    std::vector<std::pair<Token_kind, std::size_t>> chunked;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{input.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        const auto part{lexer.tokenize_all<Token_kind>(
                chunk,
                [&chunked](const Token_kind kind, const std::size_t length) { chunked.emplace_back(kind, length); })};

        ASSERT_EQ(part, chunk.size());
    }

    EXPECT_EQ(chunked, serial);
}

TEST_F(Lexer_test, Window_fallback_degrades_honestly_and_the_equality_check_has_teeth)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    // A grammar the window search exhausts plans the single whole-input chunk rather than cutting unsafely.
    Builder_dbg unbounded;

    unbounded.add_token(plus(text("a")), Token_kind::Identifier, 1);

    const std::string runs(64, 'a');

    const auto exhausted{unbounded.build().chunk_boundaries_with_windows(runs.begin(), runs.end(), 4)};

    EXPECT_EQ(exhausted, (std::vector<std::size_t>{0, runs.size()}));

    // A nullable set contributes no windows, the window proof excludes it, and with no byte certificate either
    // it takes the same degradation without any window search.
    Builder_dbg nullable;

    nullable.add_token(kleene(text("a")), Token_kind::Identifier, 1);
    nullable.add_token(text(" "), Token_kind::Whitespace, 1);

    const auto refused{nullable.build().chunk_boundaries_with_windows(runs.begin(), runs.end(), 4)};

    EXPECT_EQ(refused, (std::vector<std::size_t>{0, runs.size()}));

    // The teeth of the equality check above: a deliberately wrong cut inside a token must not reproduce the
    // serial stream, so the assertion that plans do reproduce it is falsifiable, not decorative.
    Builder_dbg words;

    words.add_token(identifier_regex(), Token_kind::Identifier, 1);
    words.add_token(text(" "), Token_kind::Whitespace, 1);

    const auto lexer{words.build()};

    const std::string input{"alpha beta"};

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); });

    std::vector<std::pair<Token_kind, std::size_t>> wrongly_chunked;

    for (const auto& chunk : {std::string_view{input}.substr(0, 2), std::string_view{input}.substr(2)})
    {
        lexer.tokenize_all<Token_kind>(chunk, [&wrongly_chunked](const Token_kind kind, const std::size_t length) {
            wrongly_chunked.emplace_back(kind, length);
        });
    }

    EXPECT_NE(wrongly_chunked, serial);
}

TEST_F(Lexer_test, Malformed_input_keeps_the_default_prefix_guarantee_and_windows_promise_nothing)
{
    enum class Token_kind : uint8_t
    {
        T,
    };

    // The reviewer's minimal vector: over {a, aa, ab, bc} the serial scan of "abc" takes ab and fails at c,
    // consuming two of three. The default plan stays byte-only and reproduces exactly that. The window plan
    // legally cuts at the certified "bc" occurrence, both fragments consume fully, and the concatenation is not
    // even a prefix of the serial stream: full per-chunk consumption proves nothing about the whole input, which
    // is precisely the conditional contract chunk_boundaries_with_windows() documents.
    Builder_dbg builder;

    builder.add_token(choice(choice(text("a"), text("aa")), choice(text("ab"), text("bc"))), Token_kind::T, 1);

    const auto lexer{builder.build()};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(symbol)));
    }

    const auto window{lexer.is_split_window("bc")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 0U);

    const std::string input{"abc"};

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    EXPECT_EQ(consumed, 2U);
    EXPECT_EQ(serial, (std::vector<std::pair<Token_kind, std::size_t>>{{Token_kind::T, 2}}));

    EXPECT_EQ(lexer.chunk_boundaries(input, 3), (std::vector<std::size_t>{0, input.size()}));

    std::vector<std::pair<Token_kind, std::size_t>> parallel;

    const auto per_chunk{lexer.tokenize_all_parallel<Token_kind>(
            input, 3, [&parallel](const std::size_t, const Token_kind kind, const std::size_t length) {
                parallel.emplace_back(kind, length);
            })};

    EXPECT_EQ(per_chunk, (std::vector<std::size_t>{2}));
    EXPECT_EQ(parallel, serial);

    const auto windowed{lexer.chunk_boundaries_with_windows(input, 3)};

    EXPECT_EQ(windowed, (std::vector<std::size_t>{0, 1, 3}));

    std::vector<std::pair<Token_kind, std::size_t>> rejoined;

    for (std::size_t index{1}; index < windowed.size(); ++index)
    {
        const std::string_view chunk{input.data() + windowed[index - 1], windowed[index] - windowed[index - 1]};

        const auto part{lexer.tokenize_all<Token_kind>(
                chunk,
                [&rejoined](const Token_kind kind, const std::size_t length) { rejoined.emplace_back(kind, length); })};

        EXPECT_EQ(part, chunk.size());
    }

    ASSERT_EQ(rejoined, (std::vector<std::pair<Token_kind, std::size_t>>{{Token_kind::T, 1}, {Token_kind::T, 2}}));
    EXPECT_NE(rejoined, serial);
}

TEST_F(Lexer_test, Default_planning_uses_the_exact_certificate_never_the_relaxed_one)
{
    enum class Token_kind : uint8_t
    {
        Run,
        Operator,
    };

    // With the run declared ignored, the relaxed map certifies bytes the exact map refuses; the default planner
    // must plan with the exact certificate only, or malformed input loses the serial-prefix guarantee the
    // relaxed certificate never carried.
    Builder_dbg builder;

    builder.add_token(plus(text("a")), Token_kind::Run, 1);
    builder.add_token(text("b"), Token_kind::Operator, 1);
    builder.set_ignored_tokens({static_cast<std::size_t>(Token_kind::Run)});

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_point_ignoring('a'));
    ASSERT_FALSE(lexer.is_split_point('a'));

    const std::string input{"aaaab"};

    // The exact map certifies only 'b', so the only interior cut available to the default plan is at the 'b'.
    const auto boundaries{lexer.chunk_boundaries(input, 3)};

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_TRUE(lexer.is_split_point(input[boundaries[index]]));

        EXPECT_EQ(input[boundaries[index]], 'b');
    }
}

TEST_F(Lexer_test, Window_refusals_pin_the_nullable_guard_and_the_live_target_filter)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Operator,
    };

    // A nullable set whose initial state is NOT re-entrant: optional(a) accepts emptily and nothing returns to
    // the start, so a model that merely forgot the nullable guard would certify "b" at 0. The refusal pins the
    // guard itself, not a coincidental re-entrancy.
    Builder_dbg nullable;

    nullable.add_token(optional(text("a")), Token_kind::Identifier, 1);
    nullable.add_token(text("b"), Token_kind::Operator, 1);

    EXPECT_FALSE(nullable.build().is_split_window("b").has_value());

    // The non-re-entrant rename guard, pinned by the language (ab)*a: reading "a" from the initial state must
    // NOT be treated as beginning a token there, because live paths re-enter the start; the completely
    // tokenizable input "aba" is one token covering its final "a" from offset 0, so certifying ("a", 0) would be
    // a false certificate. A model that dropped the re-entrancy condition certifies it.
    Builder_dbg reentrant;

    reentrant.add_token(concat(kleene(text("ab")), text("a")), Token_kind::Identifier, 1);

    EXPECT_FALSE(reentrant.build().is_split_window("a").has_value());

    // A token whose tail is the empty language leaves live-looking transitions into dead states: over
    // {ab followed by nothing acceptable, b} the window "ab" must empty the cloud and refuse, because no
    // accepting future exists past the a. Dropping the live-target filter would certify it at 0 instead.
    Builder_dbg dead_tail;

    dead_tail.add_token(concat(text("ab"), any_of(Set{})), Token_kind::Identifier, 1);
    dead_tail.add_token(text("b"), Token_kind::Operator, 1);

    const auto dead_lexer{dead_tail.build()};

    EXPECT_FALSE(dead_lexer.is_split_window("ab").has_value());

    // The acceptance-gated seed has its own live-target filter, and the window "a" reaches it alone: every
    // direct history steps dead, so the only hypothesis a mutant could keep alive is the fresh seed landing in
    // the dead tail. Dropping just that filter certifies "a" at 0 where the cloud must empty and refuse.
    EXPECT_FALSE(dead_lexer.is_split_window("a").has_value());
}

TEST_F(Lexer_test, Malformed_input_keeps_the_serial_prefix_across_real_cuts)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Separator,
    };

    // Certified cuts on both sides of a malformed byte: serial fails at the '?', earlier chunks reproduce its
    // stream exactly, and later chunks scan independently, so the serial stream is a strict prefix of the
    // concatenation, which is precisely tokenize_all_parallel()'s documented malformed-input promise.
    Builder_dbg builder;

    builder.add_token(text("a"), Token_kind::Identifier, 1);
    builder.add_token(text(";"), Token_kind::Separator, 1);

    const auto lexer{builder.build()};

    const std::string input{"a;?;a"};

    const auto boundaries{lexer.chunk_boundaries(input, 3)};

    ASSERT_GT(boundaries.size(), 2U);

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    ASSERT_EQ(consumed, 2U);

    std::vector<std::vector<std::pair<Token_kind, std::size_t>>> streams(boundaries.size() - 1);

    const auto per_chunk{lexer.tokenize_all_parallel<Token_kind>(
            input, 3, [&streams](const std::size_t chunk, const Token_kind kind, const std::size_t length) {
                streams[chunk].emplace_back(kind, length);
            })};

    ASSERT_EQ(per_chunk.size(), boundaries.size() - 1);

    std::vector<std::pair<Token_kind, std::size_t>> joined;

    for (const auto& stream : streams)
    {
        joined.insert(joined.end(), stream.begin(), stream.end());
    }

    ASSERT_GE(joined.size(), serial.size());

    for (std::size_t index{0}; index < serial.size(); ++index)
    {
        EXPECT_EQ(joined[index], serial[index]);
    }

    EXPECT_GT(joined.size(), serial.size());
}

TEST_F(Lexer_test, Window_planner_reaches_the_four_byte_study_case)
{
    enum class Token_kind : uint8_t
    {
        Whitespace,
        Comment,
        Star,
        Slash,
    };

    // The study's principal four-byte certificate: block comments over an operator alphabet, where the pair */
    // reads as a closer in comment context and as two operators otherwise, so no shorter window resolves the
    // origin. The planner promises windows of two to four bytes; this pins the upper bound at the length the
    // paper's table actually needs.
    Builder_dbg builder;

    const auto not_star{any_of(Set::all() - Set{'*'})};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'}))};

    builder.add_token(plus(any_of(Set{'\t'})), Token_kind::Whitespace, 1);
    builder.add_token(
            concat(text("/*"), kleene(choice(not_star, stars_then_other)), plus(any_of(Set{'*'})), text("/")),
            Token_kind::Comment, 1);
    builder.add_token(text("*"), Token_kind::Star, 2);
    builder.add_token(text("/"), Token_kind::Slash, 2);

    const auto lexer{builder.build()};

    const auto window{lexer.is_split_window("\t*/\t")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 3U);

    const std::string input{"\t*/\t\t*/\t"};

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 2)};

    ASSERT_EQ(boundaries.size(), 3U);
    EXPECT_EQ(boundaries[1], 7U);
}

TEST_F(Lexer_test, Complete_c_like_grammar_windows_end_to_end)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Integer,
        String,
        Comment,
        Whitespace,
        Operator,
    };

    // The study's conventional row as a complete grammar: identifiers, integers, strings whose bodies exclude
    // the newline, line comments that end before it, whitespace runs that include it, and single-byte
    // operators. No byte certifies, and the two-byte window of a newline followed by an operator resolves the
    // origin at the operator, which must begin a token.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Integer, 2);
    builder.add_token(concat(text("\""), kleene(any_of(Set::printable())), text("\"")), Token_kind::String, 2);
    builder.add_token(concat(text("//"), kleene(any_of(Set::all() - Set{'\n'}))), Token_kind::Comment, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::Whitespace, 1);

    for (const auto op : {"!", "=", "+", ";", "(", ")", "{", "}"})
    {
        builder.add_token(text(op), Token_kind::Operator, 3);
    }

    const auto lexer{builder.build()};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(symbol)));
    }

    const auto window{lexer.is_split_window("\n!")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    // Source with operator-initial lines, the occurrence shape the campaign corpus discloses as favorable.
    std::string input;

    for (int block{0}; block < 32; ++block)
    {
        input += "count = count + 42; // trailing note\n";
        input += "!(flag) name = \"a (string) with // inside\";\n";
        input += "!done = 1;\n";
    }

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    ASSERT_EQ(consumed, input.size());

    std::vector<std::pair<Token_kind, std::size_t>> rejoined;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{input.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        const auto part{lexer.tokenize_all<Token_kind>(
                chunk,
                [&rejoined](const Token_kind kind, const std::size_t length) { rejoined.emplace_back(kind, length); })};

        ASSERT_EQ(part, chunk.size());
    }

    EXPECT_EQ(rejoined, serial);
}

TEST_F(Lexer_test, Complete_json_grammar_windows_end_to_end)
{
    enum class Token_kind : uint8_t
    {
        String,
        Number,
        Punct,
        Whitespace,
    };

    // The study's JSON row as a complete grammar: strings, integers, structural punctuation, and whitespace runs
    // over space, tab, and newline. The tab-then-quote window certifies at the quote, and pretty-printed JSON
    // contains that occurrence at every indented key, so the favorable shape here is the format's own.
    Builder_dbg builder;

    builder.add_token(
            concat(text("\""), kleene(any_of(Set::printable() - Set{'"'})), text("\"")), Token_kind::String, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 2);

    for (const auto punct : {"{", "}", "[", "]", ":", ","})
    {
        builder.add_token(text(punct), Token_kind::Punct, 1);
    }

    builder.add_token(plus(any_of(Set{' '} + '\t' + '\n')), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(symbol)));
    }

    const auto window{lexer.is_split_window("\t\"")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    std::string input{"{\n"};

    for (int entry{0}; entry < 64; ++entry)
    {
        input += "\t\"key\": [1, 22, 333],\n";
        input += "\t\"name\": \"value with [brackets] and 42\",\n";
    }

    input += "\t\"last\": 0\n}";

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    ASSERT_EQ(consumed, input.size());

    std::vector<std::pair<Token_kind, std::size_t>> rejoined;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{input.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        const auto part{lexer.tokenize_all<Token_kind>(
                chunk,
                [&rejoined](const Token_kind kind, const std::size_t length) { rejoined.emplace_back(kind, length); })};

        ASSERT_EQ(part, chunk.size());
    }

    EXPECT_EQ(rejoined, serial);
}

TEST_F(Lexer_test, Complete_c_like_grammar_with_block_comments_windows_end_to_end)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Integer,
        String,
        Line_comment,
        Block_comment,
        Whitespace,
        Operator,
    };

    // The conventional row extended with multiline block comments. A newline may now sit inside a comment, so
    // every newline-anchored window dies, and only the four-byte family around the comment closer survives: a
    // guard byte ahead of */ against reading the star as an opener's star, and a newline on one side to defeat
    // line comments and strings. The origin lands directly after the closer, the study's four-byte mechanism
    // inside a complete grammar.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Integer, 2);
    builder.add_token(concat(text("\""), kleene(any_of(Set::printable())), text("\"")), Token_kind::String, 2);
    builder.add_token(concat(text("//"), kleene(any_of(Set::all() - Set{'\n'}))), Token_kind::Line_comment, 1);

    const auto not_star{any_of(Set::all() - Set{'*'})};

    const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'}))};

    builder.add_token(
            concat(text("/*"), kleene(choice(not_star, stars_then_other)), plus(any_of(Set{'*'})), text("/")),
            Token_kind::Block_comment, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::Whitespace, 1);

    for (const auto op : {"!", "=", "+", ";", "(", ")", "{", "}", "*", "/"})
    {
        builder.add_token(text(op), Token_kind::Operator, 3);
    }

    const auto lexer{builder.build()};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        EXPECT_FALSE(lexer.is_split_point(static_cast<char>(symbol)));
    }

    // The conventional row's two-byte window is gone, a block comment can contain both of its bytes.
    EXPECT_FALSE(lexer.is_split_window("\n!").has_value());

    // The study case's own window is gone too, a line comment can contain all four bytes.
    EXPECT_FALSE(lexer.is_split_window("\t*/\t").has_value());

    // A closer at the very front is refused, the leading star could be an opener's star with the slash and the
    // newline still inside the comment.
    EXPECT_FALSE(lexer.is_split_window("*/\n!").has_value());

    const auto window{lexer.is_split_window(" */\n")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 3U);

    // Source where block comments close at line ends, the occurrence shape the surviving windows require.
    std::string input;

    for (int block{0}; block < 24; ++block)
    {
        input += "/* block header\n   spanning two lines */\n";
        input += "!(flag) total = total + 7; // trailing note\n";
        input += "text = \"keep /* this */ inline\";\n";
        input += "value = value * 2 / 4; /* closing note */\n";
    }

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); })};

    ASSERT_EQ(consumed, input.size());

    std::vector<std::pair<Token_kind, std::size_t>> rejoined;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{input.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        const auto part{lexer.tokenize_all<Token_kind>(
                chunk,
                [&rejoined](const Token_kind kind, const std::size_t length) { rejoined.emplace_back(kind, length); })};

        ASSERT_EQ(part, chunk.size());
    }

    EXPECT_EQ(rejoined, serial);
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

TEST_F(Lexer_test, Chunk_boundaries_recover_windows_when_no_byte_certifies)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    // Identifier characters continue identifiers and whitespace continues runs, so no byte certifies; before
    // certified windows this plan degenerated to the single serial chunk. The two-byte window of a whitespace
    // byte followed by a letter certifies at origin 1, whitespace cannot continue an identifier and a letter
    // cannot continue a run, so the fallback now cuts at token starts the byte certificate cannot see.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const auto window{lexer.is_split_window(" a")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    const std::string input{"alpha beta gamma delta epsilon zeta eta theta"};

    ASSERT_EQ(lexer.chunk_boundaries(input, 4), (std::vector<std::size_t>{0, input.size()}));

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 4)};

    ASSERT_GT(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    lexer.tokenize_all<Token_kind>(
            input, [&serial](const Token_kind kind, const std::size_t length) { serial.emplace_back(kind, length); });

    std::vector<std::pair<Token_kind, std::size_t>> chunked;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{input.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        lexer.tokenize_all<Token_kind>(chunk, [&chunked](const Token_kind kind, const std::size_t length) {
            chunked.emplace_back(kind, length);
        });
    }

    EXPECT_EQ(chunked, serial);
}

TEST_F(Lexer_test, Dead_branches_do_not_decertify)
{
    enum class Token_kind : uint8_t
    {
        Never,
        Letter,
    };

    // any_of over an empty set denotes the empty language, so "ab" followed by it matches nothing. The states
    // leading there stay reachable, and one of them consumes 'b'. That transition can never lie on an emitted
    // token, so it must not de-certify 'b': every 'b' in an input this lexer accepts is a whole token.
    Builder_dbg builder;

    builder.add_token(concat(text("ab"), any_of(Set{})), Token_kind::Never, 1);
    builder.add_token(text("b"), Token_kind::Letter, 1);

    const auto lexer{builder.build()};

    EXPECT_TRUE(lexer.is_split_point('b'));

    // 'a' is consumed only into the dead branch, so no valid input contains it and it is not a usable split point.
    EXPECT_FALSE(lexer.is_split_point('a'));

    const std::string input{"bbbbbbbb"};

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_EQ(input[boundaries[index]], 'b');
    }
}

TEST_F(Lexer_test, Planning_ignores_vacuously_certified_symbols)
{
    enum class Token_kind : uint8_t
    {
        Letters,
    };

    // Only 'a' appears in any token, so every other byte certifies vacuously: no input this lexer accepts can
    // contain one. The plan must recognize that the useful certificate is empty and return the whole input as one
    // chunk, rather than scanning for bytes that cannot occur.
    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::Letters, 1);

    const auto lexer{builder.build()};

    // 'a' continues a token, and every other byte certifies only vacuously, so nothing is reported.
    EXPECT_FALSE(lexer.is_split_point('a'));
    EXPECT_FALSE(lexer.is_split_point('z'));

    const std::string input(4096, 'a');

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());
}

TEST_F(Lexer_test, Useful_certificates_are_those_the_initial_state_consumes)
{
    enum class Token_kind : uint8_t
    {
        Letters,
        Semicolon,
    };

    // ';' is certified and the initial state consumes it, so it is useful and planning may search for it.
    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::Letters, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    // ';' is certified and the initial state consumes it; 'z' certifies only vacuously and is not reported.
    EXPECT_TRUE(lexer.is_split_point(';'));
    EXPECT_FALSE(lexer.is_split_point('z'));

    std::string input;

    for (std::size_t index{0}; index < 512; ++index)
    {
        input += "aaa;";
    }

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_EQ(input[boundaries[index]], ';');
    }
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

TEST_F(Lexer_test, Parallel_tokenization_accepts_a_sink_callable_only_as_an_lvalue)
{
    enum class Token_kind : std::uint8_t
    {
        word,
        space
    };

    // The workers hold the sink by reference and call it as an lvalue, so the constraint has to ask for exactly
    // that. Asking whether an rvalue is callable rejects this sink, which the implementation could have driven.
    // The counter is atomic because one sink serves every chunk's thread, as the API documents.
    struct Lvalue_only_sink
    {
        std::atomic<std::size_t>* seen;

        void operator()(std::size_t, Token_kind, std::size_t) & { ++*seen; }
    };

    Builder builder;

    builder.add_token(plus(any_of(Set::alpha())), Token_kind::word, 1);
    builder.add_token(any_of(Set{' '}), Token_kind::space, 1);

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_point(' '));

    const std::string input{"ab cd ef gh"};

    std::atomic<std::size_t> seen{0};

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(input, 2, Lvalue_only_sink{.seen = &seen})};

    std::size_t total{0};

    for (const auto length : consumed)
    {
        total += length;
    }

    EXPECT_EQ(total, input.size());

    // Four words and the three spaces between them.
    EXPECT_EQ(seen, 7U);
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

TEST_F(Lexer_test, State_limit_stops_an_exploding_construction)
{
    enum class Token_kind : uint8_t
    {
        Needle,
    };

    // The classic exponential case: (a|b)* a (a|b)^12 needs a state per remembered 12-symbol suffix, around 2^12
    // of them, which is exactly the pathology a caller accepting untrusted token sets must be able to cap.
    Builder_dbg builder;

    builder.add_token(
            concat(kleene(any_of(Set{'a', 'b'})), text("a"), exact(any_of(Set{'a', 'b'}), 12)), Token_kind::Needle, 1);

    builder.set_state_limit(256);

    EXPECT_THROW(static_cast<void>(builder.build()), State_limit_error);
    EXPECT_THROW(static_cast<void>(builder.diagnose()), State_limit_error);

    // The error carries the limit and stays catchable as std::runtime_error for existing call sites.
    try
    {
        static_cast<void>(builder.build());

        FAIL() << "build() must throw";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_EQ(dynamic_cast<const State_limit_error&>(error).limit(), 256U);
    }

    // The same grammar builds once the cap allows its true size.
    builder.set_state_limit(0);

    const auto lexer{builder.build()};

    const auto [token, length]{lexer.tokenize<Token_kind>(std::string{"a"} + std::string(12, 'b'))};

    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(length, 13U);
}

TEST_F(Lexer_test, State_limit_leaves_reasonable_grammars_untouched)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Number,
    };

    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 1);

    builder.set_state_limit(64);

    const auto lexer{builder.build()};

    const auto [token, length]{lexer.tokenize<Token_kind>(std::string{"counter42"})};

    EXPECT_EQ(token, Token_kind::Identifier);
    EXPECT_EQ(length, 9U);
}

TEST_F(Lexer_test, Unicode_identifiers_tokenize_through_xid_properties)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Number,
        Space,
    };

    Builder_dbg builder;

    // The C-style profile on top of the exact properties: underscore may lead, which XID_Start alone excludes.
    builder.add_token(
            concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue())), Token_kind::Identifier,
            1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::Number, 1);
    builder.add_token(plus(any_of(Set{' '})), Token_kind::Space, 1);

    const auto lexer{builder.build()};

    // Greek, Han with a combining-free ASCII tail, an underscore head, and a number: "αβ 漢字_2 _x9 42".
    const std::string input{"\xCE\xB1\xCE\xB2 \xE6\xBC\xA2\xE5\xAD\x97_2 _x9 42"};

    std::vector<std::pair<Token_kind, std::size_t>> stream;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            input,
            [&stream](const Token_kind token, const std::size_t length) { stream.emplace_back(token, length); })};

    EXPECT_EQ(consumed, input.size());

    const std::vector<std::pair<Token_kind, std::size_t>> expected{
            {Token_kind::Identifier, 4}, {Token_kind::Space, 1}, {Token_kind::Identifier, 8}, {Token_kind::Space, 1},
            {Token_kind::Identifier, 3}, {Token_kind::Space, 1}, {Token_kind::Number, 2},
    };

    EXPECT_EQ(stream, expected);
}

TEST_F(Lexer_test, Xid_identifiers_compete_with_keywords_and_reject_ill_formed_input)
{
    enum class Token_kind : uint8_t
    {
        Keyword,
        Identifier,
    };

    Builder_dbg builder;

    builder.add_token(text("if"), Token_kind::Keyword, 1);
    builder.add_token(concat(unicode::xid_start(), kleene(unicode::xid_continue())), Token_kind::Identifier, 2);

    const auto lexer{builder.build()};

    // The keyword's own spelling wins on priority; one more code point and the identifier takes over.
    EXPECT_EQ(lexer.tokenize<Token_kind>(std::string{"if"}).token, Token_kind::Keyword);
    EXPECT_EQ(lexer.tokenize<Token_kind>(std::string{"if\xCE\xBB"}).token, Token_kind::Identifier);

    // Ill-formed UTF-8 never matches an XID class: a stray continuation byte, an overlong encoding, and a
    // truncated sequence all fail at offset zero rather than tokenize as identifiers.
    for (const std::string input : {"\x80", "\xC0\xAF", "\xE6\xBC"})
    {
        const auto [token, length]{lexer.tokenize<Token_kind>(input)};

        EXPECT_FALSE(token.has_value()) << input;
        EXPECT_EQ(length, 0U) << input;
    }
}

TEST_F(Lexer_test, Xid_membership_matches_the_generated_tables_at_every_boundary)
{
    enum class Token_kind : uint8_t
    {
        Point,
    };

    struct Range
    {
        char32_t first;
        char32_t last;
    };

    // The checked-in tables are the oracle: every interval edge and its outside neighbors go through the full
    // pipeline, covering exactly the points where membership can flip in unioning, UTF-8 expansion, lowering,
    // determinization, or minimization.
    const auto load{[](const std::string& name) {
        std::ifstream file{std::string{SOURCE_DIR} + "/libs/regex/src/xid_ranges.inc"};

        std::vector<Range> ranges;

        std::string line;

        bool inside{false};

        while (std::getline(file, line))
        {
            if (!inside)
            {
                inside = line.find(name) != std::string::npos;

                continue;
            }

            unsigned long first{};

            unsigned long last{};

            if (std::sscanf(line.c_str(), " {.first = 0x%lX, .last = 0x%lX}", &first, &last) == 2)
            {
                ranges.push_back({.first = static_cast<char32_t>(first), .last = static_cast<char32_t>(last)});
            }
            else
            {
                break;
            }
        }

        return ranges;
    }};

    const auto member{[](const std::vector<Range>& ranges, const char32_t point) {
        for (const auto& range : ranges)
        {
            if (point >= range.first && point <= range.last)
            {
                return true;
            }
        }

        return false;
    }};

    const auto encode{[](const char32_t code_point) {
        std::string bytes;

        if (code_point <= 0x7F)
        {
            bytes += static_cast<char>(code_point);
        }
        else if (code_point <= 0x7FF)
        {
            bytes += static_cast<char>(0xC0 | (code_point >> 6U));
            bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
        }
        else if (code_point <= 0xFFFF)
        {
            bytes += static_cast<char>(0xE0 | (code_point >> 12U));
            bytes += static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU));
            bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
        }
        else
        {
            bytes += static_cast<char>(0xF0 | (code_point >> 18U));
            bytes += static_cast<char>(0x80 | ((code_point >> 12U) & 0x3FU));
            bytes += static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU));
            bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
        }

        return bytes;
    }};

    const std::pair<const char*, regex::Regex (*)()> properties[]{
            {"xid_start_ranges", &unicode::xid_start},
            {"xid_continue_ranges", &unicode::xid_continue},
    };

    for (const auto& [name, property] : properties)
    {
        const auto ranges{load(name)};

        ASSERT_GT(ranges.size(), 500U) << name;

        Builder_dbg builder;

        builder.add_token(property(), Token_kind::Point, 1);

        const auto lexer{builder.build()};

        for (const auto& range : ranges)
        {
            for (const auto probe :
                 {range.first, range.last, static_cast<char32_t>(range.first - 1),
                  static_cast<char32_t>(range.last + 1)})
            {
                if (probe > 0x10FFFF || (probe >= 0xD800 && probe <= 0xDFFF))
                {
                    continue;
                }

                const auto bytes{encode(probe)};

                const auto [token, length]{lexer.tokenize<Token_kind>(bytes)};

                const auto matched{token.has_value() && length == bytes.size()};

                EXPECT_EQ(matched, member(ranges, probe)) << name << " U+" << std::hex << static_cast<unsigned>(probe);
            }
        }
    }
}

// Two ideal offsets can walk forward onto the same certified byte. Resuming the next search from the ideal offset
// rediscovers it, drops it as a duplicate, and never reaches the certified byte after it, silently costing a chunk
// the input can support.
TEST_F(Lexer_test, Chunk_boundaries_finds_adjacent_certified_bytes)
{
    enum class Token_kind : uint8_t
    {
        Run,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::Run, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaaaaaa;;a"};

    ASSERT_TRUE(lexer.is_split_point(';'));

    EXPECT_EQ(lexer.chunk_boundaries(input, 4), (std::vector<std::size_t>{0, 7, 8, 10}));

    EXPECT_EQ(lexer.chunk_boundaries(input, 2), (std::vector<std::size_t>{0, 7, 10}));
}

// A chunk needs a byte, so a request far larger than the input must neither iterate once per requested chunk nor
// overflow the ideal-offset arithmetic. The answer is capped by what the input can actually support.
TEST_F(Lexer_test, Chunk_boundaries_caps_a_request_larger_than_the_input)
{
    enum class Token_kind : uint8_t
    {
        Run,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::Run, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaaaaaa;;a"};

    const auto capped{lexer.chunk_boundaries(input, std::numeric_limits<std::size_t>::max())};

    EXPECT_EQ(capped, (std::vector<std::size_t>{0, 7, 8, 10}));

    EXPECT_EQ(
            lexer.chunk_boundaries(std::string{}, std::numeric_limits<std::size_t>::max()),
            (std::vector<std::size_t>{0, 0}));
}

// The ideal offsets must be exactly size * index / chunks. Computing them that way overflows for a large input cut
// very finely, so the planner accumulates instead; this pins the accumulation against the closed form it replaces.
// Every byte certifies here, so no boundary walks forward and the offsets are the division itself. The sizes and
// chunk counts are deliberately coprime, which is what makes the carry fire on most iterations.
TEST_F(Lexer_test, Chunk_boundaries_divide_the_input_exactly)
{
    enum class Token_kind : uint8_t
    {
        Any,
    };

    Builder_dbg builder;

    builder.add_token(any_of(Set::all()), Token_kind::Any, 1);

    const auto lexer{builder.build()};

    for (const auto size : {std::size_t{100}, std::size_t{997}, std::size_t{1024}})
    {
        const std::string input(size, 'x');

        ASSERT_TRUE(lexer.is_split_point('x'));

        for (const auto chunks : {std::size_t{3}, std::size_t{7}, std::size_t{64}})
        {
            std::vector<std::size_t> expected{0};

            for (std::size_t index{1}; index < chunks; ++index)
            {
                expected.push_back(size * index / chunks);
            }

            expected.push_back(size);

            EXPECT_EQ(lexer.chunk_boundaries(input, chunks), expected) << "size " << size << ", chunks " << chunks;
        }
    }
}

// An exception escaping a jthread's callable calls std::terminate. A throwing sink must reach the caller instead,
// and must do so identically whether it throws on a worker chunk or on the one the caller scans itself.
TEST_F(Lexer_test, Parallel_tokenization_propagates_a_throwing_sink)
{
    enum class Token_kind : uint8_t
    {
        Run,
        Semicolon,
    };

    Builder_dbg builder;

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::Run, 1);
    builder.add_token(text(";"), Token_kind::Semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaa;aaa;aaa;aaa"};

    ASSERT_GT(lexer.chunk_boundaries(input, 4).size(), 2U);

    const auto scan{[&lexer, &input](const std::size_t throwing_chunk) {
        return lexer.tokenize_all_parallel<Token_kind>(
                input, 4, [throwing_chunk](const std::size_t chunk, Token_kind, std::size_t) {
                    if (chunk == throwing_chunk)
                    {
                        throw std::runtime_error{"sink"};
                    }
                });
    }};

    EXPECT_THROW(scan(0), std::runtime_error);

    EXPECT_THROW(scan(lexer.chunk_boundaries(input, 4).size() - 2), std::runtime_error);
}

// A conforming random-access byte iterator that additionally converts to char. List-initializing a std::string
// from two such iterators selects the initializer_list constructor over the range constructor, so a planner
// that builds windows from iterator objects reads a byte pair the input never contains and can certify a cut
// inside a token of completely tokenizable input, the one thing the window theorem forbids.
struct Char_convertible_iterator
{
    using value_type = char;
    using difference_type = std::ptrdiff_t;

    const char* ptr{};

    operator char() const { return *ptr; }

    char operator*() const { return *ptr; }
    char operator[](const difference_type at) const { return ptr[at]; }

    Char_convertible_iterator& operator++()
    {
        ++ptr;
        return *this;
    }

    Char_convertible_iterator operator++(int) { return {ptr++}; }

    Char_convertible_iterator& operator--()
    {
        --ptr;
        return *this;
    }

    Char_convertible_iterator operator--(int) { return {ptr--}; }

    Char_convertible_iterator& operator+=(const difference_type by)
    {
        ptr += by;
        return *this;
    }

    Char_convertible_iterator& operator-=(const difference_type by)
    {
        ptr -= by;
        return *this;
    }

    friend Char_convertible_iterator operator+(const Char_convertible_iterator it, const difference_type by)
    {
        return {it.ptr + by};
    }

    friend Char_convertible_iterator operator+(const difference_type by, const Char_convertible_iterator it)
    {
        return {it.ptr + by};
    }

    friend Char_convertible_iterator operator-(const Char_convertible_iterator it, const difference_type by)
    {
        return {it.ptr - by};
    }

    friend difference_type operator-(const Char_convertible_iterator lhs, const Char_convertible_iterator rhs)
    {
        return lhs.ptr - rhs.ptr;
    }

    friend auto operator<=>(const Char_convertible_iterator&, const Char_convertible_iterator&) = default;
};

static_assert(std::random_access_iterator<Char_convertible_iterator>);

TEST_F(Lexer_test, Window_planning_reads_elements_never_iterator_objects)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    // Twelve spaces then eight letters puts the equal-division target where an iterator-built fake window would
    // stitch a space and a letter from two positions apart into " a", certify it, and cut inside the whitespace
    // run: the streams then diverge on completely tokenizable input. Reading elements, the plan must equal the
    // plain char plan and rejoin to the serial stream.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const std::string text{std::string(12, ' ') + "alphabet"};

    const auto boundaries{lexer.chunk_boundaries_with_windows(
            Char_convertible_iterator{text.data()}, Char_convertible_iterator{text.data() + text.size()}, 2)};

    EXPECT_EQ(boundaries, lexer.chunk_boundaries_with_windows(text, 2));

    std::vector<std::pair<Token_kind, std::size_t>> serial;

    ASSERT_EQ(
            text.size(),
            lexer.tokenize_all<Token_kind>(text, [&serial](const Token_kind kind, const std::size_t length) {
                serial.emplace_back(kind, length);
            }));

    std::vector<std::pair<Token_kind, std::size_t>> rejoined;

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const std::string_view chunk{text.data() + boundaries[index - 1], boundaries[index] - boundaries[index - 1]};

        lexer.tokenize_all<Token_kind>(chunk, [&rejoined](const Token_kind kind, const std::size_t length) {
            rejoined.emplace_back(kind, length);
        });
    }

    EXPECT_EQ(rejoined, serial);
}

TEST_F(Lexer_test, Every_entry_point_scans_std_byte_input_like_char_input)
{
    enum class Token_kind : uint8_t
    {
        Identifier,
        Whitespace,
    };

    // The concept probes below ask only overload viability, which never instantiates a body; an element type the
    // interface admits could still be rejected by a body's implicit conversions. std::byte converts to nothing
    // implicitly, so running it through every entry point pins each body to the promised domain, and the answers
    // must match the same input spelled as char. The grammar certifies no byte but certifies a window, so the
    // window walk itself executes rather than short-circuiting.
    Builder_dbg builder;

    builder.add_token(identifier_regex(), Token_kind::Identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);

    const auto lexer{builder.build()};

    const std::string text{"alpha beta gamma delta epsilon zeta eta theta"};

    std::vector<std::byte> bytes;

    for (const char symbol : text)
    {
        bytes.push_back(static_cast<std::byte>(symbol));
    }

    EXPECT_EQ(lexer.tokenize<Token_kind>(bytes), lexer.tokenize<Token_kind>(text));

    std::vector<std::pair<Token_kind, std::size_t>> from_bytes;

    const auto consumed{lexer.tokenize_all<Token_kind>(
            bytes,
            [&from_bytes](const Token_kind kind, const std::size_t length) { from_bytes.emplace_back(kind, length); })};

    std::vector<std::pair<Token_kind, std::size_t>> from_text;

    ASSERT_EQ(
            consumed,
            lexer.tokenize_all<Token_kind>(text, [&from_text](const Token_kind kind, const std::size_t length) {
                from_text.emplace_back(kind, length);
            }));

    EXPECT_EQ(from_bytes, from_text);

    EXPECT_EQ(lexer.chunk_boundaries(bytes, 4), lexer.chunk_boundaries(text, 4));

    const auto boundaries{lexer.chunk_boundaries_with_windows(bytes, 4)};

    ASSERT_GT(boundaries.size(), 2U);
    EXPECT_EQ(boundaries, lexer.chunk_boundaries_with_windows(text, 4));

    std::vector<std::size_t> parallel_from_bytes(boundaries.size() - 1, 0);

    const auto counted{lexer.tokenize_all_parallel<Token_kind>(
            bytes, 4, [&parallel_from_bytes](const std::size_t chunk, Token_kind, const std::size_t) {
                ++parallel_from_bytes[chunk];
            })};

    EXPECT_EQ(counted, lexer.tokenize_all_parallel<Token_kind>(text, 4, [](std::size_t, Token_kind, std::size_t) {}));

    // The iterator overloads run their own bodies rather than sharing the container ones' instantiations, so
    // each is driven explicitly; the answers must again match the char spelling.
    EXPECT_EQ(lexer.tokenize<Token_kind>(bytes.begin(), bytes.end()), lexer.tokenize<Token_kind>(text));

    std::size_t through_iterators{0};

    EXPECT_EQ(
            consumed,
            lexer.tokenize_all<Token_kind>(bytes.begin(), bytes.end(), [&through_iterators](Token_kind, std::size_t) {
                ++through_iterators;
            }));

    EXPECT_EQ(through_iterators, from_text.size());

    EXPECT_EQ(lexer.chunk_boundaries(bytes.begin(), bytes.end(), 4), lexer.chunk_boundaries(text, 4));

    EXPECT_EQ(lexer.chunk_boundaries_with_windows(bytes.begin(), bytes.end(), 4), boundaries);

    EXPECT_EQ(
            counted, lexer.tokenize_all_parallel<Token_kind>(
                             bytes.begin(), bytes.end(), 4, [](std::size_t, Token_kind, std::size_t) {}));

    // The NFA scanner is its own public surface with its own body.
    const auto nfa_from_bytes{nfa::Simulator::run(builder.nfa(), bytes.begin(), bytes.end())};
    const auto nfa_from_text{nfa::Simulator::run(builder.nfa(), text)};

    EXPECT_EQ(nfa_from_bytes.token, nfa_from_text.token);
    EXPECT_EQ(nfa_from_bytes.length, nfa_from_text.length);
}

// The dependent probes for the byte-domain assertions below: a requires-expression over concrete types is checked
// as ordinary code, so viability is asked through these instead. One probe per public overload, because a
// combined expression proves only that at least one call rejects a type, and a single overload could then
// quietly lose its constraint.
template <typename Container>
concept Single_scan_over =
        requires(const Lexer& lexer, const Container& container) { lexer.template tokenize<int>(container); };

template <typename Container>
concept Full_scan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.template tokenize_all<int>(container, [](int, std::size_t) {});
};

template <typename Container>
concept Byte_plan_over =
        requires(const Lexer& lexer, const Container& container) { lexer.chunk_boundaries(container, std::size_t{2}); };

template <typename Container>
concept Window_plan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.chunk_boundaries_with_windows(container, std::size_t{2});
};

template <typename Container>
concept Parallel_scan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.template tokenize_all_parallel<int>(container, std::size_t{2}, [](std::size_t, int, std::size_t) {});
};

template <typename Iterator>
concept Single_scan_through =
        requires(const Lexer& lexer, const Iterator& iterator) { lexer.template tokenize<int>(iterator, iterator); };

template <typename Iterator>
concept Full_scan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all<int>(iterator, iterator, [](int, std::size_t) {});
};

template <typename Iterator>
concept Byte_plan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.chunk_boundaries(iterator, iterator, std::size_t{2});
};

template <typename Iterator>
concept Window_plan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.chunk_boundaries_with_windows(iterator, iterator, std::size_t{2});
};

template <typename Iterator>
concept Parallel_scan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all_parallel<int>(iterator, iterator, std::size_t{2}, [](std::size_t, int, std::size_t) {});
};

template <typename Iterator>
concept Dfa_scan_through = requires(const dfa::Simulator& simulator, const Iterator& iterator) {
    simulator.run(iterator, iterator);
    simulator.run_all(iterator, iterator, [](const dfa::Token&, std::size_t, std::uint64_t) {});
};

template <typename Container>
concept Dfa_scan_over =
        requires(const dfa::Simulator& simulator, const Container& container) { simulator.run(container); };

template <typename Iterator>
concept Nfa_scan_through = requires(const nfa::Nfa& automaton, const Iterator& iterator) {
    nfa::Simulator::run(automaton, iterator, iterator);
};

template <typename Container>
concept Nfa_scan_over =
        requires(const nfa::Nfa& automaton, const Container& container) { nfa::Simulator::run(automaton, container); };

TEST_F(Lexer_test, Test_container_concepts_require_common_const_ranges)
{
    using munch::common::concepts::Iterable;
    using munch::common::concepts::Random_access_iterable;

    static_assert(Random_access_iterable<std::string>);
    static_assert(Random_access_iterable<std::string_view>);
    static_assert(Random_access_iterable<std::vector<char>>);
    static_assert(Random_access_iterable<std::array<char, 4>>);
    static_assert(Random_access_iterable<char[4]>);

    // A counted iterator paired with the default sentinel is random access but not a common range, and a begin/end
    // pair of different types cannot instantiate the single-iterator entry points, so the concept must reject it at
    // the interface instead of failing inside the body.
    using Counted = std::ranges::subrange<std::counted_iterator<const char*>, std::default_sentinel_t>;

    static_assert(std::ranges::random_access_range<Counted>);
    static_assert(!Random_access_iterable<Counted>);
    static_assert(!Iterable<Counted>);

    // A filter view iterates only mutably, and every container overload takes a const reference.
    using Filtered = decltype(std::declval<std::string&>() | std::views::filter([](char) { return true; }));

    static_assert(std::ranges::range<Filtered>);
    static_assert(!Iterable<Filtered>);

    // views::common is the caller's one-line repair, and over a sized random access range it keeps random access.
    const Builder builder;

    const auto lexer{builder.build()};

    const std::string input{"abab"};

    const auto common{
            std::ranges::subrange{std::counted_iterator{input.data(), 4}, std::default_sentinel} | std::views::common};

    static_assert(Random_access_iterable<decltype(common)>);

    EXPECT_EQ(lexer.tokenize<int>(common), Lexer::Match<int>(std::nullopt, 0));

    EXPECT_EQ(lexer.chunk_boundaries(common, 2), (std::vector<std::size_t>{0, 4}));

    // The byte domain: integral elements qualify, wider ones under the documented modulo-256 reading, and so does
    // std::byte; floating and non-byte elements fall out at overload resolution instead of failing inside a scan.
    using munch::common::concepts::Random_access_byte_iterable;

    static_assert(Random_access_byte_iterable<std::string>);
    static_assert(Random_access_byte_iterable<std::vector<std::uint8_t>>);
    static_assert(Random_access_byte_iterable<std::vector<int>>);
    static_assert(Random_access_byte_iterable<std::array<std::byte, 4>>);
    static_assert(!Random_access_byte_iterable<std::vector<double>>);
    static_assert(!Random_access_byte_iterable<std::vector<std::string>>);

    // A range that yields bytes from mutable iteration but something else from const iteration is judged by its
    // const face, the one every container overload receives.
    struct Two_faced
    {
        [[nodiscard]] char* begin() { return nullptr; }
        [[nodiscard]] char* end() { return nullptr; }
        [[nodiscard]] const std::string* begin() const { return nullptr; }
        [[nodiscard]] const std::string* end() const { return nullptr; }
    };

    static_assert(!Random_access_byte_iterable<Two_faced>);

    // The rejection happens at the interface of every entry point: each overload asserts its own acceptance and
    // its own refusals, so no single overload can lose its constraint behind the others.
    static_assert(
            Single_scan_over<std::string> && Full_scan_over<std::string> && Byte_plan_over<std::string> &&
            Window_plan_over<std::string> && Parallel_scan_over<std::string>);

    static_assert(
            !Single_scan_over<std::vector<double>> && !Full_scan_over<std::vector<double>> &&
            !Byte_plan_over<std::vector<double>> && !Window_plan_over<std::vector<double>> &&
            !Parallel_scan_over<std::vector<double>>);

    static_assert(
            !Single_scan_over<std::vector<std::string>> && !Full_scan_over<std::vector<std::string>> &&
            !Byte_plan_over<std::vector<std::string>> && !Window_plan_over<std::vector<std::string>> &&
            !Parallel_scan_over<std::vector<std::string>>);

    static_assert(
            !Single_scan_over<Two_faced> && !Full_scan_over<Two_faced> && !Byte_plan_over<Two_faced> &&
            !Window_plan_over<Two_faced> && !Parallel_scan_over<Two_faced>);

    using Good_iterator = std::string_view::iterator;
    using Bad_iterator = std::vector<double>::const_iterator;

    static_assert(
            Single_scan_through<Good_iterator> && Full_scan_through<Good_iterator> &&
            Byte_plan_through<Good_iterator> && Window_plan_through<Good_iterator> &&
            Parallel_scan_through<Good_iterator>);

    static_assert(
            !Single_scan_through<Bad_iterator> && !Full_scan_through<Bad_iterator> &&
            !Byte_plan_through<Bad_iterator> && !Window_plan_through<Bad_iterator> &&
            !Parallel_scan_through<Bad_iterator>);

    // The library scanners beneath the Lexer hold the same line.
    static_assert(Dfa_scan_through<Good_iterator> && !Dfa_scan_through<Bad_iterator>);
    static_assert(Dfa_scan_over<std::string> && !Dfa_scan_over<std::vector<double>> && !Dfa_scan_over<Two_faced>);
    static_assert(Nfa_scan_through<Good_iterator> && !Nfa_scan_through<Bad_iterator>);
    static_assert(Nfa_scan_over<std::string> && !Nfa_scan_over<std::vector<double>> && !Nfa_scan_over<Two_faced>);
}

TEST_F(Lexer_test, Test_ignored_tokens_reject_non_integral_initializers)
{
    // A floating-point initializer used to deduce the list's element type and truncate silently; the constrained
    // overload now leaves only the vector overload, where the narrowing conversion is ill-formed.
    static_assert(requires(Builder builder) { builder.set_ignored_tokens({1, 2}); });

    enum class Kind : std::size_t
    {
        comment = 7
    };

    static_assert(requires(Builder builder) { builder.set_ignored_tokens({Kind::comment}); });

    // set_ignored_tokens({1.9}) is now ill-formed outright: the constrained overload no longer deduces a double
    // list, and the vector overload rejects the narrowing conversion. That is a hard error inside the braced list,
    // not a substitution failure, so it cannot be asserted here with a negative requires-expression.
}

TEST_F(Lexer_test, A_sink_taking_either_arity_is_called_with_two_arguments)
{
    // The accepting-state payload arrived after this entry point shipped, so a sink accepting both arities keeps
    // receiving what it received before the payload existed. A generic or variadic sink accepts both silently.
    enum class Kind : std::size_t
    {
        word = 1
    };

    Builder builder;

    builder.add_token(plus(any_of(Set::alpha())), Kind::word, 1);

    const auto lexer{builder.build()};

    const std::string input{"abc"};

    std::size_t arity{0};

    std::ignore = lexer.tokenize_all<Kind>(input, [&arity](auto&&... arguments) { arity = sizeof...(arguments); });

    EXPECT_EQ(arity, 2u);
}

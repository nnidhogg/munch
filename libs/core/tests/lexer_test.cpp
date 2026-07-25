#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lexer/core/builder.hpp"
#include "lexer/dfa/tools/graphviz.hpp"
#include "lexer/nfa/tools/graphviz.hpp"
#include "lexer/regex/regex.hpp"

using namespace lexer;
using namespace lexer::core;
using namespace lexer::regex;

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

    EXPECT_EQ(lexer.tokenize<int>(input), Lexer::Result_t<int>(std::nullopt, 0));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), Result_t(Token_kind::Boolean, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), Result_t(Token_kind::Char, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), Result_t(Token_kind::String, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), Result_t(Token_kind::Int8, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), Result_t(Token_kind::Uint8, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), Result_t(Token_kind::Int16, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), Result_t(Token_kind::Uint16, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), Result_t(Token_kind::Int32, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), Result_t(Token_kind::Uint32, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), Result_t(Token_kind::Int64, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), Result_t(Token_kind::Uint64, 6));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name"), Result_t(Token_kind::Identifier, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("_someVar"), Result_t(Token_kind::Identifier, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("MyVariable123"), Result_t(Token_kind::Identifier, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("__Another_var__99"), Result_t(Token_kind::Identifier, 17));
    EXPECT_EQ(lexer.tokenize<Token_kind>("camelCase"), Result_t(Token_kind::Identifier, 9));
    EXPECT_EQ(lexer.tokenize<Token_kind>("___"), Result_t(Token_kind::Identifier, 3));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("123"), Result_t(Token_kind::Integer_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("007"), Result_t(Token_kind::Integer_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1234567890"), Result_t(Token_kind::Integer_literal, 10));
    EXPECT_EQ(lexer.tokenize<Token_kind>("5"), Result_t(Token_kind::Integer_literal, 1));
    EXPECT_EQ(lexer.tokenize<Token_kind>("0"), Result_t(Token_kind::Integer_literal, 1));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("\"Hello\""), Result_t(Token_kind::String_literal, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"\""), Result_t(Token_kind::String_literal, 2));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"Hello world\""), Result_t(Token_kind::String_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"\\\"Quote\\\"\""), Result_t(Token_kind::String_literal, 11));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("1.2"), Result_t(Token_kind::Fixed_point_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14"), Result_t(Token_kind::Fixed_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("123.456"), Result_t(Token_kind::Fixed_point_literal, 7));

    EXPECT_EQ(lexer.tokenize<Token_kind>("."), Result_t(std::nullopt, 0));
    EXPECT_EQ(lexer.tokenize<Token_kind>(".1"), Result_t(std::nullopt, 0));
    EXPECT_EQ(lexer.tokenize<Token_kind>("58."), Result_t(std::nullopt, 0));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14159"), Result_t(Token_kind::Floating_point_literal, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("2e10"), Result_t(Token_kind::Floating_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("-1.23E-4"), Result_t(Token_kind::Floating_point_literal, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("+0.5"), Result_t(Token_kind::Floating_point_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1e-10"), Result_t(Token_kind::Floating_point_literal, 5));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"Hello\""), Result_t(Token_kind::Wide_string_literal, 8));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"\""), Result_t(Token_kind::Wide_string_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"Wide world\""), Result_t(Token_kind::Wide_string_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"\\\"Escaped\\\"\""), Result_t(Token_kind::Wide_string_literal, 14));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), Result_t(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("' '"), Result_t(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'\n'"), Result_t(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'\''"), Result_t(Token_kind::Character_literal, 3));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), Result_t(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L' '"), Result_t(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'\n'"), Result_t(Token_kind::Wide_character_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'\''"), Result_t(Token_kind::Wide_character_literal, 4));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("// This is a comment"), Result_t(Token_kind::Single_line_comment, 20));
    EXPECT_EQ(lexer.tokenize<Token_kind>("//"), Result_t(Token_kind::Single_line_comment, 2));
    EXPECT_EQ(lexer.tokenize<Token_kind>("// @#$%^&*()"), Result_t(Token_kind::Single_line_comment, 12));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("/* comment */"), Result_t(Token_kind::Multi_line_comment, 13));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* multi\n   line\n   comment */"),
            Result_t(Token_kind::Multi_line_comment, 30));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/* start /* nested */ end */"), Result_t(Token_kind::Multi_line_comment, 28));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/**/"), Result_t(Token_kind::Multi_line_comment, 4));
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

    using Result_t = Lexer::Result_t<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), Result_t(Token_kind::Boolean, 7));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), Result_t(Token_kind::Char, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), Result_t(Token_kind::String, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), Result_t(Token_kind::Int8, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), Result_t(Token_kind::Uint8, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), Result_t(Token_kind::Int16, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), Result_t(Token_kind::Uint16, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), Result_t(Token_kind::Int32, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), Result_t(Token_kind::Uint32, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), Result_t(Token_kind::Int64, 5));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), Result_t(Token_kind::Uint64, 6));

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name_1"), Result_t(Token_kind::Identifier, 15));

    EXPECT_EQ(lexer.tokenize<Token_kind>("1234"), Result_t(Token_kind::Integer_literal, 4));
    EXPECT_EQ(lexer.tokenize<Token_kind>("\"hello world\""), Result_t(Token_kind::String_literal, 13));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), Result_t(Token_kind::Character_literal, 3));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L\"wide string\""), Result_t(Token_kind::Wide_string_literal, 14));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), Result_t(Token_kind::Wide_character_literal, 4));

    EXPECT_EQ(lexer.tokenize<Token_kind>("123.45"), Result_t(Token_kind::Fixed_point_literal, 6));
    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14159e+2"), Result_t(Token_kind::Floating_point_literal, 10));

    EXPECT_EQ(lexer.tokenize<Token_kind>("// a comment"), Result_t(Token_kind::Single_line_comment, 12));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/* a comment */"), Result_t(Token_kind::Multi_line_comment, 15));
}

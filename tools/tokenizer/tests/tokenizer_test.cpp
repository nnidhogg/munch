#include "lexer/tools/tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include "lexer/core/builder.hpp"
#include "lexer/regex/regex.hpp"

using namespace lexer;
using namespace lexer::core;
using namespace lexer::regex;
using namespace lexer::tools::tokenizer;

namespace
{
class Tokenizer_test : public testing::Test
{
public:
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

    static auto whitespace_regex()
    {
        const auto whitespace{plus(any_of(Set::whitespace()))};

        return whitespace;
    }

    static auto newline_regex()
    {
        const auto newline{plus(any_of(Set::newline()))};

        return newline;
    }
};

enum class Token_kind : uint8_t
{
    Boolean,
    Char,
    String,
    Identifier,
    Integer_literal,
    String_literal,
    Fixed_point_literal,
    Floating_point_literal,
    Single_line_comment,
    Multi_line_comment,
    Whitespace,
    Newline,
};

Lexer build_lexer()
{
    Builder builder;

    builder.add_token(text("boolean"), Token_kind::Boolean, 1);
    builder.add_token(text("char"), Token_kind::Char, 1);
    builder.add_token(text("string"), Token_kind::String, 1);

    builder.add_token(Tokenizer_test::identifier_regex(), Token_kind::Identifier, 4);

    builder.add_token(Tokenizer_test::integer_literal_regex(), Token_kind::Integer_literal, 2);
    builder.add_token(Tokenizer_test::string_literal_regex(), Token_kind::String_literal, 2);
    builder.add_token(Tokenizer_test::fixed_point_literal_regex(), Token_kind::Fixed_point_literal, 2);
    builder.add_token(Tokenizer_test::floating_point_literal_regex(), Token_kind::Floating_point_literal, 3);

    builder.add_token(Tokenizer_test::single_line_comment_regex(), Token_kind::Single_line_comment, 0);
    builder.add_token(Tokenizer_test::multi_line_comment_regex(), Token_kind::Multi_line_comment, 0);

    builder.add_token(Tokenizer_test::whitespace_regex(), Token_kind::Whitespace, 0);

    builder.add_token(Tokenizer_test::newline_regex(), Token_kind::Newline, 0);

    return builder.build();
}

} // namespace

TEST_F(Tokenizer_test, Tokenize_from_string_stream)
{
    const std::string input{
            "boolean x 1234 \"hello\" 3.14 // comment\n"
            "string y 5.0e+1 /* block */"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    const auto advance = [&tokenizer](const Token_kind expect_kind, const std::string_view expect_lexeme) {
        const auto result{tokenizer.next<Token_kind>()};
        ASSERT_TRUE(result.has_token());

        const auto& token{result.token()};
        EXPECT_EQ(token.kind(), expect_kind);
        EXPECT_EQ(token.lexeme(), expect_lexeme);
    };

    const auto evaluate = [&tokenizer, &advance] {
        advance(Token_kind::Boolean, "boolean");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Identifier, "x");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Integer_literal, "1234");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::String_literal, "\"hello\"");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Fixed_point_literal, "3.14");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Single_line_comment, "// comment");
        advance(Token_kind::Newline, "\n");
        advance(Token_kind::String, "string");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Identifier, "y");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Floating_point_literal, "5.0e+1");
        advance(Token_kind::Whitespace, " ");
        advance(Token_kind::Multi_line_comment, "/* block */");

        const auto eof{tokenizer.next<Token_kind>()};
        EXPECT_TRUE(eof.end_of_input());
    };

    evaluate();

    tokenizer.reset();

    evaluate();

    tokenizer.load(input);

    evaluate();
}

TEST_F(Tokenizer_test, Unknown_character)
{
    const std::string input{"$boolean"}; // '$' not recognized

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer};

    tokenizer.load(input);

    const auto result{tokenizer.next<Token_kind>()};
    ASSERT_TRUE(result.has_error());

    const auto& error{result.error()};
    EXPECT_EQ(error.position(), 0u);
    EXPECT_FALSE(error.message().empty());
}

TEST_F(Tokenizer_test, Offset_tracking)
{
    const std::string input{"boolean x 123"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    // Initial offset should be 0
    EXPECT_EQ(tokenizer.offset(), 0u);

    // After "boolean" (7 chars)
    auto result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Boolean);
    EXPECT_EQ(tokenizer.offset(), 7u);

    // After " " (1 char)
    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Whitespace);
    EXPECT_EQ(tokenizer.offset(), 8u);

    // After "x" (1 char)
    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Identifier);
    EXPECT_EQ(tokenizer.offset(), 9u);

    // After " " (1 char)
    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Whitespace);
    EXPECT_EQ(tokenizer.offset(), 10u);

    // After "123" (3 chars)
    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Integer_literal);
    EXPECT_EQ(tokenizer.offset(), 13u);

    // EOF - offset should stay at end
    result = tokenizer.next<Token_kind>();
    EXPECT_TRUE(result.end_of_input());
    EXPECT_EQ(tokenizer.offset(), 13u);

    // After reset, offset should be 0
    tokenizer.reset();
    EXPECT_EQ(tokenizer.offset(), 0u);

    // After load with new input, offset should be 0
    tokenizer.load("char");
    EXPECT_EQ(tokenizer.offset(), 0u);
}

TEST_F(Tokenizer_test, Seek)
{
    const std::string input{"boolean x"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    // Consume "boolean", then rewind and read it again
    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());
    EXPECT_EQ(tokenizer.offset(), 7u);

    tokenizer.seek(0);
    EXPECT_EQ(tokenizer.offset(), 0u);

    auto result{tokenizer.next<Token_kind>()};
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Boolean);

    // Jump over the whitespace, as a driver does after scanning a token by hand
    tokenizer.seek(8);

    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Identifier);
    EXPECT_EQ(result.token().lexeme(), "x");

    // Seeking past the end clamps to it
    tokenizer.seek(100);
    EXPECT_EQ(tokenizer.offset(), input.size());
    EXPECT_TRUE(tokenizer.next<Token_kind>().end_of_input());
}

#include "munch/tools/tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include "munch/core/builder.hpp"
#include "munch/core/mode_builder.hpp"
#include "munch/regex/regex.hpp"

using namespace munch;
using namespace munch::core;
using namespace munch::regex;
using namespace munch::tools::tokenizer;

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

TEST_F(Tokenizer_test, Throws_on_empty_lexer_list)
{
    EXPECT_THROW(Tokenizer(std::vector<Lexer>{}), std::invalid_argument);
}

TEST_F(Tokenizer_test, Zero_width_match)
{
    enum class Digits_kind : uint8_t
    {
        Digits,
    };

    Builder builder;
    builder.add_token(kleene(any_of(Set::digits())), Digits_kind::Digits, 1);

    Tokenizer tokenizer{builder.build(), std::string{"a"}};

    const auto result{tokenizer.next<Digits_kind>()};
    ASSERT_TRUE(result.has_error());

    const auto& error{result.error()};
    EXPECT_EQ(error.position(), 0u);
    EXPECT_FALSE(error.message().empty());
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

TEST_F(Tokenizer_test, Modes)
{
    enum class Mode_token : std::size_t
    {
        Whitespace = 1,
        Word,
        Include,
        Header_name
    };

    enum class Mode : std::size_t
    {
        Code,
        Header
    };

    // In code mode `<stdio.h>` is unrecognizable; only the header mode's lexer knows header-names.
    Builder code;

    code.add_token(plus(any_of(Set::whitespace())), Mode_token::Whitespace, 1);
    code.add_token(text("#include"), Mode_token::Include, 0);
    code.add_token(plus(any_of(Set::alpha())), Mode_token::Word, 1);

    Builder header;

    header.add_token(plus(any_of(Set::whitespace())), Mode_token::Whitespace, 1);
    header.add_token(concat(text("<"), plus(any_of(Set::alpha() + '.')), text(">")), Mode_token::Header_name, 0);

    Tokenizer tokenizer{{code.build(), header.build()}, "#include <stdio.h> done"};

    EXPECT_EQ(tokenizer.mode(), 0u);

    auto result{tokenizer.next<Mode_token>()};
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::Include);

    ASSERT_TRUE(tokenizer.next<Mode_token>().has_token());

    // The driver saw #include and switches to the header-name mode, then back.
    tokenizer.set_mode(Mode::Header);
    EXPECT_EQ(tokenizer.mode(), 1u);

    result = tokenizer.next<Mode_token>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::Header_name);
    EXPECT_EQ(result.token().lexeme(), "<stdio.h>");

    tokenizer.set_mode(Mode::Code);

    ASSERT_TRUE(tokenizer.next<Mode_token>().has_token());

    result = tokenizer.next<Mode_token>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::Word);
    EXPECT_EQ(result.token().lexeme(), "done");

    EXPECT_TRUE(tokenizer.next<Mode_token>().end_of_input());

    EXPECT_THROW(tokenizer.set_mode(5), std::out_of_range);
}

namespace
{
enum class Ctx : std::size_t
{
    code,
    string,
    comment
};

enum class Ctx_token : std::size_t
{
    identifier,
    quote,
    text,
    open_comment,
    close_comment,
    space
};

/**
 * @brief A grammar whose mode transitions live in the grammar rather than in the driver.
 */
munch::core::Mode_lexer contextual()
{
    using namespace munch::regex;

    munch::core::Mode_builder builder;

    builder.add_token(Ctx::code, plus(any_of(Set::alpha())), Ctx_token::identifier, 2);
    builder.add_token(Ctx::code, any_of(Set{' '}), Ctx_token::space, 2);
    builder.add_token(
            Ctx::code, text("\""), Ctx_token::quote, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = static_cast<std::size_t>(Ctx::string)});
    builder.add_token(
            Ctx::code, text("/*"), Ctx_token::open_comment, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = static_cast<std::size_t>(Ctx::comment)});

    builder.add_token(Ctx::string, text("\""), Ctx_token::quote, 1, {.kind = munch::core::Mode_action_kind::pop});
    builder.add_token(Ctx::string, plus(any_of(Set::all() - '"')), Ctx_token::text, 2);

    builder.add_token(
            Ctx::comment, text("/*"), Ctx_token::open_comment, 1,
            {.kind = munch::core::Mode_action_kind::push, .target = static_cast<std::size_t>(Ctx::comment)});
    builder.add_token(
            Ctx::comment, text("*/"), Ctx_token::close_comment, 1, {.kind = munch::core::Mode_action_kind::pop});
    builder.add_token(Ctx::comment, any_of(Set::all()), Ctx_token::text, 2);

    return builder.build();
}
} // namespace

TEST(Tokenizer_modes, The_grammar_switches_modes_without_the_driver_asking)
{
    munch::tools::tokenizer::Tokenizer tokenizer{contextual(), R"(ab "cd" ef)"};

    std::vector<std::pair<Ctx_token, std::size_t>> seen;

    for (;;)
    {
        const auto result{tokenizer.next<Ctx_token>()};

        if (result.end_of_input())
        {
            break;
        }

        ASSERT_FALSE(result.has_error()) << "at offset " << tokenizer.offset();

        seen.emplace_back(result.token().kind(), tokenizer.mode());
    }

    // The quote pushes into string mode and the closing quote pops back out, with no set_mode() call anywhere.
    ASSERT_EQ(seen.size(), 7U);

    EXPECT_EQ(seen[0].second, static_cast<std::size_t>(Ctx::code));

    EXPECT_EQ(seen[2].second, static_cast<std::size_t>(Ctx::string));

    EXPECT_EQ(seen[3].second, static_cast<std::size_t>(Ctx::string));

    EXPECT_EQ(seen.back().second, static_cast<std::size_t>(Ctx::code));
}

TEST(Tokenizer_modes, Depth_reports_nesting_and_survives_to_the_stopping_point)
{
    munch::tools::tokenizer::Tokenizer tokenizer{contextual(), "a /* x /* y"};

    while (!tokenizer.next<Ctx_token>().end_of_input())
    {
    }

    // Two opens, no closes: the scan ends inside a doubly nested comment, and says so.
    EXPECT_EQ(tokenizer.mode(), static_cast<std::size_t>(Ctx::comment));

    EXPECT_EQ(tokenizer.depth(), 2U);
}

TEST(Tokenizer_modes, Load_discards_saved_frames_but_keeps_the_mode)
{
    munch::tools::tokenizer::Tokenizer tokenizer{contextual(), R"("unterminated)"};

    while (!tokenizer.next<Ctx_token>().end_of_input())
    {
    }

    ASSERT_EQ(tokenizer.depth(), 1U);

    tokenizer.load("ab");

    // The frames described nesting in input that is gone; popping into it would enter a mode the new input never
    // opened.
    EXPECT_EQ(tokenizer.depth(), 0U);
}

TEST(Tokenizer_modes, Set_mode_still_forces_a_mode_when_the_grammar_drives_them)
{
    munch::tools::tokenizer::Tokenizer tokenizer{contextual(), "abc"};

    tokenizer.set_mode(Ctx::comment);

    EXPECT_EQ(tokenizer.mode(), static_cast<std::size_t>(Ctx::comment));

    EXPECT_THROW(tokenizer.set_mode(std::size_t{9}), std::out_of_range);
}

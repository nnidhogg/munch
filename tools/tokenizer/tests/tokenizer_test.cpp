#include "munch/tools/tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/core/mode_builder.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/tokenizer/mode_tokenizer.hpp"
#include "munch/tools/tokenizer/raw_string.hpp"

using namespace munch::core;
using namespace munch::regex;
using namespace munch::tools::tokenizer;

namespace
{
/**
 * @brief The flat token set's kinds.
 */
enum class Token_kind : std::size_t
{
    /**
     * @brief The keyword boolean.
     */
    boolean_keyword,

    /**
     * @brief The keyword char.
     */
    char_keyword,

    /**
     * @brief The keyword string.
     */
    string_keyword,

    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief An integer literal.
     */
    integer_literal,

    /**
     * @brief A string literal.
     */
    string_literal,

    /**
     * @brief A fixed-point literal.
     */
    fixed_point_literal,

    /**
     * @brief A floating-point literal.
     */
    floating_point_literal,

    /**
     * @brief A single-line comment.
     */
    single_line_comment,

    /**
     * @brief A multi-line comment.
     */
    multi_line_comment,

    /**
     * @brief A whitespace run.
     */
    whitespace,

    /**
     * @brief A run of newlines.
     */
    newline
};

/**
 * @brief The modes of the grammar-driven tests.
 */
enum class Context : std::size_t
{
    /**
     * @brief Code outside strings and comments.
     */
    code,

    /**
     * @brief A string interior.
     */
    string,

    /**
     * @brief A comment interior.
     */
    comment
};

/**
 * @brief The token kinds of the grammar-driven tests.
 */
enum class Context_token : std::size_t
{
    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief A quote, opening or closing a string.
     */
    quote,

    /**
     * @brief A string or comment body.
     */
    text,

    /**
     * @brief A comment opener.
     */
    open_comment,

    /**
     * @brief A comment closer.
     */
    close_comment,

    /**
     * @brief A space.
     */
    space
};

/**
 * @brief The token kinds of the recovery tests.
 */
enum class Recovery_token : std::size_t
{
    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief A whitespace run.
     */
    whitespace,

    /**
     * @brief A semicolon.
     */
    semicolon,

    /**
     * @brief A number.
     */
    number
};

/**
 * @brief Returns the regex of an identifier: a letter or underscore, then letters, digits and underscores.
 * @return The regex.
 */
Regex identifier_regex()
{
    const auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};

    return identifier;
}

/**
 * @brief Returns the regex of a run of digits.
 * @return The regex.
 */
Regex integer_literal_regex()
{
    const auto integer_literal{plus(any_of(Set::digits()))};

    return integer_literal;
}

/**
 * @brief Returns the regex of a double-quoted run of printable bytes.
 * @return The regex.
 */
Regex string_literal_regex()
{
    const auto string_literal{concat(text(R"(")"), kleene(any_of(Set::printable())), text(R"(")"))};

    return string_literal;
}

/**
 * @brief Returns the regex of digits, a point and digits.
 * @return The regex.
 */
Regex fixed_point_literal_regex()
{
    const auto fixed_point_literal{concat(plus(any_of(Set::digits())), text("."), plus(any_of(Set::digits())))};

    return fixed_point_literal;
}

/**
 * @brief Returns the regex of a signed floating-point literal with an optional exponent, or a forced one.
 * @return The regex.
 */
Regex floating_point_literal_regex()
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

/**
 * @brief Returns the regex of two slashes and every printable or escape byte up to a newline.
 * @return The regex.
 */
Regex single_line_comment_regex()
{
    const auto single_line_comment{
            concat(text("//"), kleene(any_of(Set::printable() + Set::escape() - Set::newline())))};

    return single_line_comment;
}

/**
 * @brief Returns the regex of a slash-star comment of printable and escape bytes.
 * @return The regex.
 */
Regex multi_line_comment_regex()
{
    const auto multi_line_comment{concat(text("/*"), kleene(any_of(Set::printable() + Set::escape())), text("*/"))};

    return multi_line_comment;
}

/**
 * @brief Returns the regex of a whitespace run.
 * @return The regex.
 */
Regex whitespace_regex()
{
    const auto whitespace{plus(any_of(Set::whitespace()))};

    return whitespace;
}

/**
 * @brief Returns the regex of a run of newlines.
 * @return The regex.
 */
Regex newline_regex()
{
    const auto newline{plus(any_of(Set::newline()))};

    return newline;
}

/**
 * @brief Builds the flat token set: three keywords, identifiers, the literals, both comment forms, whitespace and
 *        newlines, each at its priority.
 * @return The lexer.
 */
Lexer build_lexer()
{
    Builder builder{};

    builder.add_token(text("boolean"), Token_kind::boolean_keyword, 1);
    builder.add_token(text("char"), Token_kind::char_keyword, 1);
    builder.add_token(text("string"), Token_kind::string_keyword, 1);

    builder.add_token(identifier_regex(), Token_kind::identifier, 4);

    builder.add_token(integer_literal_regex(), Token_kind::integer_literal, 2);
    builder.add_token(string_literal_regex(), Token_kind::string_literal, 2);
    builder.add_token(fixed_point_literal_regex(), Token_kind::fixed_point_literal, 2);
    builder.add_token(floating_point_literal_regex(), Token_kind::floating_point_literal, 3);

    builder.add_token(single_line_comment_regex(), Token_kind::single_line_comment, 0);
    builder.add_token(multi_line_comment_regex(), Token_kind::multi_line_comment, 0);

    builder.add_token(whitespace_regex(), Token_kind::whitespace, 0);

    builder.add_token(newline_regex(), Token_kind::newline, 0);

    return builder.build();
}

/**
 * @brief Builds a grammar whose mode transitions live in the grammar rather than in the driver.
 * @return The mode lexer.
 */
Mode_lexer contextual()
{
    Mode_builder builder{};

    builder.add_token(Context::code, plus(any_of(Set::alpha())), Context_token::identifier, 2);
    builder.add_token(Context::code, any_of(Set{' '}), Context_token::space, 2);
    builder.add_token(
            Context::code, text(R"(")"), Context_token::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Context::string)});
    builder.add_token(
            Context::code, text("/*"), Context_token::open_comment, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Context::comment)});

    builder.add_token(Context::string, text(R"(")"), Context_token::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Context::string, plus(any_of(Set::all() - '"')), Context_token::text, 2);

    builder.add_token(
            Context::comment, text("/*"), Context_token::open_comment, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Context::comment)});
    builder.add_token(Context::comment, text("*/"), Context_token::close_comment, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Context::comment, any_of(Set::all()), Context_token::text, 2);

    return builder.build();
}

/**
 * @brief Reads tokens until the tokenizer reports the end of the input.
 * @param tokenizer The tokenizer, over a grammar of Context_token kinds.
 */
void drain(Mode_tokenizer& tokenizer)
{
    while (!tokenizer.next<Context_token>().end_of_input())
    {
    }
}

/**
 * @brief Builds the window fixture: identifiers over letters and whitespace runs, where no byte certifies and recovery
 *        rests on windows alone.
 * @return The lexer.
 */
Lexer window_lexer()
{
    Builder builder{};

    builder.add_token(plus(any_of(Set::alpha())), Recovery_token::identifier, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Recovery_token::whitespace, 1);

    return builder.build();
}

/**
 * @brief Builds the {ab, ;} fixture: the identifier "ab" and the semicolon, both single-literal tokens.
 * @return The lexer.
 */
Lexer ab_semicolon_lexer()
{
    Builder builder{};

    builder.add_token(text("ab"), Recovery_token::identifier, 1);
    builder.add_token(text(";"), Recovery_token::semicolon, 1);

    return builder.build();
}

} // namespace

TEST(Tokenizer_test, Reads_every_token_in_order_and_again_after_reset_and_load)
{
    const std::string input{
            "boolean x 1234 \"hello\" 3.14 // comment\n"
            "string y 5.0e+1 /* block */"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    const auto advance{[&tokenizer](const Token_kind expect_kind, const std::string_view expect_lexeme) {
        const auto result{tokenizer.next<Token_kind>()};

        ASSERT_TRUE(result.has_token());

        const auto& token{result.token()};

        EXPECT_EQ(token.kind(), expect_kind);
        EXPECT_EQ(token.lexeme(), expect_lexeme);
    }};

    const auto expect_every_token{[&tokenizer, &advance] {
        advance(Token_kind::boolean_keyword, "boolean");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::identifier, "x");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::integer_literal, "1234");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::string_literal, R"("hello")");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::fixed_point_literal, "3.14");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::single_line_comment, "// comment");
        advance(Token_kind::newline, "\n");
        advance(Token_kind::string_keyword, "string");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::identifier, "y");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::floating_point_literal, "5.0e+1");
        advance(Token_kind::whitespace, " ");
        advance(Token_kind::multi_line_comment, "/* block */");

        const auto eof{tokenizer.next<Token_kind>()};

        EXPECT_TRUE(eof.end_of_input());
    }};

    expect_every_token();

    tokenizer.reset();

    expect_every_token();

    tokenizer.load(input);

    expect_every_token();
}

TEST(Tokenizer_test, A_zero_width_match_is_an_error_at_its_position)
{
    enum class Digits_kind : std::size_t
    {
        digits
    };

    Builder builder{};

    builder.add_token(kleene(any_of(Set::digits())), Digits_kind::digits, 1);

    Tokenizer tokenizer{builder.build(), std::string{"a"}};

    const auto result{tokenizer.next<Digits_kind>()};

    ASSERT_TRUE(result.has_error());

    const auto& error{result.error()};

    EXPECT_EQ(error.position(), 0U);
    EXPECT_FALSE(error.message().empty());
}

TEST(Tokenizer_test, An_unrecognized_byte_is_an_error_at_its_position)
{
    // '$' is not recognized.
    const std::string input{"$boolean"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer};

    tokenizer.load(input);

    const auto result{tokenizer.next<Token_kind>()};

    ASSERT_TRUE(result.has_error());

    const auto& error{result.error()};

    EXPECT_EQ(error.position(), 0U);
    EXPECT_FALSE(error.message().empty());
}

TEST(Tokenizer_test, The_offset_follows_each_token_and_returns_to_zero_on_reset_and_load)
{
    const std::string input{"boolean x 123"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    const auto advance{[&tokenizer](const Token_kind expect_kind, const std::size_t expect_offset) {
        const auto result{tokenizer.next<Token_kind>()};

        ASSERT_TRUE(result.has_token());
        EXPECT_EQ(result.token().kind(), expect_kind);
        EXPECT_EQ(tokenizer.offset(), expect_offset);
    }};

    // The offset starts at 0.
    EXPECT_EQ(tokenizer.offset(), 0U);

    // "boolean" takes it to 7.
    advance(Token_kind::boolean_keyword, 7U);

    // A space takes it to 8.
    advance(Token_kind::whitespace, 8U);

    // "x" takes it to 9.
    advance(Token_kind::identifier, 9U);

    // A space takes it to 10.
    advance(Token_kind::whitespace, 10U);

    // "123" takes it to 13, the end.
    advance(Token_kind::integer_literal, 13U);

    // At the end of the input the offset stays at the end.
    const auto result{tokenizer.next<Token_kind>()};

    EXPECT_TRUE(result.end_of_input());
    EXPECT_EQ(tokenizer.offset(), 13U);

    // A reset returns it to 0.
    tokenizer.reset();

    EXPECT_EQ(tokenizer.offset(), 0U);

    // A load of new input returns it to 0.
    tokenizer.load("char");

    EXPECT_EQ(tokenizer.offset(), 0U);
}

TEST(Tokenizer_test, Seek_moves_the_reading_position_and_clamps_to_the_end)
{
    const std::string input{"boolean x"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    // The scan consumes "boolean", rewinds and reads it again.
    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());
    EXPECT_EQ(tokenizer.offset(), 7U);

    tokenizer.seek(0);

    EXPECT_EQ(tokenizer.offset(), 0U);

    auto result{tokenizer.next<Token_kind>()};

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::boolean_keyword);

    // The scan jumps over the whitespace, as a driver does after scanning a token by hand.
    tokenizer.seek(8);

    result = tokenizer.next<Token_kind>();

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::identifier);
    EXPECT_EQ(result.token().lexeme(), "x");

    // Seeking past the end clamps to it.
    tokenizer.seek(100);

    EXPECT_EQ(tokenizer.offset(), input.size());
    EXPECT_TRUE(tokenizer.next<Token_kind>().end_of_input());
}

TEST(Tokenizer_test, The_documented_raw_string_dance_scans_by_hand_and_seeks_past)
{
    // seek()'s documentation names its canonical use: read the prefix token, scan the raw string literal by hand, seek
    // past it, read on. The body holds a quote, a fake comment opener, and a byte no pattern accepts, so reaching the
    // closing token proves the hand scan carried the driver over all of them.
    const std::string input{R"input(boolean R"x(ab "c" /* @ )" )x" char)input"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());
    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());

    const auto prefix{tokenizer.next<Token_kind>()};

    ASSERT_TRUE(prefix.has_token());
    ASSERT_EQ(prefix.token().kind(), Token_kind::identifier);
    ASSERT_EQ(prefix.token().lexeme(), "R");

    const auto literal_start{tokenizer.offset() - prefix.token().lexeme().size()};

    ASSERT_EQ(tokenizer.input()[tokenizer.offset()], '"');

    const auto length{scan_raw_string(tokenizer.input(), literal_start)};

    ASSERT_TRUE(length.has_value());

    tokenizer.seek(literal_start + *length);

    auto result{tokenizer.next<Token_kind>()};

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::whitespace);

    result = tokenizer.next<Token_kind>();

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::char_keyword);

    EXPECT_TRUE(tokenizer.next<Token_kind>().end_of_input());
}

TEST(Tokenizer_test, The_documented_error_loop_skips_and_resumes)
{
    // next()'s documentation prescribes the driver loop: an error does not advance, so the driver seeks past the
    // offending byte and calls again. This runs that loop to completion and pins the tokens it reads and the positions
    // of the errors it skips.
    const std::string input{"boolean @# x $ 123"};

    const auto lexer{build_lexer()};

    Tokenizer tokenizer{lexer, input};

    std::vector<Token_kind> kinds{};

    std::vector<std::size_t> error_positions{};

    for (;;)
    {
        const auto result{tokenizer.next<Token_kind>()};

        if (result.end_of_input())
        {
            break;
        }

        if (result.has_error())
        {
            const auto position{result.error().position()};

            error_positions.push_back(position);

            tokenizer.seek(position + 1);

            continue;
        }

        kinds.push_back(result.token().kind());
    }

    const std::vector expected{Token_kind::boolean_keyword, Token_kind::whitespace, Token_kind::whitespace,
                               Token_kind::identifier,      Token_kind::whitespace, Token_kind::whitespace,
                               Token_kind::integer_literal};

    EXPECT_EQ(kinds, expected);

    EXPECT_EQ(error_positions, (std::vector<std::size_t>{8, 9, 13}));
}

TEST(Mode_tokenizer_test, Throws_on_an_empty_lexer_list)
{
    EXPECT_THROW(Mode_tokenizer(std::vector<Lexer>{}), std::invalid_argument);
}

TEST(Mode_tokenizer_test, The_driver_switches_modes_with_set_mode)
{
    enum class Mode_token : std::size_t
    {
        whitespace,
        word,
        include,
        header_name
    };

    // The two modes, by lexer index.
    enum class Mode : std::size_t
    {
        code,
        header
    };

    // In code mode `<stdio.h>` is unrecognizable; only the header mode's lexer knows header-names.
    Builder code{};

    code.add_token(plus(any_of(Set::whitespace())), Mode_token::whitespace, 1);
    code.add_token(text("#include"), Mode_token::include, 0);
    code.add_token(plus(any_of(Set::alpha())), Mode_token::word, 1);

    Builder header{};

    header.add_token(plus(any_of(Set::whitespace())), Mode_token::whitespace, 1);
    header.add_token(concat(text("<"), plus(any_of(Set::alpha() + '.')), text(">")), Mode_token::header_name, 0);

    Mode_tokenizer tokenizer{{code.build(), header.build()}, "#include <stdio.h> done"};

    EXPECT_EQ(tokenizer.mode(), 0U);

    auto result{tokenizer.next<Mode_token>()};

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::include);

    ASSERT_TRUE(tokenizer.next<Mode_token>().has_token());

    // The driver saw #include and switches to the header-name mode, then back.
    tokenizer.set_mode(Mode::header);

    EXPECT_EQ(tokenizer.mode(), 1U);

    result = tokenizer.next<Mode_token>();

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::header_name);
    EXPECT_EQ(result.token().lexeme(), "<stdio.h>");

    tokenizer.set_mode(Mode::code);

    ASSERT_TRUE(tokenizer.next<Mode_token>().has_token());

    result = tokenizer.next<Mode_token>();

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Mode_token::word);
    EXPECT_EQ(result.token().lexeme(), "done");

    EXPECT_TRUE(tokenizer.next<Mode_token>().end_of_input());

    EXPECT_THROW(tokenizer.set_mode(5), std::out_of_range);
}

TEST(Mode_tokenizer_test, The_grammar_switches_modes_without_the_driver_asking)
{
    Mode_tokenizer tokenizer{contextual(), R"(ab "cd" ef)"};

    std::vector<std::pair<Context_token, std::size_t>> seen{};

    for (;;)
    {
        const auto result{tokenizer.next<Context_token>()};

        if (result.end_of_input())
        {
            break;
        }

        ASSERT_FALSE(result.has_error()) << "at offset " << tokenizer.offset();

        seen.emplace_back(result.token().kind(), tokenizer.mode());
    }

    // The quote pushes into string mode and the closing quote pops back out, with no set_mode() call anywhere.
    ASSERT_EQ(seen.size(), 7U);

    const auto mode_after{[&seen](const std::size_t index) {
        const auto& [kind, mode]{seen[index]};

        return mode;
    }};

    EXPECT_EQ(mode_after(0), std::to_underlying(Context::code));

    EXPECT_EQ(mode_after(2), std::to_underlying(Context::string));

    EXPECT_EQ(mode_after(3), std::to_underlying(Context::string));

    EXPECT_EQ(mode_after(seen.size() - 1), std::to_underlying(Context::code));
}

TEST(Mode_tokenizer_test, Depth_reports_nesting_and_survives_to_the_stopping_point)
{
    Mode_tokenizer tokenizer{contextual(), "a /* x /* y"};

    drain(tokenizer);

    // Two opens, no closes: the scan ends inside a doubly nested comment, and says so.
    EXPECT_EQ(tokenizer.mode(), std::to_underlying(Context::comment));

    EXPECT_EQ(tokenizer.depth(), 2U);
}

TEST(Mode_tokenizer_test, Load_rewinds_the_driven_mode_with_the_frames)
{
    Mode_tokenizer tokenizer{contextual(), R"("unterminated)"};

    drain(tokenizer);

    ASSERT_EQ(tokenizer.depth(), 1U);

    ASSERT_NE(tokenizer.mode(), 0U);

    tokenizer.load("ab");

    // A load returns the depth and the mode to zero, whatever nesting the replaced input left open.
    EXPECT_EQ(tokenizer.depth(), 0U);

    EXPECT_EQ(tokenizer.mode(), 0U);
}

TEST(Mode_tokenizer_test, Reset_rewinds_the_driven_mode_with_the_position)
{
    Mode_tokenizer tokenizer{contextual(), R"("unterminated)"};

    drain(tokenizer);

    ASSERT_NE(tokenizer.mode(), 0U);

    tokenizer.reset();

    // The mode belongs to the text just rewound past, so rewinding the position rewinds it too.
    EXPECT_EQ(tokenizer.mode(), 0U);

    EXPECT_EQ(tokenizer.depth(), 0U);
}

TEST(Mode_tokenizer_test, Reset_and_load_keep_a_caller_driven_mode)
{
    enum class Header_token : std::size_t
    {
        word,
        header_name
    };

    // Where the caller drives the modes the current mode is its choice and not the input's, so a reset and a load keep
    // it, where they rewind a grammar-driven one.
    Builder code{};

    code.add_token(plus(any_of(Set::alpha())), Header_token::word, 0);

    Builder header{};

    header.add_token(plus(any_of(Set::alpha() + '.')), Header_token::header_name, 0);

    Mode_tokenizer tokenizer{{code.build(), header.build()}, "stdio.h"};

    tokenizer.set_mode(std::size_t{1});

    ASSERT_TRUE(tokenizer.next<Header_token>().has_token());
    EXPECT_EQ(tokenizer.mode(), 1U);

    tokenizer.reset();

    EXPECT_EQ(tokenizer.mode(), 1U);
    EXPECT_EQ(tokenizer.offset(), 0U);

    const auto after_reset{tokenizer.next<Header_token>()};

    ASSERT_TRUE(after_reset.has_token());
    EXPECT_EQ(after_reset.token().kind(), Header_token::header_name);

    tokenizer.load("string.h");

    EXPECT_EQ(tokenizer.mode(), 1U);

    const auto after_load{tokenizer.next<Header_token>()};

    ASSERT_TRUE(after_load.has_token());
    EXPECT_EQ(after_load.token().kind(), Header_token::header_name);
}

TEST(Mode_tokenizer_test, Reset_rewinds_a_mode_that_set_mode_forced)
{
    Mode_tokenizer tokenizer{contextual(), "ab"};

    tokenizer.set_mode(std::size_t{1});

    ASSERT_EQ(tokenizer.mode(), 1U);

    tokenizer.reset();

    // Nothing records whether the driver or the caller chose the mode, so a driven reset returns to zero either way.
    EXPECT_EQ(tokenizer.mode(), 0U);
}

TEST(Mode_tokenizer_test, Set_mode_still_forces_a_mode_when_the_grammar_drives_them)
{
    Mode_tokenizer tokenizer{contextual(), "abc"};

    tokenizer.set_mode(Context::comment);

    EXPECT_EQ(tokenizer.mode(), std::to_underlying(Context::comment));

    EXPECT_THROW(tokenizer.set_mode(std::size_t{9}), std::out_of_range);
}

TEST(Mode_tokenizer_test, Forcing_a_mode_is_the_documented_recovery_hatch_after_an_error)
{
    // set_mode() documents forcing as the recovery hatch after an error, leaving the saved frames alone. A newline is
    // illegal in this string mode, so the scan stops mid-string with the frame on the stack, as depth() diagnoses an
    // unterminated string; the driver forces code mode, seeks past the wreck and reads on, the frame kept throughout.
    Mode_builder builder{};

    builder.add_token(Context::code, plus(any_of(Set::alpha())), Context_token::identifier, 2);
    builder.add_token(Context::code, any_of(Set{' '}), Context_token::space, 2);
    builder.add_token(
            Context::code, text(R"(")"), Context_token::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Context::string)});

    builder.add_token(Context::string, text(R"(")"), Context_token::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Context::string, plus(any_of(Set::all() - '"' - '\n')), Context_token::text, 2);

    const std::string input{"ab \"cd\nef\" gh"};

    Mode_tokenizer tokenizer{builder.build(), input};

    ASSERT_TRUE(tokenizer.next<Context_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Context_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Context_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Context_token>().has_token());

    const auto stopped{tokenizer.next<Context_token>()};

    ASSERT_TRUE(stopped.has_error());
    EXPECT_EQ(stopped.error().position(), 6U);

    // The stopped scan is diagnosable: string mode at depth one says unterminated string, not stray byte.
    EXPECT_EQ(tokenizer.mode(), std::to_underlying(Context::string));
    EXPECT_EQ(tokenizer.depth(), 1U);

    tokenizer.set_mode(Context::code);

    EXPECT_EQ(tokenizer.mode(), std::to_underlying(Context::code));
    EXPECT_EQ(tokenizer.depth(), 1U);

    const auto failure{stopped.error().position()};

    const auto closing_quote{input.find('"', failure)};

    tokenizer.seek(closing_quote + 1);

    std::vector<Context_token> kinds{};

    for (;;)
    {
        const auto result{tokenizer.next<Context_token>()};

        if (result.end_of_input())
        {
            break;
        }

        ASSERT_FALSE(result.has_error());

        kinds.push_back(result.token().kind());
    }

    EXPECT_EQ(kinds, (std::vector{Context_token::space, Context_token::identifier}));

    // The forced switch left the saved frame alone, as documented.
    EXPECT_EQ(tokenizer.depth(), 1U);
    EXPECT_EQ(tokenizer.mode(), std::to_underlying(Context::code));
}

TEST(Tokenizer_recovery_test, Recover_lands_at_the_certified_window_origin)
{
    // No byte certifies over identifiers and whitespace runs, so recovery rests on windows alone: the first certificate
    // past the junk is the four-byte "def " at offset 6 with origin 3, so recovery resumes at the origin, offset 9, the
    // whitespace that begins a token, and skips exactly the six bytes from 3.
    Tokenizer tokenizer{window_lexer(), "abc@@@def ghi"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());

    const auto stopped{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(stopped.has_error());
    ASSERT_EQ(stopped.error().position(), 3U);

    const auto skipped{tokenizer.recover()};

    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(*skipped, 6U);
    EXPECT_EQ(tokenizer.offset(), 9U);

    auto result{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Recovery_token::whitespace);

    result = tokenizer.next<Recovery_token>();

    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Recovery_token::identifier);
    EXPECT_EQ(result.token().lexeme(), "ghi");

    EXPECT_TRUE(tokenizer.next<Recovery_token>().end_of_input());
}

TEST(Tokenizer_recovery_test, Recover_from_failure_returns_the_window_evidence_interval)
{
    // The window fixture, asked through the evidence-returning interface: the four-byte occurrence "def " begins at 6
    // with origin 3, so the certified start is 9 and the evidence interval is [6, 10), reported as a window.
    // recover()'s skip count is this start minus the position before.
    Tokenizer tokenizer{window_lexer(), "abc@@@def ghi"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_error());

    const auto found{tokenizer.recover_from_failure()};

    ASSERT_TRUE(found.has_value());

    const auto& [start, evidence_begin, evidence_end, window]{*found};

    EXPECT_EQ(start, 9U);
    EXPECT_EQ(evidence_begin, 6U);
    EXPECT_EQ(evidence_end, 10U);
    EXPECT_TRUE(window);
    EXPECT_EQ(tokenizer.offset(), 9U);
}

TEST(Tokenizer_recovery_test, Recover_from_clean_floors_the_search_and_covers_the_evidence)
{
    // The blind search answers at the first semicolon, inside what the caller knows is damaged; the clean-anchored
    // search starts at the caller's clean offset and answers there, its byte evidence at or past the floor by
    // construction. The two arms differ on the same input, which is the interface split's whole point.
    const auto lexer{ab_semicolon_lexer()};

    Tokenizer blind{lexer, "@;@;ab"};

    ASSERT_TRUE(blind.next<Recovery_token>().has_error());

    const auto blind_found{blind.recover_from_failure()};

    ASSERT_TRUE(blind_found.has_value());

    const auto& [blind_start, blind_evidence_begin, blind_evidence_end, blind_window]{*blind_found};

    EXPECT_EQ(blind_start, 1U);
    EXPECT_EQ(blind_evidence_begin, 1U);
    EXPECT_EQ(blind_evidence_end, 2U);
    EXPECT_FALSE(blind_window);

    Tokenizer clean{lexer, "@;@;ab"};

    ASSERT_TRUE(clean.next<Recovery_token>().has_error());

    const auto clean_found{clean.recover_from_clean(4)};

    ASSERT_TRUE(clean_found.has_value());

    const auto& [clean_start, clean_evidence_begin, clean_evidence_end, clean_window]{*clean_found};

    EXPECT_EQ(clean_start, 4U);
    EXPECT_GE(clean_evidence_begin, 4U);
    EXPECT_EQ(clean.offset(), 4U);

    const auto resumed{clean.next<Recovery_token>()};

    ASSERT_TRUE(resumed.has_token());
    EXPECT_EQ(resumed.token().lexeme(), "ab");
}

TEST(Tokenizer_recovery_test, Recover_from_clean_refuses_without_moving)
{
    // A clean floor past every certificate leaves nothing to find; the refusal must not move the position, the same
    // fail-closed shape recover() keeps.
    Tokenizer tokenizer{ab_semicolon_lexer(), "@;ab"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_error());

    const auto before{tokenizer.offset()};

    EXPECT_FALSE(tokenizer.recover_from_clean(4).has_value());
    EXPECT_EQ(tokenizer.offset(), before);
}

TEST(Tokenizer_recovery_test, Recover_uses_certified_bytes_and_promises_nothing_past_resynchronization)
{
    // The semicolon is a certified byte, so recovery lands on it directly; the suffix then errors again, which is the
    // documented weaker contract on malformed input, and a search past the last byte finds nothing and moves nothing.
    Builder builder{};

    builder.add_token(text("a"), Recovery_token::identifier, 1);
    builder.add_token(text(";"), Recovery_token::semicolon, 1);

    Tokenizer tokenizer{builder.build(), "a;?;a;?"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());

    const auto stopped{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(stopped.has_error());
    ASSERT_EQ(stopped.error().position(), 2U);

    const auto skipped{tokenizer.recover()};

    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(*skipped, 1U);
    EXPECT_EQ(tokenizer.offset(), 3U);

    for (int read{0}; read < 3; ++read)
    {
        ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());
    }

    const auto again{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(again.has_error());
    ASSERT_EQ(again.error().position(), 6U);

    EXPECT_FALSE(tokenizer.recover().has_value());
    EXPECT_EQ(tokenizer.offset(), 6U);
}

TEST(Tokenizer_recovery_test, Recover_refuses_when_nothing_certifies)
{
    // A single unbounded run certifies neither bytes nor windows, so recovery finds nothing and the position stays put:
    // refusal, not a guess.
    Builder builder{};

    builder.add_token(plus(any_of(Set::alpha())), Recovery_token::identifier, 1);

    Tokenizer tokenizer{builder.build(), "abc?def"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());
    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_error());

    EXPECT_FALSE(tokenizer.recover().has_value());
    EXPECT_EQ(tokenizer.offset(), 3U);
}

TEST(Tokenizer_recovery_test, Recover_consults_the_active_mode_only)
{
    // The two modes, by lexer index.
    enum class Recovery_mode : std::size_t
    {
        code_mode,
        digit_mode
    };

    // The same input recovers differently per mode: mode zero's grammar certifies the semicolon, mode one's single run
    // swallows it, so recovery reads the current mode's grammar, refusing under mode one and skipping one byte under
    // mode zero.
    Builder code{};

    code.add_token(plus(any_of(Set::alpha())), Recovery_token::identifier, 1);
    code.add_token(text(";"), Recovery_token::semicolon, 1);

    Builder digits{};

    digits.add_token(plus(any_of(Set::digits() + ';')), Recovery_token::number, 1);

    Mode_tokenizer tokenizer{{code.build(), digits.build()}, "12?;34"};

    tokenizer.set_mode(Recovery_mode::digit_mode);

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());

    const auto stopped{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(stopped.has_error());
    ASSERT_EQ(stopped.error().position(), 2U);

    EXPECT_FALSE(tokenizer.recover().has_value());
    EXPECT_EQ(tokenizer.offset(), 2U);

    tokenizer.set_mode(Recovery_mode::code_mode);

    const auto skipped{tokenizer.recover()};

    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(*skipped, 1U);
    EXPECT_EQ(tokenizer.offset(), 3U);

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());
}

TEST(Tokenizer_recovery_test, Recover_always_advances_past_the_offending_byte)
{
    // On malformed input a certified byte can still fail to start a token: 'a' is certified over {ab, ;}, yet "a;" has
    // no token at the 'a'. The search starts one past the offending byte, so the skip count is positive and the offset
    // moves past the error position.
    const auto lexer{ab_semicolon_lexer()};

    ASSERT_TRUE(lexer.is_split_point('a'));

    Tokenizer tokenizer{lexer, ";a;ab"};

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());

    const auto stopped{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(stopped.has_error());
    ASSERT_EQ(stopped.error().position(), 1U);

    const auto skipped{tokenizer.recover()};

    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(*skipped, 1U);
    EXPECT_EQ(tokenizer.offset(), 2U);

    ASSERT_TRUE(tokenizer.next<Recovery_token>().has_token());

    const auto last{tokenizer.next<Recovery_token>()};

    ASSERT_TRUE(last.has_token());
    EXPECT_EQ(last.token().lexeme(), "ab");
}

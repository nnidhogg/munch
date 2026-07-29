#include "munch/tools/tokenizer/raw_string.hpp"

#include <gtest/gtest.h>

#include <string>

#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/tokenizer/tokenizer.hpp"

using namespace munch;
using namespace munch::regex;
using namespace munch::tools::tokenizer;

TEST(Raw_string_test, Scans_empty_delimiter)
{
    const std::string input{R"input(R"(abc)")input"};

    const auto length{scan_raw_string(input, 0)};

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), input.size());
}

TEST(Raw_string_test, Scans_empty_content)
{
    const std::string input{R"input(R"()")input"};

    const auto length{scan_raw_string(input, 0)};

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), input.size());
}

TEST(Raw_string_test, Delimiter_makes_quote_paren_plain_content)
{
    // The content holds `)"`, which only the delimited closing sequence `)x"` may end.
    const std::string input{R"input(R"x(a)" b)x")input"};

    const auto length{scan_raw_string(input, 0)};

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), input.size());
}

TEST(Raw_string_test, Scans_at_offset_after_encoding_prefix)
{
    const std::string input{R"input(u8R"(hi)" tail)input"};

    const auto length{scan_raw_string(input, 2)};

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(length.value(), std::string{R"input(R"(hi)")input"}.size());
}

TEST(Raw_string_test, Rejects_unterminated_literal)
{
    for (const std::string input : {R"input(R"(abc)input", R"input(R"(abc))input", R"input(R"abc)input"})
    {
        const auto length{scan_raw_string(input, 0)};

        ASSERT_FALSE(length.has_value());
        EXPECT_EQ(length.error().position(), 0u);
        EXPECT_FALSE(length.error().message().empty());
    }
}

TEST(Raw_string_test, Rejects_invalid_delimiter)
{
    // A space in the delimiter, then a delimiter past sixteen characters.
    const std::string spaced{"R\" (a)\""};

    EXPECT_FALSE(scan_raw_string(spaced, 0).has_value());

    const std::string overlong{"R\"aaaaaaaaaaaaaaaaa(x)aaaaaaaaaaaaaaaaa\""};

    EXPECT_FALSE(scan_raw_string(overlong, 0).has_value());
}

TEST(Raw_string_test, Rejects_delimiter_characters_the_standard_forbids)
{
    EXPECT_FALSE(scan_raw_string("R\")(\"", 0).has_value());    // ')' in the delimiter
    EXPECT_FALSE(scan_raw_string("R\"\\(\"", 0).has_value());   // '\' in the delimiter
    EXPECT_FALSE(scan_raw_string("R\"\t(\"", 0).has_value());   // A control character in the delimiter
    EXPECT_FALSE(scan_raw_string("R\"\x7F(\"", 0).has_value()); // DEL in the delimiter
}

TEST(Raw_string_test, Rejects_other_input)
{
    EXPECT_FALSE(scan_raw_string("Q\"(x)\"", 0).has_value());
    EXPECT_FALSE(scan_raw_string("Rx", 0).has_value());
    EXPECT_FALSE(scan_raw_string("R", 0).has_value());
    EXPECT_FALSE(scan_raw_string("", 0).has_value());
}

TEST(Raw_string_test, Drives_the_tokenizer_escape_hatch)
{
    enum class Token_kind : std::size_t
    {
        Whitespace = 1,
        Identifier,
        Raw_string_prefix
    };

    core::Builder builder;

    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::Whitespace, 1);
    builder.add_token(plus(any_of(Set::alpha())), Token_kind::Identifier, 1);

    // Two bytes beat the one-byte identifier `R` by maximal munch, so the prefix needs no special priority.
    builder.add_token(text("R\""), Token_kind::Raw_string_prefix, 0);

    Tokenizer tokenizer{builder.build(), std::string{R"input(x R"d(a)"b)d" y)input"}};

    auto result{tokenizer.next<Token_kind>()};
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Identifier);

    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());

    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    ASSERT_EQ(result.token().kind(), Token_kind::Raw_string_prefix);

    // The automaton found the prefix; scan the full literal by hand and seek past it.
    const auto start{tokenizer.offset() - result.token().lexeme().size()};

    const auto length{scan_raw_string(tokenizer.input(), start)};

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(tokenizer.input().substr(start, length.value()), R"input(R"d(a)"b)d")input");

    tokenizer.seek(start + length.value());

    ASSERT_TRUE(tokenizer.next<Token_kind>().has_token());

    result = tokenizer.next<Token_kind>();
    ASSERT_TRUE(result.has_token());
    EXPECT_EQ(result.token().kind(), Token_kind::Identifier);
    EXPECT_EQ(result.token().lexeme(), "y");

    EXPECT_TRUE(tokenizer.next<Token_kind>().end_of_input());
}

#include "munch/tools/audit/expression.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/regex/set.hpp"

using namespace munch::tools::audit;
using munch::regex::Set;

TEST(Expression_test, A_hex_digit_is_a_decimal_digit_or_a_letter_from_a_to_f_in_either_case)
{
    // Each byte class at the ends of its ranges, and the neighbours just outside them.
    for (const char byte : {'0', '9', 'a', 'f', 'A', 'F'})
    {
        EXPECT_TRUE(is_hex_digit(byte)) << static_cast<int>(byte);
    }

    for (const char byte : {'/', ':', '`', 'g', '@', 'G', ' ', '\0'})
    {
        EXPECT_FALSE(is_hex_digit(byte)) << static_cast<int>(byte);
    }
}

TEST(Expression_test, A_name_byte_is_an_ascii_letter_a_digit_or_an_underscore)
{
    for (const char byte : {'a', 'z', 'A', 'Z', '0', '9', '_'})
    {
        EXPECT_TRUE(is_name_byte(byte)) << static_cast<int>(byte);
    }

    for (const char byte : {'-', ' ', '.', '`', '{', '@', '[', '/', ':', '\0', static_cast<char>(0xC3)})
    {
        EXPECT_FALSE(is_name_byte(byte)) << static_cast<int>(byte);
    }
}

TEST(Expression_test, A_letter_is_an_ascii_letter_and_no_scalar_beyond_ascii)
{
    // The letter test takes scalars, so a letter beyond ASCII is the case it must refuse.
    for (const char32_t scalar : {U'a', U'z', U'A', U'Z'})
    {
        EXPECT_TRUE(is_letter(scalar)) << static_cast<unsigned>(scalar);
    }

    for (const char32_t scalar : {U'0', U'_', U'`', U'{', U'@', U'[', U'\0', U'\u00E9', U'\u0391'})
    {
        EXPECT_FALSE(is_letter(scalar)) << static_cast<unsigned>(scalar);
    }
}

TEST(Expression_test, A_bracket_member_is_escaped_where_the_bracket_syntax_would_read_it_otherwise)
{
    // A member is escaped where the bracket syntax would read it otherwise, named for the three whitespace escapes, hex
    // where it does not print and itself elsewhere; the printable ends and the byte past them pin where hex begins.
    const std::vector<std::pair<unsigned char, std::string>> members{
            {'\n', R"(\n)"},   {'\t', R"(\t)"},   {'\r', R"(\r)"},   {'\\', R"(\\)"},   {']', R"(\])"},
            {'[', R"(\[)"},    {'^', R"(\^)"},    {'-', R"(\-)"},    {'a', "a"},        {' ', " "},
            {'~', "~"},        {'"', R"(")"},     {0x00, R"(\x00)"}, {0x01, R"(\x01)"}, {0x1F, R"(\x1f)"},
            {0x7F, R"(\x7f)"}, {0x80, R"(\x80)"}, {0xFF, R"(\xff)"}};

    for (const auto& [byte, text] : members)
    {
        EXPECT_EQ(bracket_member(byte), text) << static_cast<unsigned>(byte);
    }
}

TEST(Expression_test, A_quoted_literal_escapes_the_quote_the_backslash_and_the_bytes_that_do_not_print)
{
    // A quoted literal escapes the quote and the backslash, writes a byte that does not print as the member would, and
    // leaves the bracket's own specials alone, since a literal has no bracket syntax to read them by.
    const std::vector<std::pair<std::string_view, std::string>> literals{
            {"", R"("")"},         {"abc", R"("abc")"},       {R"(a"b\c)", R"("a\"b\\c")"},
            {"[-]^", R"("[-]^")"}, {"x\ny\t", R"("x\ny\t")"}, {"\x01\x7F\xFF", R"("\x01\x7f\xff")"}};

    for (const auto& [bytes, text] : literals)
    {
        EXPECT_EQ(quoted(bytes), text) << text;
    }
}

TEST(Expression_test, A_bracket_writes_each_run_of_its_set_as_a_range)
{
    // A bracket writes each run of the set as a range, two adjacent bytes as themselves since a dash would save
    // nothing, singletons as themselves, in byte order, its members escaped as members are.
    const std::vector<std::pair<Set, std::string>> brackets{
            {Set{'a'}, "[a]"},
            {Set{'a', 'b'}, "[ab]"},
            {Set::range('a', 'c'), "[a-c]"},
            {Set::range('a', 'c') + 'x', "[a-cx]"},
            {Set{'-', ']'}, R"([\-\]])"},
            {Set::range('\0', '\x02'), R"([\x00-\x02])"},
            {Set::range(static_cast<char>(0xFE), static_cast<char>(0xFF)), R"([\xfe\xff])"},
            {Set::digits() + Set::range('a', 'f'), "[0-9a-f]"}};

    for (const auto& [set, text] : brackets)
    {
        EXPECT_EQ(bracket(set), text) << text;
    }
}

#include "munch/tools/audit/expression.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/regex/set.hpp"

using namespace munch::tools::audit;
using munch::regex::Set;

TEST(Expression, The_helpers_spell_bytes_and_scalars_as_the_parser_reads_them)
{
    // The three byte classes, each at the ends of its ranges and at the neighbours just outside them, where a
    // range test written with the wrong comparison would first go wrong; the letter test takes scalars, so a
    // letter beyond ASCII is the case it must refuse.
    for (const char byte : {'0', '9', 'a', 'f', 'A', 'F'})
    {
        EXPECT_TRUE(is_hex_digit(byte)) << static_cast<int>(byte);
    }

    for (const char byte : {'/', ':', '`', 'g', '@', 'G', ' ', '\0'})
    {
        EXPECT_FALSE(is_hex_digit(byte)) << static_cast<int>(byte);
    }

    for (const char byte : {'a', 'z', 'A', 'Z', '0', '9', '_'})
    {
        EXPECT_TRUE(is_name_byte(byte)) << static_cast<int>(byte);
    }

    for (const char byte : {'-', ' ', '.', '`', '{', '@', '[', '/', ':', '\0', static_cast<char>(0xC3)})
    {
        EXPECT_FALSE(is_name_byte(byte)) << static_cast<int>(byte);
    }

    for (const char32_t scalar : {U'a', U'z', U'A', U'Z'})
    {
        EXPECT_TRUE(is_letter(scalar)) << static_cast<unsigned>(scalar);
    }

    for (const char32_t scalar : {U'0', U'_', U'`', U'{', U'@', U'[', U'\0', U'\u00E9', U'\u0391'})
    {
        EXPECT_FALSE(is_letter(scalar)) << static_cast<unsigned>(scalar);
    }

    // Each encoding width at both its ends, the boundaries where a width test off by one changes the byte count.
    const std::vector<std::pair<char32_t, std::string>> encodings{
            {U'\0', std::string{'\0'}},
            {U'a', "a"},
            {U'\u007F', "\x7F"},
            {U'\u0080', "\xC2\x80"},
            {U'\u00E9', "\xC3\xA9"},
            {U'\u07FF', "\xDF\xBF"},
            {U'\u0800', "\xE0\xA0\x80"},
            {U'\u20AC', "\xE2\x82\xAC"},
            {U'\uFFFF', "\xEF\xBF\xBF"},
            {U'\U00010000', "\xF0\x90\x80\x80"},
            {U'\U0001F600', "\xF0\x9F\x98\x80"},
            {U'\U0010FFFF', "\xF4\x8F\xBF\xBF"}};

    for (const auto& [scalar, bytes] : encodings)
    {
        EXPECT_EQ(encoded(scalar), bytes) << static_cast<unsigned>(scalar);
    }

    // A member is escaped where the bracket syntax would read it otherwise, named for the three whitespace
    // escapes, hex where it does not print and itself elsewhere; the printable ends and the byte past them pin
    // where hex begins.
    const std::vector<std::pair<unsigned char, std::string>> members{
            {'\n', R"(\n)"},   {'\t', R"(\t)"},   {'\r', R"(\r)"},   {'\\', R"(\\)"},   {']', R"(\])"},
            {'[', R"(\[)"},    {'^', R"(\^)"},    {'-', R"(\-)"},    {'a', "a"},        {' ', " "},
            {'~', "~"},        {'"', R"(")"},     {0x00, R"(\x00)"}, {0x01, R"(\x01)"}, {0x1F, R"(\x1f)"},
            {0x7F, R"(\x7f)"}, {0x80, R"(\x80)"}, {0xFF, R"(\xff)"}};

    for (const auto& [byte, text] : members)
    {
        EXPECT_EQ(bracket_member(byte), text) << static_cast<unsigned>(byte);
    }

    // A quoted literal escapes the quote and the backslash, writes a byte that does not print as the member
    // would, and leaves the bracket's own specials alone, since a literal has no bracket syntax to read them by.
    const std::vector<std::pair<std::string_view, std::string>> literals{
            {"", R"("")"},         {"abc", R"("abc")"},       {R"(a"b\c)", R"("a\"b\\c")"},
            {"[-]^", R"("[-]^")"}, {"x\ny\t", R"("x\ny\t")"}, {"\x01\x7F\xFF", R"("\x01\x7f\xff")"}};

    for (const auto& [bytes, text] : literals)
    {
        EXPECT_EQ(quoted(bytes), text) << text;
    }

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

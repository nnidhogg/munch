#include "lexer/regex/utf8.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "lexer/nfa/simulator.hpp"

using namespace lexer;
using namespace lexer::nfa;
using namespace lexer::regex;

namespace
{
std::string encode(const char32_t code_point)
{
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
}

Nfa make_nfa(const Regex& regex)
{
    return to_nfa(regex).set_accept_token(Token{1, 1}).build();
}

bool matches(const Nfa& nfa, const std::string& input)
{
    const auto [token, length]{Simulator::run(nfa, input)};

    return token.has_value() && length == input.size();
}

} // namespace

TEST(Utf8_test, Ascii_range)
{
    const auto nfa{make_nfa(utf8::range(U'a', U'z'))};

    EXPECT_TRUE(matches(nfa, "a"));
    EXPECT_TRUE(matches(nfa, "m"));
    EXPECT_TRUE(matches(nfa, "z"));

    EXPECT_FALSE(matches(nfa, "A"));
    EXPECT_FALSE(matches(nfa, "{"));
    EXPECT_FALSE(matches(nfa, ""));
    EXPECT_FALSE(matches(nfa, "\x80"));
}

TEST(Utf8_test, Encoding_length_boundaries)
{
    const auto nfa{make_nfa(utf8::range(0x0, 0x10FFFF))};

    for (const char32_t code_point : {0x0U, 0x7FU, 0x80U, 0x7FFU, 0x800U, 0xD7FFU, 0xE000U, 0xFFFFU, 0x10000U,
                                      0x10FFFFU})
    {
        EXPECT_TRUE(matches(nfa, encode(code_point))) << "U+" << std::hex << static_cast<unsigned>(code_point);
    }
}

TEST(Utf8_test, Rejects_ill_formed_input)
{
    const auto nfa{make_nfa(utf8::range(0x0, 0x10FFFF))};

    // A stray continuation byte, an overlong encoding of NUL, a truncated sequence, an encoded surrogate, and the
    // first code point past U+10FFFF.
    EXPECT_FALSE(matches(nfa, "\x80"));
    EXPECT_FALSE(matches(nfa, "\xC0\x80"));
    EXPECT_FALSE(matches(nfa, "\xE0\xA0"));
    EXPECT_FALSE(matches(nfa, "\xED\xA0\x80"));
    EXPECT_FALSE(matches(nfa, "\xF4\x90\x80\x80"));
}

TEST(Utf8_test, Surrogates_excised_from_spanning_range)
{
    const auto nfa{make_nfa(utf8::range(0xD000, 0xE100))};

    EXPECT_TRUE(matches(nfa, encode(0xD000)));
    EXPECT_TRUE(matches(nfa, encode(0xD7FF)));
    EXPECT_TRUE(matches(nfa, encode(0xE000)));
    EXPECT_TRUE(matches(nfa, encode(0xE100)));

    EXPECT_FALSE(matches(nfa, "\xED\xA0\x80")); // U+D800
    EXPECT_FALSE(matches(nfa, "\xED\xBF\xBF")); // U+DFFF
}

TEST(Utf8_test, Membership_sweep)
{
    // Every code point around the range bounds and the two-to-three byte encoding boundary is accepted exactly when
    // it lies inside the range.
    const auto nfa{make_nfa(utf8::range(0x600, 0x900))};

    for (char32_t code_point{0x500}; code_point <= 0xA00; ++code_point)
    {
        const auto expected{code_point >= 0x600 && code_point <= 0x900};

        EXPECT_EQ(matches(nfa, encode(code_point)), expected) << "U+" << std::hex << static_cast<unsigned>(code_point);
    }
}

TEST(Utf8_test, Rejects_invalid_ranges)
{
    EXPECT_THROW((void)utf8::range(0x20, 0x10), std::invalid_argument);
    EXPECT_THROW((void)utf8::range(0x0, 0x110000), std::invalid_argument);
    EXPECT_THROW((void)utf8::range(0xD800, 0xDFFF), std::invalid_argument);
}

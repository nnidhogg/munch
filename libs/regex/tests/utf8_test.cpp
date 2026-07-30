#include "munch/regex/utf8.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "munch/nfa/simulator.hpp"

using namespace munch;
using namespace munch::nfa;
using namespace munch::regex;

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

    for (const char32_t code_point :
         {0x0U, 0x7FU, 0x80U, 0x7FFU, 0x800U, 0xD7FFU, 0xE000U, 0xFFFFU, 0x10000U, 0x10FFFFU})
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

TEST(Utf8_test, Random_subranges_match_exactly_their_contents)
{
    // A deterministic 64-bit LCG draws arbitrary interval bounds, attacking the recursive lower/middle/upper
    // decomposition at boundaries the block-aligned tests never produce; each range is probed at its edges, just
    // outside them, and at random candidates.
    std::uint64_t state{12345};

    const auto draw{[&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;

        return static_cast<char32_t>((state >> 32U) % 0x110000);
    }};

    const auto surrogate{[](const char32_t code_point) { return code_point >= 0xD800 && code_point <= 0xDFFF; }};

    for (int round{0}; round < 100; ++round)
    {
        auto first{draw()};

        auto last{draw()};

        if (first > last)
        {
            std::swap(first, last);
        }

        if (first >= 0xD800 && last <= 0xDFFF)
        {
            continue;
        }

        const auto nfa{make_nfa(utf8::range(first, last))};

        std::vector<char32_t> candidates{first, last};

        if (first > 0)
        {
            candidates.push_back(first - 1);
        }

        if (last < 0x10FFFF)
        {
            candidates.push_back(last + 1);
        }

        for (int probe{0}; probe < 25; ++probe)
        {
            candidates.push_back(draw());
        }

        for (const auto candidate : candidates)
        {
            if (surrogate(candidate))
            {
                continue;
            }

            const auto expected{candidate >= first && candidate <= last};

            EXPECT_EQ(matches(nfa, encode(candidate)), expected)
                    << std::hex << "U+" << static_cast<unsigned>(candidate) << " against U+"
                    << static_cast<unsigned>(first) << "..U+" << static_cast<unsigned>(last);
        }
    }
}

TEST(Utf8_test, Ranges_merges_adjacent_and_matches_the_union)
{
    constexpr std::array<utf8::Code_point_range, 3> list{{
            {.first = 0x41, .last = 0x5A},
            {.first = 0x5B, .last = 0x60},
            {.first = 0x100, .last = 0x200},
    }};

    const auto nfa{make_nfa(utf8::ranges(list))};

    EXPECT_TRUE(matches(nfa, encode(0x41)));
    EXPECT_TRUE(matches(nfa, encode(0x60)));
    EXPECT_TRUE(matches(nfa, encode(0x150)));
    EXPECT_FALSE(matches(nfa, encode(0x61)));
    EXPECT_FALSE(matches(nfa, encode(0xFF)));
    EXPECT_FALSE(matches(nfa, encode(0x201)));
}

TEST(Utf8_test, Ranges_rejects_empty_unsorted_and_overlapping_input)
{
    EXPECT_THROW((void)utf8::ranges({}), std::invalid_argument);

    constexpr std::array<utf8::Code_point_range, 2> unsorted{
            {{.first = 0x100, .last = 0x200}, {.first = 0x41, .last = 0x5A}}};

    EXPECT_THROW((void)utf8::ranges(unsorted), std::invalid_argument);

    constexpr std::array<utf8::Code_point_range, 2> overlapping{
            {{.first = 0x41, .last = 0x5A}, {.first = 0x50, .last = 0x60}}};

    EXPECT_THROW((void)utf8::ranges(overlapping), std::invalid_argument);

    // A surrogate-only range is invalid even when adjacent to a valid one: merging first would silently absorb
    // it into a neighbor whose expansion excises the surrogate gap.
    constexpr std::array<utf8::Code_point_range, 2> surrogates{
            {{.first = 0xD7FF, .last = 0xD7FF}, {.first = 0xD800, .last = 0xDFFF}}};

    EXPECT_THROW((void)utf8::ranges(surrogates), std::invalid_argument);
}

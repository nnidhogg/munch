#include "munch/regex/utf8.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/nfa/simulator.hpp"

using namespace munch::nfa;
using namespace munch::regex;

namespace
{
/**
 * @brief The largest Unicode scalar value.
 */
constexpr char32_t max_code_point{0x10FFFF};

/**
 * @brief The number of code points, surrogates included.
 */
constexpr char32_t code_point_count{max_code_point + 1};

/**
 * @brief The first surrogate code point, which UTF-8 cannot carry.
 */
constexpr char32_t first_surrogate{0xD800};

/**
 * @brief The last surrogate code point.
 */
constexpr char32_t last_surrogate{0xDFFF};

/**
 * @brief The multiplier of the tests' 64-bit linear congruential generator.
 */
constexpr std::uint64_t lcg_multiplier{6364136223846793005ULL};

/**
 * @brief The increment of the tests' 64-bit linear congruential generator.
 */
constexpr std::uint64_t lcg_increment{1442695040888963407ULL};

/**
 * @brief The random ranges the range test draws.
 */
constexpr std::size_t rounds{100};

/**
 * @brief The random code points the range test probes per range, beside its bounds and their neighbours.
 */
constexpr std::size_t probes{25};

/**
 * @brief Lowers a regex to an automaton accepting one token.
 * @param regex The regex.
 * @return The automaton.
 */
Nfa make_nfa(const Regex& regex)
{
    return to_nfa(regex).set_accept_token(Token{1, 1}).build();
}

/**
 * @brief Returns whether an automaton matches the whole of an input.
 * @param nfa The automaton.
 * @param input The input.
 * @return True when the longest match is the whole input.
 */
bool matches(const Nfa& nfa, const std::string& input)
{
    const auto [token, length]{Simulator::run(nfa, input)};

    return token.has_value() && length == input.size();
}

} // namespace

TEST(Utf8_test, An_ascii_range_matches_its_bytes_alone)
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

TEST(Utf8_test, Encode_writes_the_bytes_of_each_length_boundary)
{
    using namespace std::string_literals;

    EXPECT_EQ(utf8::encode(0x0), "\0"s);
    EXPECT_EQ(utf8::encode(0x7F), "\x7F"s);
    EXPECT_EQ(utf8::encode(0x80), "\xC2\x80"s);
    EXPECT_EQ(utf8::encode(0x7FF), "\xDF\xBF"s);
    EXPECT_EQ(utf8::encode(0x800), "\xE0\xA0\x80"s);
    EXPECT_EQ(utf8::encode(0xD7FF), "\xED\x9F\xBF"s);
    EXPECT_EQ(utf8::encode(0xE000), "\xEE\x80\x80"s);
    EXPECT_EQ(utf8::encode(0xFFFF), "\xEF\xBF\xBF"s);
    EXPECT_EQ(utf8::encode(0x10000), "\xF0\x90\x80\x80"s);
    EXPECT_EQ(utf8::encode(0x10FFFF), "\xF4\x8F\xBF\xBF"s);
}

TEST(Utf8_test, Every_encoding_length_boundary_matches)
{
    const auto nfa{make_nfa(utf8::range(0x0, max_code_point))};

    for (const char32_t code_point :
         {0x0U, 0x7FU, 0x80U, 0x7FFU, 0x800U, 0xD7FFU, 0xE000U, 0xFFFFU, 0x10000U, 0x10FFFFU})
    {
        EXPECT_TRUE(matches(nfa, utf8::encode(code_point))) << "U+" << std::hex << static_cast<unsigned>(code_point);
    }
}

TEST(Utf8_test, Ill_formed_input_matches_nothing)
{
    const auto nfa{make_nfa(utf8::range(0x0, max_code_point))};

    // A stray continuation byte, an overlong encoding of NUL, a truncated sequence, an encoded surrogate, and the first
    // code point past U+10FFFF.
    EXPECT_FALSE(matches(nfa, "\x80"));
    EXPECT_FALSE(matches(nfa, "\xC0\x80"));
    EXPECT_FALSE(matches(nfa, "\xE0\xA0"));
    EXPECT_FALSE(matches(nfa, "\xED\xA0\x80"));
    EXPECT_FALSE(matches(nfa, "\xF4\x90\x80\x80"));
}

TEST(Utf8_test, A_range_spanning_the_surrogates_excises_them)
{
    const auto nfa{make_nfa(utf8::range(0xD000, 0xE100))};

    EXPECT_TRUE(matches(nfa, utf8::encode(0xD000)));
    EXPECT_TRUE(matches(nfa, utf8::encode(0xD7FF)));
    EXPECT_TRUE(matches(nfa, utf8::encode(0xE000)));
    EXPECT_TRUE(matches(nfa, utf8::encode(0xE100)));

    // The encodings of U+D800 and U+DFFF.
    EXPECT_FALSE(matches(nfa, "\xED\xA0\x80"));
    EXPECT_FALSE(matches(nfa, "\xED\xBF\xBF"));
}

TEST(Utf8_test, Membership_around_the_bounds_is_exactly_the_range)
{
    // Every code point around the range bounds and the two-to-three byte encoding boundary is accepted exactly when it
    // lies inside the range.
    const auto nfa{make_nfa(utf8::range(0x600, 0x900))};

    for (char32_t code_point{0x500}; code_point <= 0xA00; ++code_point)
    {
        const auto expected{code_point >= 0x600 && code_point <= 0x900};

        EXPECT_EQ(matches(nfa, utf8::encode(code_point)), expected)
                << "U+" << std::hex << static_cast<unsigned>(code_point);
    }
}

TEST(Utf8_test, An_invalid_range_throws)
{
    EXPECT_THROW(std::ignore = utf8::range(0x20, 0x10), std::invalid_argument);
    EXPECT_THROW(std::ignore = utf8::range(0x0, code_point_count), std::invalid_argument);
    EXPECT_THROW(std::ignore = utf8::range(first_surrogate, last_surrogate), std::invalid_argument);
}

TEST(Utf8_test, Random_subranges_match_exactly_their_contents)
{
    // A deterministic 64-bit LCG draws arbitrary interval bounds, attacking the recursive lower/middle/upper
    // decomposition at boundaries the block-aligned tests never produce; each range is probed at its edges, just
    // outside them, and at random candidates.
    std::uint64_t state{12345};

    const auto draw{[&state] {
        state = state * lcg_multiplier + lcg_increment;

        return static_cast<char32_t>((state >> 32U) % code_point_count);
    }};

    const auto surrogate{
            [](const char32_t code_point) { return code_point >= first_surrogate && code_point <= last_surrogate; }};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        auto first{draw()};

        auto last{draw()};

        if (first > last)
        {
            std::swap(first, last);
        }

        if (surrogate(first) && surrogate(last))
        {
            continue;
        }

        const auto nfa{make_nfa(utf8::range(first, last))};

        std::vector<char32_t> candidates{first, last};

        if (first > 0)
        {
            candidates.push_back(first - 1);
        }

        if (last < max_code_point)
        {
            candidates.push_back(last + 1);
        }

        for (std::size_t probe{0}; probe < probes; ++probe)
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

            EXPECT_EQ(matches(nfa, utf8::encode(candidate)), expected)
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

    EXPECT_TRUE(matches(nfa, utf8::encode(0x41)));
    EXPECT_TRUE(matches(nfa, utf8::encode(0x60)));
    EXPECT_TRUE(matches(nfa, utf8::encode(0x150)));
    EXPECT_FALSE(matches(nfa, utf8::encode(0x61)));
    EXPECT_FALSE(matches(nfa, utf8::encode(0xFF)));
    EXPECT_FALSE(matches(nfa, utf8::encode(0x201)));
}

TEST(Utf8_test, Ranges_rejects_empty_unsorted_and_overlapping_input)
{
    EXPECT_THROW(std::ignore = utf8::ranges({}), std::invalid_argument);

    constexpr std::array<utf8::Code_point_range, 2> unsorted{
            {{.first = 0x100, .last = 0x200}, {.first = 0x41, .last = 0x5A}}};

    EXPECT_THROW(std::ignore = utf8::ranges(unsorted), std::invalid_argument);

    constexpr std::array<utf8::Code_point_range, 2> overlapping{
            {{.first = 0x41, .last = 0x5A}, {.first = 0x50, .last = 0x60}}};

    EXPECT_THROW(std::ignore = utf8::ranges(overlapping), std::invalid_argument);

    // A surrogate-only range is invalid even when adjacent to a valid one: each range is validated before adjacent
    // ranges merge, so it is refused rather than absorbed into a neighbor whose expansion excises the surrogate gap.
    constexpr std::array<utf8::Code_point_range, 2> surrogates{
            {{.first = 0xD7FF, .last = 0xD7FF}, {.first = first_surrogate, .last = last_surrogate}}};

    EXPECT_THROW(std::ignore = utf8::ranges(surrogates), std::invalid_argument);
}

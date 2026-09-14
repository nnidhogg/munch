#include "munch/regex/unicode.hpp"

#include <gtest/gtest.h>

#include <string>

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

// The XID automata are large, so each property is built once and shared across the membership probes.
const Nfa& xid_start_nfa()
{
    static const Nfa nfa{to_nfa(unicode::xid_start()).set_accept_token(Token{1, 1}).build()};

    return nfa;
}

const Nfa& xid_continue_nfa()
{
    static const Nfa nfa{to_nfa(unicode::xid_continue()).set_accept_token(Token{1, 1}).build()};

    return nfa;
}

const Nfa& decimal_digit_nfa()
{
    static const Nfa nfa{to_nfa(unicode::decimal_digit()).set_accept_token(Token{1, 1}).build()};

    return nfa;
}

const Nfa& white_space_nfa()
{
    static const Nfa nfa{to_nfa(unicode::white_space()).set_accept_token(Token{1, 1}).build()};

    return nfa;
}

const Nfa& word_nfa()
{
    static const Nfa nfa{to_nfa(unicode::word()).set_accept_token(Token{1, 1}).build()};

    return nfa;
}

bool matches(const Nfa& nfa, const char32_t code_point)
{
    const auto input{encode(code_point)};

    const auto [token, length]{Simulator::run(nfa, input)};

    return token.has_value() && length == input.size();
}

} // namespace

TEST(Unicode_test, Xid_start_holds_letters_of_many_scripts)
{
    EXPECT_TRUE(matches(xid_start_nfa(), U'A'));
    EXPECT_TRUE(matches(xid_start_nfa(), U'z'));
    EXPECT_TRUE(matches(xid_start_nfa(), U'À'));          // Latin capital A with grave
    EXPECT_TRUE(matches(xid_start_nfa(), U'λ'));          // Greek small lambda
    EXPECT_TRUE(matches(xid_start_nfa(), U'漢'));         // Han 'kan'
    EXPECT_TRUE(matches(xid_start_nfa(), U'\U0001D400')); // mathematical bold capital A
}

TEST(Unicode_test, Xid_start_excludes_digits_underscore_and_symbols)
{
    EXPECT_FALSE(matches(xid_start_nfa(), U'0'));
    EXPECT_FALSE(matches(xid_start_nfa(), U'_'));
    EXPECT_FALSE(matches(xid_start_nfa(), U' '));
    EXPECT_FALSE(matches(xid_start_nfa(), U'-'));
    EXPECT_FALSE(matches(xid_start_nfa(), U'́'));           // combining acute accent
    EXPECT_FALSE(matches(xid_start_nfa(), U'€'));          // euro sign
    EXPECT_FALSE(matches(xid_start_nfa(), U'\U0010FFFF')); // last code point, unassigned
}

TEST(Unicode_test, Xid_continue_adds_digits_underscore_and_marks)
{
    EXPECT_TRUE(matches(xid_continue_nfa(), U'a'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'Z'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'9'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'_'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'́'));  // combining acute accent
    EXPECT_TRUE(matches(xid_continue_nfa(), U'٠')); // Arabic-Indic digit zero
}

TEST(Unicode_test, Xid_continue_excludes_separators_and_symbols)
{
    EXPECT_FALSE(matches(xid_continue_nfa(), U' '));
    EXPECT_FALSE(matches(xid_continue_nfa(), U'!'));
    EXPECT_FALSE(matches(xid_continue_nfa(), U'\n'));
    EXPECT_FALSE(matches(xid_continue_nfa(), U'€')); // euro sign
}

TEST(Unicode_test, Xid_start_members_continue_identifiers_too)
{
    EXPECT_TRUE(matches(xid_continue_nfa(), U'A'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'λ'));
    EXPECT_TRUE(matches(xid_continue_nfa(), U'漢'));
}

TEST(Unicode_test, Decimal_digits_are_the_Nd_category)
{
    EXPECT_TRUE(matches(decimal_digit_nfa(), U'0'));
    EXPECT_TRUE(matches(decimal_digit_nfa(), U'9'));
    EXPECT_TRUE(matches(decimal_digit_nfa(), U'٣'));          // Arabic-Indic digit three
    EXPECT_TRUE(matches(decimal_digit_nfa(), U'\U0001D7CE')); // mathematical bold digit zero
    EXPECT_FALSE(matches(decimal_digit_nfa(), U'A'));
    EXPECT_FALSE(matches(decimal_digit_nfa(), U'²')); // superscript two, category No
    EXPECT_FALSE(matches(decimal_digit_nfa(), U'Ⅳ')); // Roman numeral four, category Nl
}

TEST(Unicode_test, White_space_is_the_property_and_no_more)
{
    EXPECT_TRUE(matches(white_space_nfa(), U' '));
    EXPECT_TRUE(matches(white_space_nfa(), U'\t'));
    EXPECT_TRUE(matches(white_space_nfa(), U'\r'));
    EXPECT_TRUE(matches(white_space_nfa(), U'\u0085'));  // next line
    EXPECT_TRUE(matches(white_space_nfa(), U'\u00A0'));  // no-break space
    EXPECT_TRUE(matches(white_space_nfa(), U'\u2028'));  // line separator
    EXPECT_TRUE(matches(white_space_nfa(), U'\u3000'));  // ideographic space
    EXPECT_FALSE(matches(white_space_nfa(), U'\u200B')); // zero width space, not White_Space
    EXPECT_FALSE(matches(white_space_nfa(), U'\uFEFF')); // byte order mark
    EXPECT_FALSE(matches(white_space_nfa(), U'a'));
}

TEST(Unicode_test, Word_characters_are_the_crates_union)
{
    EXPECT_TRUE(matches(word_nfa(), U'a'));
    EXPECT_TRUE(matches(word_nfa(), U'Z'));
    EXPECT_TRUE(matches(word_nfa(), U'0'));
    EXPECT_TRUE(matches(word_nfa(), U'_'));      // connector punctuation
    EXPECT_TRUE(matches(word_nfa(), U'é'));      // alphabetic
    EXPECT_TRUE(matches(word_nfa(), U'́'));       // combining acute accent, a mark
    EXPECT_TRUE(matches(word_nfa(), U'٣'));      // Arabic-Indic digit three
    EXPECT_TRUE(matches(word_nfa(), U'\u200D')); // zero width joiner, Join_Control
    EXPECT_TRUE(matches(word_nfa(), U'漢'));
    EXPECT_FALSE(matches(word_nfa(), U' '));
    EXPECT_FALSE(matches(word_nfa(), U'-'));
    EXPECT_FALSE(matches(word_nfa(), U'€'));
    EXPECT_FALSE(matches(word_nfa(), U'\u00A0'));
}

TEST(Unicode_test, Ranges_are_the_tables_the_builders_expand)
{
    EXPECT_EQ(unicode::ranges(unicode::Property::white_space).size(), 10U);
    EXPECT_EQ(unicode::ranges(unicode::Property::white_space).front().first, U'\t');
    EXPECT_EQ(unicode::ranges(unicode::Property::white_space).back().last, U'\u3000');
    EXPECT_EQ(unicode::ranges(unicode::Property::decimal_digit).front().last, U'9');
    EXPECT_FALSE(unicode::ranges(unicode::Property::word).empty());
    EXPECT_FALSE(unicode::ranges(unicode::Property::xid_start).empty());
    EXPECT_FALSE(unicode::ranges(unicode::Property::xid_continue).empty());
}

TEST(Unicode_test, Version_names_the_pinned_database)
{
    EXPECT_EQ(unicode::version(), "17.0.0");
}

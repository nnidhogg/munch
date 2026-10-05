#include "munch/regex/unicode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "munch/nfa/simulator.hpp"
#include "munch/regex/utf8.hpp"

using namespace munch::nfa;
using namespace munch::regex;

namespace
{
/**
 * @brief The number of Unicode properties the library builds classes for.
 */
constexpr std::size_t property_count{5};

/**
 * @brief Returns the regex of a property's class, as the property's own builder spells it.
 * @param property The property.
 * @return The regex.
 */
Regex property_regex(const unicode::Property property)
{
    switch (property)
    {
    case unicode::Property::xid_start:
        return unicode::xid_start();
    case unicode::Property::xid_continue:
        return unicode::xid_continue();
    case unicode::Property::decimal_digit:
        return unicode::decimal_digit();
    case unicode::Property::white_space:
        return unicode::white_space();
    case unicode::Property::word:
        return unicode::word();
    }

    std::unreachable();
}

/**
 * @brief Returns the automaton of a property's class, built on first use and shared across the membership probes, since
 *        the XID automata are large.
 * @param property The property.
 * @return The automaton.
 */
const Nfa& class_nfa(const unicode::Property property)
{
    static std::array<std::optional<Nfa>, property_count> automata{};

    auto& automaton{automata[std::to_underlying(property)]};

    if (!automaton)
    {
        const auto regex{property_regex(property)};

        automaton.emplace(to_nfa(regex).set_accept_token(Token{1, 1}).build());
    }

    return *automaton;
}

/**
 * @brief Returns whether an automaton matches exactly the encoding of a code point.
 * @param nfa The automaton.
 * @param code_point The code point.
 * @return True when the longest match is the whole encoding.
 */
bool matches(const Nfa& nfa, const char32_t code_point)
{
    const auto input{utf8::encode(code_point)};

    const auto [token, length]{Simulator::run(nfa, input)};

    return token.has_value() && length == input.size();
}

} // namespace

TEST(Unicode_test, Xid_start_holds_letters_of_many_scripts)
{
    const auto& xid_start{class_nfa(unicode::Property::xid_start)};

    EXPECT_TRUE(matches(xid_start, U'A'));
    EXPECT_TRUE(matches(xid_start, U'z'));
    EXPECT_TRUE(matches(xid_start, U'À'));          // Latin capital A with grave
    EXPECT_TRUE(matches(xid_start, U'λ'));          // Greek small lambda
    EXPECT_TRUE(matches(xid_start, U'漢'));         // Han 'kan'
    EXPECT_TRUE(matches(xid_start, U'\U0001D400')); // mathematical bold capital A
}

TEST(Unicode_test, Xid_start_excludes_digits_underscore_and_symbols)
{
    const auto& xid_start{class_nfa(unicode::Property::xid_start)};

    EXPECT_FALSE(matches(xid_start, U'0'));
    EXPECT_FALSE(matches(xid_start, U'_'));
    EXPECT_FALSE(matches(xid_start, U' '));
    EXPECT_FALSE(matches(xid_start, U'-'));
    EXPECT_FALSE(matches(xid_start, U'́'));           // combining acute accent
    EXPECT_FALSE(matches(xid_start, U'€'));          // euro sign
    EXPECT_FALSE(matches(xid_start, U'\U0010FFFF')); // last code point, unassigned
}

TEST(Unicode_test, Xid_continue_adds_digits_underscore_and_marks)
{
    const auto& xid_continue{class_nfa(unicode::Property::xid_continue)};

    EXPECT_TRUE(matches(xid_continue, U'a'));
    EXPECT_TRUE(matches(xid_continue, U'Z'));
    EXPECT_TRUE(matches(xid_continue, U'9'));
    EXPECT_TRUE(matches(xid_continue, U'_'));
    EXPECT_TRUE(matches(xid_continue, U'́'));  // combining acute accent
    EXPECT_TRUE(matches(xid_continue, U'٠')); // Arabic-Indic digit zero
}

TEST(Unicode_test, Xid_continue_excludes_separators_and_symbols)
{
    const auto& xid_continue{class_nfa(unicode::Property::xid_continue)};

    EXPECT_FALSE(matches(xid_continue, U' '));
    EXPECT_FALSE(matches(xid_continue, U'!'));
    EXPECT_FALSE(matches(xid_continue, U'\n'));
    EXPECT_FALSE(matches(xid_continue, U'€')); // euro sign
}

TEST(Unicode_test, Xid_start_members_continue_identifiers_too)
{
    const auto& xid_continue{class_nfa(unicode::Property::xid_continue)};

    EXPECT_TRUE(matches(xid_continue, U'A'));
    EXPECT_TRUE(matches(xid_continue, U'λ'));
    EXPECT_TRUE(matches(xid_continue, U'漢'));
}

TEST(Unicode_test, Decimal_digits_are_the_Nd_category)
{
    const auto& decimal_digit{class_nfa(unicode::Property::decimal_digit)};

    EXPECT_TRUE(matches(decimal_digit, U'0'));
    EXPECT_TRUE(matches(decimal_digit, U'9'));
    EXPECT_TRUE(matches(decimal_digit, U'٣'));          // Arabic-Indic digit three
    EXPECT_TRUE(matches(decimal_digit, U'\U0001D7CE')); // mathematical bold digit zero
    EXPECT_FALSE(matches(decimal_digit, U'A'));
    EXPECT_FALSE(matches(decimal_digit, U'²')); // superscript two, category No
    EXPECT_FALSE(matches(decimal_digit, U'Ⅳ')); // Roman numeral four, category Nl
}

TEST(Unicode_test, White_space_is_the_property_and_no_more)
{
    const auto& white_space{class_nfa(unicode::Property::white_space)};

    EXPECT_TRUE(matches(white_space, U' '));
    EXPECT_TRUE(matches(white_space, U'\t'));
    EXPECT_TRUE(matches(white_space, U'\r'));
    EXPECT_TRUE(matches(white_space, U'\u0085'));  // next line
    EXPECT_TRUE(matches(white_space, U'\u00A0'));  // no-break space
    EXPECT_TRUE(matches(white_space, U'\u2028'));  // line separator
    EXPECT_TRUE(matches(white_space, U'\u3000'));  // ideographic space
    EXPECT_FALSE(matches(white_space, U'\u200B')); // zero width space, not White_Space
    EXPECT_FALSE(matches(white_space, U'\uFEFF')); // byte order mark
    EXPECT_FALSE(matches(white_space, U'a'));
}

TEST(Unicode_test, Word_characters_are_the_crates_union)
{
    const auto& word{class_nfa(unicode::Property::word)};

    EXPECT_TRUE(matches(word, U'a'));
    EXPECT_TRUE(matches(word, U'Z'));
    EXPECT_TRUE(matches(word, U'0'));
    EXPECT_TRUE(matches(word, U'_'));      // connector punctuation
    EXPECT_TRUE(matches(word, U'é'));      // alphabetic
    EXPECT_TRUE(matches(word, U'́'));       // combining acute accent, a mark
    EXPECT_TRUE(matches(word, U'٣'));      // Arabic-Indic digit three
    EXPECT_TRUE(matches(word, U'\u200D')); // zero width joiner, Join_Control
    EXPECT_TRUE(matches(word, U'漢'));
    EXPECT_FALSE(matches(word, U' '));
    EXPECT_FALSE(matches(word, U'-'));
    EXPECT_FALSE(matches(word, U'€'));
    EXPECT_FALSE(matches(word, U'\u00A0'));
}

TEST(Unicode_test, Ranges_are_the_tables_the_builders_expand)
{
    const auto white_space{unicode::ranges(unicode::Property::white_space)};

    EXPECT_EQ(white_space.size(), 10U);

    const auto [first, first_last]{white_space.front()};

    const auto [last_first, last]{white_space.back()};

    EXPECT_EQ(first, U'\t');
    EXPECT_EQ(last, U'\u3000');

    const auto [digit_first, digit_last]{unicode::ranges(unicode::Property::decimal_digit).front()};

    EXPECT_EQ(digit_last, U'9');
    EXPECT_FALSE(unicode::ranges(unicode::Property::word).empty());
    EXPECT_FALSE(unicode::ranges(unicode::Property::xid_start).empty());
    EXPECT_FALSE(unicode::ranges(unicode::Property::xid_continue).empty());
}

TEST(Unicode_test, Version_names_the_pinned_database)
{
    EXPECT_EQ(unicode::version(), "17.0.0");
}

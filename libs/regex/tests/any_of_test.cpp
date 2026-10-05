#include "munch/regex/any_of.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "munch/nfa/simulator.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

using namespace munch::nfa;
using namespace munch::regex;

/**
 * @brief A match the NFA simulator reports.
 */
using Match = Simulator::Match;

namespace
{
/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

/**
 * @brief Returns the number of states a builder's initial state reaches on a symbol.
 * @param builder The builder.
 * @param symbol The symbol.
 * @return The number of targets.
 */
std::size_t targets_on(const Builder& builder, const char symbol)
{
    const Nfa::Key_t key{builder.init_state(), Label{symbol}};

    return builder.transitions().at(key).size();
}

/**
 * @brief Expects the automaton of any_of() over a set to be one transition per member from state 0 to the one accept
 *        state 1.
 * @param set The set.
 */
void expect_one_edge_per_member(const Set& set)
{
    const auto regex{any_of(set)};

    const auto builder{to_nfa(regex)};

    EXPECT_EQ(builder.init_state(), 0U);

    EXPECT_EQ(builder.accept_states().size(), 1U);
    EXPECT_TRUE(builder.accept_states().contains(1));

    EXPECT_EQ(builder.transitions().size(), set.symbols().size());

    for (const auto symbol : set.symbols())
    {
        EXPECT_TRUE(builder.transitions().contains({builder.init_state(), Label{symbol}}));
        EXPECT_EQ(targets_on(builder, symbol), 1U);
    }
}

/**
 * @brief Builds the automaton of any_of() over a set and expects it to match every member alone and no other byte.
 * @param set The set.
 * @param token The token the automaton accepts with.
 * @return The automaton, for the test's further inputs.
 */
Nfa expect_matches_exactly_its_members(const Set& set, const Token& token)
{
    const auto regex{any_of(set)};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    for (const auto symbol : set.symbols())
    {
        EXPECT_EQ(Simulator::run(nfa, std::string{symbol}), (Match{.token = token, .length = 1}));
    }

    const auto others{Set::all() - set};

    for (const auto symbol : others.symbols())
    {
        EXPECT_EQ(Simulator::run(nfa, std::string{symbol}), no_match);
    }

    return nfa;
}

} // namespace

TEST(Any_of_test, A_single_member_matches_one_byte_and_nothing_else)
{
    const Token token{1, 1};

    const auto nfa{expect_matches_exactly_its_members(Set{'a'}, token)};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "cba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "bac"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "123"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), no_match);
}

TEST(Any_of_test, Three_members_lower_to_one_edge_each)
{
    expect_one_edge_per_member(Set{'a', 'b', 'c'});
}

TEST(Any_of_test, Three_members_each_match_one_byte)
{
    const Token token{2, 1};

    const auto nfa{expect_matches_exactly_its_members(Set{'a', 'b', 'c'}, token)};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "cba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bac"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "123"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), no_match);
}

TEST(Any_of_test, The_letters_lower_to_one_edge_each)
{
    expect_one_edge_per_member(Set::alpha());
}

TEST(Any_of_test, The_letters_each_match_one_byte)
{
    const Token token{3, 1};

    const auto nfa{expect_matches_exactly_its_members(Set::alpha(), token)};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "cba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bac"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "123"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), no_match);
}

TEST(Any_of_test, The_digits_lower_to_one_edge_each)
{
    expect_one_edge_per_member(Set::digits());
}

TEST(Any_of_test, The_digits_each_match_one_byte)
{
    const Token token{4, 1};

    const auto nfa{expect_matches_exactly_its_members(Set::digits(), token)};

    EXPECT_EQ(Simulator::run(nfa, "123"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ab"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "abc"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "cba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "bac"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), no_match);
}

TEST(Any_of_test, The_letters_and_digits_lower_to_one_edge_each)
{
    expect_one_edge_per_member(Set::alphanum());
}

TEST(Any_of_test, The_letters_and_digits_each_match_one_byte)
{
    const Token token{5, 1};

    const auto nfa{expect_matches_exactly_its_members(Set::alphanum(), token)};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "cba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bac"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "123"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), no_match);
}

TEST(Any_of_test, The_printable_bytes_lower_to_one_edge_each)
{
    expect_one_edge_per_member(Set::printable());
}

TEST(Any_of_test, The_printable_bytes_each_match_one_byte)
{
    const Token token{6, 1};

    const auto nfa{expect_matches_exactly_its_members(Set::printable(), token)};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "cba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bac"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "123"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
}

TEST(Any_of_test, Every_byte_lowers_to_one_edge_of_its_own)
{
    expect_one_edge_per_member(Set::all());
}

TEST(Any_of_test, Every_byte_matches_alone)
{
    const Token token{7, 1};

    const auto nfa{expect_matches_exactly_its_members(Set::all(), token)};

    // A string literal's terminating NUL is input like any other byte, and the NUL byte is a member of every byte.
    const std::string nul_byte{'\0'};

    EXPECT_EQ(Simulator::run(nfa, nul_byte), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "cba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bac"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "123"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "*[=]"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "a.b.c"), (Match{.token = token, .length = 1}));
}

TEST(Any_of_test, The_empty_set_matches_nothing)
{
    const Set empty{};

    const auto regex{any_of(empty)};

    const Token token{7, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ab"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "abc"), no_match);
}

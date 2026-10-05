#include "munch/regex/choice.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <tuple>

#include "munch/nfa/simulator.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::regex;

/**
 * @brief A match the NFA simulator reports.
 */
using Match = Simulator::Match;

/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

TEST(Choice_test, A_choice_of_two_bytes_matches_either)
{
    const auto a{text('a')};
    const auto b{text('b')};

    const auto regex{choice(a, b)};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "c"), no_match);
}

TEST(Choice_test, A_choice_of_three_texts_matches_each_whole)
{
    const auto a{text('a')};
    const auto bc{text("bc")};
    const auto def{text("def")};

    const auto regex{choice(a, bc, def)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "bc"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "def"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "de"), no_match);
}

TEST(Choice_test, A_choice_of_nothing_throws)
{
    EXPECT_THROW(std::ignore = to_nfa(Choice{.regexes = {}}), std::invalid_argument);
}

#include "munch/regex/concat.hpp"

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

TEST(Concat_test, Two_concatenated_bytes_match_only_together)
{
    const auto regex{concat(text('a'), text('b'))};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 2}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
}

TEST(Concat_test, Four_concatenated_bytes_match_only_together)
{
    const auto regex{concat(text('a'), text('b'), text('c'), text('d'))};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "abcd"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "abcde"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "abcd!"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ab"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "abc"), no_match);
}

TEST(Concat_test, A_concatenation_of_nothing_throws)
{
    EXPECT_THROW(std::ignore = to_nfa(Concat{.regexes = {}}), std::invalid_argument);
}

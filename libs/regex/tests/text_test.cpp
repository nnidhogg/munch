#include "munch/regex/text.hpp"

#include <gtest/gtest.h>

#include <optional>

#include "munch/nfa/nfa.hpp"
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

TEST(Text_test, A_text_matches_only_its_whole_bytes)
{
    const auto regex{text("hello")};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "hello"), (Match{.token = token, .length = 5}));
    EXPECT_EQ(Simulator::run(nfa, "hello!"), (Match{.token = token, .length = 5}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "hell"), no_match);
}

TEST(Text_test, Regex_operators_in_a_text_are_literal_bytes)
{
    const auto regex{text("a*b+c?")};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "a*b+c?"), (Match{.token = token, .length = 6}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "abc"), no_match);
}

TEST(Text_test, Every_regex_metacharacter_in_a_text_is_a_literal_byte)
{
    const auto regex{text(R"(.*+?^${}()|[]\)")};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, R"(.*+?^${}()|[]\)"), (Match{.token = token, .length = 14}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, ".*+?^${}()|[]"), no_match);
}

TEST(Text_test, An_empty_text_matches_the_empty_prefix)
{
    const auto regex{text("")};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, " "), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 0}));
}

#include "munch/regex/patterns.hpp"

#include <gtest/gtest.h>

#include <optional>

#include "munch/nfa/nfa.hpp"
#include "munch/nfa/simulator.hpp"

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

TEST(Patterns_test, An_identifier_is_a_letter_or_underscore_then_letters_digits_and_underscores)
{
    const auto regex{patterns::identifier()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "x"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "_private"), (Match{.token = token, .length = 8}));
    EXPECT_EQ(Simulator::run(nfa, "counter2"), (Match{.token = token, .length = 8}));
    EXPECT_EQ(Simulator::run(nfa, "snake_case_name"), (Match{.token = token, .length = 15}));
    EXPECT_EQ(Simulator::run(nfa, "x + y"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, "2x"), no_match);
    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
}

TEST(Patterns_test, A_decimal_integer_is_a_run_of_digits)
{
    const auto regex{patterns::decimal_integer()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "0"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "42"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "1234567890"), (Match{.token = token, .length = 10}));
    EXPECT_EQ(Simulator::run(nfa, "42 apples"), (Match{.token = token, .length = 2}));

    // No sign: that's a parser-level unary operator, not part of the lexeme.
    EXPECT_EQ(Simulator::run(nfa, "-42"), no_match);
    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "x"), no_match);
}

TEST(Patterns_test, A_decimal_float_is_digits_a_dot_and_digits)
{
    const auto regex{patterns::decimal_float()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "3.5"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "0.0"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "123.456"), (Match{.token = token, .length = 7}));
    EXPECT_EQ(Simulator::run(nfa, "3.5 + 1"), (Match{.token = token, .length = 3}));

    // No leading/trailing-dot-only forms, no sign, no exponent.
    EXPECT_EQ(Simulator::run(nfa, ".5"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "5."), no_match);
    EXPECT_EQ(Simulator::run(nfa, "5"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "-3.5"), no_match);
}

#include "munch/regex/patterns.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "munch/nfa/nfa.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;
using namespace munch::regex;

class Patterns_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Patterns_test, Identifier)
{
    const auto regex{patterns::identifier()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "x"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "_private"), Match(token, 8));
    EXPECT_EQ(Simulator::run(nfa, "counter2"), Match(token, 8));
    EXPECT_EQ(Simulator::run(nfa, "snake_case_name"), Match(token, 15));
    EXPECT_EQ(Simulator::run(nfa, "x + y"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, "2x"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
}

TEST_F(Patterns_test, Decimal_integer)
{
    const auto regex{patterns::decimal_integer()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "0"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "42"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "1234567890"), Match(token, 10));
    EXPECT_EQ(Simulator::run(nfa, "42 apples"), Match(token, 2));

    // No sign: that's a parser-level unary operator, not part of the lexeme.
    EXPECT_EQ(Simulator::run(nfa, "-42"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "x"), Match(std::nullopt, 0));
}

TEST_F(Patterns_test, Decimal_float)
{
    const auto regex{patterns::decimal_float()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "3.5"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "0.0"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "123.456"), Match(token, 7));
    EXPECT_EQ(Simulator::run(nfa, "3.5 + 1"), Match(token, 3));

    // No leading/trailing-dot-only forms, no sign, no exponent.
    EXPECT_EQ(Simulator::run(nfa, ".5"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "5."), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "5"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "-3.5"), Match(std::nullopt, 0));
}

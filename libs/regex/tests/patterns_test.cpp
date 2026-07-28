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

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "x"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "_private"), Result_t(token, 8));
    EXPECT_EQ(Simulator::run(nfa, "counter2"), Result_t(token, 8));
    EXPECT_EQ(Simulator::run(nfa, "snake_case_name"), Result_t(token, 15));
    EXPECT_EQ(Simulator::run(nfa, "x + y"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(nfa, "2x"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
}

TEST_F(Patterns_test, Decimal_integer)
{
    const auto regex{patterns::decimal_integer()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "0"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "42"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "1234567890"), Result_t(token, 10));
    EXPECT_EQ(Simulator::run(nfa, "42 apples"), Result_t(token, 2));

    // No sign: that's a parser-level unary operator, not part of the lexeme.
    EXPECT_EQ(Simulator::run(nfa, "-42"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "x"), Result_t(std::nullopt, 0));
}

TEST_F(Patterns_test, Decimal_float)
{
    const auto regex{patterns::decimal_float()};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "3.5"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "0.0"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "123.456"), Result_t(token, 7));
    EXPECT_EQ(Simulator::run(nfa, "3.5 + 1"), Result_t(token, 3));

    // No leading/trailing-dot-only forms, no sign, no exponent.
    EXPECT_EQ(Simulator::run(nfa, ".5"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "5."), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "5"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "-3.5"), Result_t(std::nullopt, 0));
}

#include "lexer/regex/regex.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "lexer/nfa/simulator.hpp"
#include "lexer/nfa/tools/graphviz.hpp"

using namespace lexer::nfa;
using namespace lexer::nfa::tools;
using namespace lexer::regex;

class Concat_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Concat_test, Two_characters)
{
    using namespace testing;

    const auto regex{concat(text('a'), text('b'))};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "ab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "abc"), Result_t(token, 2));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(std::nullopt, 0));
}

TEST_F(Concat_test, Multiple_characters)
{
    using namespace testing;

    const auto regex{concat(text('a'), text('b'), text('c'), text('d'))};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "abcd"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "abcde"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "abcd!"), Result_t(token, 4));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "abc"), Result_t(std::nullopt, 0));
}

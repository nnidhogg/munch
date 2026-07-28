#include "lexer/regex/regex.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "lexer/nfa/simulator.hpp"
#include "lexer/nfa/tools/graphviz.hpp"

using namespace lexer::nfa;
using namespace lexer::nfa::tools;
using namespace lexer::regex;

class Repeat_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Repeat_test, Kleene_star)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{kleene(a)};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Result_t(token, 0));
}

TEST_F(Repeat_test, Plus)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{plus(a)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Result_t(std::nullopt, 0));
}

TEST_F(Repeat_test, Optional)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{optional(a)};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Result_t(token, 0));
}

TEST_F(Repeat_test, Exact_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{exact(a, 3)};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Result_t(token, 3));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Result_t(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 3)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Result_t(token, 5));
    EXPECT_EQ(Simulator::run(nfa, "aaaaaa"), Result_t(token, 6));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Result_t(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_zero_repetitions_is_the_kleene_star)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 0)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));

    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Result_t(token, 0));
}

TEST_F(Repeat_test, Range_ending_before_it_starts_throws)
{
    EXPECT_THROW((void)range(text('a'), 3, 2), std::invalid_argument);
}

TEST_F(Repeat_test, Range_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{range(a, 2, 4)};

    const Token token{6, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Result_t(token, 4));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Result_t(std::nullopt, 0));
}

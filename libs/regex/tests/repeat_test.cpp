#include "munch/regex/repeat.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;
using namespace munch::regex;

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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(token, 0));
}

TEST_F(Repeat_test, Plus)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{plus(a)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, Optional)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{optional(a)};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
}

TEST_F(Repeat_test, Exact_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{exact(a, 3)};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 3)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Match(token, 5));
    EXPECT_EQ(Simulator::run(nfa, "aaaaaa"), Match(token, 6));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_zero_repetitions_is_the_kleene_star)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 0)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));

    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Match(token, 4));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, Copies_are_independent_values)
{
    // The Box gives Repeat value semantics: copying a pattern deep-copies its child, so both the original and the
    // copy lower to working automata, and an empty Repeat is no longer representable at all.
    const auto original{plus(text("ab"))};

    const auto copy{original};

    const Token token{1, 1};

    const auto original_nfa{to_nfa(original).set_accept_token(token).build()};

    const auto copied_nfa{to_nfa(copy).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(original_nfa, "abab"), Match(token, 4));
    EXPECT_EQ(Simulator::run(copied_nfa, "abab"), Match(token, 4));
}

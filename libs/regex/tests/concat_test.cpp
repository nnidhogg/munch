#include "munch/regex/concat.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;
using namespace munch::regex;

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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "abc"), Match(token, 2));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
}

TEST_F(Concat_test, Multiple_characters)
{
    using namespace testing;

    const auto regex{concat(text('a'), text('b'), text('c'), text('d'))};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "abcd"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "abcde"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "abcd!"), Match(token, 4));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "abc"), Match(std::nullopt, 0));
}

TEST_F(Concat_test, Empty_regexes_throws)
{
    EXPECT_THROW(static_cast<void>(to_nfa(Concat{.regexes = {}})), std::invalid_argument);
}

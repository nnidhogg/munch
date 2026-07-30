#include "munch/regex/choice.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;
using namespace munch::regex;

class Choice_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Choice_test, Single_character)
{
    using namespace testing;

    auto a{text('a')};
    auto b{text('b')};

    const auto regex{choice(a, b)};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "c"), Match(std::nullopt, 0));
}

TEST_F(Choice_test, Multiple_characters)
{
    using namespace testing;

    auto a{text('a')};
    auto bc{text("bc")};
    auto def{text("def")};

    const auto regex{choice(a, bc, def)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "bc"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "def"), Match(token, 3));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "de"), Match(std::nullopt, 0));
}

TEST_F(Choice_test, Empty_regexes_throws)
{
    EXPECT_THROW(static_cast<void>(to_nfa(Choice{.regexes = {}})), std::invalid_argument);
}

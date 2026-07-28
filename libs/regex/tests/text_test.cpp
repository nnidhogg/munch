#include "lexer/regex/regex.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include "lexer/nfa/nfa.hpp"
#include "lexer/nfa/simulator.hpp"
#include "lexer/nfa/tools/graphviz.hpp"

using namespace lexer::nfa;
using namespace lexer::nfa::tools;
using namespace lexer::regex;

class Text_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Text_test, Simple_text)
{
    using namespace testing;

    const auto regex{text("hello")};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "hello"), Result_t(token, 5));
    EXPECT_EQ(Simulator::run(nfa, "hello!"), Result_t(token, 5));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "hell"), Result_t(std::nullopt, 0));
}

TEST_F(Text_test, Special_characters)
{
    using namespace testing;

    const auto regex{text("a*b+c?")};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, "a*b+c?"), Result_t(token, 6));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "abc"), Result_t(std::nullopt, 0));
}

TEST_F(Text_test, More_special_characters)
{
    using namespace testing;

    const auto regex{text(".*+?^${}()|[]\\")};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, ".*+?^${}()|[]\\"), Result_t(token, 14));

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, ".*+?^${}()|[]"), Result_t(std::nullopt, 0));
}

TEST_F(Text_test, Empty_text)
{
    using namespace testing;

    const auto regex{text("")};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(nfa, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, " "), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Result_t(token, 0));
}

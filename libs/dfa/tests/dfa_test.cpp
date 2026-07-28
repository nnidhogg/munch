#include "lexer/dfa/dfa.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "lexer/dfa/builder.hpp"
#include "lexer/dfa/simulator.hpp"
#include "lexer/dfa/tools/graphviz.hpp"

using namespace lexer;
using namespace lexer::dfa;
using namespace lexer::dfa::tools;

class Dfa_test : public testing::Test
{
protected:
    void write_dot(const auto& dfa, const std::string& file_name) const
    {
        Graphviz::to_file(dfa, debug_path_ / (file_name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Dfa_test, Test_empty)
{
    const dfa::Builder dfa;

    const auto result{dfa.build()};

    const Simulator simulator{result};

    constexpr std::vector<char> input;

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(input), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Any_of)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('a'), q0);

    dfa.add_transition(q0, dfa::Label('b'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("b"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("ab"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("ba"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("aab"), Result_t(token, 3));
    EXPECT_EQ(simulator.run("baa"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("aaab"), Result_t(token, 4));
    EXPECT_EQ(simulator.run("baaa"), Result_t(token, 1));

    EXPECT_EQ(simulator.run("a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aa"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aaa"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aaaa"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Single_character)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("a"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("aa"), Result_t(token, 1));

    EXPECT_EQ(simulator.run(""), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Optional_character)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token_empty{1};
    const Token token_a{2};

    dfa.add_accept_state(q0, token_empty);
    dfa.add_accept_state(q1, token_a);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(""), Result_t(token_empty, 0));
    EXPECT_EQ(simulator.run("a"), Result_t(token_a, 1));
    EXPECT_EQ(simulator.run("aa"), Result_t(token_a, 1));

    EXPECT_EQ(simulator.run("b"), Result_t(token_empty, 0));
    EXPECT_EQ(simulator.run("ba"), Result_t(token_empty, 0));
}

TEST_F(Dfa_test, Sequence_ab)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q2, token);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    dfa.add_transition(q1, dfa::Label('b'), q2);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("ab"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("abc"), Result_t(token, 2));

    EXPECT_EQ(simulator.run("a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Kleene_star_a)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    dfa.add_accept_state(q0, token);

    dfa.add_transition(q0, dfa::Label('a'), q0);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(""), Result_t(token, 0));
    EXPECT_EQ(simulator.run("a"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("aa"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("aaa"), Result_t(token, 3));
    EXPECT_EQ(simulator.run("aaab"), Result_t(token, 3));

    EXPECT_EQ(simulator.run("b"), Result_t(token, 0));
    EXPECT_EQ(simulator.run("ba"), Result_t(token, 0));
    EXPECT_EQ(simulator.run("baa"), Result_t(token, 0));
    EXPECT_EQ(simulator.run("baaa"), Result_t(token, 0));
}

TEST_F(Dfa_test, Branch_ab)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q2, token_b);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    dfa.add_transition(q0, dfa::Label('b'), q2);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("a"), Result_t(token_a, 1));
    EXPECT_EQ(simulator.run("b"), Result_t(token_b, 1));
    EXPECT_EQ(simulator.run("ab"), Result_t(token_a, 1));
    EXPECT_EQ(simulator.run("aa"), Result_t(token_a, 1));

    EXPECT_EQ(simulator.run(""), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("c"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("ca"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("cb"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Repeat_abc)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q3, token);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    dfa.add_transition(q1, dfa::Label('b'), q2);

    dfa.add_transition(q2, dfa::Label('c'), q3);

    dfa.add_transition(q3, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("abc"), Result_t(token, 3));
    EXPECT_EQ(simulator.run("abca"), Result_t(token, 3));
    EXPECT_EQ(simulator.run("abcabc"), Result_t(token, 6));
    EXPECT_EQ(simulator.run("abcabcabc"), Result_t(token, 9));

    EXPECT_EQ(simulator.run(""), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("ab"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Contain_ab)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q2, token);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    dfa.add_transition(q1, dfa::Label('b'), q2);

    dfa.add_transition(q0, dfa::Label('x'), q0);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("ab"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("xxab"), Result_t(token, 4));

    EXPECT_EQ(simulator.run("ax"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Numeric_branch)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};
    const auto q4{dfa.next_state()};
    const auto q5{dfa.next_state()};

    const Token token_123{1};
    const Token token_45{1};

    dfa.add_accept_state(q3, token_123);
    dfa.add_accept_state(q5, token_45);

    dfa.add_transition(q0, dfa::Label('1'), q1);
    dfa.add_transition(q1, dfa::Label('2'), q2);
    dfa.add_transition(q2, dfa::Label('3'), q3);

    dfa.add_transition(q0, dfa::Label('4'), q4);
    dfa.add_transition(q4, dfa::Label('5'), q5);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("45"), Result_t(token_45, 2));
    EXPECT_EQ(simulator.run("123"), Result_t(token_123, 3));
    EXPECT_EQ(simulator.run("1234"), Result_t(token_123, 3));

    EXPECT_EQ(simulator.run("12"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("124"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("467"), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Loop_plus_a)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    dfa.add_transition(q1, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("a"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("aa"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("aaa"), Result_t(token, 3));
    EXPECT_EQ(simulator.run("aaaa"), Result_t(token, 4));

    EXPECT_EQ(simulator.run(""), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Result_t(std::nullopt, 0));
}

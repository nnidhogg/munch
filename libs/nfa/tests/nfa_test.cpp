#include "munch/nfa/nfa.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "munch/nfa/builder.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"

using namespace munch;
using namespace munch::nfa;
using namespace munch::nfa::tools;

class Nfa_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Nfa_test, Test_empty)
{
    const nfa::Builder nfa;

    const auto result{nfa.build()};

    constexpr std::vector<char> input;

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, input), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Any_of)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q2, token);

    nfa.add_transition(q0, nfa::Label('a'), q0);

    nfa.add_transition(q0, nfa::Label('b'), q1);

    nfa.add_transition(q1, nfa::Label::epsilon(), q2);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "b"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "ba"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aab"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "baa"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aaab"), Result_t(token, 4));
    EXPECT_EQ(Simulator::run(result, "baaa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(result, "a"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Single_character)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(result, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Optional_character)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q0, token);
    nfa.add_accept_state(q1, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(result, "b"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "ba"), Result_t(token, 0));
}

TEST_F(Nfa_test, Sequence_ab)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q2, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    nfa.add_transition(q1, nfa::Label('b'), q2);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "abc"), Result_t(token, 2));

    EXPECT_EQ(Simulator::run(result, "a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Kleene_star_a)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);

    nfa.add_transition(q0, nfa::Label::epsilon(), q1);

    nfa.add_transition(q1, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "aaab"), Result_t(token, 3));

    EXPECT_EQ(Simulator::run(result, "b"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "ba"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "baa"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "baaa"), Result_t(token, 0));
}

TEST_F(Nfa_test, Branch_ab)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};
    const auto q3{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q2, token);
    nfa.add_accept_state(q3, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    nfa.add_transition(q1, nfa::Label::epsilon(), q2);

    nfa.add_transition(q0, nfa::Label('b'), q3);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "b"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Result_t(token, 1));

    EXPECT_EQ(Simulator::run(result, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "c"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "ca"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "cb"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Repeat_abc)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};
    const auto q3{nfa.next_state()};
    const auto q4{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q0, token);
    nfa.add_accept_state(q4, token);

    nfa.add_transition(q0, nfa::Label::epsilon(), q1);

    nfa.add_transition(q1, nfa::Label('a'), q2);

    nfa.add_transition(q2, nfa::Label('b'), q3);

    nfa.add_transition(q3, nfa::Label('c'), q4);

    nfa.add_transition(q4, nfa::Label::epsilon(), q1);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "abc"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "abca"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "abcabc"), Result_t(token, 6));
    EXPECT_EQ(Simulator::run(result, "abcabcabc"), Result_t(token, 9));
}

TEST_F(Nfa_test, Contain_ab)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q2, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    nfa.add_transition(q1, nfa::Label('b'), q2);

    nfa.add_transition(q0, nfa::Label('x'), q0);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "xxab"), Result_t(token, 4));

    EXPECT_EQ(Simulator::run(result, "ax"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Numeric_branch)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};
    const auto q3{nfa.next_state()};
    const auto q4{nfa.next_state()};
    const auto q5{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q3, token);
    nfa.add_accept_state(q5, token);

    nfa.add_transition(q0, nfa::Label('1'), q1);
    nfa.add_transition(q1, nfa::Label('2'), q2);
    nfa.add_transition(q2, nfa::Label('3'), q3);

    nfa.add_transition(q0, nfa::Label('4'), q4);
    nfa.add_transition(q4, nfa::Label('5'), q5);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "45"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "123"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "1234"), Result_t(token, 3));

    EXPECT_EQ(Simulator::run(result, "12"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "124"), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "467"), Result_t(std::nullopt, 0));
}

TEST_F(Nfa_test, Epsilon_chain)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};
    const auto q2{nfa.next_state()};
    const auto q3{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q3, token);

    nfa.add_transition(q0, nfa::Label::epsilon(), q1);
    nfa.add_transition(q1, nfa::Label::epsilon(), q2);
    nfa.add_transition(q2, nfa::Label::epsilon(), q3);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, ""), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "ab"), Result_t(token, 0));
    EXPECT_EQ(Simulator::run(result, "abc"), Result_t(token, 0));
}

TEST_F(Nfa_test, Loop_plus_a)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q1, token);

    nfa.add_transition(q0, nfa::Label('a'), q1);

    nfa.add_transition(q1, nfa::Label('a'), q1);

    const auto result{nfa.build()};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(Simulator::run(result, "a"), Result_t(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Result_t(token, 2));
    EXPECT_EQ(Simulator::run(result, "aaa"), Result_t(token, 3));
    EXPECT_EQ(Simulator::run(result, "aaaa"), Result_t(token, 4));

    EXPECT_EQ(Simulator::run(result, ""), Result_t(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Result_t(std::nullopt, 0));
}

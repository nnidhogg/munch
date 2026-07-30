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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, input), Match(std::nullopt, 0));
}

TEST_F(Nfa_test, Empty_input_accepting_nfa)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};

    const Token token{1, 1};

    nfa.add_accept_state(q0, token);

    const auto result{nfa.build()};

    const std::vector<char> input;

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, input), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "b"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "ba"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "baa"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aaab"), Match(token, 4));
    EXPECT_EQ(Simulator::run(result, "baaa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(result, "a"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(result, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(result, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "ba"), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "abc"), Match(token, 2));

    EXPECT_EQ(Simulator::run(result, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "aaab"), Match(token, 3));

    EXPECT_EQ(Simulator::run(result, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "ba"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "baa"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "baaa"), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "b"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(result, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "c"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "ca"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "cb"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "abc"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "abca"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "abcabc"), Match(token, 6));
    EXPECT_EQ(Simulator::run(result, "abcabcabc"), Match(token, 9));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "xxab"), Match(token, 4));

    EXPECT_EQ(Simulator::run(result, "ax"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "45"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "123"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "1234"), Match(token, 3));

    EXPECT_EQ(Simulator::run(result, "12"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "124"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "467"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "ab"), Match(token, 0));
    EXPECT_EQ(Simulator::run(result, "abc"), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(result, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(result, "aaaa"), Match(token, 4));

    EXPECT_EQ(Simulator::run(result, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(result, "b"), Match(std::nullopt, 0));
}

TEST_F(Nfa_test, Prepend_init_state_preserves_the_language)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token token{1, 1};

    nfa.add_transition(q0, nfa::Label('a'), q1);
    nfa.add_accept_state(q1, token);

    const auto prepended{nfa.prepend_init_state()};

    EXPECT_NE(prepended.init_state(), q0);

    const auto result{prepended.build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(result, ""), Match(std::nullopt, 0));
}

TEST_F(Nfa_test, Merge_is_a_union_even_when_an_operand_loops_back_to_its_initial_state)
{
    nfa::Builder a_star;

    const auto a0{a_star.init_state()};
    const auto a1{a_star.next_state()};

    const Token token_a{1, 1};
    const Token token_b{2, 1};

    a_star.add_transition(a0, nfa::Label('a'), a1);
    a_star.add_epsilon_transition(a1, a0);
    a_star.add_accept_state(a0, token_a);
    a_star.add_accept_state(a1, token_a);

    nfa::Builder b;

    const auto b1{b.next_state()};

    b.add_transition(b.init_state(), nfa::Label('b'), b1);
    b.add_accept_state(b1, token_b);

    const auto result{a_star.merge(b).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(token_a, 0));
    EXPECT_EQ(Simulator::run(result, "aa"), Match(token_a, 2));
    EXPECT_EQ(Simulator::run(result, "b"), Match(token_b, 1));

    EXPECT_EQ(Simulator::run(result, "ab"), Match(token_a, 1));
}

TEST_F(Nfa_test, Token_accessors)
{
    const Token token{7, 3};

    EXPECT_EQ(token.id(), 7);
    EXPECT_EQ(token.priority(), 3);
}

TEST_F(Nfa_test, Token_equality)
{
    const Token token{1, 5};

    EXPECT_EQ(token, Token(1, 5));
    EXPECT_NE(token, Token(2, 5)); // Differing id short-circuits before comparing priority.
    EXPECT_NE(token, Token(1, 6)); // Equal id forces the priority comparison to run.
}

TEST_F(Nfa_test, Token_operator_less_ties_break_on_id)
{
    const Token lower_id{1, 5};
    const Token higher_id{2, 5};

    EXPECT_LT(lower_id, higher_id);
    EXPECT_FALSE(higher_id < lower_id);
}

TEST_F(Nfa_test, Equal_priority_accept_states_prefer_lower_id)
{
    nfa::Builder nfa;

    const auto q0{nfa.init_state()};
    const auto q1{nfa.next_state()};

    const Token higher_id{2, 1};
    const Token lower_id{1, 1};

    nfa.add_accept_state(q0, higher_id);
    nfa.add_accept_state(q1, lower_id);

    nfa.add_transition(q0, nfa::Label::epsilon(), q1);

    const auto result{nfa.build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(result, ""), Match(lower_id, 0));
}

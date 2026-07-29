#include "munch/dfa/dfa.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/tools/graphviz.hpp"

using namespace munch;
using namespace munch::dfa;
using namespace munch::dfa::tools;

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
    dfa::Builder dfa;

    const auto result{dfa.build()};

    const Simulator simulator{result};

    constexpr std::vector<char> input;

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(input), Result_t(std::nullopt, 0));
}

TEST_F(Dfa_test, Empty_and_non_empty_string_container)
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

    EXPECT_EQ(simulator.run(std::string{}), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run(std::string{"a"}), Result_t(token, 1));
}

TEST_F(Dfa_test, Non_empty_vector_container)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token{1};

    dfa.add_accept_state(q1, token);
    dfa.add_transition(q0, dfa::Label('a'), q1);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    const std::vector<char> input{'a'};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(input), Result_t(token, 1));
}

TEST_F(Dfa_test, Long_self_loop_run)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    dfa.add_accept_state(q0, token);
    dfa.add_transition(q0, dfa::Label('a'), q0);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    const std::string run(5000, 'a');

    EXPECT_EQ(simulator.run(run), Result_t(token, 5000));
    EXPECT_EQ(simulator.run(run + "b"), Result_t(token, 5000));
    EXPECT_EQ(simulator.run("b" + run), Result_t(token, 0));
}

TEST_F(Dfa_test, Self_loop_over_high_symbol_values)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    dfa.add_accept_state(q0, token);

    // Symbols across the full byte range, including values that are negative as plain char.
    for (const char symbol : {'\x3F', '\x40', '\x7F', '\x80', '\xBF', '\xC0', '\xFF'})
    {
        dfa.add_transition(q0, dfa::Label(symbol), q0);
    }

    const auto result{dfa.build()};

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    const std::string input{"\x3F\x40\x7F\x80\xBF\xC0\xFF"};

    EXPECT_EQ(simulator.run(input), Result_t(token, 7));
}

TEST_F(Dfa_test, Empty_input_accepting_dfa)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    dfa.add_accept_state(q0, token);

    const auto result{dfa.build()};

    const Simulator simulator{result};

    const std::vector<char> input;

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run(input), Result_t(token, 0));
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
    const Token token_45{2};

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

TEST_F(Dfa_test, Split_points)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    // 'a' continues its own run (q1 loops), so it is not a split point; 'b' is consumed only from the initial
    // state, so it can only begin a token; 'c' is consumed nowhere and is vacuously safe.
    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q2, token_b);

    dfa.add_transition(q0, dfa::Label('a'), q1);
    dfa.add_transition(q1, dfa::Label('a'), q1);
    dfa.add_transition(q0, dfa::Label('b'), q2);

    const Simulator simulator{dfa.build()};

    EXPECT_FALSE(simulator.is_split_point('a'));
    EXPECT_TRUE(simulator.is_split_point('b'));
    EXPECT_TRUE(simulator.is_split_point('c'));
}

TEST_F(Dfa_test, Accelerated_runs_preserve_longest_match)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token_a{1};
    const Token token_ab{2};

    // Tokens "a" and "a+b": q2 self-loops on 'a' without accepting, so a long all-'a' input walks deep into q2
    // and must still resolve to the one-character token seen before the run.
    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q3, token_ab);

    dfa.add_transition(q0, dfa::Label('a'), q1);
    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('a'), q2);
    dfa.add_transition(q1, dfa::Label('b'), q3);
    dfa.add_transition(q2, dfa::Label('b'), q3);

    const Simulator simulator{dfa.build()};

    const std::string run_with_b{std::string(40, 'a') + 'b'};

    std::vector<std::pair<std::size_t, std::size_t>> tokens;

    auto consumed{simulator.run_all(run_with_b.cbegin(), run_with_b.cend(), [&tokens](const Token& token, const std::size_t length) {
        tokens.emplace_back(token.id(), length);
    })};

    EXPECT_EQ(consumed, run_with_b.size());
    ASSERT_EQ(tokens.size(), 1U);
    EXPECT_EQ(tokens.front(), (std::pair<std::size_t, std::size_t>{token_ab.id(), 41U}));

    // Without the 'b', the longest match fails and every rescan must fall back to the length-one token, so the
    // accept position recorded before the run must survive the walk through it.
    const std::string run_without_b(40, 'a');

    tokens.clear();

    consumed = simulator.run_all(run_without_b.cbegin(), run_without_b.cend(),
                                 [&tokens](const Token& token, const std::size_t length) {
                                     tokens.emplace_back(token.id(), length);
                                 });

    EXPECT_EQ(consumed, run_without_b.size());
    EXPECT_EQ(tokens.size(), 40U);

    for (const auto& token : tokens)
    {
        EXPECT_EQ(token, (std::pair<std::size_t, std::size_t>{token_a.id(), 1U}));
    }
}

TEST_F(Dfa_test, Accelerated_runs_extend_accepting_tokens)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};

    const Token token_run{1};

    // A plus-style run token: q1 accepts and self-loops, so the accept position must advance to the run's end.
    dfa.add_accept_state(q1, token_run);

    dfa.add_transition(q0, dfa::Label(' '), q1);
    dfa.add_transition(q1, dfa::Label(' '), q1);

    const Simulator simulator{dfa.build()};

    for (const std::size_t length : {1U, 7U, 15U, 16U, 17U, 100U})
    {
        const std::string input(length, ' ');

        std::vector<std::size_t> lengths;

        const auto consumed{simulator.run_all(input.cbegin(), input.cend(),
                                              [&lengths](const Token&, const std::size_t matched) {
                                                  lengths.push_back(matched);
                                              })};

        EXPECT_EQ(consumed, length);
        ASSERT_EQ(lengths.size(), 1U);
        EXPECT_EQ(lengths.front(), length);
    }
}

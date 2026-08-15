#include "munch/dfa/dfa.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(input), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(std::string{}), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run(std::string{"a"}), Match(token, 1));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(input), Match(token, 1));
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

    using Match = Simulator::Match;

    const std::string run(5000, 'a');

    EXPECT_EQ(simulator.run(run), Match(token, 5000));
    EXPECT_EQ(simulator.run(run + "b"), Match(token, 5000));
    EXPECT_EQ(simulator.run("b" + run), Match(token, 0));
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

    using Match = Simulator::Match;

    const std::string input{"\x3F\x40\x7F\x80\xBF\xC0\xFF"};

    EXPECT_EQ(simulator.run(input), Match(token, 7));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(input), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("b"), Match(token, 1));
    EXPECT_EQ(simulator.run("ab"), Match(token, 2));
    EXPECT_EQ(simulator.run("ba"), Match(token, 1));
    EXPECT_EQ(simulator.run("aab"), Match(token, 3));
    EXPECT_EQ(simulator.run("baa"), Match(token, 1));
    EXPECT_EQ(simulator.run("aaab"), Match(token, 4));
    EXPECT_EQ(simulator.run("baaa"), Match(token, 1));

    EXPECT_EQ(simulator.run("a"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aa"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aaa"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("aaaa"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("a"), Match(token, 1));
    EXPECT_EQ(simulator.run("aa"), Match(token, 1));

    EXPECT_EQ(simulator.run(""), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(""), Match(token_empty, 0));
    EXPECT_EQ(simulator.run("a"), Match(token_a, 1));
    EXPECT_EQ(simulator.run("aa"), Match(token_a, 1));

    EXPECT_EQ(simulator.run("b"), Match(token_empty, 0));
    EXPECT_EQ(simulator.run("ba"), Match(token_empty, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("ab"), Match(token, 2));
    EXPECT_EQ(simulator.run("abc"), Match(token, 2));

    EXPECT_EQ(simulator.run("a"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run(""), Match(token, 0));
    EXPECT_EQ(simulator.run("a"), Match(token, 1));
    EXPECT_EQ(simulator.run("aa"), Match(token, 2));
    EXPECT_EQ(simulator.run("aaa"), Match(token, 3));
    EXPECT_EQ(simulator.run("aaab"), Match(token, 3));

    EXPECT_EQ(simulator.run("b"), Match(token, 0));
    EXPECT_EQ(simulator.run("ba"), Match(token, 0));
    EXPECT_EQ(simulator.run("baa"), Match(token, 0));
    EXPECT_EQ(simulator.run("baaa"), Match(token, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("a"), Match(token_a, 1));
    EXPECT_EQ(simulator.run("b"), Match(token_b, 1));
    EXPECT_EQ(simulator.run("ab"), Match(token_a, 1));
    EXPECT_EQ(simulator.run("aa"), Match(token_a, 1));

    EXPECT_EQ(simulator.run(""), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("c"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("ca"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("cb"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("abc"), Match(token, 3));
    EXPECT_EQ(simulator.run("abca"), Match(token, 3));
    EXPECT_EQ(simulator.run("abcabc"), Match(token, 6));
    EXPECT_EQ(simulator.run("abcabcabc"), Match(token, 9));

    EXPECT_EQ(simulator.run(""), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("a"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("ab"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("ab"), Match(token, 2));
    EXPECT_EQ(simulator.run("xxab"), Match(token, 4));

    EXPECT_EQ(simulator.run("ax"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("45"), Match(token_45, 2));
    EXPECT_EQ(simulator.run("123"), Match(token_123, 3));
    EXPECT_EQ(simulator.run("1234"), Match(token_123, 3));

    EXPECT_EQ(simulator.run("12"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("124"), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("467"), Match(std::nullopt, 0));
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

    using Match = Simulator::Match;

    EXPECT_EQ(simulator.run("a"), Match(token, 1));
    EXPECT_EQ(simulator.run("aa"), Match(token, 2));
    EXPECT_EQ(simulator.run("aaa"), Match(token, 3));
    EXPECT_EQ(simulator.run("aaaa"), Match(token, 4));

    EXPECT_EQ(simulator.run(""), Match(std::nullopt, 0));
    EXPECT_EQ(simulator.run("b"), Match(std::nullopt, 0));
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
    // state, so it can only begin a token; 'c' is consumed nowhere, so it is safe only vacuously and is not
    // reported, since no input this automaton accepts can contain it.
    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q2, token_b);

    dfa.add_transition(q0, dfa::Label('a'), q1);
    dfa.add_transition(q1, dfa::Label('a'), q1);
    dfa.add_transition(q0, dfa::Label('b'), q2);

    const Simulator simulator{dfa.build()};

    EXPECT_FALSE(simulator.is_split_point('a'));
    EXPECT_TRUE(simulator.is_split_point('b'));
    EXPECT_FALSE(simulator.is_split_point('c'));
    EXPECT_TRUE(simulator.has_split_points());
}

TEST_F(Dfa_test, Unreachable_states_do_not_decertify)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q3, token_b);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    // A live island no input reaches: q2 accepts by way of q3, so it survives the co-accessibility sweep, and it
    // consumes 'a' mid-token. No scan can ever be in it, so it must not cost 'a' its certificate.
    dfa.add_transition(q2, dfa::Label('a'), q3);

    const Simulator simulator{dfa.build()};

    EXPECT_TRUE(simulator.is_split_point('a'));
}

TEST_F(Dfa_test, Unreachable_transitions_do_not_make_the_initial_state_reentrant)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token_a{1};

    dfa.add_accept_state(q1, token_a);

    dfa.add_transition(q0, dfa::Label('a'), q1);

    // q2 is unreachable, so its transition back into the initial state is not a way for a scan to return there and
    // must not defeat the "can only begin a token" reasoning that certifies 'a'.
    dfa.add_transition(q2, dfa::Label('b'), q0);

    const Simulator simulator{dfa.build()};

    EXPECT_TRUE(simulator.is_split_point('a'));
}

TEST_F(Dfa_test, Mandatory_core_is_proved_when_every_death_word_carries_it)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token{1};

    // The confirming shape: q1 consumes every byte, and the only route out of its loop is 'a' into q2, whose
    // sole live byte returns. Every word that kills a scan sitting in q1 must therefore spell 'a' strictly
    // before its killing byte, which is exactly the licence the accessor reports.
    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('s'), q1);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        if (static_cast<char>(symbol) != 'a')
        {
            dfa.add_transition(q1, dfa::Label(static_cast<char>(symbol)), q1);
        }
    }

    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('c'), q1);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST_F(Dfa_test, Mandatory_core_stays_empty_when_a_second_route_dies_without_it)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token{1};

    // The refuting shape: the same loop now has a second escape, 'z' into q3, so a scan can die by spelling
    // "z" and then a byte q3 refuses, a death word that never contains the 'a' the first route proposes. The
    // proof must fail and the accessor must stay empty, since a nonempty answer here would license a planner
    // filter that skips cuts the exhaustive walk finds.
    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('s'), q1);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        if (static_cast<char>(symbol) != 'a' && static_cast<char>(symbol) != 'z')
        {
            dfa.add_transition(q1, dfa::Label(static_cast<char>(symbol)), q1);
        }
    }

    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('c'), q1);

    dfa.add_transition(q1, dfa::Label('z'), q3);
    dfa.add_transition(q3, dfa::Label('c'), q1);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_keeps_the_longest_proved_candidate)
{
    // A chain of forced escapes: the only route from the hub loop to death spells 'x' then 'y' then 'z', and
    // every deviation returns to the loop. The three loop states are all input-total and propose the nested
    // cores "xyz", "yz" and "z", every one of which is proved, so the accessor must keep the longest. The
    // same table is wired twice with the proposing states allocated in opposite orders, so neither state
    // order nor proposal order can stand in for the actual length comparison: a model that kept the first or
    // the last proposal instead of the longest fails one of the two.
    const auto build{[](const bool reversed) {
        dfa::Builder dfa;

        const auto q0{dfa.init_state()};

        const auto first{dfa.next_state()};
        const auto second{dfa.next_state()};
        const auto third{dfa.next_state()};

        const auto hub{reversed ? third : first};
        const auto middle{second};
        const auto low{reversed ? first : third};

        const auto killer{dfa.next_state()};

        const Token token{1};

        dfa.add_accept_state(hub, token);

        dfa.add_transition(q0, dfa::Label('s'), hub);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'x')
            {
                dfa.add_transition(hub, dfa::Label(byte), hub);
            }

            if (byte != 'y')
            {
                dfa.add_transition(middle, dfa::Label(byte), hub);
            }

            if (byte != 'z')
            {
                dfa.add_transition(low, dfa::Label(byte), hub);
            }
        }

        dfa.add_transition(hub, dfa::Label('x'), middle);
        dfa.add_transition(middle, dfa::Label('y'), low);
        dfa.add_transition(low, dfa::Label('z'), killer);
        dfa.add_transition(killer, dfa::Label('c'), hub);

        return Simulator{dfa.build()};
    }};

    EXPECT_EQ(build(false).mandatory_core(), "xyz");

    EXPECT_EQ(build(true).mandatory_core(), "xyz");
}

TEST_F(Dfa_test, Mandatory_core_breaks_equal_length_ties_in_state_order)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    // Twenty disconnected loops, each proving its own one-byte core within its own reach: every proposal is
    // sound, so the answer is a many-way tie the accessor has always broken toward the earliest state. The
    // published value is part of the behavior the planner tests pin against, and with this many equal
    // candidates an unstable ordering is free to land anywhere in the pack, and the widths here make the
    // library implementations at hand actually do so; the pinned value itself is what the contract owes.
    for (int region{0}; region < 20; ++region)
    {
        const auto hub{dfa.next_state()};
        const auto killer{dfa.next_state()};

        const auto escape{static_cast<char>('a' + region)};

        dfa.add_accept_state(hub, token);

        dfa.add_transition(q0, dfa::Label(static_cast<char>('A' + region)), hub);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != escape)
            {
                dfa.add_transition(hub, dfa::Label(byte), hub);
            }
        }

        dfa.add_transition(hub, dfa::Label(escape), killer);
        dfa.add_transition(killer, dfa::Label('c'), hub);
    }

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST_F(Dfa_test, Mandatory_core_origin_stamps_do_not_leak_across_proofs)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto shorter{dfa.next_state()};
    const auto longer{dfa.next_state()};
    const auto immortal{dfa.next_state()};
    const auto killer{dfa.next_state()};

    const Token token{1};

    // The state indices are chosen so the first proof's origin cell and the second proof's refuting pair
    // land on the same physical slot: the two-byte proposal's origin occupies index four, and the one-byte
    // proposal must later visit the killer at prefix zero, which is index four again under its stride. A
    // buffer that lets the first proof's origin stamp leak forward suppresses that visit, skips the only
    // refutation, and wrongly proves the shorter core; the honest answer is empty.
    dfa.add_accept_state(immortal, token);

    dfa.add_transition(q0, dfa::Label('x'), longer);
    dfa.add_transition(q0, dfa::Label('y'), shorter);
    dfa.add_transition(q0, dfa::Label('v'), immortal);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        dfa.add_transition(immortal, dfa::Label(byte), immortal);

        if (byte != 'a' && byte != 'b')
        {
            dfa.add_transition(shorter, dfa::Label(byte), immortal);
        }

        if (byte != 'a')
        {
            dfa.add_transition(longer, dfa::Label(byte), immortal);
        }
    }

    dfa.add_transition(shorter, dfa::Label('a'), killer);
    dfa.add_transition(shorter, dfa::Label('b'), killer);
    dfa.add_transition(longer, dfa::Label('a'), shorter);

    dfa.add_transition(killer, dfa::Label('c'), immortal);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_ignores_escapes_into_states_that_never_accept_again)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto hub{dfa.next_state()};
    const auto killer{dfa.next_state()};
    const auto dead{dfa.next_state()};

    const Token token{1};

    // The hub's 'z' escape leads to a loop from which acceptance is unreachable, so it is not a live
    // transition and the hub is not input-total: no candidate exists and the accessor stays empty. A
    // derivation that forgets to require LIVE targets treats the dead loop as an ordinary neighbour,
    // finds the hub total, and proves the 'a' whose only counterweight was that very escape.
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'z')
        {
            dfa.add_transition(hub, dfa::Label(byte), hub);
        }

        dfa.add_transition(dead, dfa::Label(byte), dead);
    }

    dfa.add_transition(hub, dfa::Label('a'), killer);
    dfa.add_transition(hub, dfa::Label('z'), dead);

    dfa.add_transition(killer, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_reaches_a_proposer_allocated_last)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto killer{dfa.next_state()};
    const auto hub{dfa.next_state()};

    const Token token{1};

    // The only proposing state carries the highest index on purpose: every derivation pass — predecessor
    // construction, canonical links, candidate collection — must include the final state, and a loop bound
    // trimmed by one silently forgets exactly this proposer, turning the proved core into an empty answer.
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a')
        {
            dfa.add_transition(hub, dfa::Label(byte), hub);
        }
    }

    dfa.add_transition(hub, dfa::Label('a'), killer);
    dfa.add_transition(killer, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST_F(Dfa_test, Mandatory_core_origin_stamp_uses_the_current_proofs_stride)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto shorter{dfa.next_state()};
    const auto killer{dfa.next_state()};
    const auto longer{dfa.next_state()};
    const auto immortal{dfa.next_state()};

    const Token token{1};

    // The allocation puts the one-byte proposer at index one and its killer at index two, so an origin seed
    // written with the wrong stride — the longest length instead of the current proof's — lands exactly on
    // the cell the second proof must visit to refute. The two-byte proposal refutes first, the one-byte one
    // must refute through the killer, and a mis-strided seed suppresses that visit and proves it instead.
    dfa.add_accept_state(shorter, token);
    dfa.add_accept_state(longer, token);
    dfa.add_accept_state(immortal, token);

    dfa.add_transition(q0, dfa::Label('x'), shorter);
    dfa.add_transition(q0, dfa::Label('y'), longer);
    dfa.add_transition(q0, dfa::Label('v'), immortal);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        dfa.add_transition(immortal, dfa::Label(byte), immortal);

        if (byte != 'a' && byte != 'b')
        {
            dfa.add_transition(shorter, dfa::Label(byte), immortal);
        }

        if (byte != 'd')
        {
            dfa.add_transition(longer, dfa::Label(byte), immortal);
        }
    }

    dfa.add_transition(shorter, dfa::Label('a'), killer);
    dfa.add_transition(shorter, dfa::Label('b'), killer);
    dfa.add_transition(longer, dfa::Label('d'), shorter);

    dfa.add_transition(killer, dfa::Label('c'), immortal);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_generations_survive_more_proofs_than_a_byte_can_count)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    // Two hundred and fifty-six regions, each proposing a core its own second escape refutes, so two
    // hundred and fifty-six proofs run and every one must carry a distinct stamp. An ordinal stamp one
    // byte wide wraps to the buffer's virgin zero on the last proof, reads every untouched cell as already
    // seen, skips the refutation, and falsely proves the final region's core; the honest answer is empty.
    for (int region{0}; region < 256; ++region)
    {
        const auto hub{dfa.next_state()};
        const auto killer{dfa.next_state()};

        dfa.add_accept_state(hub, token);

        dfa.add_transition(q0, dfa::Label(static_cast<char>(region)), hub);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a' && byte != 'b')
            {
                dfa.add_transition(hub, dfa::Label(byte), hub);
            }
        }

        dfa.add_transition(hub, dfa::Label('a'), killer);
        dfa.add_transition(hub, dfa::Label('b'), killer);

        dfa.add_transition(killer, dfa::Label('c'), hub);
    }

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_matcher_rows_start_fresh_for_every_candidate)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    // Three loops proposing equal-length one-byte cores in allocation order: the first is refuted along its
    // 0x81 route, the second along its 0x80 route, and the third proves. The second refutation hinges on
    // the matcher table being rebuilt from zero for its candidate: a table keeping the first proof's stale
    // row-zero entry for byte 0x80 treats that route as a completed match, prunes it, and wrongly proves
    // the second core instead of the third.
    const auto region{[&dfa, &token](const char escape, const std::optional<char> refuter) {
        const auto hub{dfa.next_state()};
        const auto killer{dfa.next_state()};

        dfa.add_accept_state(hub, token);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != escape && (!refuter || byte != *refuter))
            {
                dfa.add_transition(hub, dfa::Label(byte), hub);
            }
        }

        dfa.add_transition(hub, dfa::Label(escape), killer);

        if (refuter)
        {
            dfa.add_transition(hub, dfa::Label(*refuter), killer);
        }

        dfa.add_transition(killer, dfa::Label('c'), hub);

        return hub;
    }};

    dfa.add_transition(q0, dfa::Label('x'), region(static_cast<char>(0x80), static_cast<char>(0x81)));
    dfa.add_transition(q0, dfa::Label('y'), region(static_cast<char>(0x01), static_cast<char>(0x80)));
    dfa.add_transition(q0, dfa::Label('z'), region(static_cast<char>(0x02), std::nullopt));

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string{static_cast<char>(0x02)});
}

TEST_F(Dfa_test, Mandatory_core_survives_a_core_longer_than_the_byte_range)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    const Token token{1};

    std::vector<dfa::Dfa::State_t> chain;

    for (int at{0}; at <= 256; ++at)
    {
        chain.push_back(dfa.next_state());
    }

    // A 256-byte core: every matcher-table entry up to 256 must survive intact, so any cell narrower than
    // the count of prefixes wraps and shears the proposal down. The chain forces the full run before the
    // only death, and the accessor must report all 256 bytes.
    dfa.add_accept_state(chain[0], token);

    dfa.add_transition(q0, dfa::Label('s'), chain[0]);

    for (int at{0}; at < 256; ++at)
    {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a')
            {
                dfa.add_transition(chain[at], dfa::Label(byte), chain[0]);
            }
        }

        dfa.add_transition(chain[at], dfa::Label('a'), chain[at + 1]);
    }

    dfa.add_transition(chain[256], dfa::Label('c'), chain[0]);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string(256, 'a'));
}

TEST_F(Dfa_test, Mandatory_core_failure_table_chains_two_borders_in_construction)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto hub{dfa.next_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};
    const auto q4{dfa.next_state()};
    const auto r5{dfa.next_state()};
    const auto r6{dfa.next_state()};
    const auto r7{dfa.next_state()};
    const auto killer{dfa.next_state()};

    const Token token{1};

    // The hub's proposal "aaabb" is refuted only by the alternate route spelling "aaabaabb", and telling
    // those apart hinges on the failure table built for the proposal itself: at its fourth position the
    // construction must chain through two borders to land on zero, where a single construction hop leaves a
    // link that lets the alternate route pretend to carry the proposal. The honest answer is the next
    // proposal down, "aabb", which both routes genuinely contain.
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    const auto reset_except{[&dfa, &hub](const auto state, const std::initializer_list<char> taken) {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            auto skip{false};

            for (const auto held : taken)
            {
                skip = skip || byte == held;
            }

            if (!skip)
            {
                dfa.add_transition(state, dfa::Label(byte), hub);
            }
        }
    }};

    reset_except(hub, {'a'});
    reset_except(q1, {'a'});
    reset_except(q2, {'a'});
    reset_except(q3, {'b'});
    reset_except(q4, {'b', 'a'});
    reset_except(r5, {'a'});
    reset_except(r6, {'b'});
    reset_except(r7, {'b'});

    dfa.add_transition(hub, dfa::Label('a'), q1);
    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('a'), q3);
    dfa.add_transition(q3, dfa::Label('b'), q4);
    dfa.add_transition(q4, dfa::Label('b'), killer);

    dfa.add_transition(q4, dfa::Label('a'), r5);
    dfa.add_transition(r5, dfa::Label('a'), r6);
    dfa.add_transition(r6, dfa::Label('b'), r7);
    dfa.add_transition(r7, dfa::Label('b'), killer);

    dfa.add_transition(killer, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aabb");
}

TEST_F(Dfa_test, Mandatory_core_seeding_reads_both_ends_of_the_alphabet)
{
    // The killer consumes every byte except one, placed at either end of the alphabet, so its death is
    // visible only to a seeding pass covering the full symbol range; a pass starting one byte late misses
    // the zero case and one stopping a byte early misses the last, each forfeiting the core the hub's
    // escape plainly proves.
    const auto build{[](const int missing) {
        dfa::Builder dfa;

        const auto q0{dfa.init_state()};
        const auto hub{dfa.next_state()};
        const auto killer{dfa.next_state()};

        const Token token{1};

        dfa.add_accept_state(hub, token);

        dfa.add_transition(q0, dfa::Label('s'), hub);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a')
            {
                dfa.add_transition(hub, dfa::Label(byte), hub);
            }

            if (symbol != missing)
            {
                dfa.add_transition(killer, dfa::Label(byte), hub);
            }
        }

        dfa.add_transition(hub, dfa::Label('a'), killer);

        return Simulator{dfa.build()};
    }};

    EXPECT_EQ(build(0x00).mandatory_core(), "a");

    EXPECT_EQ(build(0xff).mandatory_core(), "a");
}

TEST_F(Dfa_test, Mandatory_core_reverse_search_reads_the_last_byte)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto hub{dfa.next_state()};
    const auto tail{dfa.next_state()};

    const Token token{1};

    // The only route from the hub to death rides the alphabet's final byte, so the backward search's
    // predecessor edges must be built through the very last symbol; a construction stopping one short
    // leaves the hub depthless and the accessor empty where the escape proves a core.
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    for (int symbol{0}; symbol < 255; ++symbol)
    {
        dfa.add_transition(hub, dfa::Label(static_cast<char>(symbol)), hub);
    }

    dfa.add_transition(hub, dfa::Label(static_cast<char>(0xff)), tail);
    dfa.add_transition(tail, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string{static_cast<char>(0xff)});
}

TEST_F(Dfa_test, Mandatory_core_canonical_word_starts_at_the_zeroth_byte)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto hub{dfa.next_state()};
    const auto middle{dfa.next_state()};
    const auto killer{dfa.next_state()};

    const Token token{1};

    // The shortest death word leaves the hub on byte zero, so the canonical-word pass choosing the smallest
    // byte one layer shallower must scan from the alphabet's start; a scan beginning at byte one finds no
    // shallower step at the hub and spells garbage in place of the two-byte core the route proves.
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    for (int symbol{1}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        dfa.add_transition(hub, dfa::Label(byte), hub);

        if (byte != 'a')
        {
            dfa.add_transition(middle, dfa::Label(byte), hub);
        }
    }

    dfa.add_transition(hub, dfa::Label(static_cast<char>(0)), middle);
    dfa.add_transition(middle, dfa::Label(static_cast<char>(0)), hub);
    dfa.add_transition(middle, dfa::Label('a'), killer);
    dfa.add_transition(killer, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), (std::string{"\0a", 2}));
}

TEST_F(Dfa_test, Mandatory_core_proofs_do_not_inherit_the_previous_searchs_footprint)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto slow{dfa.next_state()};
    const auto fast{dfa.next_state()};
    const auto immortal{dfa.next_state()};

    const Token token{1};

    // Two candidates of different lengths both refute through the initial state's dead bytes, and because
    // the initial state's index is zero, the pair it contributes lands in the same buffer cell under both
    // proofs' strides. A buffer that carries the first search's footprint into the second suppresses that
    // revisit, skips the refutation, and wrongly proves the shorter core; the honest answer is empty.
    dfa.add_accept_state(immortal, token);

    dfa.add_transition(q0, dfa::Label('s'), fast);
    dfa.add_transition(q0, dfa::Label('t'), slow);
    dfa.add_transition(q0, dfa::Label('v'), immortal);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        dfa.add_transition(immortal, dfa::Label(byte), immortal);

        if (symbol < 'a')
        {
            dfa.add_transition(fast, dfa::Label(byte), immortal);
            dfa.add_transition(slow, dfa::Label(byte), immortal);
        }
        else
        {
            dfa.add_transition(fast, dfa::Label(byte), q0);
            dfa.add_transition(slow, dfa::Label(byte), fast);
        }
    }

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_exemption_follows_the_initial_states_reentrancy)
{
    // The same table twice, differing in one edge: where the killer state's surviving byte returns. Both
    // tables funnel every death through 'p' then 'q' from the initial state and through 'q' alone from qb, so
    // q0 proposes "pq" and qb proposes "q", and both proposals are proved from their own states. When the
    // surviving byte returns to the initial state, a scan can sit there mid-input, q0 is a required state,
    // and its longer core is the answer. When it returns to the absorbing loop instead, nothing re-enters q0,
    // the window hypothesis at q0 renames rather than survives, and the exemption must drop its proposal.
    const auto build{[](const bool reentrant) {
        dfa::Builder dfa;

        const auto q0{dfa.init_state()};
        const auto qb{dfa.next_state()};
        const auto qa{dfa.next_state()};
        const auto ql{dfa.next_state()};

        const Token token{1};

        dfa.add_accept_state(ql, token);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'p')
            {
                dfa.add_transition(q0, dfa::Label(byte), ql);
            }

            if (byte != 'q')
            {
                dfa.add_transition(qb, dfa::Label(byte), ql);
            }

            dfa.add_transition(ql, dfa::Label(byte), ql);
        }

        dfa.add_transition(q0, dfa::Label('p'), qb);
        dfa.add_transition(qb, dfa::Label('q'), qa);
        dfa.add_transition(qa, dfa::Label('c'), reentrant ? q0 : ql);

        return Simulator{dfa.build()};
    }};

    EXPECT_EQ(build(true).mandatory_core(), "pq");

    EXPECT_EQ(build(false).mandatory_core(), "q");
}

TEST_F(Dfa_test, Mandatory_core_stays_empty_when_the_killing_byte_itself_completes_the_core)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token{1};

    // The refuting shape for the order of the two checks: the second escape 'z' leads to a state that dies
    // exactly on 'a', so the death word "za" carries the proposed core only as its own killing byte. Too
    // late is not carried: a certified window built on this table could end on the core with nothing after
    // it, so the proof must refuse, and a matcher that reads the killing byte before noticing the death
    // would wrongly accept.
    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('s'), q1);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'z')
        {
            dfa.add_transition(q1, dfa::Label(byte), q1);
        }

        if (byte != 'a')
        {
            dfa.add_transition(q3, dfa::Label(byte), q1);
        }
    }

    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('c'), q1);

    dfa.add_transition(q1, dfa::Label('z'), q3);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_survives_a_stretched_run_inside_its_own_prefix)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};
    const auto q4{dfa.next_state()};

    const Token token{1};

    // Death spells at least two 'a's and then 'b', but the run of 'a's can stretch, so on "aaab" the matcher
    // must fall back across the border of "aab" without losing the two 'a's it has already seen. A matcher
    // that restarts from nothing on a mismatch concludes the stretched run avoids the core and settles for
    // the shorter "ab"; the correct failure links keep the full proposal.
    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('s'), q1);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a')
        {
            dfa.add_transition(q1, dfa::Label(byte), q1);
            dfa.add_transition(q2, dfa::Label(byte), q1);
        }

        if (byte != 'a' && byte != 'b')
        {
            dfa.add_transition(q3, dfa::Label(byte), q1);
        }
    }

    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('a'), q3);
    dfa.add_transition(q3, dfa::Label('a'), q3);
    dfa.add_transition(q3, dfa::Label('b'), q4);
    dfa.add_transition(q4, dfa::Label('c'), q1);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aab");
}

TEST_F(Dfa_test, Mandatory_core_shrinks_when_an_overlapping_route_only_pretends_to_carry_it)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};
    const auto q4{dfa.next_state()};
    const auto q5{dfa.next_state()};
    const auto q6{dfa.next_state()};

    const Token token{1};

    // Two escapes share the prefix "ab": one dies after "aba" and proposes that word as the core, the other
    // dies after "abba" and must refute it, because "abba" holds no "aba". A matcher whose failure links
    // overreach reads the second 'b' of "abb" as still two matched and lets the final 'a' complete a core
    // that never occurred, proving the longer proposal on a route that does not carry it. The honest answer
    // is the shared suffix "ba", which both death words do carry.
    dfa.add_accept_state(q1, token);

    dfa.add_transition(q0, dfa::Label('s'), q1);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a')
        {
            dfa.add_transition(q1, dfa::Label(byte), q1);
        }

        if (byte != 'b')
        {
            dfa.add_transition(q2, dfa::Label(byte), q1);
        }

        if (byte != 'a' && byte != 'b')
        {
            dfa.add_transition(q3, dfa::Label(byte), q1);
        }

        if (byte != 'a')
        {
            dfa.add_transition(q5, dfa::Label(byte), q1);
        }
    }

    dfa.add_transition(q1, dfa::Label('a'), q2);
    dfa.add_transition(q2, dfa::Label('b'), q3);
    dfa.add_transition(q3, dfa::Label('a'), q4);
    dfa.add_transition(q4, dfa::Label('c'), q1);

    dfa.add_transition(q3, dfa::Label('b'), q5);
    dfa.add_transition(q5, dfa::Label('a'), q6);
    dfa.add_transition(q6, dfa::Label('c'), q1);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "ba");
}

TEST_F(Dfa_test, Mandatory_core_refutation_reaches_both_ends_of_the_alphabet)
{
    // The refuting route dies on one byte and on nothing else, and that byte sits at either end of the
    // alphabet: a proof search stopping one symbol short of the end misses the 0xff death, and one starting
    // a symbol late misses the 0x00 death, each wrongly proving the 'a' the other route proposes. The
    // shape is otherwise the familiar two-route refutation.
    const auto build{[](const int edge) {
        dfa::Builder dfa;

        const auto q0{dfa.init_state()};
        const auto q1{dfa.next_state()};
        const auto q2{dfa.next_state()};
        const auto q3{dfa.next_state()};

        const Token token{1};

        dfa.add_accept_state(q1, token);

        dfa.add_transition(q0, dfa::Label('s'), q1);

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a' && byte != 'z')
            {
                dfa.add_transition(q1, dfa::Label(byte), q1);
            }

            if (symbol != 0x01)
            {
                dfa.add_transition(q2, dfa::Label(byte), q1);
            }

            if (symbol != edge)
            {
                dfa.add_transition(q3, dfa::Label(byte), q1);
            }
        }

        dfa.add_transition(q1, dfa::Label('a'), q2);
        dfa.add_transition(q1, dfa::Label('z'), q3);

        return Simulator{dfa.build()};
    }};

    EXPECT_EQ(build(0xff).mandatory_core(), "");

    EXPECT_EQ(build(0x00).mandatory_core(), "");
}

TEST_F(Dfa_test, Mandatory_core_revisits_a_state_under_a_different_matcher_prefix)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token{1};

    // The initial state's proposal "zz" is refuted only along "zy" followed by "zz": the search reaches q1
    // first with one byte matched and later with none, and the refutation lives behind the second visit. A
    // search that keys its seen set on the pair it came from rather than the pair it reaches conflates the
    // two visits, never walks the refuting route, and wrongly proves the longer core over the honest "z".
    dfa.add_accept_state(q1, token);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'z')
        {
            dfa.add_transition(q0, dfa::Label(byte), q0);
        }

        if (byte != 'y' && byte != 'z')
        {
            dfa.add_transition(q1, dfa::Label(byte), q0);
        }

        if (byte != 'z')
        {
            dfa.add_transition(q2, dfa::Label(byte), q0);
        }
    }

    dfa.add_transition(q0, dfa::Label('z'), q1);
    dfa.add_transition(q1, dfa::Label('y'), q1);
    dfa.add_transition(q1, dfa::Label('z'), q2);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "z");
}

TEST_F(Dfa_test, Mandatory_core_matcher_falls_back_across_several_borders_at_once)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};
    const auto hub{dfa.next_state()};
    const auto n1{dfa.next_state()};
    const auto n2{dfa.next_state()};
    const auto n3{dfa.next_state()};
    const auto m4{dfa.next_state()};
    const auto m5{dfa.next_state()};
    const auto m6{dfa.next_state()};
    const auto killer{dfa.next_state()};

    const Token token{1};

    // Death spells "abab" or "abaabab" from the hub, so the proposal is "abab" and the longer route carries
    // it only because its tail overlaps its own middle: on "abaa" the matcher must fall from three matched
    // through one to zero and back up, two borders in a single byte. A matcher that takes only one fallback
    // hop loses the occurrence, treats the longer route as core-free, and shrinks the answer to "bab".
    dfa.add_accept_state(hub, token);

    dfa.add_transition(q0, dfa::Label('s'), hub);

    const auto loop_except{[&dfa, &hub](const auto state, const std::initializer_list<char> taken) {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            auto skip{false};

            for (const auto held : taken)
            {
                skip = skip || byte == held;
            }

            if (!skip)
            {
                dfa.add_transition(state, dfa::Label(byte), hub);
            }
        }
    }};

    loop_except(hub, {'a'});
    loop_except(n1, {'b'});
    loop_except(n2, {'a'});
    loop_except(n3, {'b', 'a'});
    loop_except(m4, {'b'});
    loop_except(m5, {'a'});
    loop_except(m6, {'b'});

    dfa.add_transition(hub, dfa::Label('a'), n1);
    dfa.add_transition(n1, dfa::Label('b'), n2);
    dfa.add_transition(n2, dfa::Label('a'), n3);
    dfa.add_transition(n3, dfa::Label('b'), killer);
    dfa.add_transition(n3, dfa::Label('a'), m4);
    dfa.add_transition(m4, dfa::Label('b'), m5);
    dfa.add_transition(m5, dfa::Label('a'), m6);
    dfa.add_transition(m6, dfa::Label('b'), killer);

    dfa.add_transition(killer, dfa::Label('c'), hub);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "abab");
}

TEST_F(Dfa_test, Mandatory_core_failure_links_chain_while_the_table_is_built)
{
    dfa::Builder dfa;

    const auto q0{dfa.init_state()};

    std::array<dfa::Dfa::State_t, 7> chain{};

    for (auto& state : chain)
    {
        state = dfa.next_state();
    }

    const Token token{1};

    // The KMP prefix automaton of "aabaaaa", laid out as live states: every 'a'/'b' edge follows the
    // matcher, every other byte restarts. The proposal is the full word, and the death route that reads
    // "aabaaab" mid-way still carries it, but only a failure table that inherits progress through the
    // word's border knows that; a table restarting from nothing at each mismatch misjudges the route as
    // core-free and shrinks the answer by its first byte. The construction here never needs more than one
    // fallback hop; the chained multi-hop case is the "aaabb" test's job.
    dfa.add_accept_state(chain[0], token);

    const auto pair_edges{[&dfa](const auto from, const auto on_a, const auto on_b) {
        dfa.add_transition(from, dfa::Label('a'), on_a);
        dfa.add_transition(from, dfa::Label('b'), on_b);
    }};

    pair_edges(q0, chain[0], q0);
    pair_edges(chain[0], chain[1], q0);
    pair_edges(chain[1], chain[1], chain[2]);
    pair_edges(chain[2], chain[3], q0);
    pair_edges(chain[3], chain[4], q0);
    pair_edges(chain[4], chain[5], chain[2]);
    pair_edges(chain[5], chain[6], chain[2]);

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'b')
        {
            dfa.add_transition(q0, dfa::Label(byte), q0);

            for (const auto state : chain)
            {
                dfa.add_transition(state, dfa::Label(byte), q0);
            }
        }
    }

    dfa.add_transition(chain[6], dfa::Label('x'), q0);

    const Simulator simulator{dfa.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aabaaaa");
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

    auto consumed{simulator.run_all(
            run_with_b.cbegin(), run_with_b.cend(),
            [&tokens](const Token& token, const std::size_t length, std::uint64_t) {
                tokens.emplace_back(token.id(), length);
            })};

    EXPECT_EQ(consumed, run_with_b.size());
    ASSERT_EQ(tokens.size(), 1U);
    EXPECT_EQ(tokens.front(), (std::pair<std::size_t, std::size_t>{token_ab.id(), 41U}));

    // Without the 'b', the longest match fails and every rescan must fall back to the length-one token, so the
    // accept position recorded before the run must survive the walk through it.
    const std::string run_without_b(40, 'a');

    tokens.clear();

    consumed = simulator.run_all(
            run_without_b.cbegin(), run_without_b.cend(),
            [&tokens](const Token& token, const std::size_t length, std::uint64_t) {
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

        const auto consumed{simulator.run_all(
                input.cbegin(), input.cend(),
                [&lengths](const Token&, const std::size_t matched, std::uint64_t) { lengths.push_back(matched); })};

        EXPECT_EQ(consumed, length);
        ASSERT_EQ(lengths.size(), 1U);
        EXPECT_EQ(lengths.front(), length);
    }
}

TEST_F(Dfa_test, Oversized_state_identifier_throws_before_the_count_wraps)
{
    // A hand-built DFA may number states sparsely; the largest possible identifier used to wrap the state count to
    // zero and slip past the size guard into out-of-bounds table writes instead of the promised exception.
    const Dfa dfa{std::numeric_limits<std::size_t>::max(), {}, {}};

    EXPECT_THROW((Simulator{dfa}), std::runtime_error);
}

TEST_F(Dfa_test, Graphviz_accepts_a_bare_filename)
{
    // A bare filename has an empty parent path, and directory creation used to be attempted on it and fail;
    // only a stated directory may be created.
    Builder builder;

    builder.add_transition(builder.init_state(), dfa::Label('a'), 1);
    builder.add_accept_state(1, dfa::Token{1});

    const std::filesystem::path bare{"graphviz_bare_dfa_test.dot"};

    Graphviz::to_file(builder.build(), bare);

    EXPECT_TRUE(std::filesystem::exists(bare));

    std::filesystem::remove(bare);
}

TEST_F(Dfa_test, Table_size_overflow_arithmetic_is_pinned_on_every_platform)
{
    // The constructor's product guard is unreachable on a 64-bit std::size_t, where 32-bit states times at most
    // 256 classes cannot wrap; the arithmetic is pinned here against a 32-bit-sized limit instead, so the guard
    // the 32-bit platform relies on cannot rot unnoticed on the platforms that test it.
    constexpr std::size_t limit_32{4294967295U};

    static_assert(table_size_overflows(16777216U, 256U, limit_32));
    static_assert(table_size_overflows(limit_32, 2U, limit_32));
    static_assert(!table_size_overflows(16777215U, 256U, limit_32));
    static_assert(!table_size_overflows(1024U, 256U, limit_32));

    // Zero classes means zero entries, never an overflow and never a division.
    static_assert(!table_size_overflows(limit_32, 0U, limit_32));

    // A product one past the platform limit distinguishes the checked division from an unchecked
    // multiplication, which would wrap to zero here and answer false on every width.
    static_assert(table_size_overflows(
            std::numeric_limits<std::size_t>::max() / 2 + 1, 2U, std::numeric_limits<std::size_t>::max()));

    // On a 64-bit limit the maximal state count with every class distinct stays far from the edge; on a
    // 32-bit one it does not, which is this guard's reason to exist, so the platform claim is width-guarded.
    static_assert(
            sizeof(std::size_t) < 8 || !table_size_overflows(limit_32, 256U, std::numeric_limits<std::size_t>::max()));
    static_assert(table_size_overflows(limit_32, 256U, limit_32));
}

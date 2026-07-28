#include "lexer/dfa/minimize.hpp"

#include <gtest/gtest.h>

#include <ranges>
#include <unordered_set>

#include "lexer/dfa/builder.hpp"
#include "lexer/dfa/simulator.hpp"

using namespace lexer::dfa;

namespace
{
std::size_t count_states(const Dfa& dfa)
{
    std::unordered_set<Dfa::State_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        states.insert(key.first);

        states.insert(to);
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        states.insert(state);
    }

    return states.size();
}

} // namespace

TEST(Minimize_test, Empty_dfa_is_unchanged)
{
    const auto result{minimize(Builder{}.build())};

    EXPECT_EQ(count_states(result), 1);

    EXPECT_TRUE(result.transitions().empty());

    EXPECT_TRUE(result.accept_states().empty());
}

TEST(Minimize_test, Merges_states_accepting_the_same_token)
{
    Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token{1};

    dfa.add_transition(q0, Label{'a'}, q1);
    dfa.add_transition(q0, Label{'b'}, q2);

    dfa.add_accept_state(q1, token);
    dfa.add_accept_state(q2, token);

    const auto result{minimize(dfa.build())};

    EXPECT_EQ(count_states(result), 2);

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("a"), Result_t(token, 1));
    EXPECT_EQ(simulator.run("b"), Result_t(token, 1));

    EXPECT_EQ(simulator.run("c"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run(""), Result_t(std::nullopt, 0));
}

TEST(Minimize_test, Keeps_states_accepting_different_tokens)
{
    Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    dfa.add_transition(q0, Label{'a'}, q1);
    dfa.add_transition(q0, Label{'b'}, q2);

    dfa.add_accept_state(q1, token_a);
    dfa.add_accept_state(q2, token_b);

    const auto result{minimize(dfa.build())};

    EXPECT_EQ(count_states(result), 3);

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("a"), Result_t(token_a, 1));
    EXPECT_EQ(simulator.run("b"), Result_t(token_b, 1));
}

TEST(Minimize_test, Merges_equivalent_interior_states)
{
    // Recognizes "ab" and "cb": the states reached on 'a' and 'c' behave identically and collapse, as do the two
    // accept states, shrinking five states to three.
    Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};
    const auto q4{dfa.next_state()};

    const Token token{1};

    dfa.add_transition(q0, Label{'a'}, q1);
    dfa.add_transition(q0, Label{'c'}, q2);
    dfa.add_transition(q1, Label{'b'}, q3);
    dfa.add_transition(q2, Label{'b'}, q4);

    dfa.add_accept_state(q3, token);
    dfa.add_accept_state(q4, token);

    const auto result{minimize(dfa.build())};

    EXPECT_EQ(count_states(result), 3);

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("ab"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("cb"), Result_t(token, 2));

    EXPECT_EQ(simulator.run("a"), Result_t(std::nullopt, 0));
    EXPECT_EQ(simulator.run("cc"), Result_t(std::nullopt, 0));
}

TEST(Minimize_test, Keeps_states_with_different_symbol_sets)
{
    // The states reached on 'a' and 'c' both accept the token, but only the first consumes a further 'b', so they
    // must not merge. The 'c' state and the "ab" state do merge, leaving three states.
    Builder dfa;

    const auto q0{dfa.init_state()};
    const auto q1{dfa.next_state()};
    const auto q2{dfa.next_state()};
    const auto q3{dfa.next_state()};

    const Token token{1};

    dfa.add_transition(q0, Label{'a'}, q1);
    dfa.add_transition(q0, Label{'c'}, q2);
    dfa.add_transition(q1, Label{'b'}, q3);

    dfa.add_accept_state(q1, token);
    dfa.add_accept_state(q2, token);
    dfa.add_accept_state(q3, token);

    const auto result{minimize(dfa.build())};

    EXPECT_EQ(count_states(result), 3);

    const Simulator simulator{result};

    using Result_t = Simulator::Result_t;

    EXPECT_EQ(simulator.run("ab"), Result_t(token, 2));
    EXPECT_EQ(simulator.run("cb"), Result_t(token, 1));
}

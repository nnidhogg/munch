#include "munch/dfa/minimize.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <ranges>
#include <unordered_set>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/simulator.hpp"

using namespace munch::dfa;

/**
 * @brief A match the DFA simulator reports.
 */
using Match = Simulator::Match;

namespace
{
/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

/**
 * @brief Counts the distinct states a DFA names anywhere.
 * @param dfa The DFA.
 * @return The number of states.
 */
std::size_t count_states(const Dfa& dfa)
{
    std::unordered_set<Dfa::State_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        states.insert(from);

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
    const auto minimized{minimize(Builder{}.build())};

    EXPECT_EQ(count_states(minimized), 1U);

    EXPECT_TRUE(minimized.transitions().empty());

    EXPECT_TRUE(minimized.accept_states().empty());
}

TEST(Minimize_test, Merges_states_accepting_the_same_token)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'b'}, q2);

    builder.add_accept_state(q1, token);
    builder.add_accept_state(q2, token);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 2U);

    const Simulator simulator{minimized};

    EXPECT_EQ(simulator.run("a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("b"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(simulator.run("c"), no_match);
    EXPECT_EQ(simulator.run(""), no_match);
}

TEST(Minimize_test, Keeps_states_accepting_different_tokens)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'b'}, q2);

    builder.add_accept_state(q1, token_a);
    builder.add_accept_state(q2, token_b);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 3U);

    const Simulator simulator{minimized};

    EXPECT_EQ(simulator.run("a"), (Match{.token = token_a, .length = 1}));
    EXPECT_EQ(simulator.run("b"), (Match{.token = token_b, .length = 1}));
}

TEST(Minimize_test, Merges_equivalent_interior_states)
{
    // Recognizes "ab" and "cb": the states reached on 'a' and 'c' behave identically and collapse, as do the two accept
    // states, shrinking five states to three.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};

    const Token token{1};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'c'}, q2);
    builder.add_transition(q1, Label{'b'}, q3);
    builder.add_transition(q2, Label{'b'}, q4);

    builder.add_accept_state(q3, token);
    builder.add_accept_state(q4, token);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 3U);

    const Simulator simulator{minimized};

    EXPECT_EQ(simulator.run("ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("cb"), (Match{.token = token, .length = 2}));

    EXPECT_EQ(simulator.run("a"), no_match);
    EXPECT_EQ(simulator.run("cc"), no_match);
}

TEST(Minimize_test, Keeps_a_reachable_chain_with_empty_right_language)
{
    // One of the contract's two documented non-minimal cases, one per direction of trimness, with the next test. Both
    // pin behaviour the header promises.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_accept_state(q1, token);

    // The dead branch choice(text("a"), concat(text("b"), any_of(Set{}))) compiles to: q2 and q3 both have empty right
    // language, so Myhill-Nerode merges them; refinement over the partial transition function keeps them apart, since
    // only q2 has a 'c' transition, and the scanner needs that to know how far a failing longest match reads.
    builder.add_transition(q0, Label{'b'}, q2);
    builder.add_transition(q2, Label{'c'}, q3);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 4U);

    const Simulator simulator{minimized};

    // The recognized language is exactly "a" regardless, so keeping the chain costs states and not correctness.
    EXPECT_EQ(simulator.run("a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("b"), no_match);
    EXPECT_EQ(simulator.run("bc"), no_match);
}

TEST(Minimize_test, Keeps_an_island_the_initial_state_cannot_reach)
{
    // The other documented non-minimal case, the island the previous test's chain mirrors.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token reachable{1};
    const Token island{2};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_accept_state(q1, reachable);

    // No transition leads into q2, so no input can arrive there. Refinement merges equivalent states but never drops
    // one, so both island states survive; a trim automaton for the same language needs two states, not four.
    builder.add_transition(q2, Label{'b'}, q3);
    builder.add_accept_state(q3, island);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 4U);

    const Simulator simulator{minimized};

    EXPECT_EQ(simulator.run("a"), (Match{.token = reachable, .length = 1}));

    // The island is unreachable, so its token can never be emitted no matter what the automaton still carries.
    EXPECT_EQ(simulator.run("b"), no_match);
}

TEST(Minimize_test, Keeps_states_with_different_symbol_sets)
{
    // The states reached on 'a' and 'c' both accept the token, but only the first consumes a further 'b', so they must
    // not merge. The 'c' state and the "ab" state do merge, leaving three states.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'c'}, q2);
    builder.add_transition(q1, Label{'b'}, q3);

    builder.add_accept_state(q1, token);
    builder.add_accept_state(q2, token);
    builder.add_accept_state(q3, token);

    const auto minimized{minimize(builder.build())};

    EXPECT_EQ(count_states(minimized), 3U);

    const Simulator simulator{minimized};

    EXPECT_EQ(simulator.run("ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("cb"), (Match{.token = token, .length = 1}));
}

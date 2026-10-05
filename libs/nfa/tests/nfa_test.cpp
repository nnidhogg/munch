#include "munch/nfa/nfa.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/nfa/builder.hpp"
#include "munch/nfa/simulator.hpp"

using namespace munch::nfa;

/**
 * @brief A match the NFA simulator reports.
 */
using Match = Simulator::Match;

/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

TEST(Nfa_test, An_empty_nfa_matches_nothing)
{
    const Builder builder{};

    const auto nfa{builder.build()};

    const std::vector<char> input{};

    EXPECT_EQ(Simulator::run(nfa, input), no_match);
}

TEST(Nfa_test, An_accepting_start_matches_the_empty_input)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1, 1};

    builder.add_accept_state(q0, token);

    const auto nfa{builder.build()};

    const std::vector<char> input{};

    EXPECT_EQ(Simulator::run(nfa, input), (Match{.token = token, .length = 0}));
}

TEST(Nfa_test, A_loop_then_a_final_byte_matches_any_run_ending_in_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q2, token);

    builder.add_transition(q0, Label{'a'}, q0);
    builder.add_transition(q0, Label{'b'}, q1);
    builder.add_transition(q1, Label::epsilon(), q2);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aab"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "baa"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
}

TEST(Nfa_test, Run_accepts_a_single_pass_iterator)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1, 1};

    builder.add_transition(q0, Label{'a'}, q0);
    builder.add_transition(q0, Label{'b'}, q1);
    builder.add_transition(q1, Label::epsilon(), q2);

    builder.add_accept_state(q2, token);

    const auto nfa{builder.build()};

    // istreambuf_iterator is genuinely single-pass: once the loop advances, the begin iterator can no longer be used to
    // measure how far it has come. The overload is constrained on input_iterator, so this has to work, and the reported
    // length has to agree with what the same input gives over a string.
    std::istringstream stream{"aab"};

    const std::istreambuf_iterator<char> begin{stream};

    const std::istreambuf_iterator<char> end{};

    EXPECT_EQ(Simulator::run(nfa, begin, end), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, "aab"), (Match{.token = token, .length = 3}));
}

TEST(Nfa_test, A_single_character_matches_one_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
}

TEST(Nfa_test, An_optional_character_matches_empty_or_one_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q0, token);
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 0}));
}

TEST(Nfa_test, A_sequence_matches_only_its_bytes_in_order)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q2, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 2}));

    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
}

TEST(Nfa_test, A_kleene_star_matches_every_run_including_the_empty_one)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label::epsilon(), q1);
    builder.add_transition(q1, Label{'a'}, q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "baa"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), (Match{.token = token, .length = 0}));
}

TEST(Nfa_test, A_branch_matches_either_alternative)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q2, token);
    builder.add_accept_state(q3, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label::epsilon(), q2);
    builder.add_transition(q0, Label{'b'}, q3);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "c"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ca"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "cb"), no_match);
}

TEST(Nfa_test, A_repeated_sequence_matches_every_whole_repetition)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q0, token);
    builder.add_accept_state(q4, token);

    builder.add_transition(q0, Label::epsilon(), q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'b'}, q3);
    builder.add_transition(q3, Label{'c'}, q4);
    builder.add_transition(q4, Label::epsilon(), q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "abca"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "abcabc"), (Match{.token = token, .length = 6}));
    EXPECT_EQ(Simulator::run(nfa, "abcabcabc"), (Match{.token = token, .length = 9}));
}

TEST(Nfa_test, A_prefix_loop_matches_up_to_the_sequence_after_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q2, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q0, Label{'x'}, q0);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "xxab"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(Simulator::run(nfa, "ax"), no_match);
}

TEST(Nfa_test, Two_numeric_branches_match_only_their_complete_sequences)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};
    const auto q5{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q3, token);
    builder.add_accept_state(q5, token);

    builder.add_transition(q0, Label{'1'}, q1);
    builder.add_transition(q1, Label{'2'}, q2);
    builder.add_transition(q2, Label{'3'}, q3);
    builder.add_transition(q0, Label{'4'}, q4);
    builder.add_transition(q4, Label{'5'}, q5);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "45"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "123"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "1234"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, "12"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "124"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "467"), no_match);
}

TEST(Nfa_test, An_epsilon_chain_to_an_accepting_state_matches_the_empty_prefix)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q3, token);

    builder.add_transition(q0, Label::epsilon(), q1);
    builder.add_transition(q1, Label::epsilon(), q2);
    builder.add_transition(q2, Label::epsilon(), q3);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "abc"), (Match{.token = token, .length = 0}));
}

TEST(Nfa_test, A_plus_loop_matches_one_or_more_bytes)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
}

TEST(Nfa_test, Prepend_init_state_preserves_the_language)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1, 1};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_accept_state(q1, token);

    const auto prepended{builder.prepend_init_state()};

    EXPECT_NE(prepended.init_state(), q0);

    const auto nfa{prepended.build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
}

TEST(Nfa_test, Merge_is_a_union_even_when_an_operand_loops_back_to_its_initial_state)
{
    Builder a_star{};

    const auto a0{a_star.init_state()};
    const auto a1{a_star.next_state()};

    const Token token_a{1, 1};
    const Token token_b{2, 1};

    a_star.add_transition(a0, Label{'a'}, a1);
    a_star.add_epsilon_transition(a1, a0);
    a_star.add_accept_state(a0, token_a);
    a_star.add_accept_state(a1, token_a);

    Builder b{};

    const auto b1{b.next_state()};

    b.add_transition(b.init_state(), Label{'b'}, b1);
    b.add_accept_state(b1, token_b);

    const auto nfa{a_star.merge(b).build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token_a, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token_a, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token_b, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token_a, .length = 1}));
}

TEST(Nfa_test, A_token_reports_its_id_and_priority)
{
    const Token token{7, 3};

    EXPECT_EQ(token.id(), 7U);
    EXPECT_EQ(token.priority(), 3U);
}

TEST(Nfa_test, Tokens_are_equal_exactly_when_id_and_priority_are)
{
    const Token token{1, 5};

    EXPECT_EQ(token, (Token{1, 5}));

    // A differing id short-circuits before comparing priority.
    EXPECT_NE(token, (Token{2, 5}));

    // An equal id forces the priority comparison to run.
    EXPECT_NE(token, (Token{1, 6}));
}

TEST(Nfa_test, Token_operator_less_ties_break_on_id)
{
    const Token lower_id{1, 5};
    const Token higher_id{2, 5};

    EXPECT_LT(lower_id, higher_id);
    EXPECT_FALSE(higher_id < lower_id);
}

TEST(Nfa_test, Equal_priority_accept_states_prefer_lower_id)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token higher_id{2, 1};
    const Token lower_id{1, 1};

    builder.add_accept_state(q0, higher_id);
    builder.add_accept_state(q1, lower_id);

    builder.add_transition(q0, Label::epsilon(), q1);

    const auto nfa{builder.build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = lower_id, .length = 0}));
}

TEST(Nfa_test, Merge_all_unions_under_one_initial_state)
{
    std::vector<Builder> alternatives{};

    for (const char symbol : {'a', 'b', 'c'})
    {
        Builder builder{};

        const auto accept{builder.next_state()};

        builder.add_transition(builder.init_state(), Label{symbol}, accept);

        builder.add_accept_state(accept, Token{1, 1});

        alternatives.push_back(std::move(builder));
    }

    const auto nfa{Builder::merge_all(alternatives).build()};

    for (const std::string input : {"a", "b", "c"})
    {
        const auto [token, length]{Simulator::run(nfa, input)};

        EXPECT_TRUE(token.has_value()) << input;
        EXPECT_EQ(length, 1U) << input;
    }

    const auto [token, length]{Simulator::run(nfa, std::string{"d"})};

    EXPECT_FALSE(token.has_value());

    // One key holds the root's three epsilon edges and each alternative adds one symbol key: four entries, with no
    // chain of pairwise union roots.
    EXPECT_EQ(nfa.transitions().size(), 4U);
}

TEST(Nfa_test, Oversized_identifiers_refuse_renumbering_instead_of_wrapping)
{
    // Composition renumbers the right operand past this builder's states; a hand-built operand whose highest identifier
    // is the largest std::size_t has no identifier past it, so every composing operation refuses it rather than
    // changing the language.
    Builder oversized{};

    constexpr auto top{std::numeric_limits<std::size_t>::max()};

    oversized.add_transition(0, Label{'x'}, top);
    oversized.add_accept_state(top, Token{1, 1});

    const Builder left{};

    EXPECT_THROW(std::ignore = left.append(oversized), std::runtime_error);
    EXPECT_THROW(std::ignore = left.merge(oversized), std::runtime_error);

    const std::vector<Builder> alternatives{oversized};

    EXPECT_THROW(std::ignore = Builder::merge_all(alternatives), std::runtime_error);
}

TEST(Nfa_test, Exhausted_allocator_and_near_top_compositions_refuse_instead_of_wrapping)
{
    // A legal composition can park the allocator's cursor at the last identifier, and the next allocation is refused
    // rather than reusing an existing state. The cursor-parked builder comes from valid public operations, so each
    // guard here is reachable without any oversized operand.
    constexpr auto top{std::numeric_limits<std::size_t>::max()};

    const std::vector<Builder> single{Builder{}.offset(top - 2)};

    auto parked{Builder::merge_all(single)};

    EXPECT_THROW(std::ignore = parked.next_state(), std::runtime_error);

    // The fresh-state guards of prepend_init_state() and merge() are their own doors: both mint one state past the
    // cursor, and neither is reachable through offset()'s guard alone.
    const auto at_top{Builder{}.offset(top - 1)};

    EXPECT_THROW(std::ignore = at_top.prepend_init_state(), std::runtime_error);

    const Builder left{};

    const auto near_top{Builder{}.offset(top - 2)};

    EXPECT_THROW(std::ignore = left.merge(near_top), std::runtime_error);
}

TEST(Nfa_test, Reaccepting_a_state_replaces_the_earlier_association)
{
    // Each overload replaces whatever the other associated: the association follows the latest call, matching the DFA
    // builder.
    Builder builder{};

    const auto accept{builder.next_state()};

    builder.add_accept_state(accept);
    builder.add_accept_state(accept, Token{7, 1});

    const auto& association{builder.accept_states().at(accept)};

    ASSERT_TRUE(association.has_value());
    EXPECT_EQ(association->id(), 7U);

    builder.add_accept_state(accept);

    EXPECT_FALSE(builder.accept_states().at(accept).has_value());
}

TEST(Nfa_test, A_label_is_a_symbol_or_epsilon_and_the_two_tests_are_each_others_negation)
{
    // The two kinds the variant holds, and the one symbol value a reader might mistake for a sentinel: a NUL symbol is
    // a symbol like any other.
    EXPECT_TRUE(Label{'a'}.is_symbol());
    EXPECT_FALSE(Label{'a'}.is_epsilon());

    EXPECT_TRUE(Label{'\0'}.is_symbol());
    EXPECT_FALSE(Label{'\0'}.is_epsilon());

    EXPECT_TRUE(Label::epsilon().is_epsilon());
    EXPECT_FALSE(Label::epsilon().is_symbol());
}

TEST(Nfa_test, Epsilon_closure_follows_epsilon_transitions_transitively_and_no_symbol_transition)
{
    // q0 -ε-> q1 -ε-> q2 -a-> q3 -ε-> q4, with q1 also -ε-> q0 so the chain cycles: the closure of q0 is the three
    // states before the symbol, the closure of q3 the two after it, and a set's closure is the union of its members'
    // closures, the given states included whether or not any epsilon leaves them.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};

    builder.add_epsilon_transition(q0, q1);
    builder.add_epsilon_transition(q1, q2);
    builder.add_epsilon_transition(q1, q0);
    builder.add_transition(q2, Label{'a'}, q3);
    builder.add_epsilon_transition(q3, q4);
    builder.add_accept_state(q4, Token{1, 1});

    const auto nfa{builder.build()};

    EXPECT_EQ(nfa.epsilon_closure({q0}), (Nfa::States_t{q0, q1, q2}));
    EXPECT_EQ(nfa.epsilon_closure({q2}), Nfa::States_t{q2});
    EXPECT_EQ(nfa.epsilon_closure({q3}), (Nfa::States_t{q3, q4}));
    EXPECT_EQ(nfa.epsilon_closure({q2, q3}), (Nfa::States_t{q2, q3, q4}));
    EXPECT_EQ(nfa.epsilon_closure({}), Nfa::States_t{});
}

TEST(Nfa_test, Set_accept_states_replaces_the_whole_accept_map_and_returns_the_builder)
{
    // One state accepted before the call is gone after it; the map given is the map kept, a tokenless entry included,
    // and the reference returned is the builder itself, so the next call chains onto the same object.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    builder.add_accept_state(q0, Token{1, 1});

    Builder& chained{builder.set_accept_states({{q1, Token{2, 1}}, {q2, std::nullopt}})};

    EXPECT_EQ(&chained, &builder);

    const Nfa::Accept_states_t expected{{q1, Token{2, 1}}, {q2, std::nullopt}};

    EXPECT_EQ(builder.accept_states(), expected);
}

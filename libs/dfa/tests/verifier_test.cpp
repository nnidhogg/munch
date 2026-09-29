#include "munch/dfa/verifier.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"

using namespace munch::dfa;

namespace
{
/**
 * @brief A boundary marking, one bit per byte saying whether a boundary follows it.
 */
using Marking = std::vector<bool>;

/**
 * @brief The longest input the exhaustive comparisons enumerate.
 */
constexpr std::size_t longest_input{6};

/**
 * @brief Builds the trie DFA of a set of literal tokens, each token numbered by its position.
 * @param tokens The literal tokens, nonempty.
 * @return The DFA.
 */
Dfa literal_dfa(const std::vector<std::string>& tokens)
{
    Builder builder;

    std::unordered_map<Dfa::State_t, std::unordered_map<char, Dfa::State_t>> children;

    for (std::size_t index{}; index < tokens.size(); ++index)
    {
        auto state{builder.init_state()};

        for (const auto byte : tokens[index])
        {
            const auto [entry, inserted]{children[state].try_emplace(byte, 0)};

            if (inserted)
            {
                entry->second = builder.next_state();
                builder.add_transition(state, Label{byte}, entry->second);
            }

            state = entry->second;
        }

        builder.add_accept_state(state, Token{index});
    }

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the token language (ab)*a.
 * @return The DFA.
 */
Dfa ab_star_a_dfa()
{
    Builder builder;

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q0);
    builder.add_accept_state(q1, Token{0});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the token language a(aa)*.
 * @return The DFA.
 */
Dfa a_odd_dfa()
{
    Builder builder;

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q0);
    builder.add_accept_state(q1, Token{0});

    return std::move(builder).build();
}

/**
 * @brief The boundary projection of the maximal-munch scan of an input, by a reference scan over the DFA.
 * @param dfa The DFA of the token set.
 * @param input The input.
 * @return The marking, a boundary after every token but the last, or std::nullopt when the input is not completely
 *         tokenizable.
 */
std::optional<Marking> munch_marking(const Dfa& dfa, const std::string_view input)
{
    Marking marking(input.size(), false);

    std::size_t begin{};

    while (begin < input.size())
    {
        std::optional<std::size_t> longest;

        auto state{std::optional{dfa.init_state()}};

        for (auto end{begin}; end < input.size() && state; ++end)
        {
            state = dfa.advance(*state, input[end]);

            if (state && dfa.has_accept_token(*state))
            {
                longest = end + 1;
            }
        }

        if (!longest)
        {
            return std::nullopt;
        }

        begin = *longest;

        if (begin < input.size())
        {
            marking[begin - 1] = true;
        }
    }

    return marking;
}

/**
 * @brief Whether the verifier accepts an input under a marking.
 * @param verifier The verifier.
 * @param input The input.
 * @param marking The marking, one bit per byte.
 * @return True when the marked string is accepted.
 */
bool is_accepted(const Verifier& verifier, const std::string_view input, const Marking& marking)
{
    auto state{std::optional{verifier.start()}};

    for (std::size_t index{}; index < input.size() && state; ++index)
    {
        const Marked symbol{.byte = static_cast<unsigned char>(input[index]), .boundary_after = marking[index]};

        state = verifier.step(*state, symbol);
    }

    return state && verifier.accepts(*state);
}

/**
 * @brief The markings of an input the verifier accepts, found by enumerating every marking.
 * @param verifier The verifier.
 * @param input The input.
 * @return The accepted markings.
 */
std::set<Marking> accepted_markings(const Verifier& verifier, const std::string_view input)
{
    std::set<Marking> accepted;

    for (std::size_t bits{}; bits < (std::size_t{1} << input.size()); ++bits)
    {
        Marking marking(input.size(), false);

        for (std::size_t index{}; index < input.size(); ++index)
        {
            marking[index] = ((bits >> index) & 1U) != 0;
        }

        if (is_accepted(verifier, input, marking))
        {
            accepted.insert(marking);
        }
    }

    return accepted;
}

/**
 * @brief Every string over an alphabet up to a length, the empty one first.
 * @param alphabet The bytes.
 * @param longest The longest length.
 * @return The strings.
 */
std::vector<std::string> strings_up_to(const std::string_view alphabet, const std::size_t longest)
{
    std::vector<std::string> strings{std::string{}};

    for (std::size_t begin{}; begin < strings.size(); ++begin)
    {
        if (strings[begin].size() == longest)
        {
            continue;
        }

        for (const auto byte : alphabet)
        {
            strings.push_back(strings[begin] + byte);
        }
    }

    return strings;
}

/**
 * @brief Checks that the verifier's accepted markings of every string over {a, b} up to the longest length are the
 *        maximal-munch marking when the string is completely tokenizable and none otherwise.
 * @param dfa The DFA of the token set.
 * @param verifier The verifier of the DFA.
 */
void expect_munch_markings(const Dfa& dfa, const Verifier& verifier)
{
    for (const auto& input : strings_up_to("ab", longest_input))
    {
        const auto expected{munch_marking(dfa, input)};

        const auto accepted{accepted_markings(verifier, input)};

        EXPECT_EQ(accepted, expected ? std::set<Marking>{*expected} : std::set<Marking>{}) << "input " << input;
    }
}

/**
 * @brief Checks that every state is reachable from the start and can reach an accepting state, and that no transition
 *        enters the start.
 * @param verifier The verifier.
 */
void expect_trim(const Verifier& verifier)
{
    std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>> forward;
    std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>> backward;

    for (const auto& [key, to] : verifier.transitions())
    {
        EXPECT_LT(key.first, verifier.state_count());
        EXPECT_LT(to, verifier.state_count());
        EXPECT_NE(to, verifier.start());

        forward[key.first].push_back(to);
        backward[to].push_back(key.first);
    }

    const auto closure{[](auto& edges, std::vector<Verifier::State_t> pending) {
        std::unordered_set<Verifier::State_t> seen{pending.cbegin(), pending.cend()};

        while (!pending.empty())
        {
            const auto state{pending.back()};

            pending.pop_back();

            for (const auto next : edges[state])
            {
                if (seen.insert(next).second)
                {
                    pending.push_back(next);
                }
            }
        }

        return seen;
    }};

    const auto reachable{closure(forward, {verifier.start()})};
    const auto coaccessible{closure(backward, {verifier.accept_states().cbegin(), verifier.accept_states().cend()})};

    EXPECT_EQ(reachable.size(), verifier.state_count());
    EXPECT_EQ(coaccessible.size(), verifier.state_count());
}

/**
 * @brief The marked string of an input under a marking given as the positions a boundary follows.
 * @param input The input.
 * @param boundaries The byte indices a boundary follows.
 * @return The marking.
 */
Marking marking_of(const std::string_view input, const std::vector<std::size_t>& boundaries)
{
    Marking marking(input.size(), false);

    for (const auto index : boundaries)
    {
        marking[index] = true;
    }

    return marking;
}

} // namespace

TEST(Verifier_test, Literal_a_aab_accepts_exactly_the_munch_markings)
{
    const auto dfa{literal_dfa({"a", "aab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aaaa"), std::set<Marking>{marking_of("aaaa", {0, 1, 2})});
    EXPECT_EQ(accepted_markings(verifier, "aaab"), std::set<Marking>{marking_of("aaab", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Literal_a_ab_accepts_exactly_the_munch_markings)
{
    const auto dfa{literal_dfa({"a", "ab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aab"), std::set<Marking>{marking_of("aab", {0})});
    EXPECT_TRUE(accepted_markings(verifier, "b").empty());

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Literal_a_b_ab_refuses_the_marking_that_cuts_the_longer_token)
{
    const auto dfa{literal_dfa({"a", "b", "ab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "ab"), std::set<Marking>{marking_of("ab", {})});
    EXPECT_EQ(accepted_markings(verifier, "ba"), std::set<Marking>{marking_of("ba", {0})});
    EXPECT_EQ(accepted_markings(verifier, "aab"), std::set<Marking>{marking_of("aab", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Ab_star_a_restores_the_start_configuration_and_keeps_the_unconsumed_state_distinct)
{
    const auto dfa{ab_star_a_dfa()};

    const auto verifier{armed_run(dfa)};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    const auto after_a{verifier.step(verifier.start(), a)};

    ASSERT_TRUE(after_a);

    const auto after_ab{verifier.step(*after_a, b)};

    ASSERT_TRUE(after_ab);
    EXPECT_NE(*after_ab, verifier.start());
    EXPECT_TRUE(verifier.accepts(verifier.start()));
    EXPECT_FALSE(verifier.accepts(*after_ab));
    EXPECT_EQ(verifier.step(*after_ab, a), verifier.step(verifier.start(), a));

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, A_odd_accepts_one_marking_of_aa)
{
    const auto dfa{a_odd_dfa()};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aa"), std::set<Marking>{marking_of("aa", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, The_start_accepts_the_empty_input)
{
    for (const auto& dfa : {literal_dfa({"a", "aab"}), literal_dfa({"a", "ab"}), ab_star_a_dfa(), a_odd_dfa()})
    {
        const auto verifier{armed_run(dfa)};

        EXPECT_EQ(verifier.start(), 0U);
        EXPECT_TRUE(verifier.accepts(verifier.start()));
        EXPECT_EQ(accepted_markings(verifier, ""), std::set<Marking>{Marking{}});
    }
}

TEST(Verifier_test, Constructor_redirects_a_transition_into_the_start_to_a_copy_of_it)
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};

    const Verifier verifier{5, {{{5, a}, 6}, {{6, a}, 5}}, {5, 6}};

    EXPECT_EQ(verifier.state_count(), 3U);
    EXPECT_EQ(verifier.step(0, a), std::optional<Verifier::State_t>{1});
    EXPECT_EQ(verifier.step(1, a), std::optional<Verifier::State_t>{2});
    EXPECT_EQ(verifier.step(2, a), std::optional<Verifier::State_t>{1});
    EXPECT_TRUE(verifier.accepts(2));

    for (const auto& to : verifier.transitions() | std::views::values)
    {
        EXPECT_NE(to, verifier.start());
    }
}

TEST(Verifier_test, Constructor_trims_a_caller_table_and_keeps_the_start)
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    const Verifier verifier{7, {{{7, a}, 3}, {{7, b}, 9}, {{11, a}, 3}}, {3, 11}};

    EXPECT_EQ(verifier.start(), 0U);
    EXPECT_EQ(verifier.state_count(), 2U);
    EXPECT_EQ(verifier.step(0, a), std::optional<Verifier::State_t>{1});
    EXPECT_EQ(verifier.step(0, b), std::nullopt);
    EXPECT_TRUE(verifier.accepts(1));
    EXPECT_EQ(verifier.accept_states(), Verifier::Accept_states_t{1});

    const Verifier accepting_start{4, {}, {4}};

    EXPECT_EQ(accepting_start.state_count(), 1U);
    EXPECT_TRUE(accepting_start.accepts(accepting_start.start()));

    const Verifier empty_domain{4, {{{4, a}, 5}}, {}};

    EXPECT_EQ(empty_domain.state_count(), 1U);
    EXPECT_TRUE(empty_domain.transitions().empty());
    EXPECT_FALSE(empty_domain.accepts(empty_domain.start()));
}

TEST(Verifier_test, Armed_run_refuses_an_accepting_initial_state)
{
    Builder builder;

    builder.add_accept_state(builder.init_state(), Token{0});

    EXPECT_THROW(std::ignore = armed_run(builder.build()), std::invalid_argument);
}

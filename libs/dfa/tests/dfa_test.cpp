#include "munch/dfa/dfa.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/dfa/boundary_search.hpp"
#include "munch/dfa/builder.hpp"
#include "munch/dfa/recovery.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/split_window.hpp"
#include "munch/dfa/unroll_start.hpp"

using namespace munch;
using namespace munch::dfa;

/**
 * @brief A match the DFA simulator reports.
 */
using Match = Simulator::Match;

namespace
{
/**
 * @brief The shortest input length per window, gap and whether the gap was cut, over every input inside the bound.
 */
using Shortest_t = std::map<std::tuple<std::string_view, std::size_t, bool>, std::size_t>;

/**
 * @brief The longest input the brute-force oracle of the random-table profile test reads.
 */
constexpr std::size_t oracle_bound{8};

/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

/**
 * @brief Builds the long-window family: one token kind, symbols a, b, r, t and #, and m sources, the j-th a cycle
 *        of j + 1 states accepting at its last, so a window must carry a common accepted length of every cycle.
 * @param m The number of sources.
 * @return The token set.
 */
Simulator lcm_family(const std::size_t m)
{
    Builder builder{};

    const auto root{builder.init_state()};
    const auto error{builder.next_state()};
    const auto guard{builder.next_state()};
    const auto result{builder.next_state()};

    std::vector<Dfa::State_t> timer(m + 1);
    std::vector<Dfa::State_t> header(m + 1);
    std::vector<std::vector<Dfa::State_t>> source(m);

    for (auto& state : timer)
    {
        state = builder.next_state();
    }

    for (auto& state : header)
    {
        state = builder.next_state();
    }

    for (std::size_t j{0}; j < m; ++j)
    {
        source[j].resize(j + 1);

        for (auto& state : source[j])
        {
            state = builder.next_state();
        }
    }

    /**
     * @brief Adds one transition.
     * @param from The state it leaves.
     * @param symbol The byte it reads.
     * @param to The state it enters.
     */
    const auto edge{[&builder](const Dfa::State_t from, const char symbol, const Dfa::State_t to) {
        builder.add_transition(from, Label{symbol}, to);
    }};

    /**
     * @brief Makes a state accept the one kind, as every state but the root does, and wires its countdown letters, a
     *        letter that would end the countdown resetting it.
     * @param state The state.
     * @param on_r The target on r.
     * @param on_t The target on t.
     * @param letters_reset Whether a and b lead to the error state.
     */
    const auto accept_resetting{
            [&](const Dfa::State_t state, const Dfa::State_t on_r, const Dfa::State_t on_t, const bool letters_reset) {
                builder.add_accept_state(state, Token{1});
                edge(state, 'r', on_r);
                edge(state, 't', on_t);

                if (letters_reset)
                {
                    edge(state, 'a', error);
                    edge(state, 'b', error);
                }
            }};

    edge(root, 'r', header[0]);
    edge(root, '#', result);

    for (const auto state : {error, result})
    {
        accept_resetting(state, timer[m], error, true);
        edge(state, '#', error);
    }

    accept_resetting(guard, timer[m], error, false);
    edge(guard, 'a', guard);
    edge(guard, 'b', guard);

    for (std::size_t j{1}; j <= m; ++j)
    {
        accept_resetting(timer[j], timer[j - 1], error, true);
        edge(timer[j], '#', error);
    }

    accept_resetting(timer[0], timer[0], guard, true);
    edge(timer[0], '#', error);

    for (std::size_t j{0}; j < m; ++j)
    {
        accept_resetting(header[j], header[j + 1], source[j][0], true);
        edge(header[j], '#', error);
    }

    accept_resetting(header[m], header[m], guard, true);
    edge(header[m], '#', error);

    for (std::size_t j{0}; j < m; ++j)
    {
        for (std::size_t k{0}; k <= j; ++k)
        {
            const auto next{source[j][(k + 1) % (j + 1)]};

            accept_resetting(source[j][k], timer[m], error, false);
            edge(source[j][k], 'a', next);
            edge(source[j][k], 'b', next);

            // The accepting state alone dies on #.
            if (k != j)
            {
                edge(source[j][k], '#', error);
            }
        }
    }

    return Simulator{builder.build()};
}

/**
 * @brief Returns the token starts of an input under maximal munch, nothing when the scan does not consume every byte.
 * @param simulator The table.
 * @param input The input.
 * @return The starts, ascending, or std::nullopt.
 */
std::optional<std::vector<std::size_t>> token_starts(const Simulator& simulator, const std::string_view input)
{
    std::vector<std::size_t> starts{};

    for (std::size_t at{0}; at < input.size();)
    {
        const auto [token, length]{simulator.run(input.substr(at))};

        if (!token || length == 0)
        {
            return std::nullopt;
        }

        starts.push_back(at);

        at += length;
    }

    return starts;
}

/**
 * @brief Draws a number below a bound from a sequence.
 * @param sequence The sequence, advanced by the draw.
 * @param below The bound.
 * @return The number.
 */
std::size_t draw(std::mt19937& sequence, const std::size_t below)
{
    std::uniform_int_distribution<std::size_t> distribution{0, below - 1};

    return distribution(sequence);
}

/**
 * @brief Lists every word over {a, b} of length one to a bound, shortest first.
 * @param longest The bound.
 * @return The words.
 */
std::vector<std::string> ab_words(const std::size_t longest)
{
    std::vector<std::string> out{};

    std::vector<std::string> layer{""};

    for (std::size_t length{1}; length <= longest; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : layer)
        {
            for (const auto byte : std::string_view{"ab"})
            {
                next.push_back(prefix + byte);
            }
        }

        out.insert(out.end(), next.begin(), next.end());

        layer = std::move(next);
    }

    return out;
}

/**
 * @brief Draws a random table: three to five states, the initial one among them, each byte of {a, b} leading from each
 *        to a random one of them, to a dead sink or nowhere, one or more of them but the initial accepting, and an
 *        island no transition reaches beside them.
 * @param sequence The sequence the table is drawn from.
 * @return The compiled table.
 */
Simulator random_table(std::mt19937& sequence)
{
    Builder builder{};

    std::vector<Dfa::State_t> states{builder.init_state()};

    for (std::size_t added{draw(sequence, 3) + 2}; added > 0; --added)
    {
        states.push_back(builder.next_state());
    }

    const auto sink{builder.next_state()};

    const auto island{builder.next_state()};

    for (const auto from : states)
    {
        for (const auto byte : std::string_view{"ab"})
        {
            // One target beyond the states is the sink, and one more is no transition at all.
            const auto target{draw(sequence, states.size() + 2)};

            if (target < states.size())
            {
                builder.add_transition(from, Label{byte}, states[target]);
            }
            else if (target == states.size())
            {
                builder.add_transition(from, Label{byte}, sink);
            }
        }
    }

    builder.add_transition(sink, Label{'a'}, sink);
    builder.add_transition(sink, Label{'b'}, sink);
    builder.add_transition(island, Label{'a'}, states.front());
    builder.add_transition(island, Label{'b'}, island);
    builder.add_accept_state(island, Token{7});

    const auto accepting{states[draw(sequence, states.size() - 1) + 1]};

    builder.add_accept_state(accepting, Token{1});

    for (std::size_t index{2}; index < states.size(); ++index)
    {
        if (draw(sequence, 3) == 0)
        {
            builder.add_accept_state(states[index], Token{1});
        }
    }

    return Simulator{builder.build()};
}

/**
 * @brief Returns whether a witness is what a refutation claims: a completely tokenizable input holding an occurrence of
 *        the window at which the gap is cut, a token beginning there or the input ending there, or crossed.
 * @param simulator The machine.
 * @param witness The witness.
 * @param window The window.
 * @param gap The gap.
 * @param cut Whether the gap is claimed cut rather than crossed.
 * @return True when it is.
 */
bool shows(
        const Simulator& simulator, const std::string_view witness, const std::string_view window,
        const std::size_t gap, const bool cut)
{
    const auto starts{token_starts(simulator, witness)};

    if (!starts)
    {
        return false;
    }

    for (auto at{witness.find(window)}; at != std::string_view::npos; at = witness.find(window, at + 1))
    {
        const auto is_cut{at + gap == witness.size() || std::ranges::binary_search(*starts, at + gap)};

        if (is_cut == cut)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Notes the input's length at every gap of every occurrence of the window in it, unless a shorter one was seen.
 * @param shortest The records, extended in place.
 * @param window The window.
 * @param input The input.
 * @param starts The token starts of the input's maximal-munch scan.
 */
void note(
        Shortest_t& shortest, const std::string_view window, const std::string_view input,
        const std::vector<std::size_t>& starts)
{
    for (auto at{input.find(window)}; at != std::string_view::npos; at = input.find(window, at + 1))
    {
        for (std::size_t gap{0}; gap <= window.size(); ++gap)
        {
            const auto cut{at + gap == input.size() || std::ranges::binary_search(starts, at + gap)};

            shortest.try_emplace({window, gap, cut}, input.size());
        }
    }
}

/**
 * @brief Records a table by brute force: the shortest input per window, gap and cut, over every given input.
 * @param simulator The table.
 * @param inputs The inputs read.
 * @param windows The windows looked for.
 * @return The records.
 */
Shortest_t oracle(
        const Simulator& simulator, const std::vector<std::string>& inputs, const std::vector<std::string>& windows)
{
    Shortest_t shortest{};

    for (const auto& input : inputs)
    {
        const auto starts{token_starts(simulator, input)};

        if (!starts)
        {
            continue;
        }

        for (const auto& window : windows)
        {
            note(shortest, window, input, *starts);
        }
    }

    return shortest;
}

/**
 * @brief Adds a transition on every byte but the given ones, in byte order.
 * @param builder The DFA being built.
 * @param from The state the transitions leave.
 * @param to The state they enter.
 * @param taken The bytes left out.
 */
void add_every_byte_but(
        Builder& builder, const Dfa::State_t from, const Dfa::State_t to, const std::initializer_list<char> taken)
{
    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (std::ranges::contains(taken, byte))
        {
            continue;
        }

        builder.add_transition(from, Label{byte}, to);
    }
}

/**
 * @brief Builds the six-state shape the flag and liveness tests read: q0 -a-> q1 accepting, q1 -b-> q2 accepting, q0
 *        -c-> q3 which accepts nothing and leads nowhere, and an island q4 -a-> q5 accepting that no input reaches.
 * @return The compiled table, its states q0 to q5 numbered 0 to 5.
 */
Simulator six_state_shape()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};
    const auto q5{builder.next_state()};

    builder.add_accept_state(q1, Token{1});
    builder.add_accept_state(q2, Token{2});
    builder.add_accept_state(q5, Token{3});
    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q0, Label{'c'}, q3);
    builder.add_transition(q4, Label{'a'}, q5);

    return Simulator{builder.build()};
}

} // namespace

TEST(Dfa_test, Empty_dfa_matches_nothing)
{
    Builder builder{};

    const Simulator simulator{builder.build()};

    const std::vector<char> input{};

    EXPECT_EQ(simulator.run(input), no_match);
}

TEST(Dfa_test, A_string_container_runs_empty_and_non_empty)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);
    builder.add_transition(q0, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run(std::string{}), no_match);
    EXPECT_EQ(simulator.run(std::string{"a"}), (Match{.token = token, .length = 1}));
}

TEST(Dfa_test, A_vector_container_runs_like_a_string)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);
    builder.add_transition(q0, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    const std::vector<char> input{'a'};

    EXPECT_EQ(simulator.run(input), (Match{.token = token, .length = 1}));
}

TEST(Dfa_test, A_self_loop_matches_a_long_run_to_its_end)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);
    builder.add_transition(q0, Label{'a'}, q0);

    const Simulator simulator{builder.build()};

    const std::string run(5000, 'a');

    EXPECT_EQ(simulator.run(run), (Match{.token = token, .length = run.size()}));
    EXPECT_EQ(simulator.run(run + "b"), (Match{.token = token, .length = run.size()}));
    EXPECT_EQ(simulator.run("b" + run), (Match{.token = token, .length = 0}));
}

TEST(Dfa_test, A_self_loop_matches_symbols_across_the_whole_byte_range)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);

    // Symbols across the full byte range, including values that are negative as plain char.
    for (const char symbol : {'\x3F', '\x40', '\x7F', '\x80', '\xBF', '\xC0', '\xFF'})
    {
        builder.add_transition(q0, Label{symbol}, q0);
    }

    const Simulator simulator{builder.build()};

    const std::string input{"\x3F\x40\x7F\x80\xBF\xC0\xFF"};

    EXPECT_EQ(simulator.run(input), (Match{.token = token, .length = 7}));
}

TEST(Dfa_test, An_accepting_start_matches_the_empty_input)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);

    const Simulator simulator{builder.build()};

    const std::vector<char> input{};

    EXPECT_EQ(simulator.run(input), (Match{.token = token, .length = 0}));
}

TEST(Dfa_test, Unrolling_an_accepting_start_keeps_every_scan_and_the_empty_match)
{
    // a* as one token: the start accepts and loops on a. Unrolled, a fresh start carries the loop's entry and does not
    // accept, the old start stays behind it as the loop, and nothing enters the fresh one; the simulator compiles the
    // set that way, so the empty match must still come back where the scan reports one.
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);
    builder.add_transition(q0, Label{'a'}, q0);

    const auto built{builder.build()};

    const auto unrolled{unroll_start(built)};

    EXPECT_NE(unrolled.init_state(), built.init_state());
    EXPECT_FALSE(unrolled.has_accept_token(unrolled.init_state()).has_value());
    EXPECT_EQ(unrolled.advance(unrolled.init_state(), 'a'), std::optional{built.init_state()});
    EXPECT_EQ(unrolled.advance(built.init_state(), 'a'), std::optional{built.init_state()});
    EXPECT_EQ(unrolled.transitions().size(), built.transitions().size() + 1);

    for (const auto& to : unrolled.transitions() | std::views::values)
    {
        EXPECT_NE(to, unrolled.init_state());
    }

    const Simulator simulator{built};

    EXPECT_TRUE(simulator.nullable());
    EXPECT_EQ(simulator.run(std::string{}), (Match{.token = token, .length = 0}));
    EXPECT_EQ(simulator.run(std::string{"aaa"}), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run(std::string{"b"}), (Match{.token = token, .length = 0}));
    EXPECT_FALSE(simulator.is_split_point('a'));

    // A start that does not accept is returned as it is.
    Builder plain{};

    const auto p0{plain.init_state()};
    const auto p1{plain.next_state()};

    plain.add_accept_state(p1, token);
    plain.add_transition(p0, Label{'a'}, p1);

    const auto same{unroll_start(plain.build())};

    EXPECT_EQ(same.init_state(), p0);
    EXPECT_EQ(same.transitions().size(), 1U);

    // So is a start that does not accept but is returned to, [a]*b looping on a: what is never re-entered is the fresh
    // start alone, and the compiled start's re-entrancy is the Simulator's to report and every decision's to allow for.
    Builder looping{};

    const auto r0{looping.init_state()};
    const auto r1{looping.next_state()};

    looping.add_accept_state(r1, token);
    looping.add_transition(r0, Label{'a'}, r0);
    looping.add_transition(r0, Label{'b'}, r1);

    const auto looped{looping.build()};

    const auto kept{unroll_start(looped)};

    EXPECT_EQ(kept.init_state(), r0);
    EXPECT_EQ(kept.advance(kept.init_state(), 'a'), std::optional{r0});
    EXPECT_EQ(kept.transitions().size(), 2U);

    const Simulator compiled{looped};

    EXPECT_FALSE(compiled.nullable());
    EXPECT_TRUE(compiled.init_reentrant());
    EXPECT_FALSE(compiled.is_split_point('a'));
    EXPECT_FALSE(compiled.is_split_point('b'));
}

TEST(Dfa_test, Dense_numbering_makes_the_state_count_the_identifier_unrolling_enters_through)
{
    // A builder numbers states densely from zero, so a DFA's state count is both how many states it has and the first
    // identifier none of them uses. Unrolling an accepting start is entitled to that identifier for its fresh start,
    // and the unrolled automaton spans one state more.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);
    builder.add_accept_state(q2, token);
    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);

    const auto built{builder.build()};

    /**
     * @brief Returns the identifiers a definition names anywhere: its initial state, both ends of every transition, and
     *        every accept state.
     * @param automaton The DFA.
     * @return The identifiers.
     */
    const auto named{[](const Dfa& automaton) {
        std::set<Dfa::State_t> states{automaton.init_state()};

        for (const auto& [key, to] : automaton.transitions())
        {
            const auto& [from, label]{key};

            states.insert(from);

            states.insert(to);
        }

        for (const auto& state : automaton.accept_states() | std::views::keys)
        {
            states.insert(state);
        }

        return states;
    }};

    const auto states{named(built)};

    ASSERT_FALSE(states.empty());

    EXPECT_EQ(built.state_count(), states.size());
    EXPECT_EQ(built.state_count(), *states.rbegin() + 1);

    const auto unrolled{unroll_start(built)};

    EXPECT_EQ(unrolled.init_state(), built.state_count());
    EXPECT_FALSE(states.contains(unrolled.init_state()));
    EXPECT_EQ(unrolled.state_count(), built.state_count() + 1);
    EXPECT_EQ(named(unrolled).size(), states.size() + 1);
}

TEST(Dfa_test, A_loop_then_a_final_byte_matches_any_run_ending_in_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q0);
    builder.add_transition(q0, Label{'b'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("b"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("ba"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("aab"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run("baa"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("aaab"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(simulator.run("baaa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(simulator.run("a"), no_match);
    EXPECT_EQ(simulator.run("aa"), no_match);
    EXPECT_EQ(simulator.run("aaa"), no_match);
    EXPECT_EQ(simulator.run("aaaa"), no_match);
}

TEST(Dfa_test, A_single_character_matches_one_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("aa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(simulator.run(""), no_match);
    EXPECT_EQ(simulator.run("b"), no_match);
}

TEST(Dfa_test, An_optional_character_matches_empty_or_one_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token_empty{1};
    const Token token_a{2};

    builder.add_accept_state(q0, token_empty);
    builder.add_accept_state(q1, token_a);

    builder.add_transition(q0, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run(""), (Match{.token = token_empty, .length = 0}));
    EXPECT_EQ(simulator.run("a"), (Match{.token = token_a, .length = 1}));
    EXPECT_EQ(simulator.run("aa"), (Match{.token = token_a, .length = 1}));

    EXPECT_EQ(simulator.run("b"), (Match{.token = token_empty, .length = 0}));
    EXPECT_EQ(simulator.run("ba"), (Match{.token = token_empty, .length = 0}));
}

TEST(Dfa_test, A_sequence_matches_only_its_bytes_in_order)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q2, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("abc"), (Match{.token = token, .length = 2}));

    EXPECT_EQ(simulator.run("a"), no_match);
    EXPECT_EQ(simulator.run("b"), no_match);
}

TEST(Dfa_test, A_kleene_star_matches_every_run_including_the_empty_one)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    builder.add_accept_state(q0, token);

    builder.add_transition(q0, Label{'a'}, q0);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run(""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(simulator.run("a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run("aaab"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(simulator.run("b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(simulator.run("ba"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(simulator.run("baa"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(simulator.run("baaa"), (Match{.token = token, .length = 0}));
}

TEST(Dfa_test, A_branch_matches_either_alternative_with_its_token)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    builder.add_accept_state(q1, token_a);
    builder.add_accept_state(q2, token_b);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'b'}, q2);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("a"), (Match{.token = token_a, .length = 1}));
    EXPECT_EQ(simulator.run("b"), (Match{.token = token_b, .length = 1}));
    EXPECT_EQ(simulator.run("ab"), (Match{.token = token_a, .length = 1}));
    EXPECT_EQ(simulator.run("aa"), (Match{.token = token_a, .length = 1}));

    EXPECT_EQ(simulator.run(""), no_match);
    EXPECT_EQ(simulator.run("c"), no_match);
    EXPECT_EQ(simulator.run("ca"), no_match);
    EXPECT_EQ(simulator.run("cb"), no_match);
}

TEST(Dfa_test, A_repeated_sequence_matches_every_whole_repetition)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q3, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q2, Label{'c'}, q3);
    builder.add_transition(q3, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("abc"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run("abca"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run("abcabc"), (Match{.token = token, .length = 6}));
    EXPECT_EQ(simulator.run("abcabcabc"), (Match{.token = token, .length = 9}));

    EXPECT_EQ(simulator.run(""), no_match);
    EXPECT_EQ(simulator.run("a"), no_match);
    EXPECT_EQ(simulator.run("ab"), no_match);
}

TEST(Dfa_test, A_prefix_loop_matches_up_to_the_sequence_after_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q2, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q0, Label{'x'}, q0);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("xxab"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(simulator.run("ax"), no_match);
}

TEST(Dfa_test, Two_numeric_branches_match_only_their_complete_sequences)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};
    const auto q5{builder.next_state()};

    const Token token_123{1};
    const Token token_45{2};

    builder.add_accept_state(q3, token_123);
    builder.add_accept_state(q5, token_45);

    builder.add_transition(q0, Label{'1'}, q1);
    builder.add_transition(q1, Label{'2'}, q2);
    builder.add_transition(q2, Label{'3'}, q3);
    builder.add_transition(q0, Label{'4'}, q4);
    builder.add_transition(q4, Label{'5'}, q5);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("45"), (Match{.token = token_45, .length = 2}));
    EXPECT_EQ(simulator.run("123"), (Match{.token = token_123, .length = 3}));
    EXPECT_EQ(simulator.run("1234"), (Match{.token = token_123, .length = 3}));

    EXPECT_EQ(simulator.run("12"), no_match);
    EXPECT_EQ(simulator.run("124"), no_match);
    EXPECT_EQ(simulator.run("467"), no_match);
}

TEST(Dfa_test, A_plus_loop_matches_one_or_more_bytes)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token{1};

    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.run("a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(simulator.run("aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(simulator.run("aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(simulator.run("aaaa"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(simulator.run(""), no_match);
    EXPECT_EQ(simulator.run("b"), no_match);
}

TEST(Dfa_test, Split_points_are_the_bytes_only_the_initial_state_consumes)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    // 'a' continues its own run (q1 loops), so it is not a split point; 'b' is consumed only from the initial state, so
    // it can only begin a token; 'c' is consumed nowhere, so it is safe only vacuously and is not reported, since no
    // input this automaton accepts can contain it.
    builder.add_accept_state(q1, token_a);
    builder.add_accept_state(q2, token_b);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q1);
    builder.add_transition(q0, Label{'b'}, q2);

    const Simulator simulator{builder.build()};

    EXPECT_FALSE(simulator.is_split_point('a'));
    EXPECT_TRUE(simulator.is_split_point('b'));
    EXPECT_FALSE(simulator.is_split_point('c'));
    EXPECT_TRUE(simulator.has_split_points());
}

TEST(Dfa_test, Unreachable_states_do_not_decertify)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token_a{1};
    const Token token_b{2};

    builder.add_accept_state(q1, token_a);
    builder.add_accept_state(q3, token_b);

    builder.add_transition(q0, Label{'a'}, q1);

    // A live island no input reaches: q2 accepts by way of q3, so it survives the co-accessibility sweep, and it
    // consumes 'a' mid-token. No scan can ever be in it, so it must not cost 'a' its certificate.
    builder.add_transition(q2, Label{'a'}, q3);

    const Simulator simulator{builder.build()};

    EXPECT_TRUE(simulator.is_split_point('a'));
}

TEST(Dfa_test, Unreachable_transitions_do_not_make_the_initial_state_reentrant)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token_a{1};

    builder.add_accept_state(q1, token_a);

    builder.add_transition(q0, Label{'a'}, q1);

    // q2 is unreachable, so its transition back into the initial state is not a way for a scan to return there and must
    // not defeat the "can only begin a token" reasoning that certifies 'a'.
    builder.add_transition(q2, Label{'b'}, q0);

    const Simulator simulator{builder.build()};

    EXPECT_TRUE(simulator.is_split_point('a'));
}

TEST(Dfa_test, Mandatory_core_is_proved_when_every_death_word_carries_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    // The confirming shape: q1 consumes every byte, and the only route out of its loop is 'a' into q2, whose sole live
    // byte returns. Every word that kills a scan sitting in q1 must therefore spell 'a' strictly before its killing
    // byte, which is exactly the licence the accessor reports.
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'s'}, q1);

    add_every_byte_but(builder, q1, q1, {'a'});

    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'c'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST(Dfa_test, Mandatory_core_stays_empty_when_a_second_route_dies_without_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1};

    // The refuting shape: the same loop now has a second escape, 'z' into q3, so a scan can die by spelling "z" and
    // then a byte q3 refuses, a death word that never contains the 'a' the first route proposes. The proof fails and
    // the accessor stays empty, so no planner filter skips a cut the exhaustive walk finds.
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'s'}, q1);

    add_every_byte_but(builder, q1, q1, {'a', 'z'});

    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'c'}, q1);
    builder.add_transition(q1, Label{'z'}, q3);
    builder.add_transition(q3, Label{'c'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_keeps_the_longest_proved_candidate)
{
    // A chain of forced escapes: the only route from the hub loop to death spells 'x' then 'y' then 'z', and every
    // deviation returns to the loop. The three loop states are all input-total and propose the nested cores "xyz", "yz"
    // and "z", every one of which is proved, so the accessor keeps the longest. The same table is wired twice with the
    // proposing states allocated in opposite orders, and both answer the longest proposal, whatever the state order or
    // the proposal order.

    /**
     * @brief Builds the chain of forced escapes, the proposing states allocated in either order.
     * @param reversed Whether the proposing states are allocated in the opposite order.
     * @return The compiled table.
     */
    const auto build{[](const bool reversed) {
        Builder builder{};

        const auto q0{builder.init_state()};

        const auto first{builder.next_state()};
        const auto second{builder.next_state()};
        const auto third{builder.next_state()};

        const auto hub{reversed ? third : first};
        const auto middle{second};
        const auto low{reversed ? first : third};

        const auto killer{builder.next_state()};

        const Token token{1};

        builder.add_accept_state(hub, token);

        builder.add_transition(q0, Label{'s'}, hub);

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'x')
            {
                builder.add_transition(hub, Label{byte}, hub);
            }

            if (byte != 'y')
            {
                builder.add_transition(middle, Label{byte}, hub);
            }

            if (byte != 'z')
            {
                builder.add_transition(low, Label{byte}, hub);
            }
        }

        builder.add_transition(hub, Label{'x'}, middle);
        builder.add_transition(middle, Label{'y'}, low);
        builder.add_transition(low, Label{'z'}, killer);
        builder.add_transition(killer, Label{'c'}, hub);

        return Simulator{builder.build()};
    }};

    EXPECT_EQ(build(false).mandatory_core(), "xyz");

    EXPECT_EQ(build(true).mandatory_core(), "xyz");
}

TEST(Dfa_test, Mandatory_core_breaks_equal_length_ties_in_state_order)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    // Twenty disconnected loops, each proving its own one-byte core within its own reach: every proposal is sound, so
    // the answer is a many-way tie the accessor breaks toward the earliest state. The published value is part of the
    // behavior the planner tests pin against, and the test pins it: the earliest state's core among twenty equal
    // candidates.
    constexpr std::size_t regions{20};

    for (std::size_t region{0}; region < regions; ++region)
    {
        const auto hub{builder.next_state()};
        const auto killer{builder.next_state()};

        const auto escape{static_cast<char>('a' + region)};

        builder.add_accept_state(hub, token);

        builder.add_transition(q0, Label{static_cast<char>('A' + region)}, hub);

        add_every_byte_but(builder, hub, hub, {escape});

        builder.add_transition(hub, Label{escape}, killer);
        builder.add_transition(killer, Label{'c'}, hub);
    }

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST(Dfa_test, Mandatory_core_origin_stamps_do_not_leak_across_proofs)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto shorter{builder.next_state()};
    const auto longer{builder.next_state()};
    const auto immortal{builder.next_state()};
    const auto killer{builder.next_state()};

    const Token token{1};

    // The state indices are chosen so the first proof's origin cell and the second proof's refuting pair land on the
    // same physical slot: the two-byte proposal's origin occupies index four, and the one-byte proposal visits the
    // killer at prefix zero, which is index four again under its stride. The second proof visits that slot, finds the
    // refutation, and the answer is empty.
    builder.add_accept_state(immortal, token);

    builder.add_transition(q0, Label{'x'}, longer);
    builder.add_transition(q0, Label{'y'}, shorter);
    builder.add_transition(q0, Label{'v'}, immortal);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        builder.add_transition(immortal, Label{byte}, immortal);

        if (byte != 'a' && byte != 'b')
        {
            builder.add_transition(shorter, Label{byte}, immortal);
        }

        if (byte != 'a')
        {
            builder.add_transition(longer, Label{byte}, immortal);
        }
    }

    builder.add_transition(shorter, Label{'a'}, killer);
    builder.add_transition(shorter, Label{'b'}, killer);
    builder.add_transition(longer, Label{'a'}, shorter);
    builder.add_transition(killer, Label{'c'}, immortal);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_ignores_escapes_into_states_that_never_accept_again)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto hub{builder.next_state()};
    const auto killer{builder.next_state()};
    const auto dead{builder.next_state()};

    const Token token{1};

    // The hub's 'z' escape leads to a loop from which acceptance is unreachable, and only live targets count toward
    // input-totality. The hub is therefore not input-total, the 'a' whose only counterweight is that escape is not
    // proposed, and the accessor stays empty.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'z')
        {
            builder.add_transition(hub, Label{byte}, hub);
        }

        builder.add_transition(dead, Label{byte}, dead);
    }

    builder.add_transition(hub, Label{'a'}, killer);
    builder.add_transition(hub, Label{'z'}, dead);
    builder.add_transition(killer, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_reaches_a_proposer_allocated_last)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto killer{builder.next_state()};
    const auto hub{builder.next_state()};

    const Token token{1};

    // The only proposing state carries the highest index on purpose: every derivation pass (predecessor construction,
    // canonical links, candidate collection) includes the final state, so this proposer's core is proved.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    add_every_byte_but(builder, hub, hub, {'a'});

    builder.add_transition(hub, Label{'a'}, killer);
    builder.add_transition(killer, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "a");
}

TEST(Dfa_test, Mandatory_core_origin_stamp_uses_the_current_proofs_stride)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto shorter{builder.next_state()};
    const auto killer{builder.next_state()};
    const auto longer{builder.next_state()};
    const auto immortal{builder.next_state()};

    const Token token{1};

    // The allocation puts the one-byte proposer at index one and its killer at index two, so the origin seed's stride,
    // the current proof's length rather than the longest, decides which cell the second proof visits to refute. The
    // two-byte proposal refutes first, the one-byte one refutes through the killer, and the answer is empty.
    builder.add_accept_state(shorter, token);
    builder.add_accept_state(longer, token);
    builder.add_accept_state(immortal, token);

    builder.add_transition(q0, Label{'x'}, shorter);
    builder.add_transition(q0, Label{'y'}, longer);
    builder.add_transition(q0, Label{'v'}, immortal);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        builder.add_transition(immortal, Label{byte}, immortal);

        if (byte != 'a' && byte != 'b')
        {
            builder.add_transition(shorter, Label{byte}, immortal);
        }

        if (byte != 'd')
        {
            builder.add_transition(longer, Label{byte}, immortal);
        }
    }

    builder.add_transition(shorter, Label{'a'}, killer);
    builder.add_transition(shorter, Label{'b'}, killer);
    builder.add_transition(longer, Label{'d'}, shorter);
    builder.add_transition(killer, Label{'c'}, immortal);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_generations_survive_more_proofs_than_a_byte_can_count)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    // Two hundred and fifty-six regions, each proposing a core its own second escape refutes, so two hundred and
    // fifty-six proofs run and every one carries a distinct stamp. The last proof reads every untouched cell as unseen,
    // finds its refutation, and the answer is empty.
    for (std::size_t region{0}; region < Simulator::symbol_count; ++region)
    {
        const auto hub{builder.next_state()};
        const auto killer{builder.next_state()};

        builder.add_accept_state(hub, token);

        builder.add_transition(q0, Label{static_cast<char>(region)}, hub);

        add_every_byte_but(builder, hub, hub, {'a', 'b'});

        builder.add_transition(hub, Label{'a'}, killer);
        builder.add_transition(hub, Label{'b'}, killer);
        builder.add_transition(killer, Label{'c'}, hub);
    }

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_matcher_rows_start_fresh_for_every_candidate)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    // Three loops proposing equal-length one-byte cores in allocation order: the first is refuted along its 0x81 route,
    // the second along its 0x80 route, and the third proves. The matcher table is rebuilt from zero for every
    // candidate, so the first proof's row-zero entry for byte 0x80 is gone when the second is refuted, and the third
    // core is the answer.

    /**
     * @brief Adds one loop region: a hub looping on every byte but its escape and refuter, both leading to a killer.
     * @param escape The byte the hub escapes on.
     * @param refuter A second byte leading to the killer, or nothing.
     * @return The hub.
     */
    const auto region{[&builder, &token](const char escape, const std::optional<char> refuter) {
        const auto hub{builder.next_state()};
        const auto killer{builder.next_state()};

        builder.add_accept_state(hub, token);

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != escape && (!refuter || byte != *refuter))
            {
                builder.add_transition(hub, Label{byte}, hub);
            }
        }

        builder.add_transition(hub, Label{escape}, killer);

        if (refuter)
        {
            builder.add_transition(hub, Label{*refuter}, killer);
        }

        builder.add_transition(killer, Label{'c'}, hub);

        return hub;
    }};

    const auto first_hub{region(static_cast<char>(0x80), static_cast<char>(0x81))};

    builder.add_transition(q0, Label{'x'}, first_hub);

    const auto second_hub{region(static_cast<char>(0x01), static_cast<char>(0x80))};

    builder.add_transition(q0, Label{'y'}, second_hub);

    const auto third_hub{region(static_cast<char>(0x02), std::nullopt)};

    builder.add_transition(q0, Label{'z'}, third_hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string{static_cast<char>(0x02)});
}

TEST(Dfa_test, Mandatory_core_survives_a_core_longer_than_the_byte_range)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    const Token token{1};

    constexpr std::size_t core_length{256};

    std::vector<Dfa::State_t> chain{};

    for (std::size_t at{0}; at <= core_length; ++at)
    {
        chain.push_back(builder.next_state());
    }

    // A 256-byte core: every matcher-table entry up to 256 holds its prefix count intact. The chain forces the full run
    // before the only death, and the accessor reports all 256 bytes.
    builder.add_accept_state(chain[0], token);

    builder.add_transition(q0, Label{'s'}, chain[0]);

    for (std::size_t at{0}; at < core_length; ++at)
    {
        add_every_byte_but(builder, chain[at], chain[0], {'a'});

        builder.add_transition(chain[at], Label{'a'}, chain[at + 1]);
    }

    builder.add_transition(chain[core_length], Label{'c'}, chain[0]);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string(core_length, 'a'));
}

TEST(Dfa_test, Mandatory_core_failure_table_chains_two_borders_in_construction)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto hub{builder.next_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};
    const auto r5{builder.next_state()};
    const auto r6{builder.next_state()};
    const auto r7{builder.next_state()};
    const auto killer{builder.next_state()};

    const Token token{1};

    // The hub's proposal "aaabb" is refuted only by the alternate route spelling "aaabaabb", and telling those apart
    // hinges on the failure table built for the proposal itself: at its fourth position the construction chains through
    // two borders to land on zero, so the alternate route does not carry the proposal. The answer is the next proposal
    // down, "aabb", which both routes genuinely contain.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    add_every_byte_but(builder, hub, hub, {'a'});
    add_every_byte_but(builder, q1, hub, {'a'});
    add_every_byte_but(builder, q2, hub, {'a'});
    add_every_byte_but(builder, q3, hub, {'b'});
    add_every_byte_but(builder, q4, hub, {'b', 'a'});
    add_every_byte_but(builder, r5, hub, {'a'});
    add_every_byte_but(builder, r6, hub, {'b'});
    add_every_byte_but(builder, r7, hub, {'b'});

    builder.add_transition(hub, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'a'}, q3);
    builder.add_transition(q3, Label{'b'}, q4);
    builder.add_transition(q4, Label{'b'}, killer);
    builder.add_transition(q4, Label{'a'}, r5);
    builder.add_transition(r5, Label{'a'}, r6);
    builder.add_transition(r6, Label{'b'}, r7);
    builder.add_transition(r7, Label{'b'}, killer);
    builder.add_transition(killer, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aabb");
}

TEST(Dfa_test, Mandatory_core_seeding_reads_both_ends_of_the_alphabet)
{
    // The killer consumes every byte except one, placed at either end of the alphabet, so its death is visible only to
    // a seeding pass covering the full symbol range from zero to the last byte; both placements prove the core the
    // hub's escape carries.

    /**
     * @brief Builds the table with the killer missing one byte.
     * @param missing The byte the killer does not consume.
     * @return The compiled table.
     */
    const auto build{[](const std::size_t missing) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto hub{builder.next_state()};
        const auto killer{builder.next_state()};

        const Token token{1};

        builder.add_accept_state(hub, token);

        builder.add_transition(q0, Label{'s'}, hub);

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a')
            {
                builder.add_transition(hub, Label{byte}, hub);
            }

            if (symbol != missing)
            {
                builder.add_transition(killer, Label{byte}, hub);
            }
        }

        builder.add_transition(hub, Label{'a'}, killer);

        return Simulator{builder.build()};
    }};

    EXPECT_EQ(build(0x00).mandatory_core(), "a");

    EXPECT_EQ(build(0xFF).mandatory_core(), "a");
}

TEST(Dfa_test, Mandatory_core_reverse_search_reads_the_last_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto hub{builder.next_state()};
    const auto tail{builder.next_state()};

    const Token token{1};

    // The only route from the hub to death rides the alphabet's final byte, so the backward search's predecessor edges
    // are built through the very last symbol; the hub has a depth and the escape proves a core.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    add_every_byte_but(builder, hub, hub, {static_cast<char>(0xFF)});

    builder.add_transition(hub, Label{static_cast<char>(0xFF)}, tail);
    builder.add_transition(tail, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), std::string{static_cast<char>(0xFF)});
}

TEST(Dfa_test, Mandatory_core_canonical_word_starts_at_the_zeroth_byte)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto hub{builder.next_state()};
    const auto middle{builder.next_state()};
    const auto killer{builder.next_state()};

    const Token token{1};

    // The shortest death word leaves the hub on byte zero, so the canonical-word pass choosing the smallest byte one
    // layer shallower scans from the alphabet's start, and spells the two-byte core the route proves.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    for (std::size_t symbol{1}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        builder.add_transition(hub, Label{byte}, hub);

        if (byte != 'a')
        {
            builder.add_transition(middle, Label{byte}, hub);
        }
    }

    builder.add_transition(hub, Label{static_cast<char>(0)}, middle);
    builder.add_transition(middle, Label{static_cast<char>(0)}, hub);
    builder.add_transition(middle, Label{'a'}, killer);
    builder.add_transition(killer, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), (std::string{"\0a", 2}));
}

TEST(Dfa_test, Mandatory_core_proofs_do_not_inherit_the_previous_searchs_footprint)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto slow{builder.next_state()};
    const auto fast{builder.next_state()};
    const auto immortal{builder.next_state()};

    const Token token{1};

    // Two candidates of different lengths both refute through the initial state's dead bytes, and because the initial
    // state's index is zero, the pair it contributes lands in the same buffer cell under both proofs' strides. Each
    // search starts from a clean footprint, so the second revisits the cell, finds the refutation, and the answer is
    // empty.
    builder.add_accept_state(immortal, token);

    builder.add_transition(q0, Label{'s'}, fast);
    builder.add_transition(q0, Label{'t'}, slow);
    builder.add_transition(q0, Label{'v'}, immortal);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        builder.add_transition(immortal, Label{byte}, immortal);

        if (symbol < 'a')
        {
            builder.add_transition(fast, Label{byte}, immortal);
            builder.add_transition(slow, Label{byte}, immortal);
        }
        else
        {
            builder.add_transition(fast, Label{byte}, q0);
            builder.add_transition(slow, Label{byte}, fast);
        }
    }

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_exemption_follows_the_initial_states_reentrancy)
{
    // The same table twice, differing in one edge: where the killer state's surviving byte returns. Both tables funnel
    // every death through 'p' then 'q' from the initial state and through 'q' alone from qb, so q0 proposes "pq" and qb
    // proposes "q", and both proposals are proved from their own states. When the surviving byte returns to the initial
    // state, a scan can sit there mid-input, q0 is a required state, and its longer core is the answer. When it returns
    // to the absorbing loop instead, nothing re-enters q0, the window hypothesis at q0 renames rather than survives,
    // and the exemption must drop its proposal.

    /**
     * @brief Builds the table with the killer's surviving byte returning to the initial state or to the absorbing loop.
     * @param reentrant Whether it returns to the initial state.
     * @return The compiled table.
     */
    const auto build{[](const bool reentrant) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto qb{builder.next_state()};
        const auto qa{builder.next_state()};
        const auto ql{builder.next_state()};

        const Token token{1};

        builder.add_accept_state(ql, token);

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'p')
            {
                builder.add_transition(q0, Label{byte}, ql);
            }

            if (byte != 'q')
            {
                builder.add_transition(qb, Label{byte}, ql);
            }

            builder.add_transition(ql, Label{byte}, ql);
        }

        builder.add_transition(q0, Label{'p'}, qb);
        builder.add_transition(qb, Label{'q'}, qa);
        builder.add_transition(qa, Label{'c'}, reentrant ? q0 : ql);

        return Simulator{builder.build()};
    }};

    EXPECT_EQ(build(true).mandatory_core(), "pq");

    EXPECT_EQ(build(false).mandatory_core(), "q");
}

TEST(Dfa_test, Mandatory_core_stays_empty_when_the_killing_byte_itself_completes_the_core)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token{1};

    // The refuting shape for the order of the two checks: the second escape 'z' leads to a state that dies exactly on
    // 'a', so the death word "za" carries the proposed core only as its own killing byte. Too late is not carried: a
    // certified window built on this table could end on the core with nothing after it, so the proof notices the death
    // before reading the killing byte, and refuses.
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'s'}, q1);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'z')
        {
            builder.add_transition(q1, Label{byte}, q1);
        }

        if (byte != 'a')
        {
            builder.add_transition(q3, Label{byte}, q1);
        }
    }

    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'c'}, q1);
    builder.add_transition(q1, Label{'z'}, q3);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_survives_a_stretched_run_inside_its_own_prefix)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};

    const Token token{1};

    // Death spells at least two 'a's and then 'b', but the run of 'a's can stretch, so on "aaab" the matcher falls back
    // across the border of "aab" keeping the two 'a's it has already seen. The failure links keep the full proposal,
    // and the stretched run carries the core "aab".
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'s'}, q1);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a')
        {
            builder.add_transition(q1, Label{byte}, q1);
            builder.add_transition(q2, Label{byte}, q1);
        }

        if (byte != 'a' && byte != 'b')
        {
            builder.add_transition(q3, Label{byte}, q1);
        }
    }

    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'a'}, q3);
    builder.add_transition(q3, Label{'a'}, q3);
    builder.add_transition(q3, Label{'b'}, q4);
    builder.add_transition(q4, Label{'c'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aab");
}

TEST(Dfa_test, Mandatory_core_shrinks_when_an_overlapping_route_only_pretends_to_carry_it)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};
    const auto q5{builder.next_state()};
    const auto q6{builder.next_state()};

    const Token token{1};

    // Two escapes share the prefix "ab": one dies after "aba" and proposes that word as the core, the other dies after
    // "abba" and refutes it, because "abba" holds no "aba". The failure links read the second 'b' of "abb" as nothing
    // matched, so the final 'a' completes no core. The answer is the shared suffix "ba", which both death words do
    // carry.
    builder.add_accept_state(q1, token);

    builder.add_transition(q0, Label{'s'}, q1);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a')
        {
            builder.add_transition(q1, Label{byte}, q1);
        }

        if (byte != 'b')
        {
            builder.add_transition(q2, Label{byte}, q1);
        }

        if (byte != 'a' && byte != 'b')
        {
            builder.add_transition(q3, Label{byte}, q1);
        }

        if (byte != 'a')
        {
            builder.add_transition(q5, Label{byte}, q1);
        }
    }

    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'b'}, q3);
    builder.add_transition(q3, Label{'a'}, q4);
    builder.add_transition(q4, Label{'c'}, q1);
    builder.add_transition(q3, Label{'b'}, q5);
    builder.add_transition(q5, Label{'a'}, q6);
    builder.add_transition(q6, Label{'c'}, q1);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "ba");
}

TEST(Dfa_test, Mandatory_core_refutation_reaches_both_ends_of_the_alphabet)
{
    // The refuting route dies on one byte and on nothing else, and that byte sits at either end of the alphabet: the
    // proof search reads every symbol from 0x00 to 0xFF, so both deaths refute the 'a' the other route proposes. The
    // shape is otherwise the familiar two-route refutation.

    /**
     * @brief Builds the table with the refuting route dying on one byte.
     * @param edge The byte it dies on.
     * @return The compiled table.
     */
    const auto build{[](const std::size_t edge) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto q1{builder.next_state()};
        const auto q2{builder.next_state()};
        const auto q3{builder.next_state()};

        const Token token{1};

        builder.add_accept_state(q1, token);

        builder.add_transition(q0, Label{'s'}, q1);

        for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
        {
            const auto byte{static_cast<char>(symbol)};

            if (byte != 'a' && byte != 'z')
            {
                builder.add_transition(q1, Label{byte}, q1);
            }

            if (symbol != 0x01)
            {
                builder.add_transition(q2, Label{byte}, q1);
            }

            if (symbol != edge)
            {
                builder.add_transition(q3, Label{byte}, q1);
            }
        }

        builder.add_transition(q1, Label{'a'}, q2);
        builder.add_transition(q1, Label{'z'}, q3);

        return Simulator{builder.build()};
    }};

    EXPECT_EQ(build(0xFF).mandatory_core(), "");

    EXPECT_EQ(build(0x00).mandatory_core(), "");
}

TEST(Dfa_test, Mandatory_core_revisits_a_state_under_a_different_matcher_prefix)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    const Token token{1};

    // The initial state's proposal "zz" is refuted only along "zy" followed by "zz": the search reaches q1 first with
    // one byte matched and later with none, and the refutation lives behind the second visit. The search keys its seen
    // set on the pair it reaches, so it walks the refuting route and the answer is the proved "z".
    builder.add_accept_state(q1, token);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'z')
        {
            builder.add_transition(q0, Label{byte}, q0);
        }

        if (byte != 'y' && byte != 'z')
        {
            builder.add_transition(q1, Label{byte}, q0);
        }

        if (byte != 'z')
        {
            builder.add_transition(q2, Label{byte}, q0);
        }
    }

    builder.add_transition(q0, Label{'z'}, q1);
    builder.add_transition(q1, Label{'y'}, q1);
    builder.add_transition(q1, Label{'z'}, q2);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "z");
}

TEST(Dfa_test, Mandatory_core_matcher_falls_back_across_several_borders_at_once)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto hub{builder.next_state()};
    const auto n1{builder.next_state()};
    const auto n2{builder.next_state()};
    const auto n3{builder.next_state()};
    const auto m4{builder.next_state()};
    const auto m5{builder.next_state()};
    const auto m6{builder.next_state()};
    const auto killer{builder.next_state()};

    const Token token{1};

    // Death spells "abab" or "abaabab" from the hub, so the proposal is "abab" and the longer route carries it only
    // because its tail overlaps its own middle: on "abaa" the matcher falls from three matched through one to zero and
    // back up, two borders in a single byte, and the answer is the full proposal.
    builder.add_accept_state(hub, token);

    builder.add_transition(q0, Label{'s'}, hub);

    add_every_byte_but(builder, hub, hub, {'a'});
    add_every_byte_but(builder, n1, hub, {'b'});
    add_every_byte_but(builder, n2, hub, {'a'});
    add_every_byte_but(builder, n3, hub, {'b', 'a'});
    add_every_byte_but(builder, m4, hub, {'b'});
    add_every_byte_but(builder, m5, hub, {'a'});
    add_every_byte_but(builder, m6, hub, {'b'});

    builder.add_transition(hub, Label{'a'}, n1);
    builder.add_transition(n1, Label{'b'}, n2);
    builder.add_transition(n2, Label{'a'}, n3);
    builder.add_transition(n3, Label{'b'}, killer);
    builder.add_transition(n3, Label{'a'}, m4);
    builder.add_transition(m4, Label{'b'}, m5);
    builder.add_transition(m5, Label{'a'}, m6);
    builder.add_transition(m6, Label{'b'}, killer);
    builder.add_transition(killer, Label{'c'}, hub);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "abab");
}

TEST(Dfa_test, Mandatory_core_failure_links_chain_while_the_table_is_built)
{
    Builder builder{};

    const auto q0{builder.init_state()};

    std::array<Dfa::State_t, 7> chain{};

    for (auto& state : chain)
    {
        state = builder.next_state();
    }

    const Token token{1};

    // The KMP prefix automaton of "aabaaaa", laid out as live states: every 'a'/'b' edge follows the matcher, every
    // other byte restarts. The proposal is the full word, and the death route that reads "aabaaab" mid-way still
    // carries it, which the failure table knows by inheriting progress through the word's border, so the answer is the
    // full word. The construction here never needs more than one fallback hop; the chained multi-hop case is the
    // "aaabb" test's job.
    builder.add_accept_state(chain[0], token);

    /**
     * @brief Adds a state's transitions on a and on b.
     * @param from The state.
     * @param on_a The target on a.
     * @param on_b The target on b.
     */
    const auto pair_edges{[&builder](const Dfa::State_t from, const Dfa::State_t on_a, const Dfa::State_t on_b) {
        builder.add_transition(from, Label{'a'}, on_a);
        builder.add_transition(from, Label{'b'}, on_b);
    }};

    pair_edges(q0, chain[0], q0);
    pair_edges(chain[0], chain[1], q0);
    pair_edges(chain[1], chain[1], chain[2]);
    pair_edges(chain[2], chain[3], q0);
    pair_edges(chain[3], chain[4], q0);
    pair_edges(chain[4], chain[5], chain[2]);
    pair_edges(chain[5], chain[6], chain[2]);

    for (std::size_t symbol{0}; symbol < Simulator::symbol_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        if (byte != 'a' && byte != 'b')
        {
            builder.add_transition(q0, Label{byte}, q0);

            for (const auto state : chain)
            {
                builder.add_transition(state, Label{byte}, q0);
            }
        }
    }

    builder.add_transition(chain[6], Label{'x'}, q0);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.mandatory_core(), "aabaaaa");
}

TEST(Dfa_test, Accelerated_runs_preserve_longest_match)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    const Token token_a{1};
    const Token token_ab{2};

    // Tokens "a" and "a+b": q2 self-loops on 'a' without accepting, so a long all-'a' input walks deep into q2 and must
    // still resolve to the one-character token seen before the run.
    builder.add_accept_state(q1, token_a);
    builder.add_accept_state(q3, token_ab);

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'a'}, q2);
    builder.add_transition(q1, Label{'b'}, q3);
    builder.add_transition(q2, Label{'b'}, q3);

    const Simulator simulator{builder.build()};

    const std::string run_with_b{std::string(40, 'a') + 'b'};

    std::vector<std::pair<std::size_t, std::size_t>> tokens{};

    /**
     * @brief Appends one token's ID and length to the stream.
     * @param token The token.
     * @param length Its length.
     */
    const auto collect{[&tokens](const Token& token, const std::size_t length, std::uint64_t) {
        tokens.emplace_back(token.id(), length);
    }};

    auto consumed{simulator.run_all(run_with_b.cbegin(), run_with_b.cend(), collect)};

    EXPECT_EQ(consumed, run_with_b.size());
    ASSERT_EQ(tokens.size(), 1U);
    EXPECT_EQ(tokens.front(), (std::pair<std::size_t, std::size_t>{token_ab.id(), run_with_b.size()}));

    // Without the 'b', the longest match fails and every rescan must fall back to the length-one token, so the accept
    // position recorded before the run must survive the walk through it.
    const std::string run_without_b(40, 'a');

    tokens.clear();

    consumed = simulator.run_all(run_without_b.cbegin(), run_without_b.cend(), collect);

    EXPECT_EQ(consumed, run_without_b.size());
    EXPECT_EQ(tokens.size(), run_without_b.size());

    for (const auto& token : tokens)
    {
        EXPECT_EQ(token, (std::pair<std::size_t, std::size_t>{token_a.id(), 1U}));
    }
}

TEST(Dfa_test, Accelerated_runs_extend_accepting_tokens)
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    const Token token_run{1};

    // A plus-style run token: q1 accepts and self-loops, so the accept position must advance to the run's end.
    builder.add_accept_state(q1, token_run);

    builder.add_transition(q0, Label{' '}, q1);
    builder.add_transition(q1, Label{' '}, q1);

    const Simulator simulator{builder.build()};

    for (const std::size_t length : {1U, 7U, 15U, 16U, 17U, 100U})
    {
        const std::string input(length, ' ');

        std::vector<std::size_t> lengths{};

        /**
         * @brief Appends one token's length.
         * @param matched The length.
         */
        const auto collect{
                [&lengths](const Token&, const std::size_t matched, std::uint64_t) { lengths.push_back(matched); }};

        const auto consumed{simulator.run_all(input.cbegin(), input.cend(), collect)};

        EXPECT_EQ(consumed, length);
        ASSERT_EQ(lengths.size(), 1U);
        EXPECT_EQ(lengths.front(), length);
    }
}

TEST(Dfa_test, A_state_count_the_largest_identifier_wrapped_is_refused)
{
    // A hand-built DFA may number states sparsely; the largest possible identifier wraps the state count to zero in the
    // definition itself. What is observed here is the refusal: the count wrapped when the Dfa was built, and the
    // constructor throws rather than index anything by it.
    const Dfa dfa{std::numeric_limits<std::size_t>::max(), {}, {}};

    EXPECT_EQ(dfa.state_count(), 0U);
    EXPECT_THROW((Simulator{dfa}), std::runtime_error);
}

TEST(Dfa_test, A_span_no_count_holds_names_a_state_and_is_no_identifier_to_unroll_through)
{
    // The precondition both contracts state: unroll_start() enters the automaton through Dfa::state_count(), which is
    // an identifier no state uses only while the span of the identifiers is representable. A definition naming a state
    // at the largest std::size_t spans one past it, the count is then the wrap, and zero is a state that definition
    // names, here its accepting start, so the count is neither a count nor free. The Simulator refuses such a
    // definition as given, before unrolling it, which is why no compiled decision meets one; the test observes the
    // refusal, and that the count it refuses is the wrap.
    constexpr auto highest{std::numeric_limits<Dfa::State_t>::max()};

    const Token token{1};

    const Dfa sparse{0, {{Dfa::Key_t{0, Label{'a'}}, highest}}, {{0, token}}};

    EXPECT_EQ(sparse.state_count(), 0U);
    EXPECT_TRUE(sparse.has_accept_token(sparse.state_count()).has_value());
    EXPECT_THROW((Simulator{sparse}), std::runtime_error);

    // One below it, the span is representable and the fresh start free, but the unrolled automaton spans one past the
    // largest std::size_t and reports the wrap as its count: the precondition is the span plus one, and the Simulator
    // refuses this definition too, its count reaching the table entry's sentinel.
    const Dfa edge{0, {{Dfa::Key_t{0, Label{'a'}}, highest - 1}}, {{0, token}}};

    EXPECT_EQ(edge.state_count(), highest);

    const auto unrolled{unroll_start(edge)};

    EXPECT_EQ(unrolled.init_state(), highest);
    EXPECT_FALSE(unrolled.has_accept_token(unrolled.init_state()).has_value());
    EXPECT_EQ(unrolled.state_count(), 0U);
    EXPECT_THROW((Simulator{edge}), std::runtime_error);

    // Two below, the bound holds: the unrolled automaton spans exactly the largest std::size_t.
    const Dfa within{0, {{Dfa::Key_t{0, Label{'a'}}, highest - 2}}, {{0, token}}};

    EXPECT_EQ(unroll_start(within).state_count(), highest);
}

TEST(Dfa_test, Table_size_overflow_arithmetic_is_pinned_on_every_platform)
{
    // The constructor's product guard is unreachable on a 64-bit std::size_t, where 32-bit states times at most 256
    // classes cannot wrap; the arithmetic is pinned here against a 32-bit-sized limit instead, so the guard the 32-bit
    // platform relies on cannot rot unnoticed on the platforms that test it.
    constexpr std::size_t limit_32{std::numeric_limits<std::uint32_t>::max()};

    static_assert(table_size_overflows(16777216U, 256U, limit_32));
    static_assert(table_size_overflows(limit_32, 2U, limit_32));
    static_assert(!table_size_overflows(16777215U, 256U, limit_32));
    static_assert(!table_size_overflows(1024U, 256U, limit_32));

    // Zero classes means zero entries, never an overflow and never a division.
    static_assert(!table_size_overflows(limit_32, 0U, limit_32));

    // A product one past the platform limit overflows: the check is a division, exact at every width.
    static_assert(table_size_overflows(
            std::numeric_limits<std::size_t>::max() / 2 + 1, 2U, std::numeric_limits<std::size_t>::max()));

    // On a 64-bit limit the maximal state count with every class distinct stays far from the edge; on a 32-bit one it
    // does not, which is this guard's reason to exist, so the platform claim is width-guarded.
    static_assert(
            sizeof(std::size_t) < 8 || !table_size_overflows(limit_32, 256U, std::numeric_limits<std::size_t>::max()));
    static_assert(table_size_overflows(limit_32, 256U, limit_32));
}

TEST(Dfa_test, Lag_and_rescue_freeness_ignore_accepting_states_no_input_reaches)
{
    // {a, abc} built by hand: q0 -a-> q1 accepting, q1 -b-> q2, q2 -c-> q3 accepting; one stretch of one state after
    // the accepted a, opened by b, which starts no token: lag one, rescue-free. The islanded table adds an accepting
    // state no input reaches whose successor cycles on itself; no scan can ever stand in the island, so neither the lag
    // nor rescue-freeness may see its cycle.

    /**
     * @brief Builds {a, abc}, with or without the unreachable island.
     * @param with_island Whether the island is added.
     * @return The compiled table.
     */
    const auto build{[](const bool with_island) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto q1{builder.next_state()};
        const auto q2{builder.next_state()};
        const auto q3{builder.next_state()};

        builder.add_transition(q0, Label{'a'}, q1);
        builder.add_transition(q1, Label{'b'}, q2);
        builder.add_transition(q2, Label{'c'}, q3);
        builder.add_accept_state(q1, Token{1});
        builder.add_accept_state(q3, Token{2});

        if (with_island)
        {
            const auto island{builder.next_state()};
            const auto loop{builder.next_state()};

            builder.add_accept_state(island, Token{3});
            builder.add_transition(island, Label{'a'}, loop);
            builder.add_transition(loop, Label{'a'}, loop);
        }

        return Simulator{builder.build()};
    }};

    const auto trimmed{build(false)};
    const auto islanded{build(true)};

    const auto [trimmed_witness, trimmed_exhaustive]{rescue(trimmed)};

    EXPECT_EQ(lag(trimmed), std::optional<std::size_t>{1});
    EXPECT_TRUE(trimmed_witness.empty());
    EXPECT_TRUE(trimmed_exhaustive);
    EXPECT_EQ(lag(islanded), lag(trimmed));

    // Both halves of the answer must survive the island: an empty witness says the set is rescue-free only from a
    // search that settled the question, so the islanded table has to report the exhaustion the trimmed one does.
    const auto [islanded_witness, islanded_exhaustive]{rescue(islanded)};

    EXPECT_EQ(islanded_witness, trimmed_witness);
    EXPECT_EQ(islanded_exhaustive, trimmed_exhaustive);
    EXPECT_TRUE(islanded_exhaustive);
}

TEST(Dfa_test, Lag_and_rescue_freeness_ignore_an_unreachable_accepting_island)
{
    // A table with an unreachable accepting island: initial state 0, accepting {1, 2}, 0 -a-> 1, 2 -a-> 3, 3 -a-> 3.
    // The token language is {a}. Accepting seeds are restricted to reachable states, so the island at 2 leaves lag() at
    // zero, and the search rescue() runs starts at the initial state and never sees the island.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q2, Label{'a'}, q3);
    builder.add_transition(q3, Label{'a'}, q3);
    builder.add_accept_state(q1, Token{1});
    builder.add_accept_state(q2, Token{1});

    const Simulator simulator{builder.build()};

    const auto [witness, exhaustive]{rescue(simulator)};

    EXPECT_EQ(lag(simulator), std::optional<std::size_t>{0});
    EXPECT_TRUE(witness.empty());
    EXPECT_TRUE(exhaustive);
}

TEST(Dfa_test, Window_occurrence_places_a_window_in_a_tokenizable_input_or_proves_it_in_none)
{
    // {aa} built by hand: q0 -a-> q1 -a-> q2 accepting. Its completely tokenizable inputs are the even runs of a, so
    // every window of a's occurs, the odd ones in a run one longer, and no window holding a b occurs anywhere: the
    // separating example of the certified-splitting paper, where the unrestricted certificate of b at width one is
    // vacuous and the occurring inventory is empty.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_accept_state(q2, Token{1});

    const Simulator simulator{builder.build()};

    /**
     * @brief Returns whether a witness tokenizes completely under the machine itself.
     * @param witness The witness.
     * @return True when it does.
     */
    const auto tokenizes{
            [&simulator](const std::string& witness) { return token_starts(simulator, witness).has_value(); }};

    const auto [odd, odd_settled]{window_occurrence(simulator, "aaa")};

    EXPECT_TRUE(odd_settled);
    EXPECT_EQ(odd, "aaaa");
    EXPECT_TRUE(tokenizes(odd));

    const auto [even, even_settled]{window_occurrence(simulator, "aa")};

    EXPECT_TRUE(even_settled);
    EXPECT_EQ(even, "aa");
    EXPECT_TRUE(tokenizes(even));

    const auto [none, none_settled]{window_occurrence(simulator, "b")};

    EXPECT_TRUE(none_settled);
    EXPECT_TRUE(none.empty());

    const auto [aab_witness, aab_exhaustive]{window_occurrence(simulator, "aab")};

    EXPECT_TRUE(aab_witness.empty());
    EXPECT_TRUE(aab_exhaustive);

    // The question is over nonempty inputs: the empty window has the shortest token, aa, as its witness here, and under
    // a token set accepting nothing, or the empty string alone, it occurs in no nonempty input, the empty input that
    // contains it being no input a cut could fall in.
    const auto [any, any_settled]{window_occurrence(simulator, "")};

    EXPECT_TRUE(any_settled);
    EXPECT_EQ(any, "aa");

    Builder nothing{};

    const auto lone{nothing.init_state()};

    nothing.add_transition(lone, Label{'a'}, lone);

    const Simulator accepts_nothing{nothing.build()};

    const auto [nothing_witness, nothing_exhaustive]{window_occurrence(accepts_nothing, "")};

    EXPECT_TRUE(nothing_witness.empty());
    EXPECT_TRUE(nothing_exhaustive);

    Builder epsilon{};

    epsilon.add_accept_state(epsilon.init_state(), Token{1});

    const Simulator accepts_epsilon{epsilon.build()};

    const auto [epsilon_witness, epsilon_exhaustive]{window_occurrence(accepts_epsilon, "")};

    EXPECT_TRUE(accepts_epsilon.nullable());
    EXPECT_TRUE(epsilon_witness.empty());
    EXPECT_TRUE(epsilon_exhaustive);
}

TEST(Dfa_test, Window_counterexample_finds_the_input_a_certificate_fails_on_or_proves_it_exact)
{
    // {aa} built by hand: q0 -a-> q1 -a-> q2 accepting. Its completely tokenizable inputs are the even runs of a, so an
    // occurrence of a window of a's is covered from its own start at an even position and from one byte before at an
    // odd one: no origin certifies a or aa, and the b no input holds is certified at every origin vacuously.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_accept_state(q2, Token{1});

    const Simulator simulator{builder.build()};

    /**
     * @brief Returns whether a witness is what it claims: a completely tokenizable input under the machine itself
     *        holding an occurrence of the window whose final byte is covered by a token beginning elsewhere than the
     *        origin.
     * @param witness The witness.
     * @param window The window.
     * @param origin The origin.
     * @return True when it is.
     */
    const auto fails{[&simulator](const std::string& witness, const std::string_view window, const std::size_t origin) {
        const auto starts{token_starts(simulator, witness)};

        if (!starts)
        {
            return false;
        }

        for (auto at{witness.find(window)}; at != std::string::npos; at = witness.find(window, at + 1))
        {
            const auto covering{std::ranges::upper_bound(*starts, at + window.size() - 1)};

            if (*std::prev(covering) != at + origin)
            {
                return true;
            }
        }

        return false;
    }};

    const auto [odd, odd_settled]{window_counterexample(simulator, "a", 0)};

    EXPECT_TRUE(odd_settled);
    EXPECT_EQ(odd, "aa");
    EXPECT_TRUE(fails(odd, "a", 0));

    const auto [shifted, shifted_settled]{window_counterexample(simulator, "aa", 0)};

    EXPECT_TRUE(shifted_settled);
    EXPECT_EQ(shifted, "aaaa");
    EXPECT_TRUE(fails(shifted, "aa", 0));

    const auto [late_witness, late_exhaustive]{window_counterexample(simulator, "aa", 1)};

    EXPECT_EQ(late_witness, "aa");
    EXPECT_TRUE(late_exhaustive);
    EXPECT_TRUE(fails(late_witness, "aa", 1));

    const auto [none, none_settled]{window_counterexample(simulator, "b", 0)};

    EXPECT_TRUE(none_settled);
    EXPECT_TRUE(none.empty());

    // The empty window has no final byte to cover, and an origin outside the window names none of its bytes.
    EXPECT_THROW(std::ignore = window_counterexample(simulator, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = window_counterexample(simulator, "aa", 2), std::invalid_argument);
}

TEST(Dfa_test, Boundary_profile_finds_the_input_a_gap_is_crossed_or_cut_on_or_proves_it_must_or_never)
{
    // {aa} and {ab} built by hand, each a chain q0 -> q1 -> q2 with q2 accepting. Under {aa} the completely tokenizable
    // inputs are the even runs of a, so every gap of a window of a's falls at an odd position of some occurrence and
    // none is a boundary at every occurrence; under {ab} they are the runs of ab, where every a begins a token and no b
    // does.

    /**
     * @brief Builds the chain q0 -a-> q1 -second-> q2 with q2 accepting.
     * @param second The second byte.
     * @return The compiled table.
     */
    const auto build{[](const char second) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto q1{builder.next_state()};
        const auto q2{builder.next_state()};

        builder.add_transition(q0, Label{'a'}, q1);
        builder.add_transition(q1, Label{second}, q2);
        builder.add_accept_state(q2, Token{1});

        return Simulator{builder.build()};
    }};

    const auto even{build('a')};

    const auto alternating{build('b')};

    /**
     * @brief Returns whether a witness is what it claims: a completely tokenizable input under the machine itself
     *        holding an occurrence of the window with no token beginning at the offset.
     * @param simulator The machine.
     * @param witness The witness.
     * @param window The window.
     * @param offset The offset into the window.
     * @return True when it is.
     */
    const auto misses{[](const Simulator& simulator, const std::string& witness, const std::string_view window,
                         const std::size_t offset) {
        const auto starts{token_starts(simulator, witness)};

        if (!starts)
        {
            return false;
        }

        for (auto at{witness.find(window)}; at != std::string::npos; at = witness.find(window, at + 1))
        {
            if (!std::ranges::binary_search(*starts, at + offset))
            {
                return true;
            }
        }

        return false;
    }};

    const auto [odd, odd_settled]{boundary_counterexample(even, "a", 0)};

    EXPECT_TRUE(odd_settled);
    EXPECT_EQ(odd, "aa");
    EXPECT_TRUE(misses(even, odd, "a", 0));

    const auto [shifted, shifted_settled]{boundary_counterexample(even, "aa", 0)};

    EXPECT_TRUE(shifted_settled);
    EXPECT_EQ(shifted, "aaaa");
    EXPECT_TRUE(misses(even, shifted, "aa", 0));

    const auto [even_witness, even_settled]{boundary_counterexample(even, "aa", 1)};

    EXPECT_TRUE(even_settled);
    EXPECT_EQ(even_witness, "aa");
    EXPECT_TRUE(misses(even, even_witness, "aa", 1));

    // Under {ab} the a of ba is a boundary at every occurrence and the b refuted by the shortest input holding ba.
    const auto [kept, kept_settled]{boundary_counterexample(alternating, "ba", 1)};

    EXPECT_TRUE(kept_settled);
    EXPECT_TRUE(kept.empty());

    const auto [refuted, refuted_settled]{boundary_counterexample(alternating, "ba", 0)};

    EXPECT_TRUE(refuted_settled);
    EXPECT_EQ(refuted, "abab");
    EXPECT_TRUE(misses(alternating, refuted, "ba", 0));

    /**
     * @brief Expects one gap's verdict of must or never, the witness of the search refuting the other claim, and no
     *        witness against the claim the verdict holds.
     * @param entry The gap's verdict and its two searches.
     * @param verdict The verdict expected, must or never.
     * @param witness The witness expected of the refuting search: a cut for must, a crossing for never.
     */
    const auto expect_gap{[](const Gap_verdict& entry, const Gap verdict, const std::string_view witness) {
        const auto& [found, crossed, cut]{entry};

        const auto& [crossed_witness, crossed_exhaustive]{crossed};

        const auto& [cut_witness, cut_exhaustive]{cut};

        const auto must{verdict == Gap::must};

        EXPECT_EQ(found, verdict);
        EXPECT_EQ(cut_witness, must ? witness : "");
        EXPECT_EQ(crossed_witness, must ? "" : witness);
    }};

    // The profile decides every gap both ways, the gap after the window included. Under {ab} the window ab is must at 0
    // and 2 and never at 1, and ba, whose a is always followed by the b closing its token, is never at 0 and 2 and must
    // at 1.
    const auto ab_profile{boundary_profile(alternating, "ab")};

    ASSERT_EQ(ab_profile.size(), 3U);

    expect_gap(ab_profile[0], Gap::must, "ab");
    expect_gap(ab_profile[1], Gap::never, "ab");
    expect_gap(ab_profile[2], Gap::must, "ab");

    const auto reversed{boundary_profile(alternating, "ba")};

    ASSERT_EQ(reversed.size(), 3U);

    expect_gap(reversed[0], Gap::never, "abab");
    expect_gap(reversed[1], Gap::must, "abab");
    expect_gap(reversed[2], Gap::never, "abab");

    // The gap after the window adds what no offset inside it can: under {ab} the window b has no boundary before it at
    // any occurrence, and one right after it at every occurrence, the next token's start or the input's end, which ab
    // itself shows.
    const auto [after, after_settled]{boundary_counterexample(alternating, "b", 1)};

    EXPECT_TRUE(after_settled);
    EXPECT_TRUE(after.empty());

    const auto [closing, closing_settled]{crossing_counterexample(alternating, "b", 1)};

    EXPECT_TRUE(closing_settled);
    EXPECT_EQ(closing, "ab");

    const auto single{boundary_profile(alternating, "b")};

    ASSERT_EQ(single.size(), 2U);

    expect_gap(single[0], Gap::never, "ab");
    expect_gap(single[1], Gap::must, "ab");

    // A window no completely tokenizable input contains has no counterexample at any gap, either way, and the profile
    // tells it apart as absent at every gap.
    const auto [none, none_settled]{boundary_counterexample(even, "b", 0)};

    EXPECT_TRUE(none_settled);
    EXPECT_TRUE(none.empty());

    const auto missing{boundary_profile(even, "b")};

    const auto absent{std::ranges::count(missing, Gap::absent, &Gap_verdict::verdict)};

    ASSERT_EQ(missing.size(), 2U);
    EXPECT_EQ(absent, 2);

    // The empty window has no gap, and a gap past the window's end names none of its gaps; the gap right after the
    // window is one.
    EXPECT_THROW(std::ignore = boundary_counterexample(even, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = boundary_counterexample(even, "aa", 3), std::invalid_argument);
    EXPECT_THROW(std::ignore = crossing_counterexample(even, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = crossing_counterexample(even, "aa", 3), std::invalid_argument);
    EXPECT_THROW(std::ignore = boundary_profile(even, ""), std::invalid_argument);

    const auto [end_witness, end_exhaustive]{boundary_counterexample(even, "aa", 2)};

    EXPECT_TRUE(end_exhaustive);
    EXPECT_EQ(end_witness, "aaaa");
    EXPECT_TRUE(misses(even, end_witness, "aa", 2));
}

TEST(Dfa_test, Boundary_profile_gives_absence_from_the_first_cap_at_which_a_gap_or_the_occurrence_search_proves_it)
{
    // Two tables with a transition into a dead state, which step() enters as it enters any state: {a} with b leading
    // from q0 and q1 into the dead q2, where the window ab occurs nowhere, and a(aa)* with b leading from q0 into the
    // dead q2, where aab occurs nowhere. A branch that has read the window through into the dead state never closes;
    // the occurrence search holds every such branch while a gap search drops the ones keeping its claim, so a gap's
    // pair exhausts under a cap that stops the occurrence search and proves absence there. The profile is absent at
    // every gap from the first cap at which any proof exhausts and at no gap below it, and never must, never or may at
    // any cap, the window occurring nowhere; the caps are read off the searches, four around each table's threshold.

    /**
     * @brief Builds one of the two tables with a dead state.
     * @param looping Whether the table is a(aa)* rather than {a}.
     * @return The compiled table.
     */
    const auto table{[](const bool looping) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto q1{builder.next_state()};
        const auto q2{builder.next_state()};

        builder.add_transition(q0, Label{'a'}, q1);
        builder.add_transition(q0, Label{'b'}, q2);
        builder.add_transition(q1, Label{looping ? 'a' : 'b'}, looping ? q0 : q2);
        builder.add_accept_state(q1, Token{0});

        return Simulator{builder.build()};
    }};

    /**
     * @brief Returns the smallest cap under which a search exhausts, by running it.
     * @tparam Exhausts The predicate's type.
     * @param exhausts Whether the search exhausts under a cap.
     * @return The cap.
     */
    const auto first_cap{[]<typename Exhausts>(const Exhausts& exhausts) {
        std::size_t cap{1};

        while (!exhausts(cap))
        {
            ++cap;
        }

        return cap;
    }};

    for (const auto& [looping, window, low, high] :
         std::vector<std::tuple<bool, std::string_view, std::size_t, std::size_t>>{
                 {false, "ab", 7, 10},
                 {true, "aab", 17, 20}})
    {
        const auto simulator{table(looping)};

        EXPECT_FALSE(simulator.is_live(2)) << window;

        /**
         * @brief Returns whether the occurrence search exhausts under a cap.
         * @param cap The cap.
         * @return True when it does.
         */
        const auto occurrence_exhausts{[&](const std::size_t cap) {
            const auto [witness, exhaustive]{window_occurrence(simulator, window, cap)};

            return exhaustive;
        }};

        const auto occurrence{first_cap(occurrence_exhausts)};

        const auto [occurring, occurrence_settled]{window_occurrence(simulator, window, occurrence)};

        EXPECT_TRUE(occurrence_settled) << window;
        EXPECT_TRUE(occurring.empty()) << window;

        auto proof{occurrence};

        for (std::size_t gap{0}; gap <= window.size(); ++gap)
        {
            /**
             * @brief Returns whether both refutations at the gap exhaust under a cap.
             * @param cap The cap.
             * @return True when they do.
             */
            const auto gap_exhausts{[&](const std::size_t cap) {
                const auto [boundary_witness, boundary_exhaustive]{
                        boundary_counterexample(simulator, window, gap, cap)};

                if (!boundary_exhaustive)
                {
                    return false;
                }

                const auto [crossing_witness, crossing_exhaustive]{
                        crossing_counterexample(simulator, window, gap, cap)};

                return crossing_exhaustive;
            }};

            const auto gap_proof{first_cap(gap_exhausts)};

            proof = std::min(proof, gap_proof);
        }

        EXPECT_LT(proof, occurrence) << window;
        EXPECT_TRUE(low < proof && proof <= high) << window << ' ' << proof;

        for (auto cap{low}; cap <= high; ++cap)
        {
            const auto profile{boundary_profile(simulator, window, cap)};

            const auto absent{std::ranges::count(profile, Gap::absent, &Gap_verdict::verdict)};

            ASSERT_EQ(profile.size(), window.size() + 1) << window << ' ' << cap;
            EXPECT_TRUE(absent == 0 || std::cmp_equal(absent, profile.size())) << window << ' ' << cap;
            EXPECT_EQ(std::cmp_equal(absent, profile.size()), cap >= proof) << window << ' ' << cap;

            for (std::size_t gap{0}; gap < profile.size(); ++gap)
            {
                const auto& [verdict, crossed, cut]{profile[gap]};

                const auto& [crossed_witness, crossed_exhaustive]{crossed};

                const auto& [cut_witness, cut_exhaustive]{cut};

                EXPECT_TRUE(verdict == Gap::absent || verdict == Gap::undetermined)
                        << window << ' ' << cap << ' ' << gap;
                EXPECT_TRUE(crossed_witness.empty() && cut_witness.empty()) << window << ' ' << cap << ' ' << gap;
                EXPECT_EQ(crossed_exhaustive && cut_exhaustive, verdict == Gap::absent)
                        << window << ' ' << cap << ' ' << gap;
            }
        }
    }
}

TEST(Dfa_test, Boundary_profile_over_random_tables_is_absent_everywhere_or_nowhere_and_decides_only_occurring_windows)
{
    // Tables drawn at random: three to five states, the initial one among them, each byte of {a, b} leading from each
    // to a random one of them, to a dead sink or nowhere, so that some of them may be reached by no byte, one or more
    // of them but the initial accepting, and an island no transition reaches beside them. Over every window over {a, b}
    // of length one to three and four caps drawn from 1 to 32, the profile is absent at every gap or at none, and a gap
    // is must, never or may only where its witness shows the window occurring, which window_occurrence() confirms under
    // a cap no search here reaches. Under that cap every search exhausts and the profile agrees with a brute-force
    // maximal-munch oracle over every input to length eight, which sees every witness that short: a side the oracle saw
    // is refuted by a witness as short as the oracle's, a witness inside the bound is a side the oracle saw, and where
    // the window occurs in no input or in one inside the bound, absent is exactly where the oracle saw no occurrence.
    constexpr std::size_t large{1U << 16U};

    constexpr std::size_t tables{60};

    constexpr std::size_t caps_per_window{4};

    constexpr std::size_t largest_small_cap{32};

    std::mt19937 sequence{0x5EEDU};

    const auto windows{ab_words(3)};

    const auto inputs{ab_words(oracle_bound)};

    std::map<Gap, std::size_t> tally{};

    std::size_t stopped{0};

    /**
     * @brief Holds one gap's verdict under the large cap against the oracle's records of the gap.
     * @param simulator The table.
     * @param window The window.
     * @param gap The gap.
     * @param decided The verdict under the large cap.
     * @param occurs_inside Whether the window occurs in an input inside the bound.
     * @param shortest The oracle's records.
     * @param label The failure message's label.
     */
    const auto check_decided{[&](const Simulator& simulator, const std::string_view window, const std::size_t gap,
                                 const Gap_verdict& decided, const bool occurs_inside, const Shortest_t& shortest,
                                 const std::string& label) {
        const auto& [verdict, crossed, cut]{decided};

        const auto& [crossed_witness, crossed_exhaustive]{crossed};

        const auto& [cut_witness, cut_exhaustive]{cut};

        /**
         * @brief Returns the oracle's shortest input with the gap cut or crossed.
         * @param was_cut Whether the gap is cut.
         * @return Its length, or zero when the oracle saw none.
         */
        const auto length{[&](const bool was_cut) {
            const auto found{shortest.find({window, gap, was_cut})};

            if (found == shortest.end())
            {
                return std::size_t{0};
            }

            const auto& [key, found_length]{*found};

            return found_length;
        }};

        const auto crossing{length(false)};

        const auto cutting{length(true)};

        ++tally[verdict];

        ASSERT_TRUE(crossed_exhaustive && cut_exhaustive) << label;
        EXPECT_EQ(verdict == Gap::must, crossed_witness.empty() && !cut_witness.empty()) << label;
        EXPECT_EQ(verdict == Gap::never, !crossed_witness.empty() && cut_witness.empty()) << label;
        EXPECT_EQ(verdict == Gap::absent, crossed_witness.empty() && cut_witness.empty()) << label;

        if (verdict == Gap::absent || occurs_inside)
        {
            EXPECT_EQ(verdict == Gap::absent, crossing == 0 && cutting == 0) << label;
        }

        if (crossing > 0 || crossed_witness.size() <= oracle_bound)
        {
            EXPECT_EQ(crossed_witness.size(), crossing) << label;
        }

        if (cutting > 0 || cut_witness.size() <= oracle_bound)
        {
            EXPECT_EQ(cut_witness.size(), cutting) << label;
        }

        if (!crossed_witness.empty())
        {
            EXPECT_TRUE(shows(simulator, crossed_witness, window, gap, false)) << label;
        }

        if (!cut_witness.empty())
        {
            EXPECT_TRUE(shows(simulator, cut_witness, window, gap, true)) << label;
        }
    }};

    /**
     * @brief Holds a profile under a small cap against the one under the large cap: absent at every gap or at none, and
     *        a verdict the cap left decided the large cap's, with its witnesses, showing the window occurring unless
     *        absent.
     * @param profile The profile under the small cap.
     * @param decided The profile under the large cap.
     * @param occurs Whether the window occurs.
     * @param label The failure message's label.
     */
    const auto check_capped{[&](const std::vector<Gap_verdict>& profile, const std::vector<Gap_verdict>& decided,
                                const bool occurs, const std::string& label) {
        const auto absent{std::ranges::count(profile, Gap::absent, &Gap_verdict::verdict)};

        ASSERT_EQ(profile.size(), decided.size()) << label;
        EXPECT_TRUE(absent == 0 || std::cmp_equal(absent, profile.size())) << label;

        for (std::size_t gap{0}; gap < profile.size(); ++gap)
        {
            const auto& [verdict, crossed, cut]{profile[gap]};

            const auto& [crossed_witness, crossed_exhaustive]{crossed};

            const auto& [cut_witness, cut_exhaustive]{cut};

            if (verdict == Gap::undetermined)
            {
                ++stopped;

                continue;
            }

            const auto& [decided_verdict, decided_crossed, decided_cut]{decided[gap]};

            const auto& [decided_crossed_witness, decided_crossed_exhaustive]{decided_crossed};

            const auto& [decided_cut_witness, decided_cut_exhaustive]{decided_cut};

            EXPECT_EQ(verdict, decided_verdict) << label << ' ' << gap;
            EXPECT_EQ(crossed_witness, decided_crossed_witness) << label << ' ' << gap;
            EXPECT_EQ(cut_witness, decided_cut_witness) << label << ' ' << gap;
            EXPECT_EQ(verdict == Gap::absent, !occurs) << label << ' ' << gap;
            EXPECT_EQ(verdict == Gap::absent, crossed_witness.empty() && cut_witness.empty()) << label << ' ' << gap;
        }
    }};

    for (std::size_t table{0}; table < tables; ++table)
    {
        const auto simulator{random_table(sequence)};

        const auto shortest{oracle(simulator, inputs, windows)};

        for (const auto& window : windows)
        {
            const auto name{std::format("{} {}", table, window)};

            const auto [witness, exhaustive]{window_occurrence(simulator, window, large)};

            ASSERT_TRUE(exhaustive) << name;

            const auto decided{boundary_profile(simulator, window, large)};

            ASSERT_EQ(decided.size(), window.size() + 1) << name;

            const auto occurs_inside{!witness.empty() && witness.size() <= oracle_bound};

            for (std::size_t gap{0}; gap < decided.size(); ++gap)
            {
                const auto label{std::format("{} {}", name, gap)};

                check_decided(simulator, window, gap, decided[gap], occurs_inside, shortest, label);
            }

            for (std::size_t round{0}; round < caps_per_window; ++round)
            {
                const auto cap{draw(sequence, largest_small_cap) + 1};

                const auto capped{boundary_profile(simulator, window, cap)};

                const auto label{std::format("{} cap {}", name, cap)};

                check_capped(capped, decided, !witness.empty(), label);
            }
        }
    }

    // Every verdict reached under the large cap, and some search stopped by a small one.
    for (const auto verdict : {Gap::must, Gap::never, Gap::may, Gap::absent})
    {
        EXPECT_GT(tally[verdict], 0U) << std::to_underlying(verdict);
    }

    EXPECT_GT(stopped, 0U);
}

TEST(Dfa_test, Segmentation_difference_separates_two_token_sets_by_domain_or_by_boundary_or_proves_them_one)
{
    // Three token sets over one letter built by hand: {a} is q0 -a-> q1 accepting; {aa} is q0 -a-> q1 -a-> q2
    // accepting; {aa, a} is the same chain with q1 accepting too. {a} and {aa, a} tokenize every run of a's and cut the
    // even ones apart, {aa} tokenizes the even runs alone: the first pair separates on the boundary half, the other two
    // on the domain half, and each set is one segmentation function with itself.

    /**
     * @brief Builds {a}, {aa} or {aa, a}.
     * @param pair Whether the chain has two a's.
     * @param single Whether one a is a token.
     * @return The compiled table.
     */
    const auto build{[](const bool pair, const bool single) {
        Builder builder{};

        const auto q0{builder.init_state()};
        const auto q1{builder.next_state()};

        builder.add_transition(q0, Label{'a'}, q1);

        if (single)
        {
            builder.add_accept_state(q1, Token{1});
        }

        if (pair)
        {
            const auto q2{builder.next_state()};

            builder.add_transition(q1, Label{'a'}, q2);
            builder.add_accept_state(q2, Token{2});
        }

        return Simulator{builder.build()};
    }};

    const auto single{build(false, true)};
    const auto pair{build(true, false)};
    const auto both{build(true, true)};

    /**
     * @brief Checks that a token set is one segmentation function with itself.
     * @param simulator The token set.
     */
    const auto one_function_with_itself{[](const Simulator& simulator) {
        const auto [witness, half, exhaustive]{segmentation_difference(simulator, simulator)};

        EXPECT_TRUE(exhaustive);
        EXPECT_TRUE(witness.empty());
        EXPECT_FALSE(half.has_value());
    }};

    one_function_with_itself(single);
    one_function_with_itself(pair);
    one_function_with_itself(both);

    // The contract's example: the shortest marked run only one side accepts is a with no boundary, which {a} accepts
    // and {aa} does not, where the boundary route's witness is aa, one token against two.
    const auto [domain, domain_half, domain_settled]{segmentation_difference(single, pair)};

    EXPECT_TRUE(domain_settled);
    EXPECT_EQ(domain, "a");
    EXPECT_EQ(domain_half, Separation_half::domain);

    const auto [cut_apart, cut_apart_settled]{boundary_difference(single, pair)};

    const auto [reversed, reversed_half, reversed_settled]{segmentation_difference(pair, single)};

    EXPECT_TRUE(cut_apart_settled);
    EXPECT_EQ(cut_apart, "aa");
    EXPECT_TRUE(reversed_settled);
    EXPECT_EQ(reversed, "a");
    EXPECT_EQ(reversed_half, Separation_half::domain);

    // A boundary witness is a boundary_difference() witness: both sides tokenize aa and cut it apart.
    const auto [boundary, boundary_half, boundary_settled]{segmentation_difference(single, both)};

    EXPECT_TRUE(boundary_settled);
    EXPECT_EQ(boundary, "aa");
    EXPECT_EQ(boundary_half, Separation_half::boundary);

    const auto [both_cut, both_cut_settled]{boundary_difference(single, both)};

    EXPECT_TRUE(both_cut_settled);
    EXPECT_EQ(both_cut, "aa");

    const auto [pair_both, pair_both_half, pair_both_settled]{segmentation_difference(pair, both)};

    EXPECT_TRUE(pair_both_settled);
    EXPECT_EQ(pair_both, "a");
    EXPECT_EQ(pair_both_half, Separation_half::domain);

    // Zero holds nothing, not even the state the search starts in, and settles nothing.
    const auto [capped, capped_half, capped_settled]{segmentation_difference(single, pair, 0)};

    EXPECT_FALSE(capped_settled);
    EXPECT_TRUE(capped.empty());
    EXPECT_FALSE(capped_half.has_value());
}

TEST(Dfa_test, Has_split_points_ignoring_holds_wherever_the_exact_test_does_and_on_the_sets_the_relaxation_rescues)
{
    // {a+, b} certifies b exactly, so both tests are true. a* alone re-enters its start on every a, which the exact
    // certificate refuses, and the relaxed one admits a only once the token is discarded: the exact test then reports
    // nothing to search for on precisely the set the relaxation exists to rescue.
    Builder exact{};

    const auto p0{exact.init_state()};
    const auto p1{exact.next_state()};
    const auto p2{exact.next_state()};

    exact.add_accept_state(p1, Token{1});
    exact.add_accept_state(p2, Token{2});
    exact.add_transition(p0, Label{'a'}, p1);
    exact.add_transition(p1, Label{'a'}, p1);
    exact.add_transition(p0, Label{'b'}, p2);

    const Simulator certified{exact.build()};

    EXPECT_TRUE(certified.has_split_points());
    EXPECT_TRUE(certified.has_split_points_ignoring());

    Builder run{};

    const auto q0{run.init_state()};

    run.add_accept_state(q0, Token{1});
    run.add_transition(q0, Label{'a'}, q0);

    const Simulator kept{run.build(), std::vector<std::size_t>{}};

    EXPECT_FALSE(kept.has_split_points());
    EXPECT_FALSE(kept.has_split_points_ignoring());

    const Simulator discarded{run.build(), std::vector<std::size_t>{1}};

    EXPECT_FALSE(discarded.has_split_points());
    EXPECT_TRUE(discarded.has_split_points_ignoring());
}

TEST(Dfa_test, Is_accepting_is_the_flag_alone_and_a_nullable_sets_fresh_start_never_carries_it)
{
    // The flag answers for every state the tables hold a column for, reachable or not.
    const auto simulator{six_state_shape()};

    EXPECT_FALSE(simulator.is_accepting(0));
    EXPECT_TRUE(simulator.is_accepting(1));
    EXPECT_TRUE(simulator.is_accepting(2));
    EXPECT_FALSE(simulator.is_accepting(3));
    EXPECT_FALSE(simulator.is_accepting(4));
    EXPECT_TRUE(simulator.is_accepting(5));

    // A start that accepts the empty word is compiled behind a fresh start that does not, so the state a scan starts in
    // never accepts; the old start keeps its flag under its own identifier.
    Builder nullable{};

    const auto r0{nullable.init_state()};
    const auto r1{nullable.next_state()};

    nullable.add_accept_state(r0, Token{1});
    nullable.add_accept_state(r1, Token{2});
    nullable.add_transition(r0, Label{'a'}, r1);

    const Simulator unrolled{nullable.build()};

    EXPECT_TRUE(unrolled.nullable());
    EXPECT_NE(unrolled.init_state(), r0);
    EXPECT_FALSE(unrolled.is_accepting(unrolled.init_state()));
    EXPECT_TRUE(unrolled.is_accepting(r0));
}

TEST(Dfa_test, Is_live_needs_a_state_reachable_from_the_start_that_can_still_reach_acceptance)
{
    // q3 is reached by c and can never accept, q4 and q5 can accept but nothing reaches them, so each fails one half of
    // the conjunction and only the a b route is live.
    const auto simulator{six_state_shape()};

    EXPECT_TRUE(simulator.is_live(0));
    EXPECT_TRUE(simulator.is_live(1));
    EXPECT_TRUE(simulator.is_live(2));
    EXPECT_FALSE(simulator.is_live(3));
    EXPECT_FALSE(simulator.is_live(4));
    EXPECT_FALSE(simulator.is_live(5));
}

TEST(Dfa_test, Accepted_resolves_the_token_of_an_accepting_state_and_nothing_for_any_other)
{
    // Two accepting states with different tokens, one reached and one on an island, beside a dead state and the start:
    // the token follows the flag, so the island's state resolves like a reached one.
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    builder.add_accept_state(q1, Token{1});
    builder.add_accept_state(q3, Token{3});
    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q0, Label{'c'}, q2);

    const Simulator simulator{builder.build()};

    EXPECT_EQ(simulator.accepted(q0), std::nullopt);
    EXPECT_EQ(simulator.accepted(q1), std::optional{Token{1}});
    EXPECT_EQ(simulator.accepted(q2), std::nullopt);
    EXPECT_EQ(simulator.accepted(q3), std::optional{Token{3}});
}

TEST(Dfa_test, Shortest_split_window_finds_windows_far_longer_than_the_state_count)
{
    // The shortest windows of the family are lcm(1..m) + m + 2 bytes long: 4, 6, 11, 18, 67 and 68 for m = 1 to 6,
    // already past the 31 and 39 states of the last two, which no search by length would reach.
    const std::vector<std::size_t> shortest{4, 6, 11, 18, 67, 68};

    for (std::size_t m{1}; m <= shortest.size(); ++m)
    {
        const auto simulator{lcm_family(m)};

        const auto [outcome, window, origin]{shortest_split_window(simulator)};

        ASSERT_EQ(outcome, Shortest_window::Outcome::found);

        EXPECT_EQ(window.size(), shortest[m - 1]);

        EXPECT_EQ(is_split_window(simulator, window), std::optional{origin});
    }
}

TEST(Dfa_test, Shortest_split_window_proves_absence_and_reports_an_exhausted_budget)
{
    // A single self-looping accepting state: every byte may continue the run or begin a new one, so no window ever
    // resolves where its covering token began, and the search exhausts its nodes.
    Builder run{};

    const auto q0{run.init_state()};
    const auto q1{run.next_state()};

    run.add_accept_state(q1, Token{1});
    run.add_transition(q0, Label{'a'}, q1);
    run.add_transition(q1, Label{'a'}, q1);

    const Simulator simulator{run.build()};

    const auto [absent, absent_window, absent_origin]{shortest_split_window(simulator)};

    EXPECT_EQ(absent, Shortest_window::Outcome::none);

    // A budget too small to settle the family's search is reported as such, never as an answer.
    const auto [small, small_window, small_origin]{shortest_split_window(lcm_family(5), 8)};

    EXPECT_EQ(small, Shortest_window::Outcome::budget);

    // Zero visits nothing, rather than meaning no limit.
    const auto [zero, zero_window, zero_origin]{shortest_split_window(lcm_family(1), 0)};

    EXPECT_EQ(zero, Shortest_window::Outcome::budget);
}

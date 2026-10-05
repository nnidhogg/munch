#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/minimize.hpp"
#include "munch/dfa/simulator.hpp"

using namespace munch::dfa;

namespace
{
/**
 * @brief The random tables the definition and minimization tests draw.
 */
constexpr std::size_t rounds{100};

/**
 * @brief The random inputs each of those tables is run on.
 */
constexpr std::size_t passes{50};

/**
 * @brief The random tables the discarded-token certificate tests draw.
 */
constexpr std::size_t trivia_rounds{200};

/**
 * @brief The symbols every random table and every enumerated input is drawn over.
 */
constexpr std::array alphabet{'a', 'b', 'c', 'd'};

/**
 * @brief A token stream, each token as its ID and its length.
 */
using Stream_t = std::vector<std::pair<std::size_t, std::size_t>>;

/**
 * @brief A tokenized input: the stream and how much of the input it covers.
 */
struct Tokenized
{
    /**
     * @brief Each token's ID and length, in input order.
     */
    Stream_t stream{};

    /**
     * @brief How much of the input was tokenized.
     */
    std::size_t consumed{};
};

/**
 * @brief Deterministic linear congruential generator, keeping the tests reproducible across runs and platforms.
 */
class Random
{
public:
    /**
     * @brief Seeds the generator.
     * @param seed The seed.
     */
    explicit Random(const unsigned seed) : seed_{seed} {}

    /**
     * @brief Draws the next number below a bound.
     * @param bound The bound, positive.
     * @return The number, from zero to the bound less one.
     */
    unsigned next(const unsigned bound)
    {
        seed_ = seed_ * multiplier + increment;

        return (seed_ >> 8U) % bound;
    }

private:
    /**
     * @brief The generator's multiplier.
     */
    static constexpr unsigned multiplier{1664525U};

    /**
     * @brief The generator's increment.
     */
    static constexpr unsigned increment{1013904223U};

    /**
     * @brief The generator's state.
     */
    unsigned seed_;
};

/**
 * @brief Builds a DFA with random transitions over 'a' to 'd' and random accept states.
 * @param random The generator drawn from.
 * @return The DFA.
 */
Dfa random_dfa(Random& random)
{
    Builder builder{};

    std::vector<Dfa::State_t> states{builder.init_state()};

    for (auto count{1U + random.next(8)}; count > 0; --count)
    {
        states.push_back(builder.next_state());
    }

    for (const auto from : states)
    {
        for (const auto symbol : alphabet)
        {
            if (random.next(100) < 70)
            {
                const auto target{states[random.next(states.size())]};

                builder.add_transition(from, Label{symbol}, target);
            }
        }
    }

    for (const auto state : states)
    {
        if (random.next(100) < 40)
        {
            builder.add_accept_state(state, Token{1 + random.next(3)});
        }
    }

    return builder.build();
}

/**
 * @brief Builds a random DFA containing a self-looping run, the shape a split point modulo discarded tokens lives in.
 *
 * random_dfa() wires transitions densely, so nearly every symbol is consumed from nearly every state and almost nothing
 * certifies. That makes it a good source of refutations and a poor source of positive cases. This one builds the two
 * shapes deliberately: a run state reached from the initial state on some symbols and looping back to itself on those
 * same symbols, which is what makes advancing from the run and from the initial state agree; and a state reached from
 * the initial state on symbols nothing else consumes, which is what the exact certificate admits. The remaining symbols
 * get arbitrary structure, and a run symbol is sometimes wired out of an unrelated state so that de-certifying cases
 * occur too.
 * @param random The generator drawn from.
 * @return The DFA.
 */
Dfa random_run_dfa(Random& random)
{
    Builder builder{};

    const auto init{builder.init_state()};

    const auto run{builder.next_state()};

    const auto solo{builder.next_state()};

    // One symbol is reserved for each shape up front, so neither partition the draws below index is empty.
    std::vector<char> run_symbols{'a'};

    std::vector<char> solo_symbols{'b'};

    std::vector<char> other_symbols{};

    for (const auto symbol : {'c', 'd'})
    {
        switch (random.next(3))
        {
        case 0:
            run_symbols.push_back(symbol);
            break;
        case 1:
            solo_symbols.push_back(symbol);
            break;
        default:
            other_symbols.push_back(symbol);
        }
    }

    for (const auto symbol : run_symbols)
    {
        builder.add_transition(init, Label{symbol}, run);

        builder.add_transition(run, Label{symbol}, run);
    }

    builder.add_accept_state(run, Token{1});

    // Consumed from the initial state and nowhere else, which is what the exact certificate admits.
    for (const auto symbol : solo_symbols)
    {
        builder.add_transition(init, Label{symbol}, solo);
    }

    builder.add_accept_state(solo, Token{2});

    std::vector<Dfa::State_t> others{};

    for (auto count{1U + random.next(3)}; count > 0; --count)
    {
        others.push_back(builder.next_state());
    }

    std::vector<Dfa::State_t> sources{init};

    sources.insert(sources.end(), others.begin(), others.end());

    // Nothing ever targets the initial state: re-entrancy would cost the solo symbols their exact certificate, and this
    // generator exists to produce them.
    for (const auto from : sources)
    {
        for (const auto symbol : other_symbols)
        {
            if (random.next(100) < 60)
            {
                const auto target{others[random.next(others.size())]};

                builder.add_transition(from, Label{symbol}, target);
            }
        }

        // Occasionally let an unrelated state consume a run symbol, which must cost that symbol its certificate.
        if (from != init && random.next(100) < 25)
        {
            // Two draws, the symbol first, each in its own statement so that their order is fixed.
            const auto symbol{run_symbols[random.next(run_symbols.size())]};

            const auto target{others[random.next(others.size())]};

            builder.add_transition(from, Label{symbol}, target);
        }
    }

    for (const auto state : others)
    {
        if (random.next(100) < 60)
        {
            builder.add_accept_state(state, Token{2 + random.next(2)});
        }
    }

    return builder.build();
}

/**
 * @brief Generates a random input over 'a' to 'e'; 'e' labels no transition, exercising rejection.
 * @param random The generator drawn from.
 * @return The input, up to nineteen bytes.
 */
std::string random_input(Random& random)
{
    std::string input{};

    for (auto length{random.next(20)}; length > 0; --length)
    {
        const auto symbol{static_cast<char>('a' + random.next(5))};

        input += symbol;
    }

    return input;
}

/**
 * @brief Simulates by stepping the DFA definition maps directly, mirroring Simulator::run.
 * @param dfa The DFA.
 * @param input The input.
 * @return The longest match from the input's start.
 */
Simulator::Match reference_run(const Dfa& dfa, const std::string& input)
{
    auto state{dfa.init_state()};

    Simulator::Match longest{.token = dfa.has_accept_token(state), .length = 0};

    for (std::size_t index{0}; index < input.size(); ++index)
    {
        const auto next{dfa.advance(state, input[index])};

        if (!next)
        {
            break;
        }

        state = *next;

        if (const auto token{dfa.has_accept_token(state)}; token)
        {
            longest = {.token = token, .length = index + 1};
        }
    }

    return longest;
}

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

/**
 * @brief Tokenizes the input, returning each token's ID and length, and how much of the input was consumed.
 * @param simulator The compiled token set.
 * @param input The input.
 * @return The stream and the consumed length.
 */
Tokenized tokenize(const Simulator& simulator, const std::string& input)
{
    Tokenized tokenized{};

    const auto collect{[&tokenized](const Token& token, const std::size_t length, std::uint64_t) {
        tokenized.stream.emplace_back(token.id(), length);
    }};

    tokenized.consumed = simulator.run_all(input.begin(), input.end(), collect);

    return tokenized;
}

/**
 * @brief Drops the tokens a caller discards, leaving what the weaker equivalence compares.
 * @param stream The stream.
 * @param trivia The IDs discarded.
 * @return The tokens kept, in order.
 */
Stream_t without(const Stream_t& stream, const std::unordered_set<std::size_t>& trivia)
{
    Stream_t kept{};

    const auto kept_kind{[&trivia](const std::pair<std::size_t, std::size_t>& token) {
        const auto& [id, length]{token};

        return !trivia.contains(id);
    }};

    std::ranges::copy_if(stream, std::back_inserter(kept), kept_kind);

    return kept;
}

/**
 * @brief Lists every string of length one to max_length over 'a' to 'd'.
 *
 * Exhaustive rather than sampled on purpose. A sampled corpus reports a symbol as safe whenever it happens never to
 * generate an input placing it inside a token, which is indistinguishable from the symbol genuinely being safe.
 * @param max_length The longest string.
 * @return The strings, shortest first and in lexicographic order within a length.
 */
std::vector<std::string> every_string(const std::size_t max_length)
{
    std::vector<std::string> corpus{};

    std::vector<std::string> frontier{""};

    for (std::size_t length{0}; length < max_length; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : frontier)
        {
            for (const auto symbol : alphabet)
            {
                next.push_back(prefix + symbol);
            }
        }

        corpus.insert(corpus.end(), next.begin(), next.end());

        frontier = std::move(next);
    }

    return corpus;
}

/**
 * @brief Picks a random subset of the token IDs random_dfa() assigns to stand for the kinds a caller discards.
 * @param random The generator drawn from.
 * @return The IDs.
 */
std::unordered_set<std::size_t> random_trivia(Random& random)
{
    std::unordered_set<std::size_t> trivia{};

    for (std::size_t kind{1}; kind <= 3; ++kind)
    {
        if (random.next(2) == 0)
        {
            trivia.insert(kind);
        }
    }

    return trivia;
}

} // namespace

TEST(Dfa_property_test, Simulator_agrees_with_the_definition_maps)
{
    Random random{1};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        const auto dfa{random_dfa(random)};

        const Simulator simulator{dfa};

        for (std::size_t pass{0}; pass < passes; ++pass)
        {
            const auto input{random_input(random)};

            ASSERT_EQ(simulator.run(input), reference_run(dfa, input)) << "round " << round << ", input " << input;
        }
    }
}

TEST(Dfa_property_test, Minimization_preserves_the_language)
{
    Random random{2};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        const auto dfa{random_dfa(random)};

        const Simulator original{dfa};

        const Simulator minimized{minimize(dfa)};

        for (std::size_t pass{0}; pass < passes; ++pass)
        {
            const auto input{random_input(random)};

            ASSERT_EQ(original.run(input), minimized.run(input)) << "round " << round << ", input " << input;
        }
    }
}

TEST(Dfa_property_test, Minimization_is_idempotent)
{
    Random random{3};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        const auto drawn{random_dfa(random)};

        const auto once{minimize(drawn)};

        const auto twice{minimize(once)};

        ASSERT_EQ(count_states(twice), count_states(once)) << "round " << round;
    }
}

TEST(Dfa_property_test, Re_entrant_initial_state_is_admitted_only_when_its_token_is_discarded)
{
    // The predicate exempts only a non-re-entrant initial state and otherwise lets q0 earn its place on the same three
    // conditions as any other state. These two cases pin that boundary: identical automata, differing only in whether
    // the token is discarded.
    Builder builder{};

    const auto start{builder.init_state()};

    builder.add_transition(start, Label{'a'}, start);

    builder.add_accept_state(start, Token{1});

    const auto dfa{minimize(builder.build())};

    // Kept: splitting "aa" turns one length-2 token into two length-1 tokens, which a caller can see.
    const std::vector<std::size_t> none{};

    const Simulator kept{dfa, none};

    ASSERT_FALSE(kept.is_split_point_ignoring('a'));

    // Discarded: the same split replaces one deleted token with two, and both vanish under the projection.
    const std::vector<std::size_t> discarded{1};

    const Simulator ignoring{dfa, discarded};

    ASSERT_TRUE(ignoring.is_split_point_ignoring('a'));

    // The exact certificate refuses it either way, since the initial state is re-entrant.
    ASSERT_FALSE(Simulator{dfa}.is_split_point('a'));
}

TEST(Dfa_property_test, Unreachable_states_do_not_de_certify_a_symbol)
{
    // Both certificates quantify over states that are reachable and co-accessible. Only a hand-built automaton
    // separates the two halves: subset construction discovers reachable subsets only, and minimization would drop the
    // detached state before a test could see it. So this one is assembled directly, and it pins the reachable half,
    // which a state no input enters must not be able to override.

    // q0 -b-> q1 accepting, beside a detached q2 -b-> q3 accepting that no input can enter.
    const Dfa detached{0, {{{0, Label{'b'}}, 1}, {{2, Label{'b'}}, 3}}, {{1, Token{1}}, {3, Token{1}}}};

    const std::vector<std::size_t> discarded{1};

    ASSERT_TRUE(Simulator{detached}.is_split_point('b'));

    ASSERT_TRUE((Simulator{detached, discarded}.is_split_point_ignoring('b')));

    // The same shape with q2 reachable through 'a'. It is now a live non-initial state consuming b into a state that
    // can still accept, so b is a byte that occurs inside a token and neither certificate may admit it.
    const Dfa reachable{
            0,
            {{{0, Label{'b'}}, 1}, {{0, Label{'a'}}, 2}, {{2, Label{'b'}}, 3}},
            {{1, Token{1}}, {3, Token{1}}}};

    ASSERT_FALSE(Simulator{reachable}.is_split_point('b'));

    ASSERT_FALSE((Simulator{reachable, discarded}.is_split_point_ignoring('b')));
}

TEST(Dfa_property_test, Trivia_modulo_certificate_survives_every_split_it_admits)
{
    Random random{4};

    const auto corpus{every_string(5)};

    std::size_t exercised{0};

    const auto check_cuts{[&exercised](
                                  const Simulator& simulator, const std::unordered_set<std::size_t>& trivia,
                                  const std::string& input, const char symbol, const std::size_t round) {
        const auto [serial, consumed]{tokenize(simulator, input)};

        // The promise is made only for input the token set tokenizes completely.
        if (consumed != input.size())
        {
            return;
        }

        for (std::size_t at{1}; at < input.size(); ++at)
        {
            if (input[at] != symbol)
            {
                continue;
            }

            ++exercised;

            const auto [left, left_consumed]{tokenize(simulator, input.substr(0, at))};

            const auto [right, right_consumed]{tokenize(simulator, input.substr(at))};

            ASSERT_EQ(left_consumed, at) << "round " << round << ", input " << input << " at " << at;

            ASSERT_EQ(right_consumed, input.size() - at) << "round " << round << ", input " << input << " at " << at;

            Stream_t spliced{left};

            spliced.insert(spliced.end(), right.begin(), right.end());

            ASSERT_EQ(without(spliced, trivia), without(serial, trivia))
                    << "round " << round << ", input " << input << " at " << at << " on '" << symbol << '\'';
        }
    }};

    for (std::size_t round{0}; round < trivia_rounds; ++round)
    {
        // Run-shaped grammars supply the positive cases; the dense ones keep the refuting coverage.
        const auto drawn{round % 2 == 0 ? random_run_dfa(random) : random_dfa(random)};

        const auto dfa{minimize(drawn)};

        const auto trivia{random_trivia(random)};

        const std::vector<std::size_t> ignored{trivia.begin(), trivia.end()};

        // The test runs the shipped predicate itself.
        const Simulator simulator{dfa, ignored};

        const auto certified{[&simulator](const char symbol) { return simulator.is_split_point_ignoring(symbol); }};

        for (const auto symbol : alphabet | std::views::filter(certified))
        {
            for (const auto& input : corpus)
            {
                ASSERT_NO_FATAL_FAILURE(check_cuts(simulator, trivia, input, symbol, round));
            }
        }
    }

    // More than a thousand admitted cuts were checked, a floor far below what the generators produce.
    EXPECT_GT(exercised, 1000U);
}

TEST(Dfa_property_test, Trivia_modulo_certificate_admits_every_exact_split_point)
{
    Random random{5};

    std::size_t exercised{0};

    for (std::size_t round{0}; round < trivia_rounds; ++round)
    {
        const auto drawn{round % 2 == 0 ? random_run_dfa(random) : random_dfa(random)};

        const auto dfa{minimize(drawn)};

        const Simulator exact{dfa};

        const auto certified{[&exact](const char symbol) { return exact.is_split_point(symbol); }};

        // Whatever a caller discards, a split point that reproduces the stream exactly reproduces it after deletion
        // too, so the weaker certificate can only ever admit more.
        for (const auto& trivia :
             {std::unordered_set<std::size_t>{}, random_trivia(random), std::unordered_set<std::size_t>{1, 2, 3}})
        {
            const std::vector<std::size_t> ignored{trivia.begin(), trivia.end()};

            const Simulator relaxed{dfa, ignored};

            for (const auto symbol : alphabet | std::views::filter(certified))
            {
                ++exercised;

                ASSERT_TRUE(relaxed.is_split_point_ignoring(symbol))
                        << "round " << round << " lost '" << symbol << '\'';
            }
        }
    }

    // More than fifty exact split points were checked against the relaxed certificate.
    EXPECT_GT(exercised, 50U);
}

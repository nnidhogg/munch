#include <gtest/gtest.h>

#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

#include "lexer/dfa/builder.hpp"
#include "lexer/dfa/minimize.hpp"
#include "lexer/dfa/simulator.hpp"

using namespace lexer::dfa;

namespace
{
/**
 * @brief Deterministic linear congruential generator, keeping the tests reproducible across runs and platforms.
 */
class Random
{
public:
    explicit Random(const unsigned seed) : seed_{seed} {}

    unsigned next(const unsigned bound) { return seed_ = seed_ * 1664525U + 1013904223U, (seed_ >> 8U) % bound; }

private:
    unsigned seed_;
};

/**
 * @brief Builds a DFA with random transitions over 'a' to 'd' and random accept states.
 */
Dfa random_dfa(Random& random)
{
    Builder builder;

    std::vector<Dfa::State_t> states{builder.init_state()};

    for (auto count{1U + random.next(8)}; count > 0; --count)
    {
        states.push_back(builder.next_state());
    }

    for (const auto from : states)
    {
        for (const auto symbol : {'a', 'b', 'c', 'd'})
        {
            if (random.next(100) < 70)
            {
                builder.add_transition(from, Label{symbol}, states[random.next(states.size())]);
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
 * @brief Generates a random input over 'a' to 'e'; 'e' labels no transition, exercising rejection.
 */
std::string random_input(Random& random)
{
    std::string input;

    for (auto length{random.next(20)}; length > 0; --length)
    {
        input += static_cast<char>('a' + random.next(5));
    }

    return input;
}

/**
 * @brief Reference simulation stepping the DFA definition maps directly, mirroring Simulator::run.
 */
Simulator::Result_t reference_run(const Dfa& dfa, const std::string& input)
{
    if (input.empty())
    {
        return {std::nullopt, 0};
    }

    auto state{dfa.init_state()};

    Simulator::Result_t result{Dfa::has_accept_token(dfa, state), 0};

    for (std::size_t index{0}; index < input.size(); ++index)
    {
        const auto next{Dfa::advance(dfa, state, input[index])};

        if (!next)
        {
            break;
        }

        state = *next;

        if (const auto token{Dfa::has_accept_token(dfa, state)}; token)
        {
            result = {token, index + 1};
        }
    }

    return result;
}

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

TEST(Dfa_property_test, Simulator_agrees_with_the_definition_maps)
{
    Random random{1};

    for (int round{0}; round < 100; ++round)
    {
        const auto dfa{random_dfa(random)};

        const Simulator simulator{dfa};

        for (int pass{0}; pass < 50; ++pass)
        {
            const auto input{random_input(random)};

            ASSERT_EQ(simulator.run(input), reference_run(dfa, input)) << "round " << round << ", input " << input;
        }
    }
}

TEST(Dfa_property_test, Minimization_preserves_the_language)
{
    Random random{2};

    for (int round{0}; round < 100; ++round)
    {
        const auto dfa{random_dfa(random)};

        const Simulator original{dfa};

        const Simulator minimized{minimize(dfa)};

        for (int pass{0}; pass < 50; ++pass)
        {
            const auto input{random_input(random)};

            ASSERT_EQ(original.run(input), minimized.run(input)) << "round " << round << ", input " << input;
        }
    }
}

TEST(Dfa_property_test, Minimization_is_idempotent)
{
    Random random{3};

    for (int round{0}; round < 100; ++round)
    {
        const auto once{minimize(random_dfa(random))};

        ASSERT_EQ(count_states(minimize(once)), count_states(once)) << "round " << round;
    }
}

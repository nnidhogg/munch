#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP

#include <optional>

#include "munch/common/concepts.hpp"
#include "munch/nfa/nfa.hpp"

namespace munch::nfa
{
/**
 * @brief Simulator for running an NFA over an input sequence.
 *
 * Provides static methods to simulate NFA execution over iterators or containers.
 */
class Simulator
{
public:
    /**
     * @brief The result of one match attempt: the matched token, if any, and the length of the match.
     */
    struct Match
    {
        std::optional<Token> token{};

        std::size_t length{};

        bool operator==(const Match&) const = default;
    };

    /**
     * @brief Runs the NFA simulation over a range defined by iterators.
     * @tparam Iterator Input iterator type.
     * @param nfa The NFA to simulate.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Iterator Iterator>
    [[nodiscard]] static Match run(const Nfa& nfa, Iterator begin, Iterator end)
    {
        auto states{nfa.epsilon_closure({nfa.init_state()})};

        Match result{nfa.has_accept_token(states), 0};

        for (Iterator current = begin; current != end && !states.empty(); ++current)
        {
            if (states = nfa.advance(states, *current); states.empty())
            {
                continue;
            }

            if (const auto token = nfa.has_accept_token(states); token)
            {
                result = {token, static_cast<std::size_t>(std::distance(begin, current) + 1)};
            }
        }

        return result;
    }

    /**
     * @brief Runs the NFA simulation over a container.
     * @tparam Container The container type (must be iterable).
     * @param nfa The NFA to simulate.
     * @param container The input container.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Iterable Container>
    [[nodiscard]] static auto run(const Nfa& nfa, const Container& container)
    {
        return run(nfa, std::begin(container), std::end(container));
    }
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP

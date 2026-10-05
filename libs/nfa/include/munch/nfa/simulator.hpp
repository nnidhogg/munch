#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP

#include <cstddef>
#include <optional>
#include <ranges>

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
        /**
         * @brief Equal when both attempts matched the same token at the same length.
         */
        bool operator==(const Match&) const = default;

        /**
         * @brief The token matched, or std::nullopt where nothing accepted.
         */
        std::optional<Token> token{};

        /**
         * @brief The length of input the match consumed, zero when nothing accepted.
         */
        std::size_t length{};
    };

    /**
     * @brief Runs the NFA simulation over a range defined by iterators.
     * @tparam Iterator Input iterator type.
     * @param nfa The NFA to simulate.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Byte_iterator Iterator>
    [[nodiscard]] static Match run(const Nfa& nfa, Iterator begin, Iterator end)
    {
        const Nfa::States_t initial{nfa.init_state()};

        auto states{nfa.epsilon_closure(initial)};

        Match result{.token = nfa.has_accept_token(states), .length = 0};

        // The bytes consumed so far, counted as the scan advances.
        std::size_t consumed{0};

        for (Iterator current{begin}; current != end && !states.empty(); ++current)
        {
            ++consumed;

            // Elements are read as the scanners read them, through unsigned char, so every byte-domain element type
            // reaches the transition alphabet; advance() takes char and std::byte converts to it only explicitly.
            const auto byte{static_cast<unsigned char>(*current)};

            const auto symbol{static_cast<char>(byte)};

            states = nfa.advance(states, symbol);

            if (states.empty())
            {
                continue;
            }

            if (const auto token{nfa.has_accept_token(states)})
            {
                result = {.token = token, .length = consumed};
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
    template <common::concepts::Byte_iterable Container>
    [[nodiscard]] static Match run(const Nfa& nfa, const Container& container)
    {
        return run(nfa, std::ranges::begin(container), std::ranges::end(container));
    }
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_SIMULATOR_HPP

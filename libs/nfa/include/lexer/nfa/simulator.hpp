#ifndef LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_SIMULATOR_HPP
#define LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_SIMULATOR_HPP

#include <optional>

#include "lexer/common/concepts.hpp"
#include "lexer/nfa/nfa.hpp"

namespace lexer::nfa
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
     * @brief The result type: a pair of the matched token (if any) and the length of the match.
     */
    using Result_t = std::pair<std::optional<Token>, std::size_t>;

    /**
     * @brief Runs the NFA simulation over a range defined by iterators.
     * @tparam Iterator Input iterator type.
     * @param nfa The NFA to simulate.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <common::concepts::Iterator Iterator>
    [[nodiscard]] static Result_t run(const Nfa& nfa, Iterator begin, Iterator end)
    {
        if (begin == end)
        {
            return {std::nullopt, 0};
        }

        auto states{Nfa::epsilon_closure(nfa, {nfa.init_state()})};

        Result_t result{Nfa::has_accept_token(nfa, states), 0};

        for (Iterator current = begin; current != end && !states.empty(); ++current)
        {
            if (states = Nfa::advance(nfa, states, *current); states.empty())
            {
                continue;
            }

            if (const auto token = Nfa::has_accept_token(nfa, states); token)
            {
                result = {token, std::distance(begin, current) + 1};
            }
        }

        return result;
    }

    /**
     * @brief Runs the NFA simulation over a container.
     * @tparam Container The container type (must be iterable).
     * @param nfa The NFA to simulate.
     * @param container The input container.
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <common::concepts::Iterable Container>
    [[nodiscard]] static auto run(const Nfa& nfa, const Container& container)
    {
        return run(nfa, std::begin(container), std::end(container));
    }
};

} // namespace lexer::nfa

#endif // LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_SIMULATOR_HPP

#ifndef LEXER_LIBS_CORE_INCLUDE_LEXER_CORE_LEXER_HPP
#define LEXER_LIBS_CORE_INCLUDE_LEXER_CORE_LEXER_HPP

#include <optional>

#include "lexer/common/concepts.hpp"
#include "lexer/dfa/dfa.hpp"
#include "lexer/dfa/simulator.hpp"

namespace lexer::core
{
/**
 * @brief The main Lexer class for tokenizing input using a DFA.
 *
 * Provides methods to tokenize input from iterators or containers, returning the matched token and length.
 */
class Lexer
{
public:
    /**
     * @brief The result type: a pair of the matched token (if any) and the length of the match.
     * @tparam T The token type (enum or integral).
     */
    template <typename T>
    using Result_t = std::pair<std::optional<T>, std::size_t>;

    /**
     * @brief Constructs a Lexer from a DFA.
     * @param dfa The DFA to use for tokenization.
     */
    explicit Lexer(const dfa::Dfa& dfa) : simulator_{dfa} {}

    /**
     * @brief Tokenizes input from a pair of iterators.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <typename T, common::concepts::Iterator Iterator>
        requires(std::integral<T> || std::is_enum_v<T>)
    [[nodiscard]] Result_t<T> tokenize(Iterator begin, Iterator end) const
    {
        const auto [token, offset]{simulator_.run(begin, end)};

        return {token ? std::optional<T>{static_cast<T>(token->id())} : std::nullopt, offset};
    }

    /**
     * @brief Tokenizes input from a container.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must be iterable).
     * @param container The input container.
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <typename T, common::concepts::Iterable Container>
        requires(std::integral<T> || std::is_enum_v<T>)
    [[nodiscard]] Result_t<T> tokenize(const Container& container) const
    {
        return tokenize<T>(std::begin(container), std::end(container));
    }

private:
    /**
     * @brief The simulator running the DFA the Lexer was constructed from.
     */
    dfa::Simulator simulator_;
};

} // namespace lexer::core

#endif // LEXER_LIBS_CORE_INCLUDE_LEXER_CORE_LEXER_HPP

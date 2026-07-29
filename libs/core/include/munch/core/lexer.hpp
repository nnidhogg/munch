#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>

#include "munch/common/concepts.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/simulator.hpp"

namespace munch::core
{
/**
 * @brief The main Lexer class for tokenizing input using a DFA.
 *
 * Provides methods to tokenize input from iterators or containers, returning the matched token and length.
 * Instances are only constructible through Builder::build(), which is the sole supported path from patterns to a
 * working Lexer.
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

    /**
     * @brief Tokenizes a whole input in one pass, invoking the sink once per matched token.
     *
     * Equivalent to calling tokenize() repeatedly at each token boundary, but the scan state stays live across
     * tokens, amortizing the per-call overhead. Random access is required because longest match may read past the
     * last accepting position and must resume from it.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving each matched token and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length) for every matched token, in input order. A sink returning a value
     *        convertible to bool stops the scan by returning false; the stopping token still counts as tokenized.
     * @return The number of input elements tokenized; anything short of the input's size means no token matched at
     *         the returned offset, unless the sink stopped the scan.
     */
    template <typename T, std::random_access_iterator Iterator, std::invocable<T, std::size_t> Sink>
        requires(std::integral<T> || std::is_enum_v<T>)
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink) const
    {
        return simulator_.run_all(begin, end, [&sink](const dfa::Token& token, const std::size_t length) {
            return sink(static_cast<T>(token.id()), length);
        });
    }

    /**
     * @brief Tokenizes a whole container in one pass, invoking the sink once per matched token.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must offer random access).
     * @tparam Sink Callable receiving each matched token and its length.
     * @param container The input container.
     * @param sink Invoked as sink(token, length) for every matched token, in input order.
     * @return The number of input elements tokenized; anything short of the container's size means no token matched
     *         at the returned offset.
     */
    template <typename T, common::concepts::Iterable Container, std::invocable<T, std::size_t> Sink>
        requires(std::integral<T> || std::is_enum_v<T>)
    std::size_t tokenize_all(const Container& container, Sink sink) const
    {
        return tokenize_all<T>(std::begin(container), std::end(container), std::move(sink));
    }

private:
    friend class Builder;

    /**
     * @brief Constructs a Lexer from a DFA.
     * @param dfa The DFA to use for tokenization.
     */
    explicit Lexer(const dfa::Dfa& dfa) : simulator_{dfa} {}

    /**
     * @brief The simulator running the DFA the Lexer was constructed from.
     */
    dfa::Simulator simulator_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

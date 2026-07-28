#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP

#include <concepts>
#include <type_traits>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/nfa/builder.hpp"
#include "munch/regex/regex.hpp"

namespace munch::core
{
/**
 * @brief Builder class for constructing a Lexer from regex patterns and tokens.
 *
 * Allows incremental registration of tokens with associated regex patterns and priorities, and builds the final Lexer.
 *
 * Each registered pattern is kept as a separate NFA and determinized on its own. The resulting per-pattern DFAs are
 * then recombined into a single NFA by Thompson construction, which is determinized once more to produce the DFA
 * backing the Lexer. Determinizing each pattern in isolation resolves the non-determinism introduced by its own
 * combinators before the patterns are merged, leaving the final subset construction to resolve only the
 * non-determinism between patterns, such as shared prefixes.
 */
class Builder
{
    /**
     * @brief A registered token pattern.
     */
    struct Pattern
    {
        /**
         * @brief The NFA recognizing the pattern.
         */
        nfa::Builder nfa;

        /**
         * @brief The token accepted by the pattern.
         */
        nfa::Token token;
    };

public:
    /**
     * @brief Registers a token with a regex pattern and priority.
     * @tparam T The token type (enum or integral).
     * @param regex The regex pattern for the token.
     * @param token The token value (enum or integer).
     * @param priority The priority for resolving conflicts (lower is higher priority).
     */
    template <typename T>
        requires(std::integral<T> || std::is_enum_v<T>)
    void add_token(const regex::Regex& regex, const T token, const std::size_t priority)
    {
        add_token(regex, {static_cast<std::size_t>(token), priority});
    }

    /**
     * @brief Builds and returns the constructed Lexer.
     * @return The constructed Lexer object.
     */
    [[nodiscard]] Lexer build() const;

protected:
    /**
     * @brief Returns the NFA combining the determinized patterns of the registered tokens.
     * @return The constructed NFA object.
     */
    [[nodiscard]] nfa::Nfa nfa() const;

    /**
     * @brief Returns the constructed DFA from the registered tokens.
     * @return The constructed DFA object.
     */
    [[nodiscard]] dfa::Dfa dfa() const;

private:
    /**
     * @brief Internal method to register a token with a regex and NFA token.
     * @param regex The regex pattern.
     * @param token The NFA token.
     */
    void add_token(const regex::Regex& regex, const nfa::Token& token);

    /**
     * @brief Merges the determinized patterns into a single NFA using Thompson construction.
     * @return The NFA builder representing all registered patterns.
     */
    [[nodiscard]] nfa::Builder thompson_construction() const;

    /**
     * @brief Converts an NFA to a DFA using subset construction.
     * @param nfa The NFA to convert.
     * @return The constructed DFA.
     */
    [[nodiscard]] static dfa::Dfa subset_construction(const nfa::Nfa& nfa);

    /**
     * @brief Internal patterns registered through add_token.
     */
    std::vector<Pattern> patterns_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP

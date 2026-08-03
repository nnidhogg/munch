#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector>

#include "munch/core/exceptions/state_limit_error.hpp"
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
public:
    /**
     * @brief Certified grammar diagnostics, computed from the merged automaton without building a Lexer.
     *
     * Subset construction only discovers reachable inputs, so a registered token missing from every accepting
     * state set is dead: no input ever tokenizes as it. The classic cause is a keyword registered at a worse
     * priority than the identifier pattern, which then wins the keyword's own spelling. A tie is two distinct
     * tokens accepting the same input at the same priority; the build resolves it deterministically but
     * arbitrarily, by the lower registered value, so a tie usually marks a priority the grammar author never
     * decided.
     */
    struct Diagnostics
    {
        /**
         * @brief Registered token values that never win any input, in registration order.
         */
        std::vector<std::size_t> dead_tokens;

        /**
         * @brief Pairs of token values accepting the same input at the same priority, each pair ascending.
         */
        std::vector<std::pair<std::size_t, std::size_t>> equal_priority_ties;
    };

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
     * @brief Declares the tokens the caller discards before using the stream.
     *
     * Only affects Lexer::is_split_point_ignoring(), which certifies split points under the weaker equivalence that
     * deletes these tokens from both streams before comparing. The exact certificate is unaffected, so declaring a
     * set never weakens a guarantee a caller was already relying on; it only makes the relaxed one available.
     * @tparam T The token type used with add_token().
     * @param tokens The tokens to treat as discarded.
     */
    template <typename T>
        requires(std::integral<T> || std::is_enum_v<T>)
    void set_ignored_tokens(const std::initializer_list<T> tokens)
    {
        ignored_.clear();

        for (const auto token : tokens)
        {
            ignored_.push_back(static_cast<std::size_t>(token));
        }
    }

    /**
     * @brief Declares the discarded tokens by ID, for callers holding them as values rather than as enumerators.
     * @param tokens The token IDs to treat as discarded.
     */
    void set_ignored_tokens(std::vector<std::size_t> tokens) { ignored_ = std::move(tokens); }

    /**
     * @brief Attaches a word to a token, delivered with every match of it in the built Lexer.
     *
     * A three-argument tokenize_all() sink receives it, so Mode_builder carries a token's mode action here rather
     * than looking one up. It rides the sink rather than tokenize()'s Match, which widening measured 2.5x.
     * @tparam T The token type used with add_token().
     * @param token The token to attach the word to.
     * @param word The word to report, zero meaning none.
     */
    template <typename T>
        requires(std::integral<T> || std::is_enum_v<T>)
    void set_token_payload(const T token, const std::uint64_t word)
    {
        payloads_.emplace_back(static_cast<std::size_t>(token), word);
    }

    /**
     * @brief Caps how many DFA states determinization may discover before build() and diagnose() throw.
     *
     * Subset construction has exponential worst cases, so a caller accepting untrusted token sets should cap the
     * states it may discover; build() and diagnose() then throw State_limit_error when a grammar runs into the
     * cap. Matching needs no guard against the automaton, which cannot backtrack the way a regex engine can, though
     * longest match re-reads after a failed longer match; see docs/limits.md. Zero, the default,
     * means unlimited. This caps determinization only: regex tree size, NFA expansion from large repetition counts, and
     * the number of registered patterns are the caller's to bound. The dominant allocation, the transition table, stays
     * within the cap times the number of symbol classes times four bytes per entry.
     */
    void set_state_limit(const std::size_t limit) noexcept { state_limit_ = limit; }

    /**
     * @brief Builds and returns the constructed Lexer.
     * @return The constructed Lexer object.
     */
    [[nodiscard]] Lexer build() const;

    /**
     * @brief Diagnoses the registered grammar; see Diagnostics.
     *
     * Walks the merged automaton once and leaves the builder untouched, so it can be called before build(), after
     * it, or not at all.
     */
    [[nodiscard]] Diagnostics diagnose() const;

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
     * @brief Collects the accepting candidates of every reachable determinization subset.
     *
     * The traversal is determinize()'s own, so diagnose() judges exactly the subsets the build discovers
     * rather than mirroring the walk with a second implementation. One entry per reachable subset holding at
     * least one accepting state, in discovery order.
     * @param nfa The NFA to walk.
     * @param state_limit The largest number of subsets to discover before throwing; zero means unlimited.
     * @return The accepting candidate tokens, one list per accepting subset.
     * @throws State_limit_error If the state limit is exceeded.
     */
    [[nodiscard]] static std::vector<std::vector<nfa::Token>> reachable_candidates(
            const nfa::Nfa& nfa, std::size_t state_limit);

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
    [[nodiscard]] nfa::Builder merged_nfa() const;

    /**
     * @brief The determinization cap set_state_limit() installed; zero means unlimited.
     */
    std::size_t state_limit_{0};

    /**
     * @brief Internal patterns registered through add_token.
     */
    std::vector<Pattern> patterns_;

    /**
     * @brief The token IDs set_ignored_tokens() declared, passed to the Lexer so it can certify the weaker split
     *        points. Empty by default, in which case the two certificates coincide.
     */
    std::vector<std::size_t> ignored_;

    /**
     * @brief The token payloads set_token_payload() attached, passed to the Lexer at construction so it stays
     *        immutable afterwards.
     */
    std::vector<std::pair<std::size_t, std::uint64_t>> payloads_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_BUILDER_HPP

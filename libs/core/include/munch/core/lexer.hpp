#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/simulator.hpp"

namespace munch::core
{
/**
 * @brief The main Lexer class for tokenizing input using a DFA.
 *
 * Provides methods to tokenize input from iterators or containers, returning the matched token and length.
 * Instances are obtainable through Builder::build(), the one supported path from patterns to a working Lexer.
 */
class Lexer
{
public:
    /**
     * @brief The result of one match attempt: the matched token, if any, and the length of the match.
     * @tparam T The token type (enum or integral).
     */
    template <typename T>
    struct Match
    {
        std::optional<T> token{};

        std::size_t length{};

        bool operator==(const Match&) const = default;
    };

    /**
     * @brief Tokenizes input from a pair of iterators.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Token_id T, common::concepts::Byte_iterator Iterator>
    [[nodiscard]] Match<T> tokenize(Iterator begin, Iterator end) const
    {
        const auto [token, offset]{simulator_.run(begin, end)};

        return {.token = token ? std::optional<T>{static_cast<T>(token->id())} : std::nullopt, .length = offset};
    }

    /**
     * @brief Tokenizes input from a container.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must be iterable).
     * @param container The input container.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Token_id T, common::concepts::Byte_iterable Container>
    [[nodiscard]] Match<T> tokenize(const Container& container) const
    {
        return tokenize<T>(std::ranges::begin(container), std::ranges::end(container));
    }

    /**
     * @brief Tokenizes a whole input in one call, invoking the sink once per consumed token.
     *
     * Consumes the same positive-width tokens, IDs and lengths, as calling tokenize() repeatedly at each token
     * boundary, invoking the sink after each and stopping when the sink returns false, but the loop stays in one call
     * across tokens, amortizing the per-call overhead; the automaton restarts at each token as tokenize() does.
     * Random access is required because longest match may read past the last accepting position and must resume
     * from it.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving each consumed token and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length) for every consumed token, in input order, or as
     *        sink(token, length, payload) where the sink accepts that and the payload is what
     *        Builder::set_token_payload() attached; a sink accepting both is called with two. A sink returning a
     *        value convertible to bool stops the scan by returning false; the stopping token still counts as
     *        tokenized.
     * @return The number of input elements tokenized; anything short of the input's size means the scan stopped at
     *         the returned offset: no token matched there, a zero-width token did, or the sink returned false.
     */
    template <
            common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator,
            common::concepts::Token_sink<T> Sink>
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink) const
    {
        // The payload is always delivered and dropped here for sinks that do not want it, so the scan itself has
        // one sink shape rather than one per arity. A sink accepting both arities, which a generic or variadic one
        // does, is called with two: that is what it received before the payload existed.
        return simulator_.run_all(
                begin, end, [&sink](const dfa::Token& token, const std::size_t length, const std::uint64_t payload) {
                    if constexpr (std::invocable<Sink&, T, std::size_t>)
                    {
                        return sink(static_cast<T>(token.id()), length);
                    }
                    else
                    {
                        return sink(static_cast<T>(token.id()), length, payload);
                    }
                });
    }

    /**
     * @brief Tokenizes a whole container in one call, invoking the sink once per consumed token.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must offer random access).
     * @tparam Sink Callable receiving each consumed token and its length, or those and its payload.
     * @param container The input container.
     * @param sink As for the iterator form above.
     * @return The number of input elements tokenized; anything short of the container's size means the scan stopped
     *         at the returned offset: no token matched there, a zero-width token did, or the sink returned false.
     */
    template <
            common::concepts::Token_id T, common::concepts::Random_access_byte_iterable Container,
            common::concepts::Token_sink<T> Sink>
    std::size_t tokenize_all(const Container& container, Sink sink) const
    {
        return tokenize_all<T>(std::ranges::begin(container), std::ranges::end(container), std::move(sink));
    }

    /**
     * @brief Returns whether the given symbol is a certified safe split point of this lexer's token set.
     *
     * For input the serial scan tokenizes completely, splitting immediately before a safe split point produces
     * the identical token stream, so such symbols mark chunk boundaries at which one large input may be processed
     * in independent pieces; on malformed input see tokenize_all_parallel() for the weaker prefix guarantee. Only
     * the useful subset is reported: a symbol no live state consumes, a live state being one reachable from the
     * initial state that can still reach an accepting one, is safe merely vacuously and answers false. The property
     * is decided from the compiled transition table; the derivation is dfa::Simulator::is_split_point()'s.
     */
    [[nodiscard]] bool is_split_point(const char symbol) const noexcept { return simulator_.is_split_point(symbol); }

    /**
     * @brief Reports whether the symbol is a safe split point once the discarded tokens are deleted.
     *
     * Never smaller than is_split_point(), and equal to it unless the builder was told which tokens are discarded.
     * For input the serial scan tokenizes completely, chunks cut here reproduce the serial stream once tokens of
     * those kinds are removed from both, so a caller that keeps them must use is_split_point(). The completeness
     * condition is not decoration: past the offset where the serial scan first fails, a chunk cut here can emit
     * kept tokens that scan never reaches. Note also that chunk_boundaries() and tokenize_all_parallel() plan with
     * the exact certificate, so acting on this answer means planning boundaries yourself. The derivation is
     * dfa::Simulator::is_split_point_ignoring()'s.
     * @param symbol The symbol to test.
     * @return True if the symbol can begin a token and every occurrence is safe under that weaker equivalence;
     *         symbols satisfying the condition only vacuously report false.
     */
    [[nodiscard]] bool is_split_point_ignoring(const char symbol) const noexcept
    {
        return simulator_.is_split_point_ignoring(symbol);
    }

    /**
     * @brief Decides whether the given byte string is a certified split window, returning the covering origin.
     *
     * The multi-byte generalization of is_split_point(): where the byte certificate promises that every occurrence
     * begins a token, a certified window (W, o) promises that in every completely tokenizable input containing W, the
     * token covering the occurrence's final byte begins exactly o bytes into it. On non-empty, non-nullable token sets
     * the two coincide at length one; a nullable token set, one in which some token matches the empty string, can
     * certify bytes while every window is refused here, and an empty token set refuses everything on both sides. The
     * certificate is conditional on occurrence and this call does not establish that one exists; a caller that found W
     * in its own input holds an occurrence, the promise applies to it on completely tokenizable input, and on malformed
     * input the window promise carries nothing at all, the consequence chunk_boundaries_with_windows() documents.
     * Refusals are model-relative and conservative, never proof that no certificate exists. Decided from the compiled
     * transition table; the derivation is dfa::Simulator::is_split_window()'s.
     */
    [[nodiscard]] std::optional<std::size_t> is_split_window(const std::string_view window) const
    {
        return simulator_.is_split_window(window);
    }

    /**
     * @brief The byte string every certified split window provably contains, or empty when none is proved.
     *
     * Every certified split window of this token set contains this string with at least one byte after it, so
     * the window planner narrows its candidate windows to the string's occurrences whenever it is non-empty, with
     * identical plans either way; empty means no such string is proved and the exhaustive walk stands. Decided
     * from the compiled transition table; the derivation and its proof are dfa::Simulator::mandatory_core()'s.
     * @return The proved mandatory core, or an empty view.
     */
    [[nodiscard]] std::string_view mandatory_core() const noexcept { return simulator_.mandatory_core(); }

    /**
     * @brief Computes chunk boundaries for parallel tokenization at certified safe split points.
     *
     * Each interior boundary is the first certified split point at or after the later of its equal-division target and
     * one past the previous boundary, so every chunk starts at a symbol that can only begin a token; for completely
     * tokenizable input, concatenating the chunk-local streams reproduces the whole-input token stream. The byte
     * certificate is a property of single transitions rather than of whole inputs, which is what upholds
     * tokenize_all_parallel()'s serial-prefix guarantee on malformed input; past the serial failure offset that prefix
     * relation is all the certificate promises. When the token set certifies no usable points, the result is one chunk
     * spanning the whole input, so parallel scanning degenerates to the serial scan rather than splitting unsafely; on
     * such token sets chunk_boundaries_with_windows() can recover cuts, at the price of a guarantee conditional on the
     * input being completely tokenizable.
     * @tparam Iterator Random access iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer result when certified points are scarce, and zero
     *        behaves as one, the whole input as a single chunk.
     * @return Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries(
            Iterator begin, Iterator end, const std::size_t chunks) const
    {
        const auto size{static_cast<std::size_t>(end - begin)};

        std::vector<std::size_t> boundaries{0};

        // A token set with no certified symbol that can occur in valid input yields the single whole-input chunk
        // without scanning. The test is the useful set, not the full certificate: symbols no live state consumes
        // certify vacuously, and searching for one scans to the end of the input and finds nothing.
        const auto any_certified{simulator_.has_split_points()};

        // A chunk needs at least one byte, so asking for more chunks than bytes only adds iterations that can find
        // nothing.
        const auto usable{std::min(chunks, size)};

        // The ideal offsets are size * index / usable, but that product overflows for a large input divided very
        // finely, and so does any form that multiplies the remainder by the index: both are bounded below by
        // (usable - 1) squared. Accumulating instead multiplies nothing. Adding the quotient each step and carrying
        // the remainder when it fills a whole divisor yields exactly the same offsets, with target never exceeding
        // size and carry never reaching usable, so no intermediate can leave the range the input already occupies.
        const auto step{usable == 0 ? std::size_t{0} : size / usable};

        const auto step_remainder{usable == 0 ? std::size_t{0} : size % usable};

        std::size_t target{0};

        std::size_t carry{0};

        for (std::size_t index{1}; any_certified && index < usable; ++index)
        {
            target += step;

            if (carry += step_remainder; carry >= usable)
            {
                ++target;

                carry -= usable;
            }

            // Start strictly after the previous boundary, not at the ideal offset. Two ideal offsets can walk
            // forward onto the same certified byte; resuming from the ideal offset would rediscover it, drop it as
            // a duplicate, and lose the next certified byte along with the chunk it would have opened.
            auto offset{std::max(target, boundaries.back() + 1)};

            while (offset < size && !is_split_point(static_cast<char>(begin[static_cast<std::ptrdiff_t>(offset)])))
            {
                ++offset;
            }

            if (offset < size)
            {
                boundaries.push_back(offset);
            }
        }

        boundaries.push_back(size);

        return boundaries;
    }

    /**
     * @brief Computes chunk boundaries for parallel tokenization of a whole container.
     */
    template <common::concepts::Random_access_byte_iterable Container>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries(const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries(std::ranges::begin(container), std::ranges::end(container), chunks);
    }

    /**
     * @brief Computes chunk boundaries like chunk_boundaries(), additionally recovering cuts from certified
     *        split windows where the token set certifies no usable byte.
     *
     * From the later of each equal-division target and one past the previous boundary the input is walked for the first
     * occurrence of a window of two to four bytes that is_split_window() certifies, and the cut is placed at the
     * occurrence plus the reported origin. Each window decision is memoized per distinct byte string, so those
     * decisions, the costly part, are bounded by the distinct windows tried, while the positional walk and its memo
     * lookups scale with the positions examined. A nullable token set contributes no windows, since the window proof
     * excludes it, though its byte plan, when any, stands untouched; when neither certificate offers cuts, the single
     * whole-input chunk results.
     *
     * The window guarantee is conditional where the byte certificate's is not: a certified window pins the
     * covering token's origin at occurrences in completely tokenizable input, a property of the whole input
     * rather than of single transitions. On malformed input a window cut can land inside a token of the serial
     * scan's doomed suffix, the fragments can each consume fully, and the serial stream is then not a prefix of
     * the concatenated chunk streams; full per-chunk consumption does not imply the serial scan succeeds. Use
     * these boundaries when the input is known completely tokenizable, or validate the result downstream;
     * tokenize_all_parallel() deliberately plans with chunk_boundaries() and never uses windows implicitly.
     * @tparam Iterator Random access iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer result when neither certificate offers cuts.
     * @return Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries_with_windows(
            Iterator begin, Iterator end, const std::size_t chunks) const
    {
        const auto size{static_cast<std::size_t>(end - begin)};

        auto boundaries{chunk_boundaries(begin, end, chunks)};

        if (boundaries.size() > 2 || size < 2 || simulator_.has_split_points() || simulator_.nullable())
        {
            return boundaries;
        }

        boundaries.pop_back();

        const auto usable{std::min(chunks, size)};

        const auto step{usable == 0 ? std::size_t{0} : size / usable};

        const auto step_remainder{usable == 0 ? std::size_t{0} : size % usable};

        const auto core{simulator_.mandatory_core()};

        // A proved core longer than the longest window minus its trailing byte admits no candidate at all:
        // no window certifies at these lengths, so every target refuses, exactly as the exhaustive walk
        // would conclude after scanning to the end of the input.
        if (!core.empty() && core.size() + 1 > longest_window_)
        {
            boundaries.push_back(size);

            return boundaries;
        }

        Window_memo memo;

        // No core occurrence begins at or after this offset; a scan that drains the input tightens it, so
        // targets falling in a tail already proved occurrence-free refuse without rescanning it.
        auto barren{size};

        std::size_t window_target{0};

        std::size_t window_carry{0};

        for (std::size_t index{1}; index < usable; ++index)
        {
            window_target += step;

            if (window_carry += step_remainder; window_carry >= usable)
            {
                ++window_target;

                window_carry -= usable;
            }

            const auto floor{std::max(window_target, boundaries.back() + 1)};

            const auto cut{
                    core.empty() ? window_cut(memo, begin, size, floor) :
                                   window_cut_at_core(memo, begin, size, floor, core, barren)};

            if (cut)
            {
                boundaries.push_back(*cut);
            }
        }

        boundaries.push_back(size);

        return boundaries;
    }

    /**
     * @brief Range overload of chunk_boundaries_with_windows(begin, end, chunks).
     */
    template <common::concepts::Random_access_byte_iterable Container>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries_with_windows(
            const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries_with_windows(std::ranges::begin(container), std::ranges::end(container), chunks);
    }

    /**
     * @brief Finds the first position at or after the given offset that a certificate marks as a token start.
     *
     * One forward walk consulting both certificate kinds at every position: a certified byte answers at its
     * own position, and a certified window of two to four bytes answers at its occurrence plus the certified
     * origin. Unlike the planners, byte certificates do not switch the window search off; a nullable token set
     * contributes no windows, because only the window proof excludes it, while its byte certificates, when
     * any, stand. The answer is the first certificate met in evidence order, the order in which the walk meets
     * the supporting evidence, which is not always the smallest answerable position: a window met earlier can
     * answer a byte or two past one met later, and windows beginning before the given offset are not considered.
     *
     * The contract is complete-repair invariance: in every completely tokenizable replacement of the input before the
     * answer's supporting evidence, the answer's image begins a token of the repaired segmentation, which is more than
     * the vacuous observation that a suffix which tokenizes begins a token. The evidence is the certified byte itself
     * or the whole window occurrence, beginning at most three bytes before the answer; a repair that alters the
     * evidence forfeits the guarantee, and the existence of any tokenizable repair is not promised, so where the damage
     * has no fix the guarantee holds vacuously. The certificate speaks for this automaton alone; under mode-driven
     * scanning that scoping is load-bearing. When no certificate of the searched kinds and lengths exists at or after
     * the offset, there is no answer.
     * @param input The input being scanned.
     * @param from The offset the search starts at; at or past the input's size finds nothing.
     * @return The first certified token-start position, or std::nullopt when no certified byte and no certified
     *         window of two to four bytes lies at or after the offset, the widths the search consults.
     */
    [[nodiscard]] std::optional<std::size_t> next_certified_start(
            const std::string_view input, const std::size_t from) const
    {
        const auto found{next_certified_evidence(input, from)};

        return found ? std::optional{found->start} : std::nullopt;
    }

    /**
     * @brief The answer a certificate supports, together with the evidence that supports it.
     *
     * The start is the certified token-start position; the evidence is the certified byte itself
     * (evidence_begin == start, one byte) or the whole window occurrence, and the guarantee is exactly the
     * certificate's: the repair-invariance transfer requires the evidence interval to survive whatever changed
     * and the repaired scan to commit through it. A caller comparing evidence_begin against a known-clean
     * lower bound decides the survival half alone, whether the evidence outlasted the damage; the transfer to the
     * intended input additionally needs
     * that input's scan to reach the evidence, with the whole intended input being completely tokenizable
     * the simplest sufficient condition.
     */
    struct Certified_start
    {
        std::size_t start;          ///< The certified token-start position, the answer.
        std::size_t evidence_begin; ///< First byte of the supporting evidence.
        std::size_t evidence_end;   ///< One past the supporting evidence's last byte.
        bool window;                ///< True for window evidence; false for a certified byte at the start itself.
    };

    /**
     * @brief The certificate walk of next_certified_start(), reporting the supporting evidence with the answer.
     *
     * Same walk, same evidence order, same refusal; the position-only form above is this one with the evidence
     * dropped. The evidence lies wholly at or after the search offset by construction, which is what makes the
     * comparison against a caller's clean bound meaningful; the guarantee is Certified_start's.
     * @param input The input being scanned.
     * @param from The offset the search starts at; at or past the input's size finds nothing.
     * @return The first certified answer in evidence order with its evidence interval, or std::nullopt.
     */
    [[nodiscard]] std::optional<Certified_start> next_certified_evidence(
            const std::string_view input, const std::size_t from) const
    {
        const auto bytes{simulator_.has_split_points()};

        const auto windows{!simulator_.nullable()};

        if (!bytes && !windows)
        {
            return std::nullopt;
        }

        Window_memo memo;

        for (std::size_t at{from}; at < input.size(); ++at)
        {
            if (bytes && is_split_point(input[at]))
            {
                return Certified_start{.start = at, .evidence_begin = at, .evidence_end = at + 1, .window = false};
            }

            if (!windows)
            {
                continue;
            }

            const auto limit{std::min(longest_window_, input.size() - at)};

            for (std::size_t length{2}; length <= limit; ++length)
            {
                if (const auto origin{certified_origin(memo, input.data(), at, length)})
                {
                    return Certified_start{
                            .start = at + *origin,
                            .evidence_begin = at,
                            .evidence_end = at + length,
                            .window = true};
                }
            }
        }

        return std::nullopt;
    }

    /**
     * @brief The lag of the token set: the longest run of nonaccepting states a scan can traverse after leaving
     * an accepting state, or nothing when that run is unbounded.
     *
     * States that can no longer reach an accepting one still count: a failed lookahead buffers bytes whether or
     * not the excursion could still accept, so every defined continuation counts. Zero is the premise under
     * which a scheme restarting at every accept executes serial maximal munch exactly; a bounded value prices
     * the checkpoint a rollback-aware scheme must carry.
     * @return The lag, or std::nullopt when a post-accept nonaccepting cycle makes it unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> lag() const { return simulator_.lag(); }

    /**
     * @brief Whether every byte that opens a post-accept nonaccepting stretch is dead from the initial state.
     *
     * A rescue is a rollback after a failed lookahead that lets the scan continue where a scheme restarting
     * at every accept would have declared the input malformed. On a rescue-free token set no such rollback can
     * succeed, so the two schemes agree on every input. The gate is sufficient and not necessary: on
     * {a, abc, bc} it returns false though no rescue exists there, and zero-lag sets pass vacuously.
     * @return True when no stretch-opening byte starts a viable token from the initial state; false says only
     * that this gate did not establish rescue-freeness.
     */
    [[nodiscard]] bool rescue_free() const { return simulator_.rescue_free(); }

    /**
     * @brief The first anchored-certified start in the tail at or after the offset.
     *
     * The anchored counterpart of next_certified_start(), exact where the walk is merely sound: with the
     * tail's end known to be the end of the input, every completely tokenizable repair of whatever preceded
     * the tail places a token boundary at the returned position. That quantifier is the whole contract; the
     * certificates' guarantee over repairs that merely reach their evidence is a different one, which this
     * decider does not speak about. A tail beyond repair refuses rather than answering vacuously, and nullable
     * token sets are refused outright.
     * @param tail The preserved suffix of the input, its end the end of the input.
     * @param from The offset the search starts at; at or past the tail's size finds nothing.
     * @return The first anchored-certified position, or std::nullopt when none exists or no repair does.
     */
    [[nodiscard]] std::optional<std::size_t> next_anchored_start(
            const std::string_view tail, const std::size_t from) const
    {
        return simulator_.next_anchored_start(tail, from);
    }

    /**
     * @brief A shortest repair for the tail, empty when it already tokenizes; nothing when none exists.
     * @param tail The preserved suffix of the input.
     * @return A minimal repair, or std::nullopt when the tail is beyond repair or the set is nullable.
     */
    [[nodiscard]] std::optional<std::string> minimal_repair(const std::string_view tail) const
    {
        return simulator_.minimal_repair(tail);
    }

    /**
     * @brief Whether another token set cuts some input both tokenize differently, with a witness.
     *
     * The question a tokenizer change asks, answered from the two compiled tables rather than from a corpus, so a
     * negative covers every input instead of the ones a suite happens to hold. The derivation is
     * dfa::Simulator::boundary_difference()'s.
     * @param other The lexer to compare against.
     * @param cap The largest number of product states to visit before giving up.
     * @return The witness and whether the search was exhaustive; an empty witness means identical only when it was.
     */
    [[nodiscard]] dfa::Simulator::Difference boundary_difference(
            const Lexer& other, const std::size_t cap = 1U << 20U) const
    {
        return simulator_.boundary_difference(other.simulator_, cap);
    }

    /**
     * @brief Tokenizes one input as concurrent chunks split at certified safe split points.
     *
     * The input is divided by chunk_boundaries() and each chunk is scanned by tokenize_all() on its own thread,
     * the last on the calling thread; for input the serial scan tokenizes completely, certification guarantees the
     * concatenated per-chunk token streams are identical to the serial scan's. When no token matches somewhere,
     * the serial stream is a prefix of the concatenation and chunks past the failure still scan independently, so
     * treat the output as a successful tokenization only after checking every returned consumed length. Within a chunk
     * the sink is invoked in input order. Across chunks it is invoked concurrently, so it must be safe to call from
     * different threads for different chunk indices, which per-chunk state indexed by the chunk achieves without
     * locking; give hot per-chunk accumulators their own cache lines, as adjacent counters false-share and cost real
     * scaling. There is no early-stop form.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving the chunk index, each consumed token, and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer are scanned when certified points are scarce, and zero
     *        behaves as one, the serial scan on the calling thread.
     * @param sink Invoked as sink(chunk, token, length) for every consumed token.
     * @return The number of input elements tokenized per chunk, aligned with chunk_boundaries(begin, end,
     *         chunks); an entry short of its chunk's size means the scan stopped at that offset of the chunk, no
     *         token matching there or a zero-width one doing so; this form's sink cannot stop a chunk.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator, typename Sink>
        requires std::invocable<Sink&, std::size_t, T, std::size_t>
    [[nodiscard]] std::vector<std::size_t> tokenize_all_parallel(
            Iterator begin, Iterator end, const std::size_t chunks, Sink sink) const
    {
        const auto boundaries{chunk_boundaries(begin, end, chunks)};

        std::vector<std::size_t> consumed(boundaries.size() - 1, 0);

        std::mutex failure_mutex;

        std::exception_ptr failure;

        // An exception escaping a jthread's callable calls std::terminate, so a throwing sink would abort the
        // process on a worker while the caller's own chunk merely propagated. Keep the first one and rethrow it
        // after every worker has joined, so both paths behave alike and no thread outlives the throw.
        const auto scan{[&](const std::size_t chunk) {
            try
            {
                consumed[chunk] = tokenize_all<T>(
                        begin + static_cast<std::ptrdiff_t>(boundaries[chunk]),
                        begin + static_cast<std::ptrdiff_t>(boundaries[chunk + 1]),
                        [&sink, chunk](const T token, const std::size_t length) { sink(chunk, token, length); });
            }
            catch (...)
            {
                const std::scoped_lock lock{failure_mutex};

                if (!failure)
                {
                    failure = std::current_exception();
                }
            }
        }};

        {
            std::vector<std::jthread> workers;

            workers.reserve(consumed.size() - 1);

            for (std::size_t chunk{0}; chunk + 1 < consumed.size(); ++chunk)
            {
                workers.emplace_back(scan, chunk);
            }

            scan(consumed.size() - 1);
        }

        if (failure)
        {
            std::rethrow_exception(failure);
        }

        return consumed;
    }

    /**
     * @brief Tokenizes a whole container as concurrent chunks split at certified safe split points.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterable Container, typename Sink>
        requires std::invocable<Sink&, std::size_t, T, std::size_t>
    [[nodiscard]] std::vector<std::size_t> tokenize_all_parallel(
            const Container& container, const std::size_t chunks, Sink sink) const
    {
        return tokenize_all_parallel<T>(
                std::ranges::begin(container), std::ranges::end(container), chunks, std::move(sink));
    }

private:
    // The Builder is the only construction path: a Lexer exists exclusively over a DFA the Builder
    // compiled, so the constructors below stay private and the friendship is the whole public door.
    friend class Builder;

    /**
     * @brief The longest window the planners try, four bytes. A grammar needing longer windows degrades to
     * fewer chunks, never to an unsafe cut. The shortest tried is two, and that bound is not a guard: the window
     * planners run only when no exact byte certifies and the set is not nullable, where the length-one equivalence
     * theorem makes every one-byte window refuse, so skipping length one is provably inert rather than something a test
     * could pin.
     */
    static constexpr std::size_t longest_window_{4};

    /**
     * @brief One decision per distinct byte string per plan: memoization caps the window decisions at the
     * distinct windows tried, while the position loops and their lookups remain per position examined.
     */
    using Window_memo = std::map<std::string, std::optional<std::size_t>, std::less<>>;

    /**
     * @brief One input element read as the scanners read it, through unsigned char, so every byte-domain
     * element type forms the same memo key; the string constructor's implicit conversion would reject
     * std::byte.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] static char window_byte(Iterator begin, const std::size_t at)
    {
        return static_cast<char>(static_cast<unsigned char>(begin[static_cast<std::ptrdiff_t>(at)]));
    }

    /**
     * @brief The memoized window decision at one occurrence: the certified origin, if any.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> certified_origin(
            Window_memo& memo, Iterator begin, const std::size_t at, const std::size_t length) const
    {
        std::string window;

        window.reserve(length);

        for (std::size_t offset{0}; offset < length; ++offset)
        {
            window.push_back(window_byte(begin, at + offset));
        }

        auto found{memo.find(window)};

        if (found == memo.end())
        {
            const auto verdict{is_split_window(window)};

            found = memo.emplace(std::move(window), verdict).first;
        }

        return found->second;
    }

    /**
     * @brief The exhaustive window search: every position from the floor, lengths ascending, first
     * certificate wins; the cut is the occurrence plus the certified origin.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> window_cut(
            Window_memo& memo, Iterator begin, const std::size_t size, const std::size_t floor) const
    {
        for (auto occurrence{floor}; occurrence + 2 <= size; ++occurrence)
        {
            const auto limit{std::min(longest_window_, size - occurrence)};

            for (std::size_t length{2}; length <= limit; ++length)
            {
                if (const auto origin{certified_origin(memo, begin, occurrence, length)})
                {
                    return occurrence + *origin;
                }
            }
        }

        return std::nullopt;
    }

    /**
     * @brief The core-filtered window search: every certifying window provably contains the core with a
     * byte after it, so candidates exist only where the core occurs, and visiting them in the exhaustive
     * walk's own position-then-length order gives that walk's plan, refusals included. Positions are still
     * scanned one by one, but for a byte comparison each; windows are built and certified only at
     * occurrences. The barren offset tightens across calls: once a scan drains the input, targets falling in
     * a tail already proved occurrence-free refuse without rescanning it.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::optional<std::size_t> window_cut_at_core(
            Window_memo& memo, Iterator begin, const std::size_t size, const std::size_t floor,
            const std::string_view core, std::size_t& barren) const
    {
        if (floor >= barren)
        {
            return std::nullopt;
        }

        const auto matches{[&](const std::size_t at) {
            for (std::size_t offset{0}; offset < core.size(); ++offset)
            {
                if (window_byte(begin, at + offset) != core[offset])
                {
                    return false;
                }
            }

            return true;
        }};

        std::size_t cursor{floor};

        std::size_t latest{floor};

        const auto next_occurrence{[&]() -> std::optional<std::size_t> {
            for (; cursor + core.size() <= size; ++cursor)
            {
                if (matches(cursor))
                {
                    latest = cursor + 1;

                    return cursor++;
                }
            }

            barren = std::min(barren, latest);

            return std::nullopt;
        }};

        std::vector<std::pair<std::size_t, std::size_t>> heap;

        // A window of length in (m, longest] starting at t holds the m-byte core occurring at c, plus
        // a byte after it, exactly when t lies in [c + m + 1 - length, c].
        const auto ingest{[&](const std::size_t at) {
            for (auto length{core.size() + 1}; length <= longest_window_; ++length)
            {
                const auto lowest{at + core.size() + 1 > length ? at + core.size() + 1 - length : std::size_t{0}};

                for (auto t{std::max(floor, lowest)}; t <= at && t + length <= size; ++t)
                {
                    heap.emplace_back(t, length);

                    std::ranges::push_heap(heap, std::greater{});
                }
            }
        }};

        auto pending{next_occurrence()};

        std::optional<std::pair<std::size_t, std::size_t>> last;

        // A pop waits until no unread occurrence can still contribute a smaller pair, which holds once
        // the next occurrence starts past t + longest - m - 1; overlapping occurrences propose
        // duplicate pairs, which pop adjacently and are skipped.
        while (true)
        {
            while (pending && (heap.empty() || *pending <= heap.front().first + longest_window_ - core.size() - 1))
            {
                ingest(*pending);

                pending = next_occurrence();
            }

            if (heap.empty())
            {
                return std::nullopt;
            }

            std::ranges::pop_heap(heap, std::greater{});

            const auto candidate{heap.back()};

            heap.pop_back();

            if (last == std::optional{candidate})
            {
                continue;
            }

            last = candidate;

            if (const auto origin{certified_origin(memo, begin, candidate.first, candidate.second)})
            {
                return candidate.first + *origin;
            }
        }
    }

    /**
     * @brief Constructs a Lexer from a DFA.
     * @param dfa The DFA to use for tokenization.
     */
    explicit Lexer(const dfa::Dfa& dfa) : simulator_{dfa} {}

    /**
     * @brief Constructs a lexer that also certifies split points modulo the tokens the caller discards.
     * @param dfa The compiled DFA.
     * @param ignored The IDs of tokens the caller deletes before using the stream.
     */
    Lexer(const dfa::Dfa& dfa, const std::span<const std::size_t> ignored) : simulator_{dfa, ignored} {}

    /**
     * @brief Compiles a DFA, attaching a caller's word to every match of the named tokens.
     */
    Lexer(const dfa::Dfa& dfa, const std::span<const std::size_t> ignored,
          const std::span<const std::pair<std::size_t, std::uint64_t>> payloads)
        : simulator_{dfa, ignored, payloads}
    {}

    /**
     * @brief The simulator running the DFA the Lexer was constructed from.
     */
    dfa::Simulator simulator_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
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
 * Instances are only constructible through Builder::build(), which is the sole supported path from patterns to a
 * working Lexer.
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
    template <typename T, common::concepts::Iterator Iterator>
        requires(std::integral<T> || std::is_enum_v<T>)
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
    template <typename T, common::concepts::Iterable Container>
        requires(std::integral<T> || std::is_enum_v<T>)
    [[nodiscard]] Match<T> tokenize(const Container& container) const
    {
        return tokenize<T>(std::ranges::begin(container), std::ranges::end(container));
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
     * @param sink Invoked as sink(token, length) for every matched token, in input order, or as
     *        sink(token, length, payload) where the sink accepts that and the payload is what
     *        Builder::set_token_payload() attached. A sink accepting both is called with two, which is what it
     *        received before payloads existed. A sink returning a value convertible to bool stops the scan by
     *        returning false; the stopping token still counts as tokenized.
     * @return The number of input elements tokenized; anything short of the input's size means no token matched at
     *         the returned offset, unless the sink stopped the scan.
     */
    template <typename T, std::random_access_iterator Iterator, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) &&
                (std::invocable<Sink&, T, std::size_t> || std::invocable<Sink&, T, std::size_t, std::uint64_t>)
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
     * @brief Tokenizes a whole container in one pass, invoking the sink once per matched token.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must offer random access).
     * @tparam Sink Callable receiving each matched token and its length.
     * @param container The input container.
     * @param sink Invoked as sink(token, length) for every matched token, in input order.
     * @return The number of input elements tokenized; anything short of the container's size means no token matched
     *         at the returned offset.
     */
    template <typename T, common::concepts::Random_access_iterable Container, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) &&
                (std::invocable<Sink&, T, std::size_t> || std::invocable<Sink&, T, std::size_t, std::uint64_t>)
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
     * the useful subset is reported: a symbol no live state consumes is safe merely vacuously and answers false. The
     * property is certified from the compiled transition table; see dfa::Simulator::is_split_point().
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
     * the exact certificate, so acting on this answer means planning boundaries yourself. See
     * dfa::Simulator::is_split_point_ignoring().
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
     * begins a token, a certified window (W, o) promises that in every completely tokenizable input containing W,
     * the token covering the occurrence's final byte begins exactly o bytes into it. On non-empty, non-nullable token
     * sets the two coincide at length one; a nullable set can certify bytes while every window is refused here, and an
     * empty token set refuses everything on both sides. The
     * certificate is conditional on occurrence and this call does not establish that one exists; a caller that
     * found W in its own input holds an occurrence, the promise applies to it on completely tokenizable input,
     * and on malformed input the window promise carries nothing at all, the consequence
     * chunk_boundaries_with_windows() documents. Refusals are model-relative and conservative, never proof that
     * no certificate exists. Derived from the compiled transition table; see dfa::Simulator::is_split_window().
     */
    [[nodiscard]] std::optional<std::size_t> is_split_window(const std::string_view window) const
    {
        return simulator_.is_split_window(window);
    }

    /**
     * @brief Computes chunk boundaries for parallel tokenization at certified safe split points.
     *
     * Each interior boundary is the first certified split point at or after its equal-division target offset, so
     * every chunk starts at a symbol that can only begin a token; for completely tokenizable input, concatenating
     * the chunk-local streams reproduces the whole-input token stream. The byte certificate is a property of
     * single transitions rather than of whole inputs, which is what upholds tokenize_all_parallel()'s
     * serial-prefix guarantee on malformed input; past the serial failure offset that prefix relation is all the
     * certificate promises. When the token set certifies no usable points, the result is one chunk spanning the whole
     * input, so parallel scanning degenerates to the serial scan rather than splitting unsafely; on such token
     * sets chunk_boundaries_with_windows() can recover cuts, at the price of a guarantee conditional on the
     * input being completely tokenizable.
     * @tparam Iterator Random access iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer result when certified points are scarce, and zero
     *        behaves as one, the whole input as a single chunk.
     * @return Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
     */
    template <std::random_access_iterator Iterator>
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
    template <common::concepts::Random_access_iterable Container>
        requires std::convertible_to<std::ranges::range_value_t<Container>, char>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries(const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries(std::ranges::begin(container), std::ranges::end(container), chunks);
    }

    /**
     * @brief Computes chunk boundaries like chunk_boundaries(), additionally recovering cuts from certified
     *        split windows where the token set certifies no usable byte.
     *
     * From each equal-division target the input is walked for the first occurrence of a window of two to four
     * bytes that is_split_window() certifies, and the cut is placed at the occurrence plus the reported origin.
     * Each decision is memoized per distinct byte string, so the walk's cost is bounded by the distinct windows
     * tried rather than by input positions; no representative-corpus pricing is claimed until one is measured. A
     * nullable token set contributes no windows, since the window proof excludes it, though its byte plan, when any,
     * stands untouched; when neither certificate offers cuts, the single whole-input chunk results.
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
    template <std::random_access_iterator Iterator>
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

        // The longest window the planner tries; every named certified window in the study is at most four
        // bytes. A grammar needing longer windows degrades to fewer chunks, never to an unsafe cut.
        constexpr std::size_t longest{4};

        // One decision per distinct byte string per plan: real text repeats its windows, so the walk prices
        // like the byte walk instead of one cloud walk per position.
        std::map<std::string, std::optional<std::size_t>, std::less<>> memo;

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

            for (auto occurrence{std::max(window_target, boundaries.back() + 1)}; occurrence + 2 <= size; ++occurrence)
            {
                const auto limit{std::min(longest, size - occurrence)};

                std::string window{
                        begin + static_cast<std::ptrdiff_t>(occurrence),
                        begin + static_cast<std::ptrdiff_t>(occurrence + 2)};

                auto cut{false};

                for (std::size_t length{2}; length <= limit && !cut; ++length)
                {
                    if (length > window.size())
                    {
                        window.push_back(begin[static_cast<std::ptrdiff_t>(occurrence + length - 1)]);
                    }

                    auto found{memo.find(window)};

                    if (found == memo.end())
                    {
                        found = memo.emplace(window, is_split_window(window)).first;
                    }

                    if (const auto& origin{found->second})
                    {
                        boundaries.push_back(occurrence + *origin);

                        cut = true;
                    }
                }

                if (cut)
                {
                    break;
                }
            }
        }

        boundaries.push_back(size);

        return boundaries;
    }

    /**
     * @brief Range overload of chunk_boundaries_with_windows(begin, end, chunks).
     */
    template <common::concepts::Random_access_iterable Container>
        requires std::convertible_to<std::ranges::range_value_t<Container>, char>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries_with_windows(
            const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries_with_windows(std::ranges::begin(container), std::ranges::end(container), chunks);
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
     * @tparam Sink Callable receiving the chunk index, each matched token, and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer are scanned when certified points are scarce, and zero
     *        behaves as one, the serial scan on the calling thread.
     * @param sink Invoked as sink(chunk, token, length) for every matched token.
     * @return The number of input elements tokenized per chunk, aligned with chunk_boundaries(begin, end,
     *         chunks); an entry short of its chunk's size means no token matched at that offset of the chunk.
     */
    template <typename T, std::random_access_iterator Iterator, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink&, std::size_t, T, std::size_t>
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
        const auto scan{[this, &boundaries, &consumed, begin, &sink, &failure_mutex,
                         &failure](const std::size_t chunk) {
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
    template <typename T, common::concepts::Random_access_iterable Container, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink&, std::size_t, T, std::size_t>
    [[nodiscard]] std::vector<std::size_t> tokenize_all_parallel(
            const Container& container, const std::size_t chunks, Sink sink) const
    {
        return tokenize_all_parallel<T>(
                std::ranges::begin(container), std::ranges::end(container), chunks, std::move(sink));
    }

private:
    friend class Builder;

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

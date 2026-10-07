#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_LEXER_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
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
#include "munch/core/window_planner.hpp"
#include "munch/dfa/anchor_free_span.hpp"
#include "munch/dfa/boundary_search.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/recovery.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/split_window.hpp"

namespace munch::core
{
/**
 * @brief The main Lexer class for tokenizing input using a DFA.
 *
 * Provides methods to tokenize input from iterators or containers, returning the matched token and length. Instances
 * are obtainable through Builder::build(), the one supported path from patterns to a working Lexer.
 *
 * The class reads top down as the scan, the certificates construction derived, the planners and the parallel scan they
 * feed, then the recovery queries and the decisions, each of the last two a forwarder to its dfa function. The window
 * search the planners share is Window_planner, in its own header.
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
        /**
         * @brief Equal when both attempts matched the same token at the same length.
         */
        bool operator==(const Match&) const = default;

        /**
         * @brief The token matched, or std::nullopt where nothing accepted.
         */
        std::optional<T> token{};

        /**
         * @brief The length of input the match consumed, zero when nothing accepted.
         */
        std::size_t length{};
    };

    /**
     * @brief The answer a certificate supports, together with the evidence that supports it.
     *
     * The start is the certified token-start position; the evidence is the certified byte itself (evidence_begin ==
     * start, one byte) or the whole window occurrence, and the guarantee is exactly the certificate's: the
     * repair-invariance transfer requires the evidence interval to survive whatever changed and the repaired scan to
     * commit through it. A caller comparing evidence_begin against a known-clean lower bound decides the survival half
     * alone, whether the evidence outlasted the damage; the transfer to the intended input additionally needs that
     * input's scan to reach the evidence, with the whole intended input being completely tokenizable the simplest
     * sufficient condition.
     */
    struct Certified_start
    {
        /**
         * @brief The certified token-start position, the answer.
         */
        std::size_t start{};

        /**
         * @brief First byte of the supporting evidence.
         */
        std::size_t evidence_begin{};

        /**
         * @brief One past the supporting evidence's last byte.
         */
        std::size_t evidence_end{};

        /**
         * @brief True for window evidence; false for a certified byte at the start itself.
         */
        bool window{};
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

        const auto converted{token ? std::optional<T>{static_cast<T>(token->id())} : std::optional<T>{}};

        return {.token = converted, .length = offset};
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
     * across tokens, amortizing the per-call overhead; the automaton restarts at each token as tokenize() does. Random
     * access is required because longest match may read past the last accepting position and must resume from it. The
     * scan always delivers the payload, and a sink that does not take it has it dropped here, so the scan has one sink
     * shape rather than one per arity.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving each consumed token and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length) for every consumed token, in input order, or as sink(token, length,
     *        payload) where the sink accepts that and the payload is what Builder::set_token_payload() attached; a sink
     *        accepting both is called with two. A sink returning a value convertible to bool stops the scan by
     *        returning false; the stopping token still counts as tokenized.
     * @return The number of input elements tokenized; anything short of the input's size means the scan stopped at the
     *         returned offset: no token matched there, a zero-width token did, or the sink returned false.
     */
    template <
            common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator,
            common::concepts::Token_sink<T> Sink>
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink) const
    {
        const auto forward{[&sink](const dfa::Token& token, const std::size_t length, const std::uint64_t payload) {
            if constexpr (std::invocable<Sink&, T, std::size_t>)
            {
                return sink(static_cast<T>(token.id()), length);
            }
            else
            {
                return sink(static_cast<T>(token.id()), length, payload);
            }
        }};

        return simulator_.run_all(begin, end, forward);
    }

    /**
     * @brief Tokenizes a whole container in one call, invoking the sink once per consumed token.
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must offer random access).
     * @tparam Sink Callable receiving each consumed token and its length, or those and its payload.
     * @param container The input container.
     * @param sink As for the iterator form above.
     * @return The number of input elements tokenized; anything short of the container's size means the scan stopped at
     *         the returned offset: no token matched there, a zero-width token did, or the sink returned false.
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
     * For input the serial scan tokenizes completely, splitting immediately before a safe split point produces the
     * identical token stream, so such symbols mark chunk boundaries at which one large input may be processed in
     * independent pieces; on malformed input see tokenize_all_parallel() for the weaker prefix guarantee. Only the
     * useful subset is reported: a symbol no live state consumes, a live state being one reachable from the initial
     * state that can still reach an accepting one, is safe merely vacuously and answers false. The property is decided
     * from the compiled transition table; the derivation is dfa::Simulator::is_split_point()'s.
     * @param symbol The symbol to test.
     * @return True if every occurrence of the symbol begins a token and some live state consumes it.
     */
    [[nodiscard]] bool is_split_point(const char symbol) const noexcept { return simulator_.is_split_point(symbol); }

    /**
     * @brief Returns whether the symbol is a safe split point once the discarded tokens are deleted.
     *
     * Never smaller than is_split_point(), and equal to it unless the builder was told which tokens are discarded. For
     * input the serial scan tokenizes completely, chunks cut here reproduce the serial stream once tokens of those
     * kinds are removed from both, so a caller that keeps them must use is_split_point(). The completeness condition is
     * not decoration: past the offset where the serial scan first fails, a chunk cut here can emit kept tokens that
     * scan never reaches. Note also that chunk_boundaries() and tokenize_all_parallel() plan with the exact
     * certificate, so acting on this answer means planning boundaries yourself. The derivation is
     * dfa::Simulator::is_split_point_ignoring()'s.
     * @param symbol The symbol to test.
     * @return True if the symbol can begin a token and every occurrence is safe under that weaker equivalence; symbols
     *         satisfying the condition only vacuously report false.
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
     * token covering the occurrence's final byte begins exactly o bytes into it. On non-empty token sets the two
     * coincide at length one, a nullable token set, one in which some token matches the empty string, being decided
     * through the positive-width equivalent the simulator compiled, and an empty token set refuses everything. The
     * certificate is conditional on occurrence and this call does not establish that one exists; a caller that found W
     * in its own input holds an occurrence, the promise applies to it on completely tokenizable input, and on malformed
     * input the window promise carries nothing at all, the consequence chunk_boundaries_with_windows() documents.
     * Refusals are model-relative and conservative, never proof that no certificate exists. Decided from the compiled
     * transition table; the derivation is dfa::is_split_window()'s.
     * @param window The byte string to decide.
     * @return The in-window origin every covering token begins at, or std::nullopt when the window is refused.
     */
    [[nodiscard]] std::optional<std::size_t> is_split_window(const std::string_view window) const
    {
        return dfa::is_split_window(simulator_, window);
    }

    /**
     * @brief Finds a shortest window is_split_window() certifies, with its origin, exactly rather than by trying
     *        candidates; or the proof that the model certifies no window of any length; or a budget run out.
     *
     * Its window may be much longer than the state count, and a grammar whose shortest window is longer than
     * chunk_boundaries_with_windows() tries gains nothing from that planner. A found window is conditional on
     * occurrence like every window certificate, which window_occurrence() settles. The derivation is
     * dfa::shortest_split_window()'s.
     * @param budget The most search nodes visited before the search gives up, dfa::shortest_window_budget unless told.
     * @return The window and its origin, Outcome::none when none exists, or Outcome::budget.
     */
    [[nodiscard]] dfa::Shortest_window shortest_split_window(
            const std::size_t budget = dfa::shortest_window_budget) const
    {
        return dfa::shortest_split_window(simulator_, budget);
    }

    /**
     * @brief Decides whether the given byte string occurs in some nonempty completely tokenizable input, with one that
     *        contains it.
     *
     * The question is_split_window() leaves open: its certificate is conditional on occurrence, so a window no
     * completely tokenizable input contains is certified vacuously and anchors nothing, and this call splits the two
     * readings. A certified window whose witness comes back is an occurring certificate; one an exhaustive search finds
     * no witness for is a vacuous one. Decided exactly, by the same boundary-guessing search as rescue() and
     * boundary_difference() with a window matcher beside the scan, the witness the shortest such input: over {0, 00,
     * 01} the window 1001 is certified at origin 2 and occurs in no completely tokenizable input, while 001 occurs in
     * the input 0001. The question is asked over nonempty inputs, the empty input, which contains the empty window
     * alone, being no input a cut could fall in, so the empty window has a shortest token as its witness and, under a
     * token set with no positive-width token, none. The derivation is dfa::window_occurrence()'s.
     * @param window The byte string to find.
     * @param cap The largest number of search states to hold before giving up, dfa::occurrence_cap unless told.
     * @return The witness and whether the search settled the question; an empty witness from an exhaustive search
     *         proves that no nonempty completely tokenizable input contains the window.
     */
    [[nodiscard]] dfa::Occurrence window_occurrence(
            const std::string_view window, const std::size_t cap = dfa::occurrence_cap) const
    {
        return dfa::window_occurrence(simulator_, window, cap);
    }

    /**
     * @brief Decides whether the window certificate (W, o) is failed by some completely tokenizable input, with one
     *        that does.
     *
     * The certificate promises that in every completely tokenizable input containing W, the token covering the
     * occurrence's final byte begins exactly o bytes into it; a counterexample is a completely tokenizable input
     * holding an occurrence whose covering token begins elsewhere, and an exhaustive search that finds none proves the
     * certificate exact over every completely tokenizable input. Where is_split_window() decides the certificate
     * through its conservative model, this call decides it outright: a window it certifies at o has no counterexample
     * at o, a window it refuses is settled here either way, with the witness where the refusal was right and a proof
     * where it was conservative, and a window window_occurrence() places in no input has no counterexample at any
     * origin, its certificate being vacuous. Decided by the same boundary-guessing search as window_occurrence(), with
     * one bit beside the matcher for whether the latest token start sits at the origin, the witness the shortest
     * failing input: over {a, abc, bx, x} the window ab is refused, and its refusal is right at both origins, abx
     * covering the b from offset 1 and abc from offset 0. The derivation is dfa::window_counterexample()'s.
     * @param window The byte string of the certificate, non-empty.
     * @param origin The offset into the window the certificate places the covering token's start at, inside it.
     * @param cap The largest number of search states to hold before giving up, dfa::counterexample_cap unless told.
     * @return The witness and whether the search settled the question; an empty witness from an exhaustive search
     *         proves that no completely tokenizable input fails the certificate.
     * @throws std::invalid_argument If the window is empty or the origin lies outside it, neither being a certificate.
     */
    [[nodiscard]] dfa::Counterexample window_counterexample(
            const std::string_view window, const std::size_t origin,
            const std::size_t cap = dfa::counterexample_cap) const
    {
        return dfa::window_counterexample(simulator_, window, origin, cap);
    }

    /**
     * @brief Decides whether the gap g is a token boundary at every occurrence of the window W in every completely
     *        tokenizable input, with an input holding an occurrence a token crosses there.
     *
     * Gap g sits before byte g of the occurrence and gap |W| right after its final byte, and a boundary is a token
     * start or the input's end. It is the weaker guarantee beside the window certificate: a certificate places the
     * start of the token covering the final byte, so a window window_counterexample() proves exact at o has a boundary
     * at gap o of every occurrence, while such a boundary asks nothing of the tokens after it and holds where no
     * covering origin is fixed. A worker cutting there rescans the window's suffix rather than resuming at the covering
     * token. Decided by the same boundary-guessing search as window_counterexample(), the bit beside the matcher
     * recording whether the gap is a boundary, the witness the shortest refuting input: over {0, 1, x, 001x, 011x} no
     * certificate holds of the window 0011 at any origin, its final byte covered from offset 1 in 0011x and from offset
     * 3 in 0011, while gaps 0 and 1 are boundaries at every occurrence and 0011x refutes gaps 2, 3 and 4. The claim is
     * monotone, a boundary at every occurrence of W staying one in every extension of it at the shifted gap, and a
     * window window_occurrence() places in no input has no counterexample at any gap. The derivation is
     * dfa::boundary_counterexample()'s.
     * @param window The byte string whose occurrences are asked about, non-empty.
     * @param gap The gap of the window at which a boundary must sit, from zero to the window's length.
     * @param cap The largest number of search states to hold before giving up, dfa::refutation_cap unless told.
     * @return The witness and whether the search settled the question; an empty witness from an exhaustive search
     *         proves the gap a boundary at every occurrence.
     * @throws std::invalid_argument If the window is empty or the gap lies past its end, neither naming a gap of it.
     */
    [[nodiscard]] dfa::Refutation boundary_counterexample(
            const std::string_view window, const std::size_t gap, const std::size_t cap = dfa::refutation_cap) const
    {
        return dfa::boundary_counterexample(simulator_, window, gap, cap);
    }

    /**
     * @brief Decides whether a token crosses the gap g at every occurrence of the window W in every completely
     *        tokenizable input, with an input holding an occurrence cut there.
     *
     * The other side of boundary_counterexample(), decided by the same search with the gap's bit read the other way,
     * the input's end counting as a boundary for the gap after the window. The two claims together are the window
     * certificate: (W, o) holds exactly when gap o is a boundary at every occurrence and every later gap inside the
     * window is crossed at every occurrence. The claim is monotone in the same way. The derivation is
     * dfa::crossing_counterexample()'s.
     * @param window The byte string whose occurrences are asked about, non-empty.
     * @param gap The gap of the window a token must cross, from zero to the window's length.
     * @param cap The largest number of search states to hold before giving up, dfa::refutation_cap unless told.
     * @return The witness and whether the search settled the question; an empty witness from an exhaustive search
     *         proves the gap crossed at every occurrence.
     * @throws std::invalid_argument If the window is empty or the gap lies past its end, neither naming a gap of it.
     */
    [[nodiscard]] dfa::Refutation crossing_counterexample(
            const std::string_view window, const std::size_t gap, const std::size_t cap = dfa::refutation_cap) const
    {
        return dfa::crossing_counterexample(simulator_, window, gap, cap);
    }

    /**
     * @brief Decides every gap of the window both ways, returning each gap's verdict.
     *
     * Absence is a property of the window and given at every gap or at none: proved by window_occurrence() under the
     * cap, asked first, or by any gap whose two searches under the cap, boundary_counterexample() and
     * crossing_counterexample(), both exhaust without a witness, which can come under a cap that stops the occurrence
     * search. Otherwise the two searches per gap give must, never or may, each with a witness showing the window
     * occurring, and undetermined where the cap stopped a search without such proof. The derivation is
     * dfa::boundary_profile()'s.
     * @param window The byte string whose gaps are decided, non-empty.
     * @param cap The largest number of search states each search holds before giving up, dfa::refutation_cap unless
     *        told.
     * @return One verdict per gap of the window, the gap's index, |W| + 1 of them.
     * @throws std::invalid_argument If the window is empty, which has no occurrence to hold a gap in.
     */
    [[nodiscard]] std::vector<dfa::Gap_verdict> boundary_profile(
            const std::string_view window, const std::size_t cap = dfa::refutation_cap) const
    {
        return dfa::boundary_profile(simulator_, window, cap);
    }

    /**
     * @brief Returns the compiled machine itself, for decisions written outside this class over its read-only view.
     *
     * Everything the class answers is answered from these tables; a tool reading a token set from elsewhere and asking
     * its own questions, which state consumes a byte mid-token and on the way to which token, needs the same view the
     * library's own decisions use, and gets it here rather than through a copy.
     * @return The simulator.
     */
    [[nodiscard]] const dfa::Simulator& simulator() const noexcept { return simulator_; }

    /**
     * @brief Returns the byte string every certified split window provably contains, or empty when none is proved.
     *
     * Every certified split window of this token set contains this string with at least one byte after it, so the
     * window planner narrows its candidate windows to the string's occurrences whenever it is non-empty, with identical
     * plans either way; empty means no such string is proved and the exhaustive walk stands. Decided from the compiled
     * transition table; the derivation and its proof are dfa::Simulator::mandatory_core()'s.
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
     * @param chunks The number of chunks aimed for; fewer result when certified points are scarce, and zero behaves as
     *        one, the whole input as a single chunk.
     * @return Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
     */
    template <common::concepts::Random_access_byte_iterator Iterator>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries(
            Iterator begin, Iterator end, const std::size_t chunks) const
    {
        const auto size{static_cast<std::size_t>(end - begin)};

        // A token set with no certified symbol that can occur in valid input yields the single whole-input chunk
        // without scanning. The test is the useful set, not the full certificate: symbols no live state consumes
        // certify vacuously, and searching for one scans to the end of the input and finds nothing.
        if (!simulator_.has_split_points())
        {
            return {0, size};
        }

        std::vector<std::size_t> boundaries{0};

        // A chunk needs at least one byte, so asking for more chunks than bytes only adds iterations that can find
        // nothing.
        const auto usable{std::min(chunks, size)};

        // The ideal offsets size * index / usable, accumulated step by step so that nothing multiplies.
        Division_targets targets{size, usable};

        const auto byte_at{[begin](const std::size_t at) {
            const auto element{begin[static_cast<std::ptrdiff_t>(at)]};

            return static_cast<char>(element);
        }};

        for (std::size_t index{1}; index < usable; ++index)
        {
            const auto target{targets.next()};

            // Start at the target or one past the previous boundary, whichever is later, so that adjacent certified
            // bytes each open a chunk.
            auto offset{std::max(target, boundaries.back() + 1)};

            while (offset < size && !is_split_point(byte_at(offset)))
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
     * @tparam Container The input container type (must offer random access).
     * @param container The input container.
     * @param chunks As for the iterator form above.
     * @return As for the iterator form above.
     */
    template <common::concepts::Random_access_byte_iterable Container>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries(const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries(std::ranges::begin(container), std::ranges::end(container), chunks);
    }

    /**
     * @brief Computes chunk boundaries like chunk_boundaries(), additionally recovering cuts from certified split
     *        windows where the token set certifies no usable byte.
     *
     * From the later of each equal-division target and one past the previous boundary the input is walked for the first
     * occurrence of a window of two to four bytes that is_split_window() certifies, and the cut is placed at the
     * occurrence plus the reported origin. Each window decision is memoized per distinct byte string, so those
     * decisions, the costly part, are bounded by the distinct windows tried, while the positional walk and its memo
     * lookups scale with the positions examined. When neither certificate offers cuts, the single whole-input chunk
     * results.
     *
     * The window guarantee is conditional where the byte certificate's is not: a certified window pins the covering
     * token's origin at occurrences in completely tokenizable input, a property of the whole input rather than of
     * single transitions. On malformed input a window cut can land inside a token of the serial scan's doomed suffix,
     * the fragments can each consume fully, and the serial stream is then not a prefix of the concatenated chunk
     * streams; full per-chunk consumption does not imply the serial scan succeeds. Use these boundaries when the input
     * is known completely tokenizable, or validate the result downstream; tokenize_all_parallel() deliberately plans
     * with chunk_boundaries() and never uses windows implicitly.
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
        if (simulator_.has_split_points())
        {
            return chunk_boundaries(begin, end, chunks);
        }

        const auto size{static_cast<std::size_t>(end - begin)};

        std::vector<std::size_t> boundaries{0};

        const auto usable{std::min(chunks, size)};

        Division_targets targets{size, usable};

        Window_planner planner{};

        for (std::size_t index{1}; index < usable; ++index)
        {
            const auto target{targets.next()};

            const auto floor{std::max(target, boundaries.back() + 1)};

            if (const auto cut{planner.cut(simulator_, begin, size, floor)})
            {
                boundaries.push_back(*cut);
            }
        }

        boundaries.push_back(size);

        return boundaries;
    }

    /**
     * @brief Computes chunk_boundaries_with_windows(begin, end, chunks) over a whole container.
     * @tparam Container The input container type (must offer random access).
     * @param container The input container.
     * @param chunks As for the iterator form above.
     * @return As for the iterator form above.
     */
    template <common::concepts::Random_access_byte_iterable Container>
    [[nodiscard]] std::vector<std::size_t> chunk_boundaries_with_windows(
            const Container& container, const std::size_t chunks) const
    {
        return chunk_boundaries_with_windows(std::ranges::begin(container), std::ranges::end(container), chunks);
    }

    /**
     * @brief Tokenizes one input as concurrent chunks split at certified safe split points.
     *
     * The input is divided by chunk_boundaries() and each chunk is scanned by tokenize_all() on its own thread, the
     * last on the calling thread; for input the serial scan tokenizes completely, certification guarantees the
     * concatenated per-chunk token streams are identical to the serial scan's. When no token matches somewhere, the
     * serial stream is a prefix of the concatenation and chunks past the failure still scan independently, so treat the
     * output as a successful tokenization only after checking every returned consumed length. Within a chunk the sink
     * is invoked in input order. Across chunks it is invoked concurrently, so it must be safe to call from different
     * threads for different chunk indices, which per-chunk state indexed by the chunk achieves without locking; give
     * hot per-chunk accumulators their own cache lines, as adjacent counters false-share and cost real scaling. There
     * is no early-stop form. An exception a sink throws is kept and rethrown on the calling thread once every worker
     * has joined, the first one when several chunks throw, since one escaping a jthread's callable would call
     * std::terminate.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving the chunk index, each consumed token, and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param chunks The number of chunks aimed for; fewer are scanned when certified points are scarce, and zero
     *        behaves as one, the serial scan on the calling thread.
     * @param sink Invoked as sink(chunk, token, length) for every consumed token.
     * @return The number of input elements tokenized per chunk, aligned with chunk_boundaries(begin, end, chunks); an
     *         entry short of its chunk's size means the scan stopped at that offset of the chunk, no token matching
     *         there or a zero-width one doing so; this form's sink cannot stop a chunk.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator, typename Sink>
        requires std::invocable<Sink&, std::size_t, T, std::size_t>
    [[nodiscard]] std::vector<std::size_t> tokenize_all_parallel(
            Iterator begin, Iterator end, const std::size_t chunks, Sink sink) const
    {
        const auto boundaries{chunk_boundaries(begin, end, chunks)};

        std::vector<std::size_t> consumed(boundaries.size() - 1, 0);

        std::mutex failure_mutex{};

        std::exception_ptr failure{};

        const auto scan{[&](const std::size_t chunk) {
            const auto chunk_begin{begin + static_cast<std::ptrdiff_t>(boundaries[chunk])};

            const auto chunk_end{begin + static_cast<std::ptrdiff_t>(boundaries[chunk + 1])};

            const auto chunk_sink{
                    [&sink, chunk](const T token, const std::size_t length) { sink(chunk, token, length); }};

            try
            {
                consumed[chunk] = tokenize_all<T>(chunk_begin, chunk_end, chunk_sink);
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
            std::vector<std::jthread> workers{};

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
     * @tparam T The token type (enum or integral).
     * @tparam Container The input container type (must offer random access).
     * @tparam Sink Callable receiving the chunk, each consumed token and its length.
     * @param container The input container.
     * @param chunks As for the iterator form above.
     * @param sink As for the iterator form above.
     * @return As for the iterator form above.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterable Container, typename Sink>
        requires std::invocable<Sink&, std::size_t, T, std::size_t>
    [[nodiscard]] std::vector<std::size_t> tokenize_all_parallel(
            const Container& container, const std::size_t chunks, Sink sink) const
    {
        return tokenize_all_parallel<T>(
                std::ranges::begin(container), std::ranges::end(container), chunks, std::move(sink));
    }

    /**
     * @brief Finds the first position at or after the given offset that a certificate marks as a token start.
     *
     * One forward walk consulting both certificate kinds at every position: a certified byte answers at its own
     * position, and a certified window of two to four bytes answers at its occurrence plus the certified origin. Unlike
     * the planners, byte certificates do not switch the window search off. The answer is the first certificate met in
     * evidence order, the order in which the walk meets the supporting evidence, which is not always the smallest
     * answerable position: a window met earlier can answer a byte or two past one met later, and windows beginning
     * before the given offset are not considered.
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
     * @return The first certified token-start position, or std::nullopt when no certified byte and no certified window
     *         of two to four bytes lies at or after the offset, the widths the search consults.
     */
    [[nodiscard]] std::optional<std::size_t> next_certified_start(std::string_view input, std::size_t from) const;

    /**
     * @brief Runs the certificate walk of next_certified_start(), reporting the supporting evidence with the answer.
     *
     * Same walk, same evidence order, same refusal; the position-only form above is this one with the evidence dropped.
     * The evidence lies wholly at or after the search offset by construction, which is what makes the comparison
     * against a caller's clean bound meaningful; the guarantee is Certified_start's.
     * @param input The input being scanned.
     * @param from The offset the search starts at; at or past the input's size finds nothing.
     * @return The first certified answer in evidence order with its evidence interval, or std::nullopt.
     */
    [[nodiscard]] std::optional<Certified_start> next_certified_evidence(
            std::string_view input, std::size_t from) const;

    /**
     * @brief Returns the first anchored-certified start in the tail at or after the offset.
     *
     * The anchored counterpart of next_certified_start(), exact where the walk is merely sound: with the tail's end
     * known to be the end of the input, every completely tokenizable repair of whatever preceded the tail places a
     * token boundary at the returned position. That quantifier is the whole contract; the certificates' guarantee over
     * repairs that merely reach their evidence is a different one, which this decider does not speak about. A tail
     * beyond repair refuses rather than answering vacuously.
     * @param tail The preserved suffix of the input, its end the end of the input.
     * @param from The offset the search starts at; at or past the tail's size finds nothing.
     * @return The first anchored-certified position, or std::nullopt when none exists or no repair does.
     */
    [[nodiscard]] std::optional<std::size_t> next_anchored_start(
            const std::string_view tail, const std::size_t from) const
    {
        return dfa::next_anchored_start(simulator_, tail, from);
    }

    /**
     * @brief Returns a shortest repair for the tail, empty when it already tokenizes; nothing when none exists.
     * @param tail The preserved suffix of the input.
     * @return A minimal repair, or std::nullopt when the tail is beyond repair.
     */
    [[nodiscard]] std::optional<std::string> minimal_repair(const std::string_view tail) const
    {
        return dfa::minimal_repair(simulator_, tail);
    }

    /**
     * @brief Returns the lag of the token set: the longest run of nonaccepting states a scan can traverse after leaving
     *        an accepting state, or nothing when that run is unbounded.
     *
     * States that can no longer reach an accepting one still count: a failed lookahead buffers bytes whether or not the
     * excursion could still accept, so every defined continuation counts. Zero is the premise under which a scheme
     * restarting at every accept executes serial maximal munch exactly; a bounded value prices the checkpoint a
     * rollback-aware scheme must carry.
     * @return The lag, or std::nullopt when a post-accept nonaccepting cycle makes it unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> lag() const { return dfa::lag(simulator_); }

    /**
     * @brief Decides whether some completely tokenizable input makes the scan roll back, with a witness.
     *
     * A rescue is a rollback after a failed lookahead that lets the scan continue where a scheme restarting at every
     * accept would have declared the input malformed: a token of a completely tokenizable input whose scan read past
     * the token's end before rolling back to it. Decided exactly, by the same boundary-guessing search as
     * boundary_difference(), the witness the shortest such input: {a, abb, b, c} is rescued on ab, where the scan of a
     * reads the b before rolling back, while {a, abc, bc} is rescue-free with lag one, since every completely
     * tokenizable continuation of the stretch after a closes the longer token abc instead.
     * @param cap The largest number of search states to hold before giving up, dfa::rescue_cap unless told.
     * @return The witness and whether the search settled the question; an empty witness from an exhaustive search
     *         proves the token set rescue-free.
     */
    [[nodiscard]] dfa::Rescue rescue(const std::size_t cap = dfa::rescue_cap) const
    {
        return dfa::rescue(simulator_, cap);
    }

    /**
     * @brief Returns whether the token set is rescue-free, so that a scheme restarting at every accept emits the tokens
     *        of serial maximal munch on every completely tokenizable input.
     * @return True when rescue() found no witness in an exhaustive search; false when a witness exists or the search
     *         stopped at its cap, which rescue() tells apart.
     */
    [[nodiscard]] bool rescue_free() const;

    /**
     * @brief Returns the longest run of positions a tokenizable input can carry with no certified byte, or nothing when
     *        such runs are unbounded.
     *
     * The gap chunk_boundaries() can be asked to span. The derivation is dfa::anchor_free_span()'s.
     * @return The exact supremum, or std::nullopt when it is unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> anchor_free_span() const { return dfa::anchor_free_span(simulator_); }

    /**
     * @brief Returns the longest run of positions with no certified anchor over a supplied inventory of certified
     *        windows, which can bound what the bytes cannot.
     *
     * The derivation is dfa::anchor_free_span()'s window overload.
     * @param inventory The certified windows and their origins, each refused by is_split_window() being an error.
     * @return The exact supremum, or std::nullopt when it is unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> anchor_free_span(
            const std::span<const std::pair<std::string_view, std::size_t>> inventory) const
    {
        return dfa::anchor_free_span(simulator_, inventory);
    }

    /**
     * @brief Decides whether another token set cuts some input both tokenize differently, with a witness.
     *
     * The question a tokenizer change asks, answered from the two compiled tables rather than from a corpus, so a
     * negative covers every input instead of the ones a suite happens to hold. The derivation is
     * dfa::boundary_difference()'s.
     * @param other The lexer to compare against.
     * @param cap The largest number of product states to hold before giving up, dfa::difference_cap unless told.
     * @return The witness and whether the search was exhaustive; an empty witness means identical only when it was.
     */
    [[nodiscard]] dfa::Difference boundary_difference(
            const Lexer& other, const std::size_t cap = dfa::difference_cap) const
    {
        return dfa::boundary_difference(simulator_, other.simulator_, cap);
    }

    /**
     * @brief Decides whether another token set is a different segmentation function, with an input the two segment
     *        differently and the half it falls in.
     *
     * Full equivalence, the whole of it: the two sets tokenize the same inputs completely and cut every one of them
     * alike, which boundary_difference() decides only the second half of. A token set's segmentation function is the
     * set of marked runs its scan accepts, one marking per input of the domain, so two sets are fully equivalent
     * exactly when those languages are equal, and the decision is that language equality, by the same boundary-guessing
     * search as boundary_difference() with one guessed marking fed to both scans at once, the witness the bytes of the
     * shortest marked run exactly one side accepts. The half is a fact about the witness: a domain witness one set
     * tokenizes completely and the other does not, a boundary witness both do and cut apart, and a boundary witness
     * here is a boundary_difference() witness, while an exhaustive negative here is one there too. The witnesses need
     * not agree: over {a} against {aa} the shortest marked run only one side accepts is a, the domain witness, where
     * boundary_difference() returns aa, one token against two. The derivation is dfa::segmentation_difference()'s.
     * @param other The lexer to compare against.
     * @param cap The largest number of product states to hold before giving up, dfa::segmentation_cap unless told.
     * @return The witness, the half it falls in, and whether the search settled the question; an empty witness from an
     *         exhaustive search proves the two token sets the same segmentation function.
     */
    [[nodiscard]] dfa::Separation segmentation_difference(
            const Lexer& other, const std::size_t cap = dfa::segmentation_cap) const
    {
        return dfa::segmentation_difference(simulator_, other.simulator_, cap);
    }

private:
    // The Builder is the only construction path: a Lexer exists exclusively over a DFA the Builder compiled, so the
    // constructor below stays private and the friendship is the whole public door.
    friend class Builder;

    /**
     * @brief The equal-division targets of an input, the offsets size * index / parts for index 1, 2 and so on, in
     *        turn.
     *
     * The product overflows for a large input divided very finely, and so does any form that multiplies the remainder
     * by the index: both are bounded below by (parts - 1) squared. Adding the quotient each step and carrying the
     * remainder when it fills a whole divisor yields exactly the same offsets and multiplies nothing, with the target
     * never exceeding the size and the carry never reaching the divisor, so no intermediate can leave the range the
     * input already occupies.
     */
    class Division_targets
    {
    public:
        /**
         * @brief Constructs the walk over an input divided into parts, positioned before the first target.
         * @param size The input's size.
         * @param parts The number of parts; zero leaves every step at zero.
         */
        constexpr Division_targets(const std::size_t size, const std::size_t parts) noexcept
            : step_{parts == 0 ? std::size_t{0} : size / parts}
            , remainder_{parts == 0 ? std::size_t{0} : size % parts}
            , parts_{parts}
        {}

        /**
         * @brief Advances to the next target.
         * @return The next equal-division offset.
         */
        constexpr std::size_t next() noexcept
        {
            target_ += step_;

            carry_ += remainder_;

            if (carry_ >= parts_)
            {
                ++target_;

                carry_ -= parts_;
            }

            return target_;
        }

    private:
        /**
         * @brief The quotient of the size by the parts, added at every step.
         */
        std::size_t step_;

        /**
         * @brief The remainder of the size by the parts, accumulated into the carry at every step.
         */
        std::size_t remainder_;

        /**
         * @brief The number of parts, the divisor the carry is reduced by.
         */
        std::size_t parts_;

        /**
         * @brief The latest target, zero before the first step.
         */
        std::size_t target_{0};

        /**
         * @brief The accumulated remainders not yet carried into the target, always below the divisor.
         */
        std::size_t carry_{0};
    };

    /**
     * @brief Constructs a lexer over a compiled DFA, attaching a caller's word to every match of the named tokens.
     * @param dfa The compiled DFA.
     * @param ignored The IDs of tokens the caller deletes before using the stream.
     * @param payloads Token ID and word pairs; a token named more than once keeps the last word given.
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

#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP

#include <array>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/dfa/dfa.hpp"

namespace munch::dfa
{
/**
 * @brief Whether a transition table of the given states and symbol classes would overflow the given size limit.
 *
 * The compiled table is one row of states entries per class. On 64-bit platforms the product cannot overflow,
 * since states fit 32 bits and classes fit 8; on a 32-bit std::size_t it can, which the constructor refuses.
 * Stated as its own function with the limit as a parameter so the arithmetic is testable on every platform,
 * even where the guard itself is unreachable.
 */
[[nodiscard]] constexpr bool table_size_overflows(
        const std::size_t states, const std::size_t classes, const std::size_t limit) noexcept
{
    // A table of zero classes has zero entries and cannot overflow; stated explicitly because the division
    // would otherwise be undefined, and a public constexpr function answers for every input, not only the one
    // to two hundred fifty-six classes the constructor supplies.
    return classes != 0 && states > limit / classes;
}

/**
 * @brief Runs a DFA over input sequences.
 *
 * Compiles the DFA it is constructed from into flat tables indexed by state and input symbol, so that advancing on an
 * input character is a single table read instead of a hash lookup. Symbols the automaton never distinguishes share a
 * table row: each input character is first mapped to its equivalence class, shrinking the table from one row per
 * symbol value to one per class, which keeps far more of it in cache. The tables assume states are numbered densely
 * from zero, as the subset construction numbers them; a sparsely numbered DFA still works but wastes a table column
 * per unused identifier.
 *
 * Acceptance is tracked during the scan through a per-state flag byte rather than the accept tokens themselves:
 * the flag load depends on the new state but feeds nothing, so it stays off the state-to-state dependency chain,
 * and the matched Token is resolved exactly once after the scan.
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
     * @brief Compiles the given DFA into transition and accept tables.
     * @param dfa The DFA to simulate.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition
     *         table's size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    explicit Simulator(const Dfa& dfa);

    /**
     * @brief Compiles the given DFA, additionally certifying split points modulo a set of discarded tokens.
     * @param dfa The DFA to simulate.
     * @param ignored The IDs of tokens the caller discards before the stream is used.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition
     *         table's size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    Simulator(const Dfa& dfa, std::span<const std::size_t> ignored);

    /**
     * @brief Compiles the given DFA, attaching a caller's word to every state accepting one of the named tokens.
     *
     * Stored by accepting state and handed back with the match, never read here, so a caller answers a per-token
     * question without a second lookup and state numbering stays inside this class.
     * @param dfa The DFA to simulate.
     * @param ignored The IDs of tokens the caller discards before the stream is used.
     * @param payloads Token ID and word pairs; a token named more than once keeps the last word given.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition
     *         table's size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    Simulator(
            const Dfa& dfa, std::span<const std::size_t> ignored,
            std::span<const std::pair<std::size_t, std::uint64_t>> payloads);

    /**
     * @brief Returns whether the given symbol is a certified safe split point.
     *
     * A symbol is a safe split point when no state reachable after consuming input consumes it into a state that can
     * still accept, so on any completely tokenizable input every occurrence can only be the first byte of a token: a
     * scan reaching it mid-token either finds no transition or enters a state from which acceptance is unreachable,
     * so it may read farther but records no further accepting position and its token ended at an earlier one, and a
     * token can only contain it by starting with it. Input the serial scan cannot tokenize completely carries the
     * promise only up to the offset where that scan first fails, even though a valid prefix of tokens may be emitted
     * before it; see core::Lexer::tokenize_all_parallel() for what survives there. For input that tokenizes
     * completely, splitting immediately before such a symbol therefore produces the identical token stream, which is
     * what makes chunked processing of one large input safe. The initial state is exempt only while no transition
     * re-enters it; a nullable pattern such as a kleene token minimizes to an accepting, self-looping start state, and
     * its symbols certify nothing. A symbol no live state consumes is safe only vacuously, since no input this lexer
     * accepts contains it, and is deliberately not reported: a caller cannot use it, and searching for one scans the
     * whole input for nothing. Transitions into states that can never accept are ignored throughout, since no emitted
     * token can traverse one; a pattern denoting the empty language leaves exactly such states behind. States the
     * initial state cannot reach are ignored on the same grounds, since no scan can arrive in one; a Dfa built by
     * subset construction has none, but one assembled by hand may. Token sets whose runs or literals may contain any
     * byte certify no split points.
     */
    [[nodiscard]] bool is_split_point(const char symbol) const noexcept
    {
        const auto value{static_cast<unsigned char>(symbol)};

        return ((split_points_[value >> 6U] >> (value & 63U)) & 1U) != 0;
    }

    /**
     * @brief Reports whether the symbol is a safe split point once discarded tokens are deleted from the stream.
     *
     * Weaker than is_split_point(), and never stronger: every certified symbol satisfies this too, and with an empty
     * ignored set the two coincide. A state may consume the symbol into a state that can still accept, provided the
     * token the cut would sever vanishes from both streams. That holds when the state accepts an ignored token, when
     * every token still reachable from it is ignored, and when advancing on the symbol from it and from the initial
     * state reach the same state, so the restarted scan rejoins the interrupted one at once and only the one token
     * containing the cut is disturbed.
     *
     * The guarantee is correspondingly weaker in two independent ways. Chunks cut here reproduce the serial stream
     * only after tokens of the ignored kinds are deleted from both, so a caller that keeps them must use
     * is_split_point() instead. And it holds only for input the serial scan tokenizes completely: past the offset
     * where that scan first fails, a chunk cut here can run on and emit kept tokens the serial scan never reaches,
     * so a boundary must lie before that offset to be covered at all.
     * Like is_split_point(), this reports the useful subset: a symbol no live state consumes satisfies the condition
     * only vacuously, and both maps deliberately answer false for it.
     * @param symbol The symbol to test.
     * @return True if the symbol can begin a token and every occurrence is a safe split point under that weaker
     *         equivalence; false for symbols that satisfy the condition only vacuously.
     */
    [[nodiscard]] bool is_split_point_ignoring(const char symbol) const noexcept
    {
        const auto value{static_cast<unsigned char>(symbol)};

        return ((split_points_ignoring_[value >> 6U] >> (value & 63U)) & 1U) != 0;
    }

    /**
     * @brief Decides whether the given byte string is a certified split window, returning the covering origin.
     *
     * A certified split window (W, o) promises: in every completely tokenizable input containing W, the token
     * covering the occurrence's final byte begins exactly o bytes into the occurrence. The certificate is
     * conditional on occurrence; a window no completely tokenizable input contains satisfies it vacuously, and
     * this decision does not establish that an occurrence exists. A caller that has just found W in its input at
     * hand holds that occurrence, and on completely tokenizable input the promise applies to it directly; on
     * malformed input the promise carries nothing at all.
     *
     * The decision runs the conservative cloud model over the compiled tables: every live state starts as a
     * hypothesis whose token began before the window, each byte advances hypotheses deterministically, a fresh
     * token may begin exactly where some represented history just ended one, and reading from a non-re-entrant
     * initial state begins a token at that offset. The window is certified when every surviving hypothesis
     * agrees on one in-window origin. A refusal is model-relative: the model deliberately refuses some windows a
     * greedy scanner would allow, and refusal never proves that no certificate exists semantically. On
     * non-empty, non-nullable token sets this coincides at length one with is_split_point(); nullable sets are
     * outside the window proof and refused outright here, while the byte predicate can still certify for them,
     * and an empty token set refuses everything on both sides.
     */
    [[nodiscard]] std::optional<std::size_t> is_split_window(std::string_view window) const;

    /**
     * @brief Reports whether the token set certifies any usable split point.
     * @return True if at least one symbol is a split point.
     */
    [[nodiscard]] bool has_split_points() const noexcept
    {
        return (split_points_[0] | split_points_[1] | split_points_[2] | split_points_[3]) != 0;
    }

    /**
     * @brief Returns whether some token matches the empty string, the compiled signature being an accepting
     *        initial state.
     *
     * Nullable token sets sit outside the split window soundness proof, and every window is refused for them.
     * The byte predicate instead withdraws only its initial-state exemption there, so byte certificates can
     * remain; a planner consults this before spending any search on windows, never to discard a byte plan.
     */
    [[nodiscard]] bool nullable() const noexcept { return is_accepting(init_state_); }

    /**
     * @brief Reports whether the token set certifies any usable split point once discarded tokens are deleted.
     *
     * Never false when has_split_points() is true, since the relaxed map contains the exact one. A caller planning
     * boundaries from is_split_point_ignoring() wants this test rather than has_split_points(), which would report
     * nothing to search for on precisely the token sets the relaxation exists to rescue.
     * @return True if at least one symbol is a split point modulo the discarded tokens.
     */
    [[nodiscard]] bool has_split_points_ignoring() const noexcept
    {
        return (split_points_ignoring_[0] | split_points_ignoring_[1] | split_points_ignoring_[2] |
                split_points_ignoring_[3]) != 0;
    }

    /**
     * @brief The byte string every certified split window provably contains, or empty when none is proved.
     *
     * Derived once at construction. A live state whose every transition is live cannot be killed by any
     * single byte, only led along a longer word to death, so any window certifying in its presence must
     * carry that state's forced exit; the shortest
     * exit is proposed as a core and proved mandatory by exhausting core-avoiding death words over the live
     * tables, with the killing byte never fed to the matcher. The longest proved core is kept: every window
     * is_split_window() certifies contains it with at least one byte following, which is what lets the
     * planner narrow its candidate windows to occurrences of this string. Empty means no core is
     * proved, because no such state exists, the candidate was refuted by a core-free death word, or the
     * token set is nullable; the planner then keeps its exhaustive walk, and nothing weakens: the core is an
     * accelerator's licence, never a certificate itself.
     * @return The proved mandatory core, or an empty view.
     */
    [[nodiscard]] std::string_view mandatory_core() const noexcept { return mandatory_core_; }

    /**
     * @brief The lag of the token set: the longest run of nonaccepting states a scan can traverse after
     *        leaving an accepting state, or nothing when that run is unbounded.
     *
     * Co-accessibility is deliberately not required: a failed lookahead buffers bytes whether or not the
     * excursion could still accept, so the measure counts every defined continuation. Zero is the premise
     * under which a restart-style scheme executes serial maximal munch exactly; a bounded value prices the
     * checkpoint a rollback-aware scheme must carry; an unbounded region, reported as nothing, carries a
     * cycle witness in the tables themselves.
     * @return The lag, or std::nullopt when a post-accept nonaccepting cycle makes it unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> lag() const;

    /**
     * @brief Whether every byte that opens a post-accept nonaccepting stretch is dead from the initial state.
     *
     * Rescue-free token sets are exactly those where a rollback can never rescue an input the restart
     * abstraction declares malformed: every rollback fires into an instant dead end, so a synchronous-restart
     * observer agrees with serial maximal munch on every input. Zero-lag sets pass vacuously, no stretch
     * existing; the gate is strictly weaker than zero lag.
     * @return True when no stretch-opening byte starts a viable token from the initial state.
     */
    [[nodiscard]] bool rescue_free() const;

    /**
     * @brief The first anchored-certified start in the tail at or after an offset, under the exact
     *        complete-repair invariance contract: every completely tokenizable repair of whatever preceded
     *        the tail places a token boundary there, the tail's end being the end of the input.
     *
     * The anchored decider is exact for that complete-repair question, where the certificate walk is
     * merely sound: certificates quantify over every input containing their evidence and cannot use the
     * end of input, while this query can, so on a repairable tail it answers at or before any certificate.
     * The certificates' own guarantee also binds repairs whose scans merely commit through their evidence
     * without completing, a larger set this decider does not speak about, so neither subsumes the other
     * outright. Decided by one scenario play per reachable state, no
     * repair enumerated. When no repair of any prefix makes the whole tokenizable, every position is
     * vacuously invariant and this query deliberately refuses instead of answering; nullable token sets sit
     * outside the underlying model and are refused outright, as for is_split_window().
     * @param tail The preserved suffix of the input, its end the end of input.
     * @param from The offset the search starts at; at or past the tail's size finds nothing.
     * @return The first anchored-certified position, or std::nullopt when none exists or the tail is
     *         beyond repair.
     */
    [[nodiscard]] std::optional<std::size_t> next_anchored_start(std::string_view tail, std::size_t from) const;

    /**
     * @brief A shortest repair for the tail: a byte string of minimal length whose concatenation with the
     *        tail is completely tokenizable, empty when the tail already tokenizes.
     *
     * Existence and minimality are exact: the minimal repair is a shortest path to a completing crossing
     * entry, and a refusal certifies that no repair of any length exists. Nullable token sets are refused,
     * as for next_anchored_start().
     * @param tail The preserved suffix of the input.
     * @return A minimal repair, or std::nullopt when the tail is beyond repair.
     */
    [[nodiscard]] std::optional<std::string> minimal_repair(std::string_view tail) const;

    /**
     * @brief Runs the DFA over a range defined by iterators.
     * @tparam Iterator Input iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Byte_iterator Iterator>
    [[nodiscard]] Match run(Iterator begin, Iterator end) const
    {
        if (begin == end)
        {
            return {.token = accepted(init_state_), .length = 0};
        }

        // A 64-bit state spares the dependency chain a zero-extension per byte when indexing the tables.
        std::size_t state{init_state_};

        // The last accepting state seen and the length of input it had consumed. The Token itself is resolved once
        // after the scan, keeping its load off the per-byte dependency chain.
        std::size_t accept_state{(flags_[state] & accept_flag_) != 0 ? state : no_state_};

        std::size_t accept_consumed{0};

        std::size_t consumed{0};

        // The tables only hold valid states: init_state_ indexes a column, and every entry is either a column index
        // or no_state_. The loop therefore needs no bounds checks.
        for (auto current{begin}; current != end; ++current)
        {
            const auto entry{table_[row_offsets_[static_cast<unsigned char>(*current)] + state]};

            if (entry == no_state_)
            {
                break;
            }

            state = entry;

            ++consumed;

            if (flags_[state] & accept_flag_)
            {
                prevent_if_conversion();

                accept_state = state;

                accept_consumed = consumed;
            }
        }

        return accept_state != no_state_ ? Match{.token = std::optional<Token>{accept_table_[accept_state].token},
                                                 .length = accept_consumed} :
                                           Match{.token = std::nullopt, .length = 0};
    }

    /**
     * @brief Runs the DFA over a container.
     * @tparam Container The container type (must be iterable).
     * @param container The input container.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Byte_iterable Container>
    [[nodiscard]] Match run(const Container& container) const
    {
        return run(std::ranges::begin(container), std::ranges::end(container));
    }

    /**
     * @brief Tokenizes a whole input in one pass, invoking the sink once per matched token.
     *
     * Equivalent to calling run() repeatedly at each token boundary, but the scan state stays live across tokens,
     * amortizing the per-call overhead. Random access is required because longest match may read past the last
     * accepting position and must resume from it. A zero-width match stops the scan rather than looping in place.
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving each matched token and its length.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length) for every matched token, in input order. A sink returning a value
     *        convertible to bool stops the scan by returning false; the stopping token still counts as tokenized.
     * @return The number of input elements tokenized; anything short of the input's size means no token matched at
     *         the returned offset, unless the sink stopped the scan. Input elements are read as unsigned char, so
     *         wider element types reduce modulo 256.
     */
    template <common::concepts::Random_access_byte_iterator Iterator, typename Sink>
        requires std::invocable<Sink&, const Token&, std::size_t, std::uint64_t>
    std::size_t run_all(Iterator begin, Iterator end, Sink sink) const
    {
        const auto size{static_cast<std::size_t>(end - begin)};

        std::size_t offset{0};

        while (offset < size)
        {
            // A 64-bit state spares the dependency chain a zero-extension per byte when indexing the tables.
            std::size_t state{init_state_};

            std::size_t accept_state{(flags_[state] & accept_flag_) != 0 ? state : no_state_};

            std::size_t accept_consumed{0};

            std::size_t consumed{0};

            for (auto current{begin + static_cast<std::ptrdiff_t>(offset)}; current != end; ++current)
            {
                const auto entry{table_[row_offsets_[static_cast<unsigned char>(*current)] + state]};

                if (entry == no_state_)
                {
                    break;
                }

                state = entry;

                ++consumed;

                if (flags_[state] & accept_flag_)
                {
                    prevent_if_conversion();

                    accept_state = state;

                    accept_consumed = consumed;
                }
            }

            if (accept_state == no_state_ || accept_consumed == 0)
            {
                return offset;
            }

            offset += accept_consumed;

            const auto& accept{accept_table_[accept_state]};

            if constexpr (std::convertible_to<
                                  std::invoke_result_t<Sink&, const Token&, std::size_t, std::uint64_t>, bool>)
            {
                if (!sink(accept.token, accept_consumed, accept.payload))
                {
                    return offset;
                }
            }
            else
            {
                sink(accept.token, accept_consumed, accept.payload);
            }
        }

        return offset;
    }

private:
    /**
     * @brief Type of a symbol equivalence class, i.e. a row index of the transition table.
     *
     * There are at most symbol_count_ classes, so the widest index fits.
     */
    using Class_t = std::uint8_t;

    /**
     * @brief Type of a transition table entry.
     *
     * Narrower than Dfa::State_t, as the table is read once per input character and halving it keeps twice as much
     * of it in cache. This bounds the number of states a DFA can have, which the constructor checks.
     */
    using Entry_t = std::uint32_t;

    /**
     * @brief Table entry marking the absence of a transition.
     */
    static constexpr Entry_t no_state_{std::numeric_limits<Entry_t>::max()};

    /**
     * @brief Per-state flag marking an accepting state.
     */
    static constexpr std::uint8_t accept_flag_{1};

    /**
     * @brief Per-state flag marking a live state: reachable from the initial state and able to still accept.
     */
    static constexpr std::uint8_t live_flag_{2};

    /**
     * @brief Number of distinct symbol values a transition can be labelled with, i.e. the size of per-symbol tables.
     */
    static constexpr std::size_t symbol_count_{1U << (sizeof(Label::Symbol_t) * 8U)};

    /**
     * @brief The equivalence class of each symbol value.
     *
     * Declared after the constants it depends on, out of size order.
     */
    using Classes_t = std::array<Class_t, symbol_count_>;

    /**
     * @brief What a state accepts: the token, and the caller's opaque word for it.
     *
     * Together because a reported match reads both at once: split across two arrays they cost a second cache line
     * per accepted token, measured at 12% on string-heavy input. Acceptance itself is left to flags_, which the scan
     * already tests, so the entry stays its old width and a lexer using no payload pays nothing.
     */
    struct Accept
    {
        Token token{0};

        std::uint64_t payload{0};
    };

    /**
     * @brief Whether the state accepts some token; the flag test, named once.
     */
    [[nodiscard]] bool is_accepting(const std::size_t state) const noexcept
    {
        return (flags_[state] & accept_flag_) != 0;
    }

    /**
     * @brief Whether the state is reachable and can still reach acceptance; the flag test, named once.
     */
    [[nodiscard]] bool is_live(const std::size_t state) const noexcept { return (flags_[state] & live_flag_) != 0; }

    /**
     * @brief Keeps the accepting-state updates on a branch rather than conditional moves.
     *
     * As conditional moves the updates make the accepted length data-dependent on every state load of the token,
     * so the next token's loads cannot start until that chain resolves; as a branch, consecutive tokens overlap in
     * the out-of-order window. An empty asm statement carries implicit volatile semantics, so it cannot be hoisted
     * out of the branch, while having no operands and no clobbers keeps it from emitting an instruction or
     * touching the dependency chain. Deliberately not a memory barrier. Clang 19 converts without it and loses
     * half its throughput; GCC 13 is unaffected either way. Deliberately not always_inline: both compilers inline
     * this at -O2 regardless, and the attribute makes gcov emit negative branch counts in coverage builds, which
     * fails the coverage report (GCC bug 68080). This is a measured workaround, not an invariant: recheck it when
     * the toolchain moves, and see docs/performance.md for the numbers.
     */
    static void prevent_if_conversion() noexcept { asm(""); }

    /**
     * @brief Groups the symbols of the DFA into equivalence classes.
     *
     * Two symbols are equivalent when every state either moves on both to the same state or on neither, i.e. when
     * their transition table rows would be identical.
     * @param dfa The DFA whose symbols are classified.
     * @return The class of each symbol value, numbered densely from zero.
     */
    [[nodiscard]] static Classes_t classify(const Dfa& dfa);

    /**
     * @brief The token a state accepts, or nullopt where it accepts nothing.
     */
    [[nodiscard]] std::optional<Token> accepted(const std::size_t state) const
    {
        return is_accepting(state) ? std::optional<Token>{accept_table_[state].token} : std::nullopt;
    }

    /**
     * @brief Fills split_points_ignoring_ from the tables the constructor has already built.
     * @param ignored The token IDs the caller discards.
     * @param reachable Which states a scan can arrive in.
     * @param co_accessible Which states can still reach acceptance.
     * @param predecessors The reverse index the constructor built for co-accessibility, reused here.
     * @param init_reentrant Whether a reachable state re-enters the initial state, as the exact map judges it.
     */
    void derive_split_points_ignoring(
            std::span<const std::size_t> ignored, const std::vector<bool>& reachable,
            const std::vector<bool>& co_accessible, const std::vector<std::vector<Entry_t>>& predecessors,
            bool init_reentrant);

    /**
     * @brief Derives and proves mandatory_core() from the live tables the constructor has already built.
     */
    void derive_mandatory_core();

    /**
     * @brief The state a simulation starts in.
     */
    Dfa::State_t init_state_;

    /**
     * @brief Whether any reachable transition re-enters the initial state.
     *
     * A nullable-free grammar can still re-enter its start state; when it does, arriving there no longer proves a
     * token boundary, and both the byte predicate and the window walk withdraw the initial-state exemption.
     */
    bool init_reentrant_{};

    /**
     * @brief The proved mandatory window core, empty when none is; see mandatory_core().
     */
    std::string mandatory_core_;

    /**
     * @brief Transitions as one row per symbol class and one column per state, holding no_state_ where there is none.
     */
    std::vector<Entry_t> table_;

    /**
     * @brief Accept entries as one per state, meaningful only where flags_ marks the state accepting.
     */
    std::vector<Accept> accept_table_;

    /**
     * @brief The flag byte of each state, read during the scan in place of the wide accept entries.
     */
    std::vector<std::uint8_t> flags_;

    /**
     * @brief The certified safe split points, as a 256-bit mask indexed by symbol value.
     */
    std::array<std::uint64_t, 4> split_points_{};

    /**
     * @brief The same bitmap under the weaker equivalence, indexed identically.
     *
     * A separate map rather than a mode on the first one: the two guarantees differ, so a caller that asks for one
     * must not silently receive the other. With an empty ignored set they hold the same bits.
     */
    std::array<std::uint64_t, 4> split_points_ignoring_{};

    /**
     * @brief The table offset of the class row of each symbol value.
     *
     * Holds `class * states` rather than the class itself, so looking a transition up is an addition and a read with
     * no multiplication left on the run() loop's critical path.
     */
    std::array<std::size_t, symbol_count_> row_offsets_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP

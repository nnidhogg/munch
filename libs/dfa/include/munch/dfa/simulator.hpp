#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP

#include <array>
#include <concepts>
#include <cstddef>
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
 * @brief Returns whether a transition table of the given states and symbol classes would overflow the given size limit.
 *
 * The compiled table is one row of states entries per class. On 64-bit platforms the product cannot overflow, since
 * states fit 32 bits and classes fit 8; on a 32-bit std::size_t it can, which the constructor refuses. Stated as its
 * own function with the limit as a parameter so the arithmetic is testable on every platform, even where the guard
 * itself is unreachable.
 * @param states The number of states the table has one column for.
 * @param classes The number of symbol equivalence classes the table has one row for.
 * @param limit The largest representable size the table's entry count must stay within.
 * @return True when the product of states and classes exceeds the limit.
 */
[[nodiscard]] constexpr bool table_size_overflows(
        const std::size_t states, const std::size_t classes, const std::size_t limit) noexcept
{
    // A table of zero classes has zero entries and cannot overflow, and testing it first keeps the division defined.
    return classes != 0 && states > limit / classes;
}

/**
 * @brief Runs a DFA over input sequences.
 *
 * Compiles the DFA it is constructed from into flat tables indexed by state and input symbol, so that advancing on an
 * input character is a single table read instead of a hash lookup. Symbols the automaton never distinguishes share a
 * table row: each input character is first mapped to its equivalence class, shrinking the table from one row per symbol
 * value to one per class, which keeps far more of it in cache. The tables have one column per identifier in the span
 * Dfa::state_count() reports: a DFA numbered densely from zero, as the subset construction numbers them, fills them,
 * and one numbered sparsely, as dfa::Builder allows, still works but wastes a table column per unused identifier. A
 * definition whose span no count holds, one naming a state at the largest std::size_t, the constructor refuses before
 * it unrolls or sizes anything.
 *
 * Acceptance is tracked during the scan through a per-state flag byte rather than the accept tokens themselves: the
 * flag load depends on the new state but feeds nothing, so it stays off the state-to-state dependency chain, and the
 * matched Token is resolved exactly once after the scan.
 *
 * The one-line table reads stay in this header rather than the source, against the usual rule: core::Lexer's
 * chunk_boundaries() calls is_split_point() once per byte, and moving the eight of them out cost 22% on plan/rare and
 * 24% on plan/absent, measured at 16 MiB over fifteen passes.
 *
 * Beside the scan, the class carries what construction derives: the byte certificates and the mandatory window core.
 * The decisions built on the compiled machine live beside it as free functions over its read-only view, state_count()
 * through is_live(): is_split_window() in split_window.hpp, the recovery decisions in recovery.hpp, anchor_free_span()
 * in the header of its name and the five boundary-guessing decisions, rescue() through segmentation_difference(), in
 * boundary_search.hpp, so that the machine and each decision can be read on their own and a new decision adds nothing
 * here.
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
     * @brief Number of distinct symbol values a transition can be labelled with, i.e. the size of per-symbol tables;
     *        the one member of the read-only view below that is a constant rather than a function.
     */
    static constexpr std::size_t symbol_count{1U << (sizeof(Label::Symbol_t) * 8U)};

    /**
     * @brief Compiles the given DFA into transition and accept tables.
     * @param dfa The DFA to simulate.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition table's
     *         size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    explicit Simulator(const Dfa& dfa);

    /**
     * @brief Compiles the given DFA, additionally certifying split points modulo a set of discarded tokens.
     * @param dfa The DFA to simulate.
     * @param ignored The IDs of tokens the caller discards before the stream is used.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition table's
     *         size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    Simulator(const Dfa& dfa, std::span<const std::size_t> ignored);

    /**
     * @brief Compiles the given DFA, attaching a caller's word to every state accepting one of the named tokens.
     *
     * Stored by accepting state and handed to run_all()'s sink with each consumed token, never interpreted here, so a
     * caller answers a per-token question without a second lookup and state numbering stays inside this class.
     * @param dfa The DFA to simulate.
     * @param ignored The IDs of tokens the caller discards before the stream is used.
     * @param payloads Token ID and word pairs; a token named more than once keeps the last word given.
     * @throws std::runtime_error If the DFA has more states than a table entry can index, or if the transition table's
     *         size would overflow std::size_t, which only a 32-bit platform can reach.
     */
    Simulator(
            const Dfa& dfa, std::span<const std::size_t> ignored,
            std::span<const std::pair<std::size_t, std::uint64_t>> payloads);

    /**
     * @brief Returns whether the given symbol is a certified safe split point.
     *
     * A symbol is a safe split point when no state reachable after consuming input consumes it into a state that can
     * still accept, so on any completely tokenizable input every occurrence can only be the first byte of a token: a
     * scan reaching it mid-token either finds no transition or enters a state from which acceptance is unreachable, so
     * it may read farther but records no further accepting position and its token ended at an earlier one, and a token
     * can only contain it by starting with it. Input the serial scan cannot tokenize completely carries the promise
     * only up to the offset where that scan first fails, even though a valid prefix of tokens may be emitted before it;
     * see core::Lexer::tokenize_all_parallel() for what survives there. For input that tokenizes completely, splitting
     * immediately before such a symbol therefore produces the identical token stream, which is what makes chunked
     * processing of one large input safe. The initial state is exempt only while no transition re-enters it; a nullable
     * set is compiled through a fresh start state that never is, and a kleene token's own symbol is then consumed by
     * the looping state behind it and certifies nothing. A symbol no live state consumes is safe only vacuously, since
     * no input this lexer accepts contains it, and is deliberately not reported: a caller cannot use it, and searching
     * for one scans the whole input for nothing. Transitions into states that can never accept are ignored throughout,
     * since no emitted token can traverse one; a pattern denoting the empty language leaves exactly such states behind.
     * States the initial state cannot reach are ignored on the same grounds, since no scan can arrive in one; a Dfa
     * built by subset construction has none, but one assembled by hand may. Token sets whose runs or literals may
     * contain any byte certify no split points.
     * @param symbol The symbol to test.
     * @return True if every occurrence of the symbol begins a token; false for symbols that satisfy the condition only
     *         vacuously.
     */
    [[nodiscard]] bool is_split_point(const char symbol) const noexcept
    {
        const auto value{static_cast<unsigned char>(symbol)};

        return ((split_points_[value >> 6U] >> (value & 63U)) & 1U) != 0;
    }

    /**
     * @brief Returns whether the symbol is a safe split point once discarded tokens are deleted from the stream.
     *
     * Weaker than is_split_point(), and never stronger: every certified symbol satisfies this too, and with an empty
     * ignored set the two coincide. A state may consume the symbol into a state that can still accept, provided the
     * token the cut would sever vanishes from both streams. That holds when the state accepts an ignored token, when
     * every token the severed one can still become past the cut is ignored, and when advancing on the symbol from it
     * and from the initial state reach states with the same future once ignored kinds are not told apart, so the
     * restarted scan ends its token at the same byte as the interrupted one, also ignored, and only that one token is
     * disturbed. The test is sound and still not complete: some symbols safe under this equivalence are refused.
     *
     * The guarantee is correspondingly weaker in two independent ways. Chunks cut here reproduce the serial stream only
     * after tokens of the ignored kinds are deleted from both, so a caller that keeps them must use is_split_point()
     * instead. And it holds only for input the serial scan tokenizes completely: past the offset where that scan first
     * fails, a chunk cut here can run on and emit kept tokens the serial scan never reaches, so a boundary must lie
     * before that offset to be covered at all.
     *
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
     * @brief Returns whether the token set certifies any usable split point.
     * @return True if at least one symbol is a split point.
     */
    [[nodiscard]] bool has_split_points() const noexcept
    {
        return (split_points_[0] | split_points_[1] | split_points_[2] | split_points_[3]) != 0;
    }

    /**
     * @brief Returns whether the token set certifies any usable split point once discarded tokens are deleted.
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
     * @brief Returns whether some token matches the empty string.
     *
     * The tables never show it: a nullable set is compiled as its positive-width equivalent, the automaton entered
     * through a fresh start state that does not accept, since the scan never emits an empty token and every decision's
     * proof assumes a start state that does not accept. The fresh start is never re-entered either; the start of a set
     * that is not nullable is compiled as it is and may be, which init_reentrant() reports and every decision allows
     * for. What remains of the empty match is what the scan reports for it, the empty input and a first byte no token
     * matches, both answered with the token the old start state accepted and length zero.
     * @return True when some token matches the empty string.
     */
    [[nodiscard]] bool nullable() const noexcept { return empty_state_ != no_state_; }

    /**
     * @brief Returns the byte string every certified split window provably contains, or empty when none is proved.
     *
     * Derived once at construction. A live state whose every transition is live cannot be killed by any single byte,
     * only led along a longer word to death, so any window certifying in its presence must carry that state's forced
     * exit; the shortest exit is proposed as a core and proved mandatory by exhausting core-avoiding death words over
     * the live tables, with the killing byte never fed to the matcher. The longest proved core is kept: every window
     * is_split_window() certifies contains it with at least one byte following, which is what lets the planner narrow
     * its candidate windows to occurrences of this string. Empty means no core is proved, because no such state exists
     * or the candidate was refuted by a core-free death word; the planner then keeps its exhaustive walk, and nothing
     * weakens: the core is an accelerator's licence, never a certificate itself.
     * @return The proved mandatory core, or an empty view.
     */
    [[nodiscard]] std::string_view mandatory_core() const noexcept { return mandatory_core_; }

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
            return {.token = empty_match(), .length = 0};
        }

        // A 64-bit state spares the dependency chain a zero-extension per byte when indexing the tables.
        std::size_t state{init_state_};

        // The last accepting state seen and the length of input it had consumed, the empty match to begin with where
        // the set has one. The Token itself is resolved once after the scan, keeping its load off the per-byte
        // dependency chain.
        std::size_t accept_state{empty_state_};

        std::size_t accept_consumed{0};

        std::size_t consumed{0};

        // The tables only hold valid states: init_state_ indexes a column, and every entry is either a column index or
        // no_state_. The loop therefore needs no bounds checks.
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
     * @brief Tokenizes a whole input in one call, invoking the sink once per consumed token.
     *
     * Consumes the same positive-width tokens, IDs and lengths, as calling run() repeatedly at each token boundary,
     * invoking the sink after each and stopping when the sink returns false, but the loop stays in one call across
     * tokens, amortizing the per-call overhead; the automaton restarts at each token as run() does. Random access is
     * required because longest match may read past the last accepting position and must resume from it. A zero-width
     * match stops the scan rather than looping in place.
     * @tparam Iterator Random access iterator type.
     * @tparam Sink Callable receiving each consumed token, its length, and its payload.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length, payload) for every consumed token, in input order, the payload being
     *        the word the constructor's payloads attached to that token, zero when none was. The Simulator takes the
     *        three-argument form only; the Lexer adapts a two-argument sink. A sink returning a value convertible to
     *        bool stops the scan by returning false; the stopping token still counts.
     * @return The number of input elements tokenized; anything short of the input's size means the scan stopped at the
     *         returned offset: no token matched there, only the empty one did, or the sink returned false. Input
     *         elements are read as unsigned char, so wider element types reduce modulo 256.
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

            std::size_t accept_state{no_state_};

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

            if (accept_state == no_state_)
            {
                return offset;
            }

            offset += accept_consumed;

            const auto& [token, payload]{accept_table_[accept_state]};

            if constexpr (std::convertible_to<
                                  std::invoke_result_t<Sink&, const Token&, std::size_t, std::uint64_t>, bool>)
            {
                if (!sink(token, accept_consumed, payload))
                {
                    return offset;
                }
            }
            else
            {
                sink(token, accept_consumed, payload);
            }
        }

        return offset;
    }

    /**
     * @brief Returns the number of states the tables hold a column for, one past the highest state identifier.
     *
     * With init_state(), init_reentrant(), step(), is_accepting(), is_live(), accepted() and symbol_count, the view of
     * the compiled machine that the decisions in split_window.hpp, recovery.hpp, anchor_free_span.hpp and
     * boundary_search.hpp are written over, so that a new decision needs nothing this class keeps private.
     * @return That count.
     */
    [[nodiscard]] std::size_t state_count() const noexcept { return flags_.size(); }

    /**
     * @brief Returns the state a scan starts in and restarts in at every token boundary.
     * @return That state.
     */
    [[nodiscard]] std::size_t init_state() const noexcept { return init_state_; }

    /**
     * @brief Returns whether some live transition re-enters the initial state.
     *
     * A nullable-free token set can still re-enter its start state; when it does, arriving there no longer proves a
     * token boundary, and both the byte certificate and the window decision withdraw the initial-state exemption.
     * @return True when a live transition leads back to init_state().
     */
    [[nodiscard]] bool init_reentrant() const noexcept { return init_reentrant_; }

    /**
     * @brief Follows one transition of the compiled table.
     *
     * The step every anchored walk takes; the tables hold only valid states, so no bounds check is needed.
     * @param state The state the walk stands in.
     * @param symbol The byte read.
     * @return The state the transition leads to, or std::nullopt where the table has none.
     */
    [[nodiscard]] std::optional<std::size_t> step(std::size_t state, unsigned char symbol) const noexcept;

    /**
     * @brief Returns whether the state accepts some token; the flag test, named once.
     * @param state The state to test.
     * @return True when the state's flag byte marks it accepting.
     */
    [[nodiscard]] bool is_accepting(const std::size_t state) const noexcept
    {
        return (flags_[state] & accept_flag_) != 0;
    }

    /**
     * @brief Returns whether the state is reachable and can still reach acceptance; the flag test, named once.
     * @param state The state to test.
     * @return True when the state's flag byte marks it live.
     */
    [[nodiscard]] bool is_live(const std::size_t state) const noexcept { return (flags_[state] & live_flag_) != 0; }

    /**
     * @brief Returns the token a state accepts, or std::nullopt where it accepts nothing.
     * @param state The state to resolve.
     * @return The accepted token, or std::nullopt when the state accepts nothing.
     */
    [[nodiscard]] std::optional<Token> accepted(const std::size_t state) const
    {
        if (!is_accepting(state))
        {
            return std::nullopt;
        }

        return accept_table_[state].token;
    }

private:
    /**
     * @brief Type of a symbol equivalence class, i.e. a row index of the transition table.
     *
     * There are at most symbol_count classes, so the widest index fits.
     */
    using Class_t = std::uint8_t;

    /**
     * @brief Type of a transition table entry.
     *
     * Narrower than Dfa::State_t, as the table is read once per input character and halving it keeps twice as much of
     * it in cache. This bounds the number of states a DFA can have, which the constructor checks.
     */
    using Entry_t = std::uint32_t;

    /**
     * @brief The equivalence class of each symbol value.
     */
    using Classes_t = std::array<Class_t, symbol_count>;

    /**
     * @brief What a state accepts: the token, and the caller's opaque word for it.
     *
     * Together because a reported match reads both at once: split across two arrays they cost a second cache line per
     * accepted token, measured at 12% on string-heavy input. Acceptance itself is left to flags_, which the scan
     * already tests, so the entry keeps one token's width and a lexer using no payload pays nothing.
     */
    struct Accept
    {
        /**
         * @brief The token the state accepts.
         */
        Token token{0};

        /**
         * @brief The caller's opaque word for that token, zero where the constructor attached none.
         */
        std::uint64_t payload{0};
    };

    /**
     * @brief The shortest death words of the live states, the material the mandatory core is proposed from.
     *
     * A death word of a live state is a word leading it, through live states only, to a byte with no live target. The
     * words are never stored: each state keeps the first byte of its chosen word and the state that byte leads to, and
     * a word is spelled by walking that chain.
     */
    struct Death_words
    {
        /**
         * @brief Returns whether a death word of two bytes or more leaves a state, so that its chain has a next link.
         * @param state The state.
         * @return False when no death word leaves the state or its shortest is one byte.
         */
        [[nodiscard]] bool has_chain(std::size_t state) const noexcept;

        /**
         * @brief Per state, the length of its shortest death word, no_death_word_ where none leaves it.
         */
        std::vector<std::size_t> depth{};

        /**
         * @brief Per state, the first byte of its chosen death word: the killing byte at depth one, else the smallest
         *        byte stepping one layer shallower.
         */
        std::vector<char> entered_by{};

        /**
         * @brief Per state of depth two or more, the state its first byte leads to, the next link of its chain.
         */
        std::vector<std::size_t> onward{};
    };

    /**
     * @brief The stamp of one core proof in the visited buffer every proof shares.
     */
    using Stamp_t = std::uint32_t;

    /**
     * @brief A matcher position: the length of the longest prefix of a core the bytes read so far end with.
     */
    using Prefix_t = std::size_t;

    /**
     * @brief Table entry marking the absence of a transition.
     */
    static constexpr Entry_t no_state_{std::numeric_limits<Entry_t>::max()};

    /**
     * @brief The death word depth of a state no death word leaves.
     */
    static constexpr std::size_t no_death_word_{std::numeric_limits<std::size_t>::max()};

    /**
     * @brief Per-state flag marking an accepting state.
     */
    static constexpr std::uint8_t accept_flag_{1};

    /**
     * @brief Per-state flag marking a live state: reachable from the initial state and able to still accept.
     */
    static constexpr std::uint8_t live_flag_{2};

    /**
     * @brief Refuses a state count no table entry can index.
     *
     * One table column per state the definition spans. A hand-built DFA may number states sparsely, up to a highest
     * identifier no count holds, which the DFA reports as a count of zero: neither that nor a count reaching the entry
     * width's sentinel can index a table. The definition is refused as given, before it is unrolled: unroll_start()
     * enters the automaton through the count, which is one of the DFA's own states where the count is the wrap.
     * @param states The state count.
     * @throws std::runtime_error If the count is zero or reaches the entry width's sentinel.
     */
    static void require_indexable(std::size_t states);

    /**
     * @brief Groups the symbols of the DFA into equivalence classes.
     *
     * Two symbols are equivalent when every state either moves on both to the same state or on neither, i.e. when their
     * transition table rows would be identical.
     * @param dfa The DFA whose symbols are classified.
     * @return The class of each symbol value, numbered densely from zero.
     */
    [[nodiscard]] static Classes_t classify(const Dfa& dfa);

    /**
     * @brief Fills row_offsets_ from the symbol classes.
     * @param classes The class of each symbol value.
     * @param states The number of table columns.
     */
    void index_rows(const Classes_t& classes, std::size_t states);

    /**
     * @brief Fills accept_table_ and the accepting flags, attaching each named token's word to its accepting states.
     * @param dfa The DFA being compiled.
     * @param payloads Token ID and word pairs; a token named more than once keeps the last word given.
     */
    void fill_accepts(const Dfa& dfa, std::span<const std::pair<std::size_t, std::uint64_t>> payloads);

    /**
     * @brief Fills table_ with the DFA's transitions, no_state_ wherever there is none.
     * @param dfa The DFA being compiled.
     * @param classes The class of each symbol value.
     * @param class_count The number of table rows.
     */
    void fill_transitions(const Dfa& dfa, const Classes_t& classes, std::size_t class_count);

    /**
     * @brief Returns the reverse index of the transition table: for each state, the states stepping into it.
     *
     * Built once per class row, not once per symbol value: the table is compressed by symbol equivalence class, so
     * walking all 256 values would push the same edge once per symbol sharing a class and leave the index larger than
     * the table it indexes.
     * @return The predecessors of each state, rows ascending and states ascending within a row.
     */
    [[nodiscard]] std::vector<std::vector<Entry_t>> predecessors() const;

    /**
     * @brief Returns the table offset of every distinct class row, ascending.
     * @return The row offsets, each once.
     */
    [[nodiscard]] std::vector<std::size_t> distinct_rows() const;

    /**
     * @brief Marks the states from which acceptance is still reachable.
     *
     * Only transitions that can still end in acceptance can lie on an emitted token. A pattern denoting the empty
     * language, such as any_of() over an empty set, leaves reachable states behind from which no accepting state is
     * reachable; a scan entering one always fails, so the bytes it consumes are consumed by no token and must not be
     * allowed to de-certify anything.
     * @param predecessors The reverse index of the transition table.
     * @return Per state, whether some accepting state is reachable from it.
     */
    [[nodiscard]] std::vector<bool> co_accessible_states(const std::vector<std::vector<Entry_t>>& predecessors) const;

    /**
     * @brief Marks the states a scan can arrive in from the initial state.
     *
     * Symmetrically to co-accessibility, only states a scan can actually arrive in may de-certify a symbol. A Dfa
     * handed to the Simulator directly can number states no input reaches, and an unreachable live island, or an
     * unreachable transition back into the initial state, would otherwise cost certificates that no scan can ever
     * contradict. Builder's subset construction supplies reachability on its own, so this only matters for a hand-built
     * Dfa.
     * @return Per state, whether it is reachable.
     */
    [[nodiscard]] std::vector<bool> reachable_states() const;

    /**
     * @brief Returns the transition table entry of a symbol and a state.
     * @param symbol The symbol value.
     * @param state The state.
     * @return The target state, or no_state_ when there is no transition.
     */
    [[nodiscard]] Entry_t entry(std::size_t symbol, std::size_t state) const noexcept;

    /**
     * @brief Returns whether some reachable state steps back into the initial state.
     *
     * A symbol only the initial state consumes can only begin a token, but the exemption is valid only while the
     * initial state cannot be reached again after consuming input, where the "first byte of a token" reasoning no
     * longer holds. A compiled start state can be re-entered, `[\n]*b` returning to it on every newline, and unrolling
     * separates only an accepting start, so the test decides for compiled and hand-built tables alike.
     * @param reachable Which states a scan can arrive in.
     * @return True when a reachable transition re-enters the initial state.
     */
    [[nodiscard]] bool reenters_init(const std::vector<bool>& reachable) const;

    /**
     * @brief Flags every state both reachable and co-accessible as live.
     * @param reachable Which states a scan can arrive in.
     * @param co_accessible Which states can still reach acceptance.
     */
    void mark_live(const std::vector<bool>& reachable, const std::vector<bool>& co_accessible);

    /**
     * @brief Fills split_points_ from the tables already built.
     *
     * A symbol no live state consumes is certified vacuously: no input this lexer accepts can contain it, so it is
     * useless to a caller and searching for one scans to the end of the input for nothing. Since a certified symbol is
     * consumed only by the initial state, it can occur in valid input exactly when the initial state consumes it into a
     * state that can still accept, and only those are reported.
     * @param reachable Which states a scan can arrive in.
     * @param co_accessible Which states can still reach acceptance.
     * @param init_reentrant Whether a reachable state re-enters the initial state.
     */
    void derive_split_points(
            const std::vector<bool>& reachable, const std::vector<bool>& co_accessible, bool init_reentrant);

    /**
     * @brief Returns whether a state consumes a symbol into a state that can still accept.
     * @param symbol The symbol value.
     * @param state The state.
     * @param co_accessible Which states can still reach acceptance.
     * @return True when it does.
     */
    [[nodiscard]] bool consumes(std::size_t symbol, std::size_t state, const std::vector<bool>& co_accessible) const;

    /**
     * @brief Fills split_points_ignoring_ from the tables the constructor has already built.
     *
     * Condition three of the weaker certificate asks whether every token still reachable from a state is discarded: the
     * complement of "some kept token is still reachable", which one backward closure settles for every state at once.
     * The closure walks the reverse index the constructor built for co-accessibility, and whether a state accepts a
     * discarded token is resolved once per state, so the derivation stays linear in the table.
     *
     * A state leaves a symbol certified when the left chunk ends on a complete token the caller discards and the
     * severed token's rest only ever becomes discarded tokens. The restart need not land where the interrupted scan is,
     * only in a state whose future is the same once discarded kinds are not told apart: both scans then end their
     * tokens at the same byte, both discarded, and are back in step from there.
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
     * @brief Returns the right-language classes of the states once every discarded kind shares one colour.
     *
     * Moore's refinement with every discarded kind given one colour: two states end up in one class exactly when every
     * continuation is accepted from both or from neither, and with the same kind wherever that kind is kept. Index
     * `states` stands for the missing transition, a sink that accepts nothing, so a partial table needs no completion.
     * @param accepts_discarded Per state, whether it accepts a token of a discarded kind.
     * @return One class per state, and one more at index size() for the missing transition.
     */
    [[nodiscard]] std::vector<std::size_t> observed_classes(const std::vector<bool>& accepts_discarded) const;

    /**
     * @brief Derives and proves mandatory_core() from the live tables the constructor has already built.
     *
     * The planner's accelerator licence. A live state alive on every byte survives any window that lacks its forced
     * exit, so a window certifying at all must carry that exit: the derivation proposes each such state's shortest exit
     * and keeps the longest one that survives the proof, which exhausts core-avoiding death words over the live tables
     * and refutes the candidate on the first one found. The matcher reads only the live prefix; the killing byte is
     * never fed to it, since a core completing on the killing byte is too late. Refusal leaves the core empty and the
     * planner exhaustive: the core is an accelerator's licence, never a certificate.
     */
    void derive_mandatory_core();

    /**
     * @brief Returns the shortest death word depths by search from the deaths backward.
     *
     * Depth one where some byte has no live target, and each layer of the reverse traversal one byte deeper, every live
     * transition read once. A state no death word leaves keeps no_death_word_ and proposes nothing.
     * @return The depths, with the killing byte of every depth-one state; the chains are not linked yet.
     */
    [[nodiscard]] Death_words death_depths() const;

    /**
     * @brief Returns the live successor of a state on a symbol.
     * @param state The state.
     * @param symbol The symbol value.
     * @return The successor, or std::nullopt when the step is undefined or leads to a dead state.
     */
    [[nodiscard]] std::optional<std::size_t> advance_live(std::size_t state, std::size_t symbol) const;

    /**
     * @brief Returns, for each live state, the live states stepping into it, each once.
     * @return The reverse live index, sources ascending.
     */
    [[nodiscard]] std::vector<std::vector<std::size_t>> live_sources() const;

    /**
     * @brief Links each death word's chain once the depths are final: each state of depth two or more keeps the
     *        smallest byte stepping one layer shallower and the state it leads to.
     * @param words The depths and the depth-one killing bytes.
     * @return The same words with every chain linked.
     */
    [[nodiscard]] Death_words chain_death_words(Death_words words) const;

    /**
     * @brief Returns the states that propose a core, in state order.
     *
     * Candidates come from input-total states of the required set: a non-re-entrant initial state is exempt, since its
     * window hypothesis renames rather than survives. The core is the death word with the killing byte removed, and a
     * depth of at least two is input-totality itself, since the seeding pass gave depth one to every live state missing
     * a byte. Only the state is kept; its core is spelled when its proof runs.
     * @param words The linked death words.
     * @return The proposing states.
     */
    [[nodiscard]] std::vector<std::size_t> core_candidates(const Death_words& words) const;

    /**
     * @brief Spells a candidate's core by walking its chain of death-word bytes.
     * @param words The linked death words.
     * @param origin The proposing state.
     * @return The core, its death word without the killing byte.
     */
    [[nodiscard]] static std::string spell_core(const Death_words& words, std::size_t origin);

    /**
     * @brief Proves a candidate's core: no core-avoiding death word leaves the proposing state.
     *
     * A stack-driven reachability search over pairs of a live state and a matcher prefix, refuted the moment any
     * reachable pair meets a byte with no live target, since the word spelled to that point is a core-avoiding death
     * word. Pairs whose matcher completed the core are satisfied for every continuation and are not expanded; visiting
     * order carries nothing, only the reachable set.
     * @param origin The proposing state.
     * @param core The core.
     * @param stamp The proof's stamp, distinct from every earlier proof's.
     * @param seen The visited buffer every proof shares, one cell per state and prefix of the longest core; an entry
     *        from an older proof reads as unseen under a newer stamp.
     * @return True when the core is proved.
     */
    [[nodiscard]] bool proves_core(
            std::size_t origin, const std::string& core, Stamp_t stamp, std::vector<Stamp_t>& seen) const;

    /**
     * @brief Returns the matcher of a core precomputed as a table, one lookup per transition.
     *
     * A graph search defeats the usual amortization of chained failure links, so paying them once here keeps a proof's
     * cost at the pairs it visits.
     * @param core The core.
     * @return Per prefix length and symbol value, the prefix length after reading the symbol.
     */
    [[nodiscard]] static std::vector<Prefix_t> core_matcher(const std::string& core);

    /**
     * @brief Returns the token the empty string matches, or std::nullopt when no token does.
     * @return That token.
     */
    [[nodiscard]] std::optional<Token> empty_match() const
    {
        if (empty_state_ == no_state_)
        {
            return std::nullopt;
        }

        return accept_table_[empty_state_].token;
    }

    /**
     * @brief Keeps the accepting-state updates on a branch rather than conditional moves.
     *
     * As conditional moves the updates make the accepted length data-dependent on every state load of the token, so the
     * next token's loads cannot start until that chain resolves; as a branch, consecutive tokens overlap in the
     * out-of-order window. An empty asm statement carries implicit volatile semantics, so it cannot be hoisted out of
     * the branch, while having no operands and no clobbers keeps it from emitting an instruction or touching the
     * dependency chain. Deliberately not a memory barrier. Clang 19 converts without it and loses half its throughput;
     * GCC 13 is unaffected either way. Deliberately not always_inline: both compilers inline this at -O2 regardless,
     * and the attribute makes gcov emit negative branch counts in coverage builds, which fails the coverage report (GCC
     * bug 68080). This is a measured workaround, not an invariant: recheck it when the toolchain moves, and see
     * docs/performance.md for the numbers.
     */
    static void prevent_if_conversion() noexcept { asm(""); }

    /**
     * @brief The state a simulation starts in.
     */
    Dfa::State_t init_state_;

    /**
     * @brief The start state of the set as given, kept only when it accepted, so the empty match is still known;
     *        no_state_ otherwise. The compiled start state, init_state_, is then the fresh one in front of it.
     */
    Entry_t empty_state_{no_state_};

    /**
     * @brief Whether any reachable transition re-enters the initial state.
     *
     * A nullable-free grammar can still re-enter its start state; when it does, arriving there no longer proves a token
     * boundary, and both the byte predicate and the window walk withdraw the initial-state exemption.
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
     * A separate map rather than a mode on the first one: the two guarantees differ, so a caller that asks for one must
     * not silently receive the other. With an empty ignored set they hold the same bits.
     */
    std::array<std::uint64_t, 4> split_points_ignoring_{};

    /**
     * @brief The table offset of the class row of each symbol value.
     *
     * Holds `class * states` rather than the class itself, so looking a transition up is an addition and a read with no
     * multiplication left on the run() loop's critical path.
     */
    std::array<std::size_t, symbol_count> row_offsets_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP

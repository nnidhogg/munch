#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SIMULATOR_HPP

#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/dfa/dfa.hpp"

namespace munch::dfa
{
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
     * @brief Number of distinct symbol values a transition can be labelled with, i.e. the size of per-symbol tables.
     */
    static constexpr std::size_t symbol_count_{1U << (sizeof(Label::Symbol_t) * 8U)};

    /**
     * @brief The equivalence class of each symbol value.
     *
     * Declared after the constants it depends on, out of size order.
     */
    using Classes_t = std::array<Class_t, symbol_count_>;

public:
    /**
     * @brief The result of one match attempt: the matched token, if any, and the length of the match.
     */
    struct Match
    {
        std::optional<Token> token;

        std::size_t length;

        bool operator==(const Match&) const = default;
    };

    /**
     * @brief Compiles the given DFA into transition and accept tables.
     * @param dfa The DFA to simulate.
     * @throws std::runtime_error If the DFA has more states than a table entry can index.
     */
    explicit Simulator(const Dfa& dfa);

    /**
     * @brief Runs the DFA over a range defined by iterators.
     * @tparam Iterator Input iterator type.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Iterator Iterator>
    [[nodiscard]] Match run(Iterator begin, Iterator end) const
    {
        if (begin == end)
        {
            return {accept_table_[init_state_], 0};
        }

        auto state{static_cast<Entry_t>(init_state_)};

        // The last accepting state seen and the length of input it had consumed. The Token itself is resolved once
        // after the scan, keeping its load off the per-byte dependency chain.
        auto accept_state{accept_table_[state] ? state : no_state_};

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
                accept_state = state;

                accept_consumed = consumed;
            }
        }

        return accept_state != no_state_ ? Match{accept_table_[accept_state], accept_consumed} : Match{std::nullopt, 0};
    }

    /**
     * @brief Runs the DFA over a container.
     * @tparam Container The container type (must be iterable).
     * @param container The input container.
     * @return The match: the token, if any, and the length it consumed.
     */
    template <common::concepts::Iterable Container>
    [[nodiscard]] Match run(const Container& container) const
    {
        return run(std::begin(container), std::end(container));
    }

    /**
     * @brief Returns whether the given symbol is a certified safe split point.
     *
     * A symbol is a safe split point when no state reachable after consuming input consumes it, so every
     * occurrence in any input can only be the first byte of a token: a scan reaching it mid-token finds no
     * transition and must have ended its token earlier, and a token can only contain it by starting with it.
     * Input split immediately before such a symbol therefore tokenizes identically to the unsplit input, which is
     * what makes chunked processing of one large input safe. The initial state is exempt only while no transition
     * re-enters it; a nullable pattern such as a kleene token minimizes to an accepting, self-looping start state,
     * and its symbols certify nothing. A symbol no state consumes is vacuously safe: input holding it fails at it
     * either way. Token sets whose runs or literals may contain any byte certify no split points.
     */
    [[nodiscard]] bool is_split_point(const char symbol) const noexcept
    {
        const auto value{static_cast<unsigned char>(symbol)};

        return ((split_points_[value >> 6U] >> (value & 63U)) & 1U) != 0;
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
     *         the returned offset, unless the sink stopped the scan.
     */
    template <std::random_access_iterator Iterator, std::invocable<const Token&, std::size_t> Sink>
    std::size_t run_all(Iterator begin, Iterator end, Sink sink) const
    {
        const auto size{static_cast<std::size_t>(end - begin)};

        std::size_t offset{0};

        while (offset < size)
        {
            auto state{static_cast<Entry_t>(init_state_)};

            auto accept_state{(flags_[state] & accept_flag_) != 0 ? state : no_state_};

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
                    accept_state = state;

                    accept_consumed = consumed;
                }
            }

            if (accept_state == no_state_ || accept_consumed == 0)
            {
                return offset;
            }

            offset += accept_consumed;

            if constexpr (std::convertible_to<std::invoke_result_t<Sink&, const Token&, std::size_t>, bool>)
            {
                if (!sink(*accept_table_[accept_state], accept_consumed))
                {
                    return offset;
                }
            }
            else
            {
                sink(*accept_table_[accept_state], accept_consumed);
            }
        }

        return offset;
    }

private:
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
     * @brief The state a simulation starts in.
     */
    Dfa::State_t init_state_;

    /**
     * @brief Transitions as one row per symbol class and one column per state, holding no_state_ where there is none.
     */
    std::vector<Entry_t> table_;

    /**
     * @brief Accept tokens as one entry per state, holding nullopt where the state does not accept.
     */
    std::vector<std::optional<Token>> accept_table_;

    /**
     * @brief The flag byte of each state, read during the scan in place of the wide accept entries.
     */
    std::vector<std::uint8_t> flags_;

    /**
     * @brief The certified safe split points, as a 256-bit mask indexed by symbol value.
     */
    std::array<std::uint64_t, 4> split_points_{};

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

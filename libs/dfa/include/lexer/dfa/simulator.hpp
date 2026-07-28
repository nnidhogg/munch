#ifndef LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP
#define LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP

#include <array>
#include <cstdint>
#include <experimental/mdspan>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "lexer/common/concepts.hpp"
#include "lexer/dfa/dfa.hpp"

namespace lexer::dfa
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
     * @brief Two-dimensional `(class, state)` view over a transition table.
     *
     * Rows are per class rather than per state, so the row offset of a lookup depends only on the input character,
     * which is known before the state it is consumed in: the offset computation stays off the state-to-state
     * dependency chain that limits how fast the run() loop can advance.
     * @tparam Entry The viewed entry type, const-qualified for reading.
     */
    template <typename Entry>
    using Table_view_t = std::mdspan<Entry, std::dextents<std::size_t, 2>>;

public:
    /**
     * @brief The result type: a pair of the matched token (if any) and the length of the match.
     */
    using Result_t = std::pair<std::optional<Token>, std::size_t>;

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
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <common::concepts::Iterator Iterator>
    [[nodiscard]] Result_t run(Iterator begin, Iterator end) const
    {
        if (begin == end)
        {
            return {std::nullopt, 0};
        }

        auto state{init_state_};

        // The tables only hold valid states: init_state_ indexes a column, and every entry is either a column index
        // or no_state_. The loop therefore needs no bounds checks.
        Result_t result{accept_table_[state], 0};

        for (Iterator current{begin}; current != end; ++current)
        {
            const auto entry{table_[row_offsets_[static_cast<unsigned char>(*current)] + state]};

            if (entry == no_state_)
            {
                break;
            }

            state = entry;

            if (const auto& token = accept_table_[state]; token)
            {
                result = {token, static_cast<std::size_t>(std::distance(begin, current)) + 1};
            }
        }

        return result;
    }

    /**
     * @brief Runs the DFA over a container.
     * @tparam Container The container type (must be iterable).
     * @param container The input container.
     * @return A pair containing the matched token (if any) and the length of the match.
     */
    template <common::concepts::Iterable Container>
    [[nodiscard]] Result_t run(const Container& container) const
    {
        return run(std::begin(container), std::end(container));
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
     * @brief The table offset of the class row of each symbol value.
     *
     * Holds `class * states` rather than the class itself, so looking a transition up is an addition and a read with
     * no multiplication left on the run() loop's critical path.
     */
    std::array<std::size_t, symbol_count_> row_offsets_;
};

} // namespace lexer::dfa

#endif // LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP

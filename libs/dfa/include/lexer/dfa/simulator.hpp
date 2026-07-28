#ifndef LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP
#define LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP

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
 * input character is a single table read instead of a hash lookup. The tables assume states are numbered densely from
 * zero, as the subset construction numbers them; a sparsely numbered DFA still works but wastes a table row per
 * unused identifier.
 */
class Simulator
{
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

        const auto transitions{transitions_view()};

        auto state{init_state_};

        // The tables only hold valid states: init_state_ indexes a row, and every entry is either a row index or
        // no_state_. The loop therefore needs no bounds checks.
        Result_t result{accept_table_[state], 0};

        for (Iterator current{begin}; current != end; ++current)
        {
            const auto entry{transitions[state, static_cast<unsigned char>(*current)]};

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
     * @brief Type of a transition table entry.
     *
     * Narrower than Dfa::State_t, as the table is read once per input character and halving it keeps twice as much
     * of it in cache. This bounds the number of states a DFA can have, which the constructor checks.
     */
    using Entry_t = std::uint32_t;

    /**
     * @brief Number of distinct symbol values a transition can be labelled with, i.e. the width of a table row.
     */
    static constexpr std::size_t symbol_count_{1U << (sizeof(Label::Symbol_t) * 8U)};

    /**
     * @brief Table entry marking the absence of a transition.
     */
    static constexpr Entry_t no_state_{std::numeric_limits<Entry_t>::max()};

    /**
     * @brief Two-dimensional `(state, symbol)` view over a transition table.
     * @tparam Entry The viewed entry type, const-qualified for reading.
     */
    template <typename Entry>
    using Table_view_t = std::mdspan<Entry, std::extents<std::size_t, std::dynamic_extent, symbol_count_>>;

    /**
     * @brief The state a simulation starts in.
     */
    Dfa::State_t init_state_;

    /**
     * @brief Transitions as one row of symbol_count_ entries per state, holding no_state_ where there is none.
     */
    std::vector<Entry_t> table_;

    /**
     * @brief Accept tokens as one entry per state, holding nullopt where the state does not accept.
     */
    std::vector<std::optional<Token>> accept_table_;

    /**
     * @brief Returns the transition table as a `(state, symbol)` view.
     */
    [[nodiscard]] Table_view_t<const Entry_t> transitions_view() const noexcept
    {
        return Table_view_t<const Entry_t>{table_.data(), accept_table_.size()};
    }
};

} // namespace lexer::dfa

#endif // LEXER_LIBS_DFA_INCLUDE_LEXER_DFA_SIMULATOR_HPP

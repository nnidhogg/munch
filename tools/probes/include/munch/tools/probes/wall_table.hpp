#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_TABLE_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_TABLE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/window_model.hpp"

/**
 * @brief A compiled automaton as dense rows over its live part and the zero-lag maximal-munch scan over it, dead, Flag,
 *        Row_t, Table, table_of, extract, live_states, is_init_reentrant, scan_step, scan_word and serial_boundaries.
 */
namespace munch::tools::probes
{
/**
 * @brief The target of a missing transition in a Table row.
 */
inline constexpr int dead{-1};

/**
 * @brief A per-state or per-pair flag, held in one byte per element.
 */
enum class Flag : std::uint8_t
{
    /**
     * @brief The flag is clear.
     */
    off,

    /**
     * @brief The flag is set.
     */
    on
};

/**
 * @brief One state's row: the target of every byte value, a state index or dead.
 */
using Row_t = std::array<int, byte_count>;

/**
 * @brief An automaton as dense rows: one row of byte_count targets per state, each a state index or dead.
 */
struct Table
{
    /**
     * @brief The number of states, the rows' count.
     */
    std::size_t states{};

    /**
     * @brief The initial state.
     */
    std::size_t init{};

    /**
     * @brief Per state, the target of every byte, dead where the byte has no transition.
     */
    std::vector<Row_t> next{};

    /**
     * @brief Per state, Flag::on when the state accepts.
     */
    std::vector<Flag> accept{};
};

/**
 * @brief Builds a table by hand with initial state 0, every row filled from a function of the state and the byte.
 * @tparam Target The type of the function giving each target.
 * @param states The number of states.
 * @param accept Per state, Flag::on when it accepts.
 * @param target The target of every state and byte value, a state index or dead.
 * @return The table.
 */
template <typename Target>
[[nodiscard]] Table table_of(const std::size_t states, std::vector<Flag> accept, const Target& target)
{
    Table table{.states = states, .init = 0, .next = std::vector<Row_t>(states), .accept = std::move(accept)};

    for (std::size_t state{0}; state < states; ++state)
    {
        for (int byte{0}; byte < byte_count; ++byte)
        {
            table.next[state][static_cast<std::size_t>(byte)] = target(state, byte);
        }
    }

    return table;
}

/**
 * @brief Reads a compiled automaton into a Table: the states numbered in the order a breadth-first walk from the
 *        initial state, bytes ascending, first meets them, the initial state 0, and every transition into a state from
 *        which no accepting state is reachable redirected to dead.
 * @param dfa The compiled automaton.
 * @return The table.
 */
[[nodiscard]] Table extract(const dfa::Dfa& dfa);

/**
 * @brief Returns the states that accept or have a transition on some byte.
 * @param table The table.
 * @return The live states, ascending.
 */
[[nodiscard]] std::vector<std::size_t> live_states(const Table& table);

/**
 * @brief Returns whether some transition enters the initial state, so a token start there cannot be read off the state
 *        alone.
 * @param table The table.
 * @return True when some state has a transition to the initial state.
 */
[[nodiscard]] bool is_init_reentrant(const Table& table);

/**
 * @brief Takes one maximal-munch step with the zero-lag restart: the byte's transition when there is one, otherwise the
 *        initial state's transition on the byte when the state accepts.
 * @param table The table.
 * @param state The current state.
 * @param byte The byte.
 * @return The next state, std::nullopt when the scan fails at the byte.
 */
[[nodiscard]] std::optional<std::size_t> scan_step(const Table& table, std::size_t state, unsigned char byte);

/**
 * @brief Runs scan_step over every byte of a word.
 * @param table The table.
 * @param state The state the word is read from.
 * @param word The word.
 * @return The state after the word, std::nullopt when the scan fails inside it.
 */
[[nodiscard]] std::optional<std::size_t> scan_word(const Table& table, std::size_t state, std::string_view word);

/**
 * @brief Returns the token starts of the serial maximal-munch scan of an input under the zero-lag restart, which is the
 *        whole scan when the premise holds: a byte without a transition ends the token at an accepting state and starts
 *        the next one with the initial state's transition on that byte.
 * @param table The table.
 * @param input The input.
 * @return The token starts followed by the input's size, std::nullopt when the input does not tokenize completely.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> serial_boundaries(const Table& table, std::string_view input);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_TABLE_HPP

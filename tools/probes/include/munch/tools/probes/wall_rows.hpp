#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_ROWS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_ROWS_HPP

#include <cstddef>

#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/probes/wall_table.hpp"

/**
 * @brief The compiled rows the wall probe pins, Local, delimited, add_gadget_tokens, gadget, two_string, csv_row,
 *        json_strict, c_like_row and rollback_family.
 */
namespace munch::tools::probes
{
/**
 * @brief The token kinds of the probe's own rows and fixtures.
 */
enum class Local : std::size_t
{
    /**
     * @brief A double-quoted string.
     */
    str,

    /**
     * @brief A backtick string.
     */
    tick,

    /**
     * @brief A run of bytes outside every string.
     */
    chunk,

    /**
     * @brief A CSV quoted field.
     */
    quoted,

    /**
     * @brief A CSV bare field.
     */
    bare,

    /**
     * @brief A CSV comma.
     */
    comma,

    /**
     * @brief A CSV line end.
     */
    newline,

    /**
     * @brief The token `a` of the rollback family.
     */
    a,

    /**
     * @brief The token `ab*c` of the rollback family.
     */
    abc,

    /**
     * @brief The token `b` of the rollback family.
     */
    b,

    /**
     * @brief The token `x` of the rollback family.
     */
    x
};

/**
 * @brief Returns the string token between two copies of a delimiter, every other byte inside.
 * @param delimiter The delimiter.
 * @return The regex.
 */
[[nodiscard]] regex::Regex delimited(char delimiter);

/**
 * @brief Adds the parity gadget's two tokens: double-quoted strings at priority 1 and a run of every other byte at
 *        priority 2.
 * @param builder The builder the tokens are added to.
 */
void add_gadget_tokens(core::Builder& builder);

/**
 * @brief Builds the parity gadget, an absolute wall: double-quoted strings over an outside that accepts every other
 *        byte, so no elimination evidence exists and the quote parity survives forever.
 * @return The compiled table.
 */
[[nodiscard]] Table gadget();

/**
 * @brief Builds the two-string gadget: double-quoted and backtick strings over an outside that accepts every other
 *        byte, each delimiter mixed into the other's content; its carry group is the non-abelian S3.
 * @return The compiled table.
 */
[[nodiscard]] Table two_string();

/**
 * @brief Builds the RFC 4180 CSV row, whose doubled-quote continuation re-enters the interior after an accept, so it
 *        fails the end-of-input premise and every decider verdict for it is approximation-scoped.
 * @return The compiled table.
 */
[[nodiscard]] Table csv_row();

/**
 * @brief Builds the RFC 8259 JSON row, a premise refusal: numbers accept at `12`, continue non-accepting through `12.`
 *        and die on a non-digit, a stale rollback.
 * @return The compiled table.
 */
[[nodiscard]] Table json_strict();

/**
 * @brief Builds the C-like row of the figures, figures::c_like with split_friendly false.
 * @return The compiled table.
 */
[[nodiscard]] Table c_like_row();

/**
 * @brief Builds the rollback family {a, ab*c, b, x}, the classic premise refusal.
 * @return The compiled table.
 */
[[nodiscard]] Table rollback_family();

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_ROWS_HPP

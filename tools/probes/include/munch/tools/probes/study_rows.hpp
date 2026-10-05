#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_STUDY_ROWS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_STUDY_ROWS_HPP

#include "munch/core/builder.hpp"

/**
 * @brief The C-like study rows several probes measure, each added to a builder token by token with its priorities:
 *        consumption_complete_c_row, published_cumulative_row, conventional_row and split_friendly_conventional_row.
 */
namespace munch::tools::probes
{
/**
 * @brief Adds the consumption-complete C row: the published cumulative row's identifiers, numbers and operators, its
 *        punctuation widened by `#`, backslash, `@`, backtick, `$` and the apostrophe, carriage return in its
 *        whitespace run, escape-carrying strings, char literals, and line and block comments.
 * @param builder The builder the row's tokens are added to.
 */
void consumption_complete_c_row(core::Builder& builder);

/**
 * @brief Adds the published cumulative C-like row: the conventional C-like base with string literals, line comments and
 *        block comments.
 * @param builder The builder the row's tokens are added to.
 */
void published_cumulative_row(core::Builder& builder);

/**
 * @brief Adds the conventional C-like row: the conventional C-like base, newline inside the whitespace run, with string
 *        literals and line comments.
 * @param builder The builder the row's tokens are added to.
 */
void conventional_row(core::Builder& builder);

/**
 * @brief Adds the split-friendly conventional C-like row: the split-friendly C-like base, newline its own token, with
 *        string literals and line comments.
 * @param builder The builder the row's tokens are added to.
 */
void split_friendly_conventional_row(core::Builder& builder);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_STUDY_ROWS_HPP

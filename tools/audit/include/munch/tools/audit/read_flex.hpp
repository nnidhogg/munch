#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
/**
 * @brief Reads a flex file's definitions and rules sections.
 *
 * The file's shape is flex's: a definitions section, `%%`, a rules section, and optionally `%%` and user code, which
 * is skipped. In the definitions section a line beginning at the margin with a name declares a definition, `%s` and
 * `%x` declare start conditions, `%option` lines are recorded, and `%{ ... %}`, `%top{ ... }` blocks and indented
 * lines are code and skipped. In the rules section a line beginning at the margin is a rule: an optional `<...>`
 * prefix, a pattern ending at the first whitespace outside quotes and brackets, and an action running to the end of
 * the line or, when it opens a brace, to the matching close. Patterns are kept as written; regex::parse() reads them
 * against the definitions when a token set is built, so a pattern the parser refuses is refused then, with the rule's
 * line; a flex pattern is already in the syntax the parser reads, so each rule's expression is its pattern. `<<EOF>>`
 * rules are skipped, since end of input is not a token. Whether an action returns a token is read by returned(). A
 * flex file is one scanner, so the list returned holds one specification, as every reader returns the scanners of
 * its file.
 * @param source The file's text.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @return The one scanner the file declares.
 * @throws Spec_error If the file has no rules section, a definition has no pattern, a rule has no pattern, or a
 *         brace, quote or bracket is left open.
 */
[[nodiscard]] std::vector<Lexer_spec> read_flex(std::string_view source, const Returning_t& returning = {});

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP

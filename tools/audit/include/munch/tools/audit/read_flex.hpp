#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief The flex reader, read_flex(), which the command reads a flex file with.
 */
namespace munch::tools::audit
{
/**
 * @brief Reads a flex file's definitions and rules sections into the one scanner the file is.
 *
 * The file's shape is flex's: a definitions section, `%%`, a rules section, and optionally `%%` and user code, which is
 * skipped. flex_lines.hpp reads the file into lines, flex_sections.hpp reads the two sections as flex 2.6.4 reads them,
 * the definitions, start conditions, `%option` lines and copied code of the first and the rules, start-condition scopes
 * and actions of the second, and flex_options.hpp resolves the `%option` words in the order the file sets them, the
 * last word naming a setting deciding it.
 *
 * Patterns are kept as written, each rule's expression its pattern and each definition's its text with the backslash
 * before a `u` dropped, since a flex pattern is otherwise already in the syntax the parser reads and flex reads `\u` as
 * the letter, where the parser reads a code point; flex_pattern.hpp finds where a pattern ends on its line, and
 * regex::parse() reads the patterns against the definitions when a token set is built, so a pattern the parser refuses
 * is refused then, with the rule's line. `<<EOF>>` rules are skipped, end of input being no token, once a `|` rule
 * above has taken their action. Whether an action returns a token is read by returned(). After the file's rules stands
 * flex's default rule, add_default_rule(), which makes a byte no rule of the file's matches a one-byte discarded token;
 * `%option nodefault` drops it, since under it such a byte stops the scanner with a fatal error, which is what a token
 * set answers of itself where no rule matches.
 *
 * What the reading cannot follow is refused by name, with its line, rather than recorded and ignored: an option that
 * changes what a rule matches beyond what the pattern parser reads, or declares out of sight a call that moves a match,
 * as refuse_unmodelled() says; an action that moves the bounds of its match, reruns it or ends the scan, as
 * stateful_use() says, or holds what else the reading does not follow, as refuse_action() says; and a `YY_USER_ACTION`
 * that does either, as user_action_use() says.
 * @param source The file's text.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @param includes How a file the copied code includes by a quoted name is reached, whose definitions are read as the
 *        file's own, `YY_USER_ACTION` and `YY_BREAK` among them; none unless given, under which a quoted include is
 *        refused by name, its definitions being out of sight.
 * @param case_insensitive flex's `-i`: the case option on before the file's own `%option` words, which override it,
 *        `caseful` turning it off again as flex 2.6.4 has it.
 * @return The one scanner the file declares.
 * @throws Spec_error If the file has no rules section, a definition has no pattern, a rule has no pattern, a comment
 *         stands at the margin of the rules section, a start-condition scope is never closed, a quote or bracket is
 *         left open in a pattern, an action's brace, comment or `%{` block is left open at the end of the file, which
 *         flex refuses as an end of file inside an action, a rule's action leaves a quote open at the end of a line
 *         where its braces balance, which flex 2.6.4 ends the action at inside the literal without closing the code it
 *         emits for it, so that the m4 it runs stops with an end of file in string, an action calls one of the calls
 *         named above, or a standing option changes what a rule matches beyond the reading or declares such a call out
 *         of sight.
 */
[[nodiscard]] std::vector<Lexer_spec> read_flex(
        std::string_view source, const Returning_t& returning = {}, const Include_reader_t& includes = {},
        bool case_insensitive = false);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP

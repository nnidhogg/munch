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
 * The file's shape is flex's: a definitions section, `%%`, a rules section, and optionally `%%` and user code, which is
 * skipped; either delimiter is `%%` at the margin, whatever follows it on its line dropped as flex drops it, a comment
 * usually, and an indented `%%` is no delimiter but code in the first section and a rule in the second. In the
 * definitions section a line beginning at the margin with a name declares a definition, `%s` and `%x` declare start
 * conditions, `%option` lines are recorded word by word, a quoted value one word with the name it follows whatever
 * blanks it holds, as flex lexes it, and the settings among them that decide what a rule matches are resolved in the
 * order the file sets them, the case option's `caseless`, `case-insensitive`, `nocaseful` and `nocase-sensitive`
 * turning it on and `caseful`, `case-sensitive`, `nocaseless` and `nocase-insensitive` off, each further `no` flipping
 * the sense again as flex lexes one, and `%{ ... %}`, `%top{ ... }` blocks and indented lines are code and skipped. The
 * rules section opens with a prologue of indented code, which flex copies into the scanner ahead of the rules and the
 * reading skips, ended by the first line at the margin; from there a line, indented or not, is a rule, as flex reads
 * it, or a start-condition scope, `<s>{` through the line opening with `}`, whose rules take its conditions, whatever
 * code stands after either brace on its line being copied out and dropped, read to the end an action is read to, so
 * that a comment or a brace block there runs on to a later line; an indented comment is such code too, while a comment
 * at the margin is refused, since flex reads its slash as the start of a rule and refuses the rule as unrecognized. A
 * rule is an optional `<...>` prefix, a pattern ending at the first blank outside a quote and a bracket expression,
 * whose own first `]` and whose `[:class:]` are no close, so that the blank of `[[:alpha:] ]+` is a member, and an
 * action running to the first end of a line at which its braces balance, a stray close counting below zero, as flex
 * 2.6.4's action scanner reads one: a brace inside a block comment or a string or character literal does not count, the
 * literal ending at its closing quote or at its line's end, whichever comes first, a backslash before the newline
 * carrying it on to the next line as C splices lines, and a `//` comment hides nothing, since that scanner has no state
 * for one, so that a brace after it on the line counts and a quote there opens a literal; an action opening with `%{`
 * runs to the end of the first line holding `%}`, with no comment or literal read inside it; and a `|` action is its
 * whole line, which flex takes unread. Patterns are kept as written; regex::parse() reads them against the definitions
 * when a token set is built, so a pattern the parser refuses is refused then, with the rule's line; a flex pattern is
 * already in the syntax the parser reads, so each rule's expression is its pattern. `<<EOF>>` rules are skipped, since
 * end of input is not a token. Whether an action returns a token is read by returned(). After the file's rules stands
 * flex's default rule, the one it adds once the section is read, which matches one byte where no rule of the file's
 * does and echoes it: read as the rule `.|\n` in every start condition, at the lowest priority and returning nothing,
 * so that such a byte is a one-byte discarded token, with the line the rules section ends on; `%option nodefault` drops
 * it, since under it such a byte stops the scanner with a fatal error, which is what a token set answers of itself
 * where no rule matches, and `%option default` restores it, the last word naming it deciding as with every setting. A
 * flex file is one scanner, so the list returned holds one specification, as every reader returns the scanners of its
 * file.
 *
 * An option that changes what a rule matches beyond what the pattern parser reads is refused by name rather than
 * recorded and ignored: under `%option lex-compat` and `%option posix-compat`, which are flags of their own and
 * either of which suffices, a counted repetition binds the whole expression before it, so that `ab{3}` matches
 * "ababab"; and `%option 7bit`, as well as a `full` or `fast` table with the equivalence classes off, leaves flex
 * building its tables over the 128 bytes of ASCII and refusing outright a pattern that names a byte above 127. So is
 * an action that moves the bounds of its match or reruns it, which the token language has no place for, named with its
 * line: `yymore()`, which appends the next match to this one; `REJECT`, which drops the match for the next rule's;
 * `yyless()`, which gives the end of the match back to be matched again; `unput()`, which pushes a byte onto the
 * input; and `input()` or `yyinput()`, which consume bytes no rule matched. The action is read as C reads it, a
 * comment or a literal holding none of them, a call being a whole word followed by a parenthesis and not reached
 * through `.` or `->`, and `REJECT` a whole word in capitals, as flex takes it; a `|` line is taken unread, as flex
 * takes it. `%option reject` and `%option yymore`, which declare such a use where flex cannot see it, through a macro
 * or code of the file's own, are refused the same way. `BEGIN`, `yy_push_state`, `yy_pop_state` and `yy_top_state`
 * change the start condition and nothing else, which the token set per condition already stands for, so an action
 * calling them is read as any other.
 * @param source The file's text.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @return The one scanner the file declares.
 * @throws Spec_error If the file has no rules section, a definition has no pattern, a rule has no pattern, a comment
 *         stands at the margin of the rules section, a start-condition scope is never closed, a quote or bracket is
 *         left open in a pattern, an action's brace, comment or `%{` block is left open at the end of the
 *         file, which flex refuses as an end of file inside an action, a rule's action leaves a quote open at the end
 *         of a line where its braces balance, which flex 2.6.4 ends the action at inside the literal without closing
 *         the code it emits for it, so that the m4 it runs stops with an end of file in string, an action calls one
 *         of the calls named above, or a standing option changes what a rule matches beyond the reading or declares
 *         such a call out of sight.
 */
[[nodiscard]] std::vector<Lexer_spec> read_flex(std::string_view source, const Returning_t& returning = {});

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_FLEX_HPP

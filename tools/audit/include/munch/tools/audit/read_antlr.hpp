#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
/**
 * @brief Reads an ANTLR 4 grammar, a `lexer grammar` or a combined `grammar`, into the token set its lexer scans with.
 *
 * ANTLR's lexer is maximal munch with the first rule winning a tie, so a rule's index is its priority. Modes are the
 * start conditions, each exclusive, the rules before the first `mode` line the default mode's; `fragment` rules are
 * definitions and a reference to any lexer rule becomes `{NAME}`, a rule that reaches itself being refused when the
 * token set is built, since that is no regular language. A rule's commands, `-> skip`, `channel(...)`, `type(...)`,
 * `mode(...)`, `pushMode(...)`, `popMode`, are its action; `skip` and any channel make the token one a parser never
 * sees, `type(X)` makes it return X, and `more`, which joins the match onto the next token's, is refused. Commands
 * are per outermost alternative, and a rule whose alternatives carry different ones is read as one rule per
 * alternative, in order, which is how ANTLR ranks them. In a combined grammar the literals the parser rules use are
 * implicit tokens, placed before every explicit rule as ANTLR places them, unless a lexer rule spells exactly that
 * literal. The `caseInsensitive` option, at the grammar or on a rule, doubles every ASCII letter's case.
 *
 * The body is rewritten into the syntax regex::parse() reads, kept beside the body as written: `'abc'` and
 * `'a'..'z'` become a quoted literal and a bracket, `[...]` a bracket, `.` and `~[...]` the UTF-8 encodings of the
 * scalars they admit, since ANTLR reads characters and the audit reads bytes; a non-greedy loop before a literal, the
 * block comment's dot-star-question before its closing star-slash, becomes the loop over what avoids that literal,
 * which is what the loop matches; a non-greedy loop before anything else, a set member above U+007F, `EOF` inside a
 * rule, a semantic predicate `{...}?`, and an `import` are refused, each naming its line. Actions `{...}` inside
 * rules are skipped, since they do not change what a rule matches.
 * @param source The grammar's text.
 * @return The one scanner the grammar declares, its line that of the `grammar` declaration; a list, as every reader
 *         returns the scanners of its file.
 * @throws Spec_error If the grammar is not one with lexer rules, a rule is malformed, or a rule uses a construct the
 *         rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_antlr(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP

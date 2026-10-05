#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief The ANTLR reader, read_antlr(), which the command reads a grammar with.
 */
namespace munch::tools::audit
{
/**
 * @brief Reads an ANTLR 4 grammar, a `lexer grammar` or a combined `grammar`, into the token set its lexer scans with.
 *
 * ANTLR's lexer is maximal munch with the first rule winning a tie, so a rule's index is its priority. Modes are the
 * start conditions, each exclusive, the rules before the first `mode` line the default mode's, and a `mode` line in a
 * combined grammar is refused, since only a lexer grammar may declare one; `fragment` rules are definitions and a
 * reference to any lexer rule becomes `{NAME}`, a rule that reaches itself being refused when the token set is built,
 * since that is no regular language. A byte order mark is a blank wherever it stands outside a literal, a set or an
 * action, as ANTLR's lexer drops one, so a grammar may open with one. A closure, `*` or `+` in either form, whose body
 * can match the empty string is refused as ANTLR's error 153 rejects it, the body's answer running through every rule
 * it reaches.
 *
 * A rule's commands, `-> skip`, `channel(...)`, `type(...)`, `mode(...)`, `pushMode(...)`, `popMode`, are its action,
 * read by commands_of() and applied to the token the rule matches by apply_commands(), as antlr_commands.hpp says,
 * refusing in ANTLR's words what ANTLR refuses there and by name what the token stream cannot hold. The `tokens` and
 * `channels` blocks, the rules and their commands' place are read by Grammar_reader as ANTLR's parser reads them, and
 * the whole grammar is held to ANTLR's checks once the last rule is in by finish_grammar().
 *
 * In a combined grammar the literals the parser rules use are implicit tokens, placed before every explicit rule as
 * ANTLR places them, unless a lexer rule spells exactly that literal in a shape ANTLR's own patterns match, whose token
 * the parser's literal then is. Element options, `<name=value, ...>`, are metadata on the element and no token, read by
 * Antlr_cursor::element_options(); the `caseInsensitive` option, at the grammar or on a rule, folds a literal and a set
 * as admit() says, and a character beyond ASCII named under it is refused, since ANTLR folds it with the Unicode case
 * mappings the library has not got.
 *
 * The body is rewritten by Expression_reader into the syntax regex::parse() reads, kept beside the body as written:
 * `'abc'` and `'a'..'z'` become a quoted literal and a bracket, `[...]` a bracket, `.` and `~[...]` the UTF-8 encodings
 * of the scalars they admit, since ANTLR reads characters and the audit reads bytes. A literal is read as
 * Antlr_cursor::literal() reads it, a surrogate pair the one character it encodes and a lone surrogate refused; a
 * range, a set or an escape ANTLR rejects is refused in the words of its errors 144, 152, 156 and 174; and a non-greedy
 * loop or option becomes the greedy rewrite that stops where ANTLR's fewest characters stop it, where lazy_element()
 * finds one, and is refused where it does not. A set member above U+007F is read as a bracket over its code points'
 * UTF-8, refused only under `caseInsensitive`. `EOF` inside a rule, a semantic predicate `{...}?` and an `import` are
 * refused, each naming its line, and so is an action `{...}` inside a rule unless its body is blanks and comments,
 * since ANTLR runs it at that point of the match and its code may produce another token than the rule's own,
 * `{more();}` joining the match onto the next token's and `{setType(X);}` renaming it; an empty body runs nothing and
 * is skipped.
 * @param source The grammar's text.
 * @return The one scanner the grammar declares, its line that of the `grammar` declaration; a list, as every reader
 *         returns the scanners of its file.
 * @throws Spec_error If the grammar is not one with lexer rules, a rule is malformed, or a rule uses a construct the
 *         rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_antlr(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP

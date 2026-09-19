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
 * start conditions, each exclusive, the rules before the first `mode` line the default mode's, and a `mode` line in a
 * combined grammar is refused, since only a lexer grammar may declare one; `fragment` rules are definitions and a
 * reference to any lexer rule becomes `{NAME}`, a rule that reaches itself being refused when the token set is built,
 * since that is no regular language. A byte order mark is a blank wherever it stands outside a literal, a set or an
 * action, as ANTLR's lexer drops one, so a grammar may open with one. A closure, `*` or `+` in either form, whose body
 * can match the empty string is refused as ANTLR's error 153 rejects it, the body's answer running through every rule
 * it reaches. A rule's commands, `-> skip`, `channel(...)`, `type(...)`, `mode(...)`, `pushMode(...)`, `popMode`, are
 * its action, read as the names and arguments ANTLR's own lexer reads there, so that the grammar's blanks and comments
 * between them are no part of any command; `skip` and `type(X)` both set the token's type and a channel other than the
 * default one sets a field of its own, the rightmost command for a field winning, so a rule whose commands are a hidden
 * channel and a type returns a renamed token a parser never sees, the channel's argument resolved as ANTLR resolves it,
 * `HIDDEN` and `DEFAULT_TOKEN_CHANNEL` its constants, a name a lexer grammar's `channels` block declares a channel of
 * its own, and anything else a decimal number, `00` being zero and the default channel, while another reserved name,
 * `SKIP`, is its error 172, a number beyond its int or a name nothing declares its error 177 and a `channels` block in
 * a combined grammar its error 164, each in its words; a type's argument is a token's name or its number, a number read
 * as Integer.parseInt reads it, so that `type(0)` in any spelling of zero sets ANTLR's value for no type and the token
 * keeps the rule's own, its name where ANTLR gives the rule a type, a lexer grammar's `tokens` block naming it or the
 * rule spelling a parser literal's shape, and its type zero otherwise, a token no rule names, kept as `0`, while
 * `type(EOF)` is minus one, the token that ends the stream, so a rule whose type is EOF once its commands are read ends
 * the stream where it matches, on any channel, and is refused by name at that command's line; `more`, which joins the
 * match onto the next token's, is refused, as is a command ANTLR has not got or one of its seven given an argument it
 * takes none of or none where it takes one, in the words of its errors 149, 150 and 151 at the command's own line, and
 * what ANTLR's parser rejects as a syntax error is refused at that byte's line in words naming the cause: a command
 * with parens holding nothing, `skip()`, rejected at the `)` in ANTLR's own words, a command with no comma before it,
 * the `type(B)` of `skip type(B)`, rejected at the command's name, a comma no command name follows, before the first,
 * `, skip`, after another, `skip,, type(B)`, or ending the clause, `skip,`, an arrow no command follows, `-> ;`, parens
 * holding more than one token or anything but a name or a number, `type(Y Z)`, `type(Y.Z)` and `type(-1)`, parens never
 * closed, `type(Y` and then the `;`, and any other byte where a name, an argument or a comma should stand, the second
 * `)` of `type(Y))`; and one of the seven with its first letter capitalised, `Skip`, which names a code template of
 * ANTLR's target, `LexerSkipCommand`, that the generated lexer runs as an action and ANTLR's own interpreter leaves
 * out, so that what the token stream holds is the target's to say. A `tokens` block or a `channels` block holds names
 * parted by commas and nothing else, the tokens block alone allowed to hold none, so `{ ONE TWO }` and `{ ONE, }` are
 * its syntax errors at the byte after the name or the comma, refused in words naming the cause. A command must be the
 * last element of a rule's single outermost alternative, so a rule of several alternatives carries none and is one
 * token whatever its alternatives are, and a command on the alternatives of such a rule is refused as the grammar ANTLR
 * rejects. In a combined grammar the literals the parser rules use are implicit tokens, placed before every explicit
 * rule as ANTLR places them, a parser rule's argument block `[...]`, its arguments, `returns`, `locals`, a reference's
 * arguments or a `catch` clause's, skipped as ANTLR's lexer reads one, brackets nested and a quoted string whole, so
 * that a literal inside one is no token, unless a lexer rule spells exactly that literal in a shape ANTLR's own
 * patterns match, a rule that is no fragment and carries no options, of one alternative whose literal carries no
 * element options and is the literal alone, the literal and one action, or the literal and one or two commands of which
 * at most one takes an argument, whatever comments stand in the rule; the parser's literal is then that rule's token,
 * skipped or renamed as the rule says, and a literal two such rules spell is refused as ANTLR's error 126 rejects the
 * parser's use of it. Element options, `<name=value, ...>` after a predicate, a token reference, a literal, a rule
 * reference or the dot, are read as ANTLR's parser reads them, names alone or with a value that is a name, a number, a
 * string or a brace block, and are metadata on the element and no token of the lexer, so that a string among them is no
 * implicit token; on an element that takes none, a set, a range or a group, the `<` is refused as ANTLR rejects it. An
 * option's name and value are read as ANTLR's parser reads them, a name dotted or not with blanks and comments allowed
 * around each dot, a value also a number, a string or a brace block, the comments beside it no part of it; the
 * `caseInsensitive` option, at the grammar or on a rule, folds as ANTLR folds, a literal's ASCII letters and a set's
 * members each in both cases and a range by its two ends alone, so that `[a-z]` and `'b'..'y'` gain the copy in the
 * other case, both ends being letters of one case, while `[A-t]`, `[0-Z]` and `[a-\u007f]`, whose ends differ in case
 * or are no letters, admit exactly what they spell, the letters inside them folded no further, which ANTLR's warning
 * 185 remarks on where the ends differ in case, and no closure of the set under case; `true` and `false` are the
 * spellings ANTLR takes for the option and any other its warning 84 that sets nothing, and a character beyond ASCII
 * named under it is refused, since ANTLR folds it with the Unicode case mappings the library has not got; the dot,
 * which admits every scalar in both cases already, is no such character.
 *
 * The body is rewritten into the syntax regex::parse() reads, kept beside the body as written: `'abc'` and `'a'..'z'`
 * become a quoted literal and a bracket, `[...]` a bracket, `.` and `~[...]` the UTF-8 encodings of the scalars they
 * admit, since ANTLR reads characters and the audit reads bytes; a high surrogate escape and a low one after it in a
 * literal are the one character the pair encodes, as ANTLR joins them before it builds the rule, and a surrogate on its
 * own is a code point no UTF-8 input decodes to, which ANTLR's lexer never matches, so a set member that is one is left
 * out and a literal holding one, or a set holding nothing else, is refused, the byte reading having no literal that
 * never matches; a range's end or a negated literal ANTLR's error 144 calls multi-character, two characters, none, a
 * pair of escapes or a character beyond the basic multilingual plane written out, is refused in its words, as a range
 * whose end is below its start and an empty set are in the words of its error 174, and an escape ANTLR has not got,
 * `\q` anywhere, `\]` in a literal or `\'` in a set, in the words of its error 156, which its lexer also reports of a
 * braced Unicode escape in a literal whose closing brace stands twelve or more UTF-16 units past the opening quote,
 * counting the digits from the quote; a raw line break inside a literal or a set, which ANTLR's lexer takes in neither,
 * is refused in the words of its error 152 for the literal, unterminated string literal, and of its syntax error at the
 * break for the set; a non-greedy loop in an outermost alternative of a rule nothing references, whose rest of the rule
 * spells one ASCII string, the block comment's dot-star-question before its closing star-slash, becomes the loop that
 * stops where that string first matches, which is ANTLR's fewest characters that still let the rest match, an
 * occurrence overlapping the loop's own last bytes included, and where the body can only be empty the terminator
 * matches alone. Its body is read as one set, the dot, one character, or a literal of one length whose first byte that
 * string cannot begin with; over a body of several lengths, a group among them, ANTLR takes its alternatives in order
 * and stops at the fewest characters of them all, which `('x'|'xa')*? 'a'` and `('xa'|'x')*? 'a'` answer differently
 * and no greedy rewrite keeps apart, so such a loop is refused. ANTLR's lexer follows every path through the rule at
 * once, in the order the alternatives before the loop and the rule's own alternatives give them, and the first path to
 * reach the rule's end stops every later path that has passed the loop's decision, `('a'|'aa') .*? 'a'` and `'ab' | 'a'
 * .*? 'c'` ending where their shorter path does; so the loop is read after elements whose every match has one length in
 * characters, which reach the loop at one character together, and in an alternative no earlier alternative of the rule
 * can begin with the same character as, which is dead before the loop is reached, what an alternative can begin with
 * reaching past every element of it that can match the empty string, and refused after elements of more than one length
 * or of a length unknown, a reference among them, or after an alternative that can begin with the same character, and
 * in a rule an alternative of which can match the empty string, since the empty match reaches the rule's end at the
 * loop's decision and stops it. A non-greedy option is read as the greedy one where its body cannot begin the rest,
 * which the bypass ANTLR tries first would end the rule with at once, and has one length in characters, since ANTLR
 * takes the body's alternatives in order too, `('x'|'xa')?? 'a'` and `('xa'|'x')?? 'a'` answering "xaa" differently;
 * one over any other body is refused. A non-greedy loop whose rest reaches past its own sequence, in a group or in a
 * rule another rule inlines, or a rest of any other shape, a set member above U+007F, `EOF` inside a rule, a semantic
 * predicate `{...}?`, and an `import` are refused, each naming its line. An action `{...}` inside a rule is refused
 * unless its body is blanks and comments, since ANTLR runs it at that point of the match and its code may produce
 * another token than the rule's own, `{more();}` joining the match onto the next token's and `{setType(X);}` renaming
 * it; an empty body runs nothing and is skipped.
 * @param source The grammar's text.
 * @return The one scanner the grammar declares, its line that of the `grammar` declaration; a list, as every reader
 *         returns the scanners of its file.
 * @throws Spec_error If the grammar is not one with lexer rules, a rule is malformed, or a rule uses a construct the
 *         rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_antlr(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_ANTLR_HPP

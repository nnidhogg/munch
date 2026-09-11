#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
/**
 * @brief Reads the logos lexers of a Rust source file, one specification per enum deriving `Logos`.
 *
 * logos, the Rust lexer generator, declares a lexer as an enum: every `#[derive(...)]` list naming `Logos`, bare or
 * as `logos::Logos`, marks the enum that follows, and the rest of the file, its uses, functions, impl blocks,
 * comments and doc comments, is skipped along with strings and character literals, so that nothing inside them is
 * mistaken for an attribute. The specification is named by the line of that derive. On the enum, `#[logos(...)]`
 * attributes carry `skip "regex"` and `skip("regex", callback, priority = n)`, each a discarded rule, in file order
 * and before the variants as logos orders them; `subpattern name = "regex"`, a definition, referenced from a pattern
 * as `(?&name)`; and the other keys, `extras`, `error`, `crate`, `utf8` and the rest, which are recorded as options
 * and say nothing about the token set. On a variant, any number of `#[token("text")]` and `#[regex("regex")]`
 * attributes are that many rules for the one token, each with an optional callback, bare or as `callback = ...`,
 * an optional `priority = n`, `ignore(case)`, and `allow_greedy`; a variant's payload, discriminant, and `#[default]`
 * and the like are passed over. A rule's token is its variant's name, and std::nullopt for a skip attribute and for a
 * callback spelled `logos::skip` or `skip`, or a closure whose whole body is `logos::Skip`; a callback that skips
 * only after doing something else is read as a token, since only the callback's spelling is read. The callback's
 * text is the rule's action. logos has no start conditions, so no rule names one and every rule is active in
 * INITIAL.
 *
 * The pattern is a Rust string literal, and the rule keeps it as written: a plain string has its escapes decoded,
 * a raw string `r"..."`, `r#"..."#` is taken verbatim, and a byte string `b"..."` makes the pattern a byte one. A
 * token's text is matched byte for byte. A regex is the regex crate's, in Unicode mode unless the string is a byte
 * string or `(?-u)` turns the mode off, and it is rewritten into the syntax regex::parse() reads over the bytes the
 * lexer actually scans, the UTF-8 of its `&str` source, which is the rule's expression: a scalar's UTF-8 bytes stand
 * where the scalar stood; a class, the dot and a negated class are the set of scalars they admit, written as one
 * bracket of the ASCII members and the code point ranges of the others, which the parser reads as the encodings of
 * those scalars, surrogates left out; groups of every kind group, the capture being lost as logos loses it;
 * the flags `i`, `s` and `u` are honoured with their scoping, `i` folding case as the regex crate folds it, an ASCII
 * letter to both cases and, in Unicode mode, `k` and `s` to the Kelvin sign and the long s as well; the postfix
 * operators, the counted forms and their lazy variants, which logos takes since 0.16, are the greedy forms, since
 * logos takes the longest match whatever the operator's greed; and `(?&name)` is `{name}`, the definition rewritten
 * the same way, unless a flag is in force at the reference, when the definition is expanded in place under that flag
 * as logos 0.16 expands it, in its own mode. Under `(?-u)` a `&str` pattern is read as the crate reads it, a scalar
 * still its UTF-8 and a class or `\xHH` a byte, though logos's own check refuses what could match outside UTF-8
 * unless the enum declares `utf8 = false`.
 *
 * What the byte reading cannot say exactly is refused rather than approximated, naming the rule's line: the
 * classes `\d`, `\w`, `\s`, their negations and `\p{...}` in Unicode mode, which need the Unicode tables, their
 * `(?-u)` forms and the ASCII `[[:digit:]]` family being read; a non-ASCII scalar under `i`, whose case folding is
 * not modelled; the anchors `^`, `$`, `\A`, `\z`, `\b`, `\B` and lookaround, which condition the context a match
 * stands in, as regex::parse() refuses flex's; the flags `x`, `m`, `U` and `R`; the class operators `&&`, `--` and
 * `~~`; `ignore(ascii_case)`, which logos itself no longer accepts; and a pattern matching only the empty string,
 * which logos refuses to compile.
 *
 * The priority is logos's own, kept on the rule as the number logos ranks by, higher winning among rules matching
 * the same longest lexeme, and mapped by token_set() onto the builder's scale. A token's is twice its byte length.
 * A regex's is computed from its shape as logos 0.14 and later compute it: a literal counts two per scalar, two per
 * byte when the run is not UTF-8; a class counts two, the dot and a folded letter among them; a concatenation adds
 * its parts; an alternation takes the least of its branches; a repetition counts its operand its minimum number of
 * times, so `?` and `*` count nothing and `+` the operand once; and `priority = n` overrides the computation. Two
 * rules of one priority that can match the same lexeme are a logos compile error, so a file that compiles never
 * asks the tie-break, which token_set() settles by file order.
 * @param source The file's text.
 * @return The lexers, in file order, one per enum deriving Logos.
 * @throws Spec_error If an enum, attribute, string or group is left open or malformed, an attribute carries an
 *         argument logos does not know, a subpattern is unknown, or a pattern uses a construct the rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_logos(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP

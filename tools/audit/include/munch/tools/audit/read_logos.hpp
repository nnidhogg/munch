#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief The logos reader, read_logos(), which the command reads a Rust file with.
 */
namespace munch::tools::audit
{
/**
 * @brief Reads the logos lexers of a Rust source file, one specification per enum deriving `Logos`.
 *
 * logos, the Rust lexer generator, declares a lexer as an enum: every `#[derive(...)]` list naming `Logos`, bare or as
 * `logos::Logos`, marks the enum that follows, wherever it stands, inside a module, a function's body or a constant's
 * initializer among them, and the rest of the file, its uses, functions, impl blocks, comments and doc comments, is
 * skipped along with strings and character literals, so that nothing inside them is mistaken for an attribute; a
 * macro's text, a `macro_rules!` body or an invocation's arguments, is not entered, since it is nothing until the macro
 * is expanded, which the reading does not do, so an enum written there is no scanner. An enum or a variant under a
 * `#[cfg(...)]` whose predicate is false by its form alone, `any()` of nothing, `not(all())` or an `all` holding one
 * such argument, is stripped before the derive runs and is no scanner or rule, and any other item under one, a function
 * or a constant, is gone with every name it would bind, a `pub` before it notwithstanding, so that a callback names the
 * one standing; under a predicate the build alone decides, `test`, `unix` or `feature = "x"`, an enum or a variant is
 * read as standing, and the scanner's options say so, `cfg=` and the predicate. A `#[cfg_attr(predicate, a, b)]` is the
 * attributes it carries, `#[a] #[b]`, where the predicate is true by its form, nothing where it is false, and where the
 * build alone decides it the attributes as well, read as applied and the options saying so the same way, as rustc
 * expands the attribute before the derive runs, so a `regex` or a `derive(Logos)` carried is one as written. The
 * specification is named by the line of that derive.
 *
 * On the enum, `#[logos(...)]` attributes carry `skip "regex"` and `skip("regex", callback, priority = n)`, each a
 * discarded rule, in file order and before the variants as logos orders them; `subpattern name = "regex"`, a
 * definition, referenced from a pattern as `(?&name)`; and the other keys logos 0.15.1 knows, `crate`, `error`,
 * `export_dir`, `extras`, `source` and `type`, which are recorded as options and say nothing about the token set,
 * `export_dir` among them though the crate takes it only with its `debug` feature on, any other key, a later crate's
 * `utf8` among them, being refused as the crate refuses it, as is an entry after a parenthesised one, `skip(...)` or
 * `error(...)`, in one attribute, whose comma the crate leaves unread, a bare key, a key with a value of another shape
 * than it takes, `extras(T)` or `skip = "x"`, `extras`, `error`, `source` or the type of one parameter given twice, and
 * `#[logos]` without its parentheses, each in the crate's words. The enum's generics are the crate's too: one lifetime
 * at most, no const generics, and every type parameter given its type by `type T = ...` and no `type` for a parameter
 * the enum has not got.
 *
 * On a variant, any number of `#[token("text")]` and `#[regex("regex")]` attributes are that many rules for the one
 * token, each with an optional callback, bare or as `callback = ...`, an optional `priority = n` and `ignore(case)` or
 * `ignore(ascii_case)`, the attribute refused as the crate refuses it without its parentheses or with nothing in them,
 * with a second `priority` or callback, with `priority(...)` or `callback(...)`, and with any argument after
 * `ignore(...)`, whose comma the crate leaves unread as well; the legacy `#[error]` attribute and a variant with
 * several or named fields are refused as the crate refuses them; a variant's discriminant, and `#[default]` and the
 * like, are passed over, and its payload's type is kept for the callbacks to be read against.
 *
 * A rule's token is its variant's name, and std::nullopt for a skip attribute, whatever its callback returns, and for a
 * callback that skips; what a callback returns, which logos decides by its type against the variant's payload, and how
 * it may use the lexer are read as Callback_reader reads them. The callback's text is the rule's action. logos has no
 * start conditions, so no rule names one and every rule is active in INITIAL.
 *
 * Every name in a type, a path or a result is read as the file binds it in the module the text stands in, the enum's
 * for its callbacks and a named function's own for that function's type and body, and in the namespace the text asks,
 * as canonical() resolves a path over the bindings Names_t keeps, block scopes, the crate's bare names and the
 * prelude's `Result` and `Option` among them; so a local `struct Skip` is a payload and not the crate's `Skip`, a local
 * `fn skip` is read by its type in place of the crate's function, and a constructor of the enum must name one of its
 * variants, `T::make(0)` being a call whose result is out of sight.
 *
 * The pattern is a Rust string literal, and the rule keeps it as written: a plain string has its escapes decoded, a raw
 * string `r"..."`, `r#"..."#` is taken verbatim, and a byte string `b"..."` makes the pattern a byte one. A token's
 * text is matched byte for byte. A regex is the regex crate's, in Unicode mode unless the string is a byte string or
 * `(?-u)` turns the mode off, and it is rewritten into the syntax regex::parse() reads over the bytes the lexer
 * actually scans, the UTF-8 of its `&str` source, which is the rule's expression: a scalar's UTF-8 bytes stand where
 * the scalar stood; a class, the dot and a negated class are the set of scalars they admit, written as one bracket of
 * the ASCII members and the code point ranges of the others, which the parser reads as the encodings of those scalars,
 * surrogates left out; groups of every kind group, the capture being lost as logos loses it; the flags `i`, `s` and `u`
 * are honoured with their scoping, `i` folding case as the regex crate folds it, an ASCII letter to both cases and, in
 * Unicode mode, `k` and `s` to the Kelvin sign and the long s as well; the postfix operators and the counted forms,
 * their lazy variants refused as logos 0.15.1 refuses them; and `(?&name)` is `{name}`, the definition rewritten the
 * same way, where the reference stands in the definition's own mode under no flag, and otherwise the definition
 * expanded in place under the flags and in the mode of the pattern it stands in, since logos substitutes the
 * definition's text, a byte string's bytes beyond ASCII spelled `\xHH`, into the pattern before the crate parses it: a
 * byte string's subpattern referenced from a string pattern is read over scalars, its `\xHH` the scalar U+00HH and its
 * dot any scalar, and a string's referenced from a byte pattern over bytes, a scalar still its UTF-8 and a class
 * holding one refused as the crate refuses it. Under `(?-u)` a `&str` pattern is read as the crate reads it, a scalar
 * still its UTF-8 and a class or `\xHH` a byte, and a byte beyond ASCII there, alone, written in a class whatever the
 * class comes to, or admitted by a class, the dot, a negated class and a nested one among them, is refused as the crate
 * refuses it, since logos parses a `&str` pattern with the crate's UTF-8 check on and 0.15.1 has no option to turn it
 * off.
 *
 * What the byte reading cannot say exactly is refused rather than approximated, naming the rule's line: the property
 * classes `\p{...}`, whose tables the library has not got, where `\d`, `\w`, `\s` and their negations are read in
 * Unicode mode as the crate's classes, Nd, White_Space and the word class, over the tables of the Unicode version the
 * locked regex-syntax was generated from rather than the library's own pinned one, since the audited language is the
 * scanner's; the version is the first of the scanner's options, `unicode-classes=16.0.0`. Under `(?-u)` the escapes are
 * their ASCII forms beside the `[[:digit:]]` family; a non-ASCII scalar under `i`, whose case folding is not modelled;
 * the anchors `^`, `$`, `\A`, `\z`, `\b`, `\B` and lookaround, which condition the context a match stands in, as
 * regex::parse() refuses flex's; the flags `x`, `m`, `U` and `R`; the class operators `&&`, `--` and `~~`; the lazy
 * operators, which logos 0.15.1 refuses as unsupported non-greedy parsing; a `*` or `+` over the dot under `s` or over
 * a class of every scalar or every byte, as check_dot_repetitions() says; an `allow_greedy` argument, which logos
 * 0.15.1 does not know and calls an unknown nested attribute, as it calls an `ignore` flag on a `skip`; a token or a
 * regex matching only the empty string once its subpatterns are pasted in, which is no rule to logos 0.15.1, which
 * panics on such a token and compiles such a regex into a rule matching no input, while an empty subpattern definition
 * is valid and adds nothing to the patterns referencing it; and an unbounded repetition whose body can begin with a
 * byte that may also follow it, as check_repetitions() says.
 *
 * `ignore(case)` and `ignore(ascii_case)` fold the pattern by the routes Ignore_case says; an `(?i)` inside the pattern
 * keeps the crate's own folding for its scope either way, and logos refuses the two flags together.
 *
 * The priority is logos's own, kept on the rule as the number logos ranks by, higher winning among rules matching the
 * same longest lexeme, and mapped by token_set() onto the builder's scale. A token's is twice its byte length, except
 * that an ignore flag makes a token a regex to logos, which escapes the literal for the regex crate, compiles that
 * regex and takes its priority from the tree like any other; the escaping writes a byte string's byte beyond ASCII out
 * as the characters of its `\xNN` escape and escapes that backslash again, so such a token matches the escape's own
 * text and not the byte, which is the language read here. A regex's is computed from its shape as logos 0.14 and later
 * compute it, over the pattern with its subpatterns pasted in as text, which is what the crate parses, merging adjacent
 * literals across a reference, so that a scalar whose UTF-8 is split between a pattern and a subpattern is one scalar
 * and a run the pasting joins into invalid UTF-8 is bytes: a literal counts two per scalar, two per byte when the run
 * is not UTF-8 as strictly as Rust's own validation reads it, an encoded surrogate, an overlong form and a scalar above
 * U+10FFFF being bytes and not characters, since logos asks `std::str::from_utf8` and counts bytes where it fails; a
 * class counts two, the dot and a folded letter among them; a concatenation adds its parts; an alternation takes the
 * least of its branches; a repetition counts its operand its minimum number of times, so `?` and `*` count nothing and
 * `+` the operand once; and `priority = n` overrides the computation. Two rules of one priority that can match the same
 * lexeme are a logos compile error, so a file that compiles never asks the tie-break, which token_set() settles by file
 * order.
 * @param source The file's text.
 * @return The lexers, in file order, one per enum deriving Logos.
 * @throws Spec_error If an enum, attribute, string or group is left open or malformed, an attribute carries an argument
 *         logos does not know, a subpattern is unknown, or a pattern uses a construct the rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_logos(std::string_view source);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_LOGOS_HPP

#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_PATTERN_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_PATTERN_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief Where a flex rule's pattern ends on its line, pattern_length(), and the pattern forms whose bytes flex decides
 *        under the locale it runs under, which the byte-level reading refuses: a negated POSIX class, negated_class()
 *        and negated_class_refusal(), and a byte beyond ASCII where case folds, beyond_ascii(), the case folding either
 *        the case option or a group turning it on, folding_group(); and the pattern in the parser's syntax,
 *        expression_of().
 *
 * A pattern is read here as flex lexes it, not as the pattern parser does: a quote opens text only outside a bracket, a
 * bracket holds no bracket of its own and a `[:class:]` inside one is one token, so each scan tracks which state of a
 * bracket it stands in rather than a depth. What the pattern means is left to the parser, which reads the rule's
 * expression once the reader has checked it.
 */
namespace munch::tools::audit
{
/**
 * @brief Returns where a pattern ends on a rule line: the first blank outside a quote and a bracket, escapes honoured.
 *
 * A bracket expression holds no bracket of its own, so the scan tracks which state of one it stands in rather than a
 * depth: a `]` closes the bracket unless it is its first member, `[]a]` and `[^]a]`, a `^` negates the bracket only
 * where it opens one, so the second caret of `[^^]` is a member, and a `[:class:]` is one token whose `]` is no close,
 * which is what keeps the blank of `[[:alpha:] ]+` inside the pattern.
 * @param line The rule line, its `<...>` prefix already removed.
 * @param number The line number, for the error.
 * @return The pattern's length.
 * @throws Spec_error If a quote or bracket is left open.
 */
[[nodiscard]] std::size_t pattern_length(std::string_view line, std::size_t number);

/**
 * @brief Returns the first negated POSIX class in a pattern, `[:^alpha:]`, or nothing.
 *
 * flex fills a class with its ASCII members whatever locale it runs under, its CCL_EXPR testing isascii() first, and
 * fills the negation without that test, so `[[:^alpha:]]` drops the letters of the locale flex ran under beside the
 * ASCII ones: flex 2.6.4 under fr_FR.ISO8859-1 leaves `\xE9` out of it, where under the C locale it is a member. The
 * file does not decide the locale, so the class is refused by name.
 * @param pattern The pattern or definition text.
 * @return The class as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> negated_class(std::string_view pattern);

/**
 * @brief Returns what the refusal of a negated POSIX class says after naming what holds it: the class, and that flex
 *        fills it under the locale it runs under, as negated_class() says.
 * @param negated The class as negated_class() returns it.
 * @return The refusal's text from "holds" on.
 */
[[nodiscard]] std::string negated_class_refusal(std::string_view negated);

/**
 * @brief Returns the first group in a pattern that turns the case option on, `(?i:`, with `s`, `x` and `r` beside it in
 *        any order and a `-` turning what follows off, `(?s-i:` counting as none; or nothing.
 * @param pattern The pattern or definition text.
 * @return The group's opening as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> folding_group(std::string_view pattern);

/**
 * @brief Returns the first byte beyond ASCII a pattern spells, written out, as `\xHH` or as an octal escape, or
 *        nothing.
 *
 * Under the case option flex folds every byte of a pattern with the C library's case functions under the locale it runs
 * under, so a byte beyond ASCII gains its other case there and not under the C locale: flex 2.6.4 under fr_FR.ISO8859-1
 * with `\xE9 return 7;` before `\xC9 return 8;` returns 7 on `\xC9`, where the C locale returns 8, and a bracket member
 * folds the same way. The file does not decide the locale, so such a pattern is refused by name under the case option.
 * @param pattern The pattern or definition text.
 * @return The byte as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> beyond_ascii(std::string_view pattern);

/**
 * @brief Returns a pattern or definition text in the syntax regex::parse() reads, as flex reads it.
 *
 * The parser's escapes are flex's but for one: it reads `\u{X...}` as a code point, for the readers of character-level
 * generators, where flex reads `\u` as the letter, as it reads every escaped byte it gives no meaning, and the braces
 * after it as a count, so flex 2.6.4 matches `\u{61}` on sixty-one u's as one token. The backslash before a `u` is
 * dropped, the letter alone being the same byte to both, in a bracket and a quoted text as outside them; every other
 * escape is kept as written, an escaped backslash among them, so the `u` after `\\` stays a letter, and `\U` the
 * parser reads as the letter already. The escapes flex reads as bytes the parser refuses, `\x` without a hex digit and
 * an octal escape past 255, which flex 2.6.4 truncates to a byte, stay refused.
 * @param pattern The pattern or definition text.
 * @return The text the parser reads.
 */
[[nodiscard]] std::string expression_of(std::string_view pattern);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_PATTERN_HPP

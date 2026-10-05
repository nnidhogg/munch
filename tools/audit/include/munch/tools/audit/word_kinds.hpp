#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WORD_KINDS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WORD_KINDS_HPP

#include "munch/regex/regex.hpp"

/**
 * @brief Whether every word a pattern matches holds a byte fixed past its first byte, fixed_mid_token(), read off the
 *        kinds of word the pattern matches.
 *
 * A word's kind says where the byte stands fixed in it, in a text or a class of the one byte, which no narrowing
 * removes: nowhere, as its first byte only, or past its first byte; the empty word is a kind of its own. The kinds of a
 * pattern's words are computed over its tree, a sequence's from its parts' and a repetition's from what it repeats, so
 * that the pattern can be asked whether every word it matches is of one kind.
 */
namespace munch::tools::audit
{
/**
 * @brief Returns whether every word of a pattern holds a fixed occurrence of the byte past its first byte, which is
 *        what makes a token that cannot lose the byte an obstruction rather than a token the narrowing decides nothing
 *        about.
 *
 * An edit that keeps any of the token's words, a class narrowed, an alternative dropped, a repetition run fewer times,
 * keeps that word's fixed occurrences, so when every word holds one past its first byte the token consumes the byte
 * mid-token in whatever narrowed form it keeps, and the byte cannot certify while the token stays: `[x]\n[x]` and
 * `\n{2}` are such tokens. Where some word holds the byte fixed only as its first byte, the occurrence the initial
 * state consumes, an edit this analysis does not make can keep that word and certify the byte: the class of `\n[\nx]`,
 * `[\n][\nx]`, `\n{1}[\nx]` or `(\n[xy])[\nx]` narrowed to `[x]`, or the second alternative of `(\n[\nx]|\ny)` dropped
 * and the first's class narrowed.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool fixed_mid_token(const regex::Regex& regex, unsigned char byte);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WORD_KINDS_HPP

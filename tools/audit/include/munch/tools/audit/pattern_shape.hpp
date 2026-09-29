#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PATTERN_SHAPE_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PATTERN_SHAPE_HPP

#include <vector>

#include "munch/regex/regex.hpp"

/**
 * @brief A pattern's shape for a byte it consumes: a run over a class holding the byte, is_run(), a body the byte
 *        terminates, is_terminated(), and a fixed opener before a body holding it, is_delimited(); with the parts of
 *        its top-level sequence, parts_of(), and the normalised form every shape is read off, normalized().
 *
 * Every question is asked of what the pattern matches rather than of how it is written, so that `\n`, `[\n]` and
 * `\n{1}` are one terminator, and a component matching the empty word alone is no part of a sequence.
 */
namespace munch::tools::audit
{
/**
 * @brief Whether a pattern is a run over a class the byte is in: one or more of a set, possibly the sole part of a
 *        sequence.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it is.
 */
[[nodiscard]] bool is_run(const regex::Regex& regex, unsigned char byte);

/**
 * @brief Whether a sequence ends in the byte and holds it nowhere else: a body, then the one byte that terminates
 *        it.
 *
 * The last component has to be the terminator itself, the one byte and no other, because the edit this shape names
 * deletes that component whole: the last component of `[a]"xb"` admits the `x` while the token ends in `b`, and the
 * class of `[a][xb]` admits the `x` while the token may end in `b`, so deleting either would delete the token's own `b`
 * with it; neither is a terminated shape, and each is priced as what it is. The terminator is read by what it matches,
 * so `[a]x{1}` is terminated as `[a]x` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_terminated(const regex::Regex& regex, unsigned char byte);

/**
 * @brief Whether a sequence opens with a fixed word the byte is not in, followed by a body it is in.
 *
 * The opener is read by what it matches, as the terminator is, so `[x][ab]*` and `x{1}[ab]*`
 * are delimited as `"x"[ab]*` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_delimited(const regex::Regex& regex, unsigned char byte);

/**
 * @brief The parts of a pattern's top-level sequence, once it is normalised, none when it is no sequence.
 * @param regex The pattern.
 * @return The parts.
 */
[[nodiscard]] std::vector<regex::Regex> parts_of(const regex::Regex& regex);

/**
 * @brief The pattern the scanner sees, the spellings that match the same words taken off it: a repetition of exactly
 *        one is what it repeats, a component matching the empty word alone is no part of a sequence, a sequence
 *        of one part is that part, an alternative matching nothing is no part of a choice and a choice of one part
 *        is that part, a sequence with a part matching nothing matches nothing, and a repetition of nothing that
 *        need not repeat it is the empty word, each applied until none is left to apply. So `([ab]"x"){1}`,
 *        `[ab]"x"[cd]{0}`, `([ab]"x"){1}[cd]{0}`, the choice of `[ab]"x"` and `any_of("")` and `[ab]"x"` followed
 *        by `(any_of(""))*` are the one sequence `[ab]"x"`, which every shape and every edit then reads, and
 *        `"x"{0} "y"{0}` is the empty word rather than a sequence of nothing.
 * @param regex The pattern.
 * @return The pattern normalised.
 */
[[nodiscard]] regex::Regex normalized(const regex::Regex& regex);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PATTERN_SHAPE_HPP

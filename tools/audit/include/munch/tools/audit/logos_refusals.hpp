#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REFUSALS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REFUSALS_HPP

#include <bitset>
#include <cstddef>
#include <string>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/logos_regex.hpp"

/**
 * @brief What logos 0.15.1 compiles into a scanner matching nothing, or refuses outright, refused by name over a
 *        pattern's tree: an unbounded repetition whose body can begin with a byte that may also follow it,
 *        check_repetitions(), and a `*` or `+` over the dot under `s` or over a class of every scalar or byte,
 *        check_dot_repetitions().
 *
 * Both are read over the whole pattern as logos compiled it, its subpatterns pasted in, since the crate's graph and its
 * check for the dot see nothing of the references.
 */
namespace munch::tools::audit
{
/**
 * @brief A set of bytes, one bit per byte value.
 */
using Byte_set_t = std::bitset<byte_values>;

/**
 * @brief Refuses a repetition logos 0.15.1 cannot resolve at its boundary, whose scanner then matches nothing.
 *
 * The crate's graph decides a repetition's end on one byte, so an unbounded repetition whose operand can begin with a
 * byte that may also follow it leaves it nowhere to go: logos compiles `a+a`, a dot-star between quotes and the flex
 * spelling of the block comment, whose loop and closer both admit a star, into scanners that match no input at all,
 * while the same patterns with the two byte sets apart, `[0-9]+k` and the block comment spelled so that its loop cannot
 * begin with a star, it scans as their language says. A bounded repetition is unrolled and needs no such decision, so
 * `ab?be` and `a{2,3}a` are read as they stand.
 * @param node The node, its references expanded.
 * @param follow The bytes that may follow a match of this node.
 * @param line The line, for refusals.
 * @throws Spec_error If a repetition's operand and its follow share a byte.
 */
void check_repetitions(const Node& node, const Byte_set_t& follow, std::size_t line);

/**
 * @brief Refuses an unbounded repetition of the dot under `s`, which logos 0.15.1 refuses.
 *
 * Before it builds anything, the crate compares the operand of every `*`, `+`, `{0,}` and `{1,}` with the dot that
 * matches every scalar, or every byte outside Unicode mode, and refuses the pattern when they are equal, since the
 * repetition would consume the source to its end with no backtracking to give any of it up: `(?s).*`, `(?s).+`,
 * `[\s\S]*`, `[\x00-\x{10FFFF}]*` and `(?:[^\n]|[\n\r])*`, an alternation of classes the crate merges into one, are all
 * that dot to it; the plain dot, which leaves out the newline, is not, nor is `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]`,
 * which the crate holds as two ranges where its dot is one, and a captured dot, `(?s)(.)*`, escapes the comparison,
 * which the crate makes before it strips the capture (logos-codegen 0.15.1, mir.rs). A bounded repetition and one of at
 * least two are unrolled and pass.
 * @param node The node, its references expanded.
 * @param written The pattern as written, for the refusal.
 * @param line The line, for the refusal.
 * @throws Spec_error If such a repetition stands anywhere in the node.
 */
void check_dot_repetitions(const Node& node, const std::string& written, std::size_t line);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LOGOS_REFUSALS_HPP

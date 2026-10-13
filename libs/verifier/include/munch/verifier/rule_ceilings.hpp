#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RULE_CEILINGS_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RULE_CEILINGS_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "munch/verifier/merge_tower.hpp"

namespace munch::verifier
{
/**
 * @brief A symbol appended to an input, the pair whose right ripple attains a value.
 */
struct Appended_input
{
    /**
     * @brief The input the symbol is appended to.
     */
    std::string input{};

    /**
     * @brief The spelling of the appended symbol, one byte, the table's alphabet being the bytes.
     */
    std::string symbol{};
};

/**
 * @brief The ceilings a merge table's rules put on its origin radius and its right start radius, and its two right
 *        radii computed exactly from the rules, each with an appended input attaining it.
 */
struct Rule_ceilings
{
    /**
     * @brief The ceiling on the right start radius, max(0, S - 1) for S the lengths of every merge's left part summed.
     */
    std::size_t start_ceiling{};

    /**
     * @brief The ceiling on the origin radius under the ordering condition, the lengths of the useful merges' right
     *        parts summed, none when an effective self-pair makes the radius infinite.
     */
    std::optional<std::size_t> radius_ceiling{};

    /**
     * @brief The ceiling on the origin radius without the ordering condition, n_0^2 for n_0 one more than the bytes the
     *        table names plus the lengths of the products summed, none when the radius is infinite.
     */
    std::optional<std::size_t> loose_ceiling{};

    /**
     * @brief The right span radius, the furthest from the end of an input that an appended byte changes a covering
     *        token.
     */
    std::size_t span_radius{};

    /**
     * @brief An appended input whose right span ripple is the span radius, none when the radius is zero.
     */
    std::optional<Appended_input> span_witness{};

    /**
     * @brief The right start radius, the furthest from the end of an input that an appended byte moves the start of a
     *        covering token.
     */
    std::size_t start_radius{};

    /**
     * @brief An appended input whose right start ripple is the start radius, none when the radius is zero.
     */
    std::optional<Appended_input> start_witness{};
};

/**
 * @brief Reads the ceilings on the radii of a byte-pair table off its merges, with no pair graph, and computes its two
 *        right radii exactly from the merges by the chain search.
 *
 * The encoder is merge_tower()'s, the lowest-ranked merge present anywhere applied at its leftmost occurrence until
 * none applies. For an input x and an appended byte b, the right span ripple is the largest |x| - p over the starts p
 * of the tokens of the encoding of x that the encoding of xb does not hold, and the right start ripple the largest
 * |x| - p over the bytes p of x whose covering token begins elsewhere in the encoding of xb; their suprema are the
 * right span radius Q_span and the right start radius Q_start, both finite for every table. With the origin radius R of
 * ripple_radius(), every window of width R + Q_start + 1 pins its byte R at the start that byte has in the window's own
 * encoding, which is the pinned-byte theorem of the report The Left Ripple of Byte-Pair Encoding, so these are the
 * radii a planner cutting at that theorem's windows needs, and a ceiling serves it in place of a radius, since a window
 * of width R' + q' + 1 with R' >= R and q' >= Q_start pins its byte R' by the same theorem.
 *
 * The ceilings are the report's Proposition 'Bounds in the rules'. With S the lengths of every merge's left part
 * summed, Q_start <= max(0, S - 1), which the chain table (a_(m-1), a_m), (a_(m-2), a_(m-1)), ..., (a_0, a_1) over
 * distinct bytes attains. Under the ordering condition, every merge producing a token ranked before every merge
 * consuming it, a finite R is at most the lengths of the useful merges' right parts summed, an input of ripple R
 * beginning with the right parts of distinct useful merges. Without the condition a finite R is at most n_0^2 for n_0
 * one more than the symbols plus the lengths of the products summed, the overlap graph having at most that many pairs
 * and its longest path being simple; the bytes counted are the ones the table names, since a byte no merge names is a
 * token no merge crosses. An admissible table, as Merge states, satisfies the ordering condition, so both radius
 * ceilings hold, and both are none exactly when an effective self-pair makes R infinite, a useful merge with equal
 * parts, as ripple_radius() decides it. A merge is useful when both parts are canonical, a byte or the product of a
 * useful merge, and the merges before it encode the two parts as the two of them.
 *
 * The radii are the report's Theorem 'Chain search', exact under the ordering condition. A state is a string s and a
 * byte b whose encodings of s and sb differ in their first token; Q_span is the longest s over the states, Q_start the
 * largest |s| less the shorter of the two first tokens, and each state's right ripples are those values. The states
 * with one merge in their chain are the useful merges whose right part is a byte, the left part with the right part
 * appended, and a state of last rank r extends by a useful merge r' > r to the state with its left part u prepended
 * exactly when the right part of r' is the first token of one of the two encodings with the merges below r', and u
 * followed by the other first token is encoded as those two tokens by the same merges. Every state is reached from one
 * state of one merge by one sequence of extensions, so the search visits every state once, each test looking up the
 * first token of an encoding at a rank in the history of that token's growth over the encoding, and encoding two
 * tokens' spellings. The search enumerates every state, which is exponential in Q_span in the worst case.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @return The three ceilings, the radius ceilings none when the origin radius is infinite, and the two right radii with
 *         an appended input attaining each.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank,
 *         each spelling quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Rule_ceilings rule_ceilings(std::span<const Merge> merges);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RULE_CEILINGS_HPP

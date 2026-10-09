#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MERGE_TOWER_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MERGE_TOWER_HPP

#include <span>
#include <string>

#include "munch/dfa/verifier.hpp"

namespace munch::dfa
{
/**
 * @brief One merge of a byte-pair table: the two tokens it joins, in order, whose concatenation is its product.
 */
struct Merge
{
    /**
     * @brief The left part, a byte or the product of an earlier merge.
     */
    std::string left{};

    /**
     * @brief The right part, a byte or the product of an earlier merge.
     */
    std::string right{};
};

/**
 * @brief Builds the merge tower of a byte-pair table: the verifier of canonical byte-pair encoding over the table.
 *
 * The encoder applies, at every step, the lowest-ranked merge present anywhere in the sequence at its leftmost
 * occurrence, until none applies. Under distinct products this is the same segmentation as applying the merges in rank
 * order, each exhaustively over the sequence the earlier ones leave, so each merge is one stage over the stage below
 * it: a one-symbol-lookahead transducer that holds a symbol while it equals the merge's left part, joins it with an
 * arriving right part into the product, and otherwise passes what it holds and what arrives upward. A configuration is
 * the symbol each stage holds with the boundary claimed after it, and whether the top stage has emitted nothing, a
 * token with a boundary claimed after it, or a token claimed to be the last, after which any further symbol is refused.
 * A step delivers the byte with its boundary bit to the lowest stage and cascades; it is refused when a claimed
 * boundary lies inside a product or follows a token claimed to be the last. One normalization keeps the configurations
 * few: a held symbol whose merge no possible next arrival completes is passed upward at once, the arrivals being those
 * the nearest holding stage below can send through the empty stages between. A configuration accepts when passing every
 * held symbol upward is not refused and the top stage's last token carries no claimed boundary; the start, no symbol
 * held and nothing emitted, accepts the empty input.
 *
 * Built by exploration from the start over every byte, each configuration interned as it is found, so that the states
 * are the reachable configurations only; the verifier's constructor then trims the result by coaccessibility. The
 * verifier accepts one marking per byte string, the encoder's segmentation of it, so its domain is every byte string.
 * The products must be distinct from the bytes and from one another, since the encoder merges a recreated token again
 * where the rank-order passes cannot: under (c,c), (c,cc), (ccc,a), (cc,c) the string ccca is one token for the encoder
 * and ccc|a for the passes. A merge whose product the alphabet or an earlier merge already spells is refused.
 * @param merges The merge table in rank order, each part a byte or the product of an earlier merge.
 * @return The merge-tower verifier.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank.
 */
[[nodiscard]] Verifier merge_tower(std::span<const Merge> merges);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_MERGE_TOWER_HPP

#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_MERGE_TOWER_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_MERGE_TOWER_HPP

#include <span>
#include <string>
#include <string_view>

#include "munch/verifier/verifier.hpp"

namespace munch::verifier
{
/**
 * @brief One merge of a byte-pair table: the two tokens it joins, in order, whose concatenation is its product.
 *
 * A table is its merges in rank order, and merge_tower(), token_automaton() and ripple_radius() take a table that is
 * admissible: each part of a merge a symbol of the alphabet or the product of an earlier merge, and no product spelled
 * as a symbol or as the product of an earlier merge. Each product is then made by one merge only, ranked before every
 * merge that consumes it, and a table that breaks either condition is refused at its first merge that does. The table
 * that reproduces a SentencePiece model is its self-split table ranked by score, not the table the Hugging Face
 * converter derives from every split of every piece, as docs/usage.md describes, and it too is refused where it breaks
 * these conditions.
 */
struct Merge
{
    /**
     * @brief The left part, a symbol of the alphabet, a byte or a character, or the product of an earlier merge.
     */
    std::string left{};

    /**
     * @brief The right part, a symbol of the alphabet, a byte or a character, or the product of an earlier merge.
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
 * and ccc|a for the passes. A table that is not admissible, as Merge states, is refused.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @return The merge-tower verifier.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank,
 *         each spelling quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Verifier merge_tower(std::span<const Merge> merges);

/**
 * @brief Builds the merge tower of a merge table over an alphabet of characters: the verifier of canonical byte-pair
 *        encoding over the table, reading each character as its UTF-8 bytes.
 *
 * The tokens before any merge are the alphabet's characters in place of the bytes, as in a character-level table such
 * as SentencePiece's, and each part of a merge is one of them or the product of an earlier merge, spelled in UTF-8. The
 * stages are the byte tower's over those tokens, and a configuration also holds the bytes of the character under way: a
 * step adds its byte to them and, once they spell a character of the alphabet, delivers the character to the lowest
 * stage with the byte's boundary bit; while they only begin one it waits, and a boundary claimed there is refused, as
 * is a byte with which they begin none. A configuration with a character under way does not accept. The verifier's
 * domain is the strings over the alphabet, each under the encoder's segmentation of its characters, so the decisions on
 * it are exact over those strings, every position a byte, since no character's encoding is a prefix of another's, and
 * each begins with a byte that is not a continuation byte, so a window of characters occurs only at character
 * boundaries. Over the characters U+0000 to U+007F the tower is the byte tower of the same table restricted to those
 * bytes.
 * @param merges The admissible merge table in rank order, each part a character or the product of an earlier merge.
 * @param alphabet The alphabet, UTF-8 text whose characters are the tokens before any merge, each once.
 * @return The merge-tower verifier.
 * @throws std::invalid_argument If the alphabet holds a character twice, or a merge names a part that neither the
 *         alphabet nor an earlier merge carries, or a product that the alphabet or an earlier merge already carries;
 *         the message names the character, or the merge and its rank, each spelling quoted, a byte outside printable
 *         ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Verifier merge_tower(std::span<const Merge> merges, std::string_view alphabet);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_MERGE_TOWER_HPP

#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "munch/verifier/merge_tower.hpp"

namespace munch::verifier
{
/**
 * @brief A symbol prepended to an input, the pair whose ripple attains a value.
 */
struct Prepended_input
{
    /**
     * @brief The spelling of the prepended symbol, one byte, the table's alphabet being the bytes.
     */
    std::string symbol{};

    /**
     * @brief The input the symbol is prepended to.
     */
    std::string input{};
};

/**
 * @brief A finite origin radius and the mirrored constant, each with a prepended input attaining it.
 */
struct Finite_ripple
{
    /**
     * @brief The origin radius, the furthest a prepended byte moves a covering token.
     */
    std::size_t radius{};

    /**
     * @brief A prepended input whose origin ripple is the radius, none when the radius is zero.
     */
    std::optional<Prepended_input> radius_witness{};

    /**
     * @brief The mirrored constant, one past the start of the furthest token a prepended byte changes.
     */
    std::size_t mirrored{};

    /**
     * @brief A prepended input whose mirrored quantity is the constant, none when the constant is zero.
     */
    std::optional<Prepended_input> mirrored_witness{};
};

/**
 * @brief The witness of an infinite origin radius: a merge (t,t) whose two copies of t the merges before it leave as
 *        two tokens, so that prepending t to t^(2h) moves the covering token of every byte.
 */
struct Effective_self_pair
{
    /**
     * @brief The merge's rank, the lowest of every effective self-pair of the table.
     */
    std::size_t rank{};
};

/**
 * @brief The origin radius of a merge table, finite with the mirrored constant, or infinite with the effective
 *        self-pair that makes it so.
 */
using Ripple_radius = std::variant<Finite_ripple, Effective_self_pair>;

/**
 * @brief Decides the origin radius of a byte-pair table, the lookbehind of canonical byte-pair encoding: how far a byte
 *        prepended to an input can change the tokens of the input.
 *
 * The encoder is merge_tower()'s, the lowest-ranked merge present anywhere applied at its leftmost occurrence until
 * none applies. The origin ripple of a byte a and an input x is one past the last byte of x whose covering token in the
 * encoding of x differs, as an interval, from its covering token in the encoding of ax shifted left by one, zero when
 * none differs; a token that also covers a begins at minus one and so differs wherever it reaches. The origin radius is
 * its supremum over every byte and input, and a longer prepended string reaches no further. The mirrored constant is
 * the mirror of a lookahead constant: the supremum of one past the start of the last token of the encoding of x whose
 * span changes, the least length beyond which every token boundary of x survives every prepended string. It is at most
 * the radius and at least the radius less the longest token's length plus one, so the two are finite together.
 *
 * The radius is decided two ways: for every table by a product of two runs of the automaton that spells the final
 * tokens, the overlap graph, and under the ordering condition, every merge producing a token ranked before every merge
 * consuming it, by the merges themselves, in time polynomial in their number. This function takes an admissible table,
 * as Merge states, which satisfies the ordering condition, since the one merge producing a token precedes each merge
 * consuming it. On such a table the two decisions are one: the radius is infinite exactly when an effective self-pair
 * exists, a merge (t,t) under which the merges before it encode tt as two tokens, and when none does the overlap
 * graph's marked part is acyclic and gives both constants. So this is one function: the verdict comes from the merges
 * and the finite values from the graph, which spells its tokens out as the table already does, leaving nothing for the
 * compressed words the polynomial computation is built on to save.
 *
 * The merges are taken in rank order. A merge is useful when both parts are canonical, a byte or the product of a
 * useful merge, and the useful merges before it encode the two parts as the two of them; the first useful merge with
 * equal parts is the effective self-pair. Without one the canonical tokens are the bytes the table names and the useful
 * products, the encoding is one left-to-right pass per useful merge in rank order, and a token sequence is an encoding
 * exactly when each adjacent pair encodes as itself. The graph runs two copies of the automaton that guesses those
 * tokens over one input, the second having read the prepended byte as the first byte of a token, and marks a step where
 * the two copies' tokens covering the byte differ in offset or length. Explored from every prepended byte over the
 * bytes the table names, since a byte no merge names is a token no merge crosses, it is trimmed to the pairs that can
 * end both copies at a token end; the pairs from which a marked step leads to such a pair form an acyclic graph, and
 * the longest path to a marked step, plus one, is the radius, less the step's offset in the first copy's token the
 * mirrored constant, the path completed to a pair ending both copies spelling the attaining input. With T the total
 * length of the canonical tokens the graph has at most (1 + T)^2 pairs, and each usefulness and each adjacency encodes
 * the spellings of two tokens by one pass per useful merge.
 *
 * The two parts of a useful merge differ, so its pass joins every occurrence of them, and it moves the first byte from
 * which the encodings of x and of ax agree by at most the length of its right part: the radius is at most the total
 * length of the useful merges' right parts. With n useful merges that total is at most F(n + 3) - 2, F the Fibonacci
 * numbers from F(1) = F(2) = 1, since at most one part of a useful merge is the product of the useful merge just before
 * it, and the table whose every merge joins the token before the latest to the latest, from a and b, attains it.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @return The radius with the mirrored constant when finite, otherwise the first effective self-pair.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank,
 *         each spelling quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Ripple_radius ripple_radius(std::span<const Merge> merges);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP

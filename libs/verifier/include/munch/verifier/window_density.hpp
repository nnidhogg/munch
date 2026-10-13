#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_WINDOW_DENSITY_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_WINDOW_DENSITY_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/ripple_radius.hpp"

namespace munch::verifier
{
/**
 * @brief The guarantee a finite origin radius gives the pinned bytes: every window of the width pins its byte at the
 *        radius, so the cuts are dense, with a density bound between the radius plus one and the width.
 */
struct Pinned_density
{
    /**
     * @brief The origin radius R, ripple_radius()'s.
     */
    std::size_t radius{};

    /**
     * @brief The right start radius Q_start, rule_ceilings()'s.
     */
    std::size_t start_radius{};

    /**
     * @brief The width R + Q_start + 1, at which every window pins its byte R at the start that byte has in the
     *        window's own encoding and so has a cut there.
     */
    std::size_t width{};
};

/**
 * @brief A token whose powers avoid every certified window, and the byte that shows it: the token followed by itself is
 *        its own encoding, and the encoding of the two copies followed by the byte does not begin with the token.
 */
struct Pumping_double
{
    /**
     * @brief The token's spelling.
     */
    std::string token{};

    /**
     * @brief The byte, as a one-byte spelling.
     */
    std::string symbol{};
};

/**
 * @brief Dense certified windows: every string at least as long as the density bound contains a window of width at most
 *        R + 1 that is certified, and a string one shorter avoids them all.
 */
struct Dense_windows
{
    /**
     * @brief The pinned-byte guarantee beside it.
     */
    Pinned_density pinned{};

    /**
     * @brief The density bound N, one more than the longest string that contains no certified window.
     */
    std::size_t bound{};
};

/**
 * @brief Certified windows that are not dense: a loop every power of which contains no certified window, and the
 *        pumping double when one shows it.
 */
struct Sparse_windows
{
    /**
     * @brief The pinned-byte guarantee beside it.
     */
    Pinned_density pinned{};

    /**
     * @brief The loop, nonempty, every power of which contains no certified window of any width.
     */
    std::string loop{};

    /**
     * @brief The first pumping double of the table, none when no token has one.
     */
    std::optional<Pumping_double> pumping_double{};
};

/**
 * @brief Marks a decision that ran out of its budget of avoiding strings before the verdict was established.
 */
struct Density_budget
{
    /**
     * @brief The pinned-byte guarantee, which needs no avoiding string.
     */
    Pinned_density pinned{};
};

/**
 * @brief Whether a merge table's certified windows are dense: dense with the density bound, not dense with a loop that
 *        avoids them, infinite in origin radius with the effective self-pair that makes them not dense, or a budget
 *        spent first.
 */
using Window_density = std::variant<Dense_windows, Sparse_windows, Effective_self_pair, Density_budget>;

/**
 * @brief The number of avoiding strings window_density() enumerates before giving up unless told otherwise, each a
 *        string of at most R bytes and each costing one window decision per byte the table names.
 */
inline constexpr std::size_t window_density_budget{1U << 16U};

/**
 * @brief Decides whether the window certificates of a byte-pair table are dense: whether only finitely many strings
 *        contain no window that is certified at its last byte, and the density bound when they are.
 *
 * The encoder is merge_tower()'s, the lowest-ranked merge present anywhere applied at its leftmost occurrence until
 * none applies. A window W is certified at an origin o when at every occurrence of W in every string the token covering
 * its last byte begins o bytes into it, miscovering()'s certificate; a set of windows is dense when finitely many
 * strings contain none of them, and its density bound N is one more than the longest such string. The decision is
 * Proposition 'The last byte' of the report The Left Ripple of Byte-Pair Encoding. With an infinite origin radius R, as
 * ripple_radius() decides it, the certified windows of no width are dense, by its Theorem 'Density', since a certified
 * window has a cut; the effective self-pair is the reason. With R finite, a window of width h > R + 1 certified at o
 * has its suffix of width R + 1 certified at o - (h - R - 1), so the certified windows of any width are dense exactly
 * when those of width at most R + 1 are, with the same avoiding strings and so the same N.
 *
 * Those are decided by a graph: the strings of R bytes that contain no certified window, each with an edge on a byte c
 * to the string of its last R - 1 bytes and c when no suffix of the R + 1 bytes is a certified window. An avoiding
 * string of R bytes or more is a state followed by a path's label, so the avoiding strings are finite exactly when the
 * graph has no cycle, and N is one more than R plus the longest path, since by Theorem 'Density' it is at least R + 1.
 * A cycle's label is the loop: by factor closure every power of it avoids every certified window. The bytes are those
 * the table names, since a byte no merge names is a token that no merge crosses, certified by itself at zero, and
 * splits every string into parts encoded alone. A window is decided by miscovering() on the table's token automaton at
 * the start its last byte has in the window's own encoding, the only origin that can hold, since the window occurs in
 * itself. The graph is built layer by layer from the empty string, each layer the avoiding strings one byte longer, and
 * searched depth first from its states in ascending order, each byte in ascending order, the loop being the first cycle
 * closed. When Q_start = 0 every window of width R + 1 is certified, the report's Corollary 'Zero start radius', and
 * the table is dense with the density bound R + 1, which is returned before the graph is built, whatever the budget.
 *
 * Beside the verdict, the report's Theorem 'Pinned byte' with the right start radius Q_start of rule_ceilings(): every
 * window of width R + Q_start + 1 pins its byte R at the start that byte has in its own encoding, at every occurrence,
 * and so has a cut there, so the cuts are dense whatever the verdict, and by Theorem 'Density' their density bound lies
 * between R + 1 and R + Q_start + 1. The first pumping double is named on a table that is not dense: a canonical token
 * t whose pair (t, t) encodes as itself and a byte b whose encoding of ttb does not begin with t, the tokens taken in
 * number order, the bytes and then the products by rank, and the bytes ascending. Every power of t then avoids every
 * certified window, since for n >= 2 the encoding of t^n b keeps a prefix of whole copies of t and shares no boundary
 * with that of t^n after it, two copies or more from the end, so the starts that change cover at least |t| consecutive
 * bytes, which lie past R once n|t| reaches R plus the table's right lookahead, and a window of width R + 1 ending at
 * each residue modulo |t| among them is uncertified. The condition is sufficient and not necessary: some tables that
 * are not dense have none.
 *
 * The graph has at most |B|^R states, B the bytes the table names, and each state costs one window decision per byte of
 * B, so the decision is exponential in R in the worst case. What bounds it in practice is that the avoiding strings are
 * few: a string that contains a certified window is cut off with every extension of it, and on a dense table none
 * reaches N bytes, so GPT-2's prefixes of 7, 20 and 41 merges, of radius 3, enumerate at most a hundred. The budget
 * bounds the avoiding strings enumerated, and running out is reported as such, never as a verdict. A window decision is
 * miscovering()'s, at most (|W| - 1) |Q| nodes for Q the automaton's states, and the table's token automaton,
 * rule_ceilings() and ripple_radius() are each computed once.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @param budget The most avoiding strings enumerated before the decision gives up; zero enumerates none.
 * @return Dense windows with the density bound, sparse windows with the loop and the pumping double, the effective
 *         self-pair when the origin radius is infinite, or the budget spent first, the first two and the last with the
 *         pinned-byte guarantee.
 * @throws std::invalid_argument If a merge names a part that neither the alphabet nor an earlier merge carries, or a
 *         product that the alphabet or an earlier merge already carries; the message names the merge and its rank,
 *         each spelling quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Window_density window_density(std::span<const Merge> merges, std::size_t budget = window_density_budget);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_WINDOW_DENSITY_HPP

#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
     * @brief The spelling of the prepended symbol: one byte over the bytes, and a character's UTF-8 bytes over an
     *        alphabet of characters.
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
     * @brief The origin radius, the furthest a prepended symbol moves a covering token, in bytes.
     */
    std::size_t radius{};

    /**
     * @brief A prepended input whose origin ripple is the radius, none when the radius is zero.
     */
    std::optional<Prepended_input> radius_witness{};

    /**
     * @brief The mirrored constant, one past the start of the furthest token a prepended symbol changes, in bytes.
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
 * @brief The part of the overlap graph ripple_radius_with_graph() explores, both giving the same decision to the byte.
 */
enum class Overlap_graph : std::size_t
{
    /**
     * @brief The steps that agree with the bytes still to read, as every step leading to a pair ending both copies
     *        does: a copy at a token end begins only a token whose spelling and the bytes the other copy's token has
     *        still to read are one a prefix of the other, in O(S^2) steps.
     */
    live,

    /**
     * @brief The graph stopped at the pairs ending both copies, whole: a copy at a token end begins every token that
     *        begins with the byte read, in O(S^2 + V^2 S) steps.
     */
    stopped
};

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
 * exactly when each adjacent pair encodes as itself. Both tests read the encoding of two canonical tokens together off
 * their traces, the ranks of the merges of the run on each token alone with the first and the last token after each,
 * recorded as the useful merges are found: until a merge crosses the join, the run on the two is the two runs
 * interleaved, its next merge the lowest-ranked of the next merge inside the left token, the merge across the join and
 * the next merge inside the right token, the leftmost first between equal ranks, so the two encode as themselves
 * exactly when both traces end with no merge across, after at most |u| + |v| - 1 steps for tokens u and v. The graph
 * runs two copies of the automaton that guesses those tokens over one input, the second having read the prepended byte
 * as the first byte of a token, and marks a step where the two copies' tokens covering the byte differ in offset or
 * length. It is explored from every prepended byte the table names, since a byte no merge names is a token no merge
 * crosses, and stops at every pair ending both copies at a token end: such a pair is a token boundary the two encodings
 * share, past which they agree, so no marked step on an accepting path lies beyond it. Every other pair has a copy
 * inside its token, whose next byte is the one byte the pair reads, so only a copy at a token end branches. On a step
 * that leads on to a pair ending both copies the token it begins and the bytes the other copy's token has still to read
 * are one a prefix of the other, since both copies spell the bytes that follow and end their tokens on them, so the
 * live graph, the one ripple_radius() explores, begins only those tokens and the stopped graph every token beginning
 * with the byte read. The pairs from which a marked step leads to a pair ending both copies are the same in both and
 * form an acyclic graph, and the longest path to a marked step, plus one, is the radius, less the step's offset in the
 * first copy's token the mirrored constant, the path completed to a pair ending both copies spelling the attaining
 * input.
 *
 * With S the total length of the canonical tokens and V their number, the pairs the stopped graph expands have one copy
 * inside a token and the other inside one or at a token end, O(S^2 + V S) of them, each with at most V steps and a pair
 * of two copies inside tokens with at most one, so it has O(S^2 + V^2 S) steps, where the product of the two copies,
 * every pair expanded, has up to (1 + S)^2 pairs and O(S^2 + V^2 S + V^4) steps. The part of the product the sources
 * reach has O(S^2 + V^2 S) steps as well, since a pair ending both copies that they reach ends them in two tokens one a
 * suffix of the other, at most 2 S such pairs with at most V^2 steps each, so stopping is sufficient for cubic time and
 * not necessary. The live graph has O(S^2) steps. Take a step from a pair with one copy inside a token t, its first
 * bytes P read and the bytes R still to read, and the other at the end of a token u, beginning a token v: u and P are
 * one a suffix of the other on every pair the sources reach, both suffixes of what the second copy has read, and v and
 * R one a prefix of the other on every step of the live graph. Over the four orders of |u| against |P| and of |v|
 * against |R| the triples of t with |P|, u and v number at most 5 S^2 for either copy inside; the pairs of two copies
 * inside tokens number at most (S - V)^2 with one step each, and the sources at most V with at most V steps each. The
 * tokens one of whose spelling and R is a prefix of the other are read once per t and |P| off a trie of the canonical
 * tokens, in |R| steps and their number: the shorter are listed, ascending, at the node the path that spells R reaches
 * short of its last byte, and the longer at its end, and the lists of the tokens spelled on each node's path hold at
 * most L S tokens together, L the longest token's length. Each usefulness and each adjacency takes at most as many
 * steps of the traces as its two tokens have bytes, less one, so the usefulnesses take O(T) steps together and the at
 * most V^2 adjacencies fewer than 2 V S. With each step's pair, each adjacency and the choices beside each position
 * kept in a hash map, the live graph is explored in O(S^2) space and expected time and the stopped graph in O(S^2 + V^2
 * S), the hash lookups taking constant time on average. S and V are at most twice the table's size T, the lengths of
 * its merges' parts summed. The whole call takes expected time quadratic in T, O(T + S^2), and space O(T + S^2); over
 * the stopped graph it takes expected time cubic in T, O(T + S^2 + V^2 S), and space the same, where the whole product
 * takes quartic time.
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

/**
 * @brief Decides the origin radius of a byte-pair table as ripple_radius(merges) does, over the part of the overlap
 *        graph asked for.
 *
 * The live graph is the one ripple_radius(merges) explores, and the stopped graph, every token beginning with the byte
 * read, gives the same decision to the byte in O(T + S^2 + V^2 S) expected time and space.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @param graph The part of the overlap graph explored.
 * @return As for ripple_radius(merges).
 * @throws std::invalid_argument As for ripple_radius(merges).
 */
[[nodiscard]] Ripple_radius ripple_radius_with_graph(std::span<const Merge> merges, Overlap_graph graph);

/**
 * @brief Decides the origin radius of a merge table over an alphabet of characters, reading each character as its
 *        UTF-8 bytes: how far a character prepended to an input can change the tokens of the input, counted in bytes.
 *
 * The tokens before any merge are the alphabet's characters in place of the bytes, as in a character-level table such
 * as SentencePiece's, and each part of a merge is one of them or the product of an earlier merge, spelled in UTF-8. The
 * prepended symbol is a character of the alphabet and the inputs are the strings over the alphabet. The decision is the
 * byte overload's, the verdict from the merges and the finite values from the overlap graph, which reads the canonical
 * tokens' spellings byte by byte, the second copy having read the bytes of the prepended character as the first
 * character of a token. Its distances are bytes, as every position over the character overloads is: the radius is one
 * past the last byte of the input whose covering token changes, the mirrored constant one past the first byte of the
 * last changed token, and the prepended input spells its character by its UTF-8 bytes. No token begins or ends inside a
 * character, so the bytes of a character share their covering token and the radius ends where a character ends. The
 * bounds the byte overload states on lengths hold with every length in bytes and with T counting the alphabet's bytes
 * as well, and its Fibonacci bound counts characters, not bytes.
 *
 * The radius counted in characters, one past the last character of the input whose covering token changes, is finite
 * exactly when this one is, but it is not this radius divided by a length, since this one weighs each character by its
 * bytes and the inputs attaining the two can differ. With every character the table names at least p and at most q
 * bytes long, the character radius r and this radius R satisfy p r <= R <= q r; over characters of one length k, R is k
 * times r, and a mirrored constant m > 0 counted in characters is k (m - 1) + 1 in bytes. The report The Left Ripple of
 * Byte-Pair Encoding (arXiv:2611.13061) counts symbols, which are characters here, so a figure it gives for a table of
 * characters longer than a byte counts characters where this returns bytes. Over the characters U+0000 to U+007F, in
 * order, it returns what the byte overload returns on the same table, to the byte.
 * @param merges The admissible merge table in rank order, each part a character or the product of an earlier merge.
 * @param alphabet The alphabet, UTF-8 text whose characters are the tokens before any merge, each once.
 * @return The radius with the mirrored constant when finite, otherwise the first effective self-pair.
 * @throws std::invalid_argument If the alphabet holds a character twice, or a merge names a part that neither the
 *         alphabet nor an earlier merge carries, or a product that the alphabet or an earlier merge already carries;
 *         the message names the character, or the merge and its rank, each spelling quoted, a byte outside printable
 *         ASCII written `\xHH` and the quote and the backslash escaped.
 */
[[nodiscard]] Ripple_radius ripple_radius(std::span<const Merge> merges, std::string_view alphabet);

/**
 * @brief Decides the origin radius of a merge table over an alphabet of characters as ripple_radius(merges, alphabet)
 *        does, over the part of the overlap graph asked for.
 *
 * The live graph is the one ripple_radius(merges, alphabet) explores, and the stopped graph, every token beginning with
 * the byte read, gives the same decision to the byte in O(T + S^2 + V^2 S) expected time and space, T counting the
 * alphabet's bytes as well.
 * @param merges The admissible merge table in rank order, each part a character or the product of an earlier merge.
 * @param alphabet The alphabet, UTF-8 text whose characters are the tokens before any merge, each once.
 * @param graph The part of the overlap graph explored.
 * @return As for ripple_radius(merges, alphabet).
 * @throws std::invalid_argument As for ripple_radius(merges, alphabet).
 */
[[nodiscard]] Ripple_radius ripple_radius_with_graph(
        std::span<const Merge> merges, std::string_view alphabet, Overlap_graph graph);

/**
 * @brief Returns the number of steps of the overlap graph that ripple_radius_with_graph(merges, graph) explores on a
 *        byte-pair table, the edges its exploration follows, zero when the table is empty or has an effective
 *        self-pair.
 *
 * It runs the decision ripple_radius_with_graph(merges, graph) runs and returns the number of steps of the graph that
 * decision explores and reads its radius off. The sources are the pairs of the start with every canonical token
 * beginning with a byte the table names, the second copy having read that byte as the first byte of the token, so one
 * call explores the one graph every prepended byte the table names reaches, each step counted once. From the sources
 * the exploration expands every pair it reaches except those ending both copies at a token end, and it counts every
 * step out of a pair expanded, one for each reading of the pair's byte by the first copy together with each by the
 * second, a step into a pair found before counted as well. Either graph gives the same decision to the byte, the live
 * graph in O(S^2) steps and the stopped graph in O(S^2 + V^2 S), the counts the bounds of ripple_radius(merges) are
 * stated in. With an effective self-pair the decision returns it before exploring any graph, and the number is zero. It
 * takes the decision's time and space, O(T + S^2) expected over the live graph and O(T + S^2 + V^2 S) over the stopped
 * graph.
 * @param merges The admissible merge table in rank order, each part a byte or the product of an earlier merge.
 * @param graph The part of the overlap graph explored.
 * @return The number of steps.
 * @throws std::invalid_argument As for ripple_radius(merges).
 */
[[nodiscard]] std::size_t overlap_graph_steps(std::span<const Merge> merges, Overlap_graph graph);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_RIPPLE_RADIUS_HPP

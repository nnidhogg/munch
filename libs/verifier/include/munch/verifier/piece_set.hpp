#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_PIECE_SET_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_PIECE_SET_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
/**
 * @brief A string with an occurrence of a window and a segmentation of it with no boundary at the window's cut, a piece
 *        straddling the cut, and that occurrence.
 */
struct Straddling
{
    /**
     * @brief The string, the window extended by the bytes of the straddling piece that hang past either end, marked
     *        with the piece as one token and every other character as one.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte in the string.
     */
    std::size_t occurrence{};
};

/**
 * @brief A finite set of pieces, the vocabulary of a segmenter that cuts every string it reads into pieces of the set,
 *        with the Aho-Corasick automaton of the pieces, which finds the positions of an input that no occurrence of a
 *        piece straddles.
 *
 * The cut theorem: let a segmenter cut a string x into pieces of the set, by any rule, optimal or not. If no occurrence
 * of a piece in x straddles a position t, 0 < t < |x|, beginning before t and ending after it, then t is a boundary of
 * every segmentation of x into pieces. Proof: in any segmentation the token holding the byte before t is an occurrence
 * [i, j) of a piece with i < t <= j; it does not straddle t, so j = t and t is a boundary. Nothing in the proof looks
 * at how the segmentation was chosen, so the theorem holds for every segmenter whose tokens are pieces of the set,
 * whatever chooses among the segmentations: Unigram's Viterbi pass, its sampled and n-best segmentations, maximal munch
 * over the set, or any other rule. A string with no segmentation into pieces satisfies it vacuously.
 *
 * What lies outside the set is the caller's to map onto it. The cuts are positions of the string the segmenter reads,
 * so under a normalizer, such as SentencePiece's NFKC normalization and its whitespace collapse, they are positions of
 * the normalized string, and cuts of the raw text only through the normalizer's alignment. SentencePiece's Unigram
 * writes a run of characters that have no piece of one character as one unknown token, which is no piece; a wrapper
 * outside munch keeps the theorem by also refusing every cut inside a character and every cut between two such
 * characters. Byte fallback writes such a character as byte tokens, and user-defined symbols are tokens beside the
 * pieces; neither is read here, and a symbol added to the set keeps the theorem sound, while its protection from
 * normalization is again the normalizer's.
 *
 * The set is read in bytes, each piece a byte string and each position a byte index. Over pieces of whole UTF-8
 * characters a position inside a character is straddled by every occurrence of a piece holding it, and a string holding
 * a character that no piece holds has no segmentation, so the cuts of a string that has one fall between characters.
 * piece_set() builds the set and its automaton once, straddle_free() reads an input in one pass, and piece_cut()
 * decides a window's cut for every string the window occurs in.
 */
class Piece_set
{
public:
    /**
     * @brief Returns the pieces, in the order given.
     * @return The pieces.
     */
    [[nodiscard]] const std::vector<std::string>& pieces() const noexcept;

    /**
     * @brief Returns the length of the longest piece, L, in bytes.
     * @return L, zero for a set with no piece.
     */
    [[nodiscard]] std::size_t longest() const noexcept;

    /**
     * @brief Returns whether some piece holds two bytes side by side, the pair table: the window of the two bytes is
     *        certified at its middle exactly when none does.
     * @param first The first byte.
     * @param second The byte after it.
     * @return True when some piece holds the first byte directly followed by the second.
     */
    [[nodiscard]] bool has_pair(char first, char second) const noexcept;

    /**
     * @brief Returns every position of an input that no occurrence of a piece straddles, each a boundary of every
     *        segmentation of the input into pieces by the cut theorem.
     *
     * The input is read once through the automaton. After each byte the automaton's state is the longest suffix of the
     * bytes read that is a prefix of a piece, and the longest piece ending at the byte is the longest piece among the
     * suffixes of that state's spelling, stored with the state; every other occurrence ending at the byte lies inside
     * that one, so it straddles every position they straddle. Each such occurrence of two bytes or more records its end
     * at its start, and a sweep keeps the furthest end recorded before each position, which lies past the position
     * exactly when an occurrence straddles it, in O(|input|) transitions amortized, each a look-up among a node's
     * children, and O(|input|) memory.
     * @param input The input.
     * @return The positions t with 0 < t < |input| that no occurrence of a piece straddles, ascending.
     */
    [[nodiscard]] std::vector<std::size_t> straddle_free(std::string_view input) const;

private:
    // piece_set() builds the set through the constructor.
    friend Piece_set piece_set(std::span<const std::string> pieces);

    /**
     * @brief A node of the automaton, by its index; the root, the empty prefix, is zero.
     */
    using Node_t = std::size_t;

    /**
     * @brief A node of the automaton: a prefix of a piece, its children by byte, its failure link, and the longest
     *        piece that is a suffix of its spelling.
     */
    struct Node
    {
        /**
         * @brief The children, by the byte read to each.
         */
        std::map<unsigned char, Node_t> children{};

        /**
         * @brief The node spelling the longest proper suffix of this node's spelling that is a prefix of a piece, the
         *        root for the root and its children.
         */
        Node_t failure{};

        /**
         * @brief The length of the longest piece that is a suffix of the node's spelling, zero when none is.
         */
        std::size_t ending{};
    };

    /**
     * @brief The root of the automaton, the empty prefix.
     */
    static constexpr Node_t root{0};

    /**
     * @brief The number of ordered pairs of bytes, the size of the pair table.
     */
    static constexpr std::size_t pair_count{256 * 256};

    /**
     * @brief Builds the trie of the pieces, then the failure links and each node's longest ending piece breadth first,
     *        and the pair table.
     * @param pieces The pieces, each spelled by its bytes.
     * @throws std::invalid_argument If a piece is empty or the pieces hold a spelling twice.
     */
    explicit Piece_set(std::span<const std::string> pieces);

    /**
     * @brief Adds a piece to the trie, a node per byte of it past the nodes already there, and its pairs to the table.
     * @param piece The piece, nonempty.
     */
    void add_piece(std::string_view piece);

    /**
     * @brief Returns the index of an ordered pair of bytes in the pair table.
     * @param first The first byte.
     * @param second The byte after it.
     * @return The index.
     */
    [[nodiscard]] static std::size_t pair_index(char first, char second) noexcept;

    /**
     * @brief Sets every node's failure link and longest ending piece, breadth first from the root, so that a node's
     *        failure link, which is shallower, is set before the node's own.
     */
    void link_failures();

    /**
     * @brief Returns the node the automaton enters from a node on a byte: the child on the byte of the node or of the
     *        first node on its failure chain that has one, else the root.
     * @param node The node.
     * @param byte The byte.
     * @return The node entered.
     */
    [[nodiscard]] Node_t next(Node_t node, unsigned char byte) const;

    /**
     * @brief The pieces, in the order given.
     */
    std::vector<std::string> pieces_{};

    /**
     * @brief The nodes, the root first.
     */
    std::vector<Node> nodes_{};

    /**
     * @brief Whether some piece holds each ordered pair of bytes side by side.
     */
    std::vector<bool> pairs_{};

    /**
     * @brief The length of the longest piece.
     */
    std::size_t longest_{0};
};

/**
 * @brief Builds a piece set and the Aho-Corasick automaton of its pieces (Aho and Corasick, Efficient String Matching:
 *        An Aid to Bibliographic Search, Communications of the ACM 18(6), 1975): the trie of the pieces, each node's
 *        failure link to the longest proper suffix of its spelling that is a node too, and each node's longest piece
 *        among the suffixes of its spelling, so that one scan finds the longest piece ending at every byte.
 *
 * The trie has at most one node more than the pieces have bytes, each with its children in a map, and the links are set
 * breadth first in O(total length) transitions amortized; the pair table records, in 65,536 bits, every two bytes some
 * piece holds side by side. A caller reading many inputs builds the set once. A set may be empty, and then every
 * position is a cut and only the empty string has a segmentation.
 * @param pieces The pieces, each spelled by its bytes, nonempty and each once.
 * @return The piece set.
 * @throws std::invalid_argument If a piece is empty, the message naming its index, or the pieces hold a spelling twice,
 *         the message naming it quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash
 *         escaped.
 */
[[nodiscard]] Piece_set piece_set(std::span<const std::string> pieces);

/**
 * @brief Decides the cut offset bytes into a window for every segmenter over a piece set: whether every occurrence of
 *        the window in every string puts a boundary at the cut in every segmentation of the string into pieces.
 *
 * The cut is certified when no piece p, placed with its byte j on the window's byte at the offset for some 0 < j < |p|,
 * agrees with the window wherever the two overlap. At an occurrence of the window a piece straddling the cut is such a
 * placement, its bytes inside the window being the window's, so when there is none no piece straddles the cut and the
 * cut theorem of Piece_set makes it a boundary of every segmentation. Conversely a placement that agrees gives a
 * string, the window extended by the bytes of the piece that hang past either end, in which the piece straddles the
 * cut, and writing the piece as one token and every other character alone segments it without that boundary. That
 * string, so segmented, is the refutation returned, for the first piece in the set's order at its first placement that
 * agrees, the bytes outside the piece split before every byte that is not a UTF-8 continuation byte. So the decision is
 * exact whenever every character of the window is a piece by itself, a byte or, for a window and pieces of whole UTF-8
 * characters, a character, and the refutation is then a segmentation into pieces whenever each character it writes
 * alone is a piece, as for a window and pieces of whole characters or a window of ASCII; over a set lacking some it is
 * sound and may refuse a cut that holds.
 *
 * With at least L - 1 bytes on either side of the cut, L the longest piece, every piece placed across it lies inside
 * the window, so the decision is whether a piece occurs in the window across the cut, which straddle_free() answers on
 * the window itself; the least width with that property is 2L - 2 for L at least 2, the cut in the middle. A narrower
 * window is decided exactly by the same condition, a piece that hangs past an end agreeing only where it overlaps, and
 * certification is monotone, a certified window staying certified with bytes added on either side. At width two the
 * decision is the pair table, the window of two bytes certified at its middle exactly when no piece holds them side by
 * side, and a pair at the cut that no piece holds certifies at any width, which has_pair() checks first. Otherwise
 * every piece is placed at each of its bytes but the first, at most the pieces' total length of placements, each
 * comparing at most L bytes.
 * @param set The piece set.
 * @param window The window, nonempty.
 * @param offset The cut, the index of the window's first byte after it, between two of its bytes.
 * @return std::nullopt when the cut is certified, a boundary at every occurrence of the window in every segmentation,
 *         otherwise a string with an occurrence of the window, segmented with a piece straddling the cut, and that
 *         occurrence.
 * @throws std::invalid_argument If the window is empty or the offset does not lie between two of its bytes.
 */
[[nodiscard]] std::optional<Straddling> piece_cut(const Piece_set& set, std::string_view window, std::size_t offset);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_PIECE_SET_HPP

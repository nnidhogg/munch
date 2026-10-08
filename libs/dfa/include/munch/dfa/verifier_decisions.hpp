#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "munch/dfa/verifier.hpp"

/**
 * @file
 * @brief The six decisions over a verifier: miscovering(), boundary_gap(), realizable(), divergence(), dependence()
 *        and chunk_dependence().
 *
 * Each is a product with the verifier and a search on the product, and each witness is replayable through
 * Verifier::step().
 */
namespace munch::dfa
{
/**
 * @brief An input with its boundary marking, one bit per byte saying whether a boundary follows it.
 */
struct Marked_string
{
    /**
     * @brief The bytes.
     */
    std::string bytes{};

    /**
     * @brief Whether a boundary follows each byte, as many bits as bytes.
     */
    std::vector<bool> boundaries{};
};

/**
 * @brief An accepted marked string with an occurrence of a window whose covering boundary is not the origin, and that
 *        occurrence.
 */
struct Miscovering
{
    /**
     * @brief The accepted marked string, the shortest one.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte at an occurrence whose covering boundary is not the origin.
     */
    std::size_t occurrence{};
};

/**
 * @brief Three marked strings such that stem, then loop repeated any number of times, then suffix is accepted, the loop
 *        carrying no boundary.
 */
struct Lasso
{
    /**
     * @brief The marked string from the start to the loop.
     */
    Marked_string stem{};

    /**
     * @brief The markless cycle, nonempty.
     */
    Marked_string loop{};

    /**
     * @brief The marked string from the loop to an accepting state.
     */
    Marked_string suffix{};
};

/**
 * @brief The supremum of the distance between consecutive boundaries over the accepted marked strings, or the lasso
 *        whose pumping makes it unbounded.
 */
using Boundary_gap = std::variant<std::size_t, Lasso>;

/**
 * @brief Per state, the marked symbols whose steps keep completion possible, in ascending order.
 */
using Mask = std::vector<std::vector<Marked>>;

/**
 * @brief The half of full equivalence a diverging input falls in.
 */
enum class Half : std::size_t
{
    /**
     * @brief Both verifiers accept the input under markings that differ.
     */
    boundary,

    /**
     * @brief Exactly one verifier accepts the input under some marking.
     */
    domain
};

/**
 * @brief A text on which two verifiers diverge, and the half it diverges in.
 */
struct Divergence
{
    /**
     * @brief The half the witness falls in.
     */
    Half half{};

    /**
     * @brief The diverging input, the shortest one of its half, marked as the first verifier accepts it for the
     *        boundary half and as the accepting verifier accepts it for the domain half.
     */
    Marked_string witness{};
};

/**
 * @brief An accepted marked string with an occurrence of a certified window whose cut is not independent, that
 *        occurrence, the cut, and the side or sides that fail: the prefix before the cut or the suffix from it is not
 *        accepted under the whole string's marking restricted to it.
 */
struct Dependence
{
    /**
     * @brief The accepted marked string, the shortest one.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte at the occurrence whose cut is not independent.
     */
    std::size_t occurrence{};

    /**
     * @brief The cut, the occurrence plus the origin, a boundary of the segmentation.
     */
    std::size_t cut{};

    /**
     * @brief Whether the prefix before the cut, under the segmentation's marks with no boundary after its last byte, is
     *        not accepted.
     */
    bool prefix_fails{};

    /**
     * @brief Whether the suffix from the cut, under the segmentation's marks, is not accepted.
     */
    bool suffix_fails{};
};

/**
 * @brief A pair the verifier certifies: a window and the origin, the byte of the window at which the token containing
 *        an occurrence's final byte begins.
 */
struct Certified_pair
{
    /**
     * @brief The window, nonempty.
     */
    std::string_view window{};

    /**
     * @brief The certified origin, a byte index of the window.
     */
    std::size_t origin{};
};

/**
 * @brief A chunk of an input: the half-open range from its first byte to the byte after its last.
 */
struct Chunk
{
    /**
     * @brief Ordered by the first byte, then by the end.
     */
    auto operator<=>(const Chunk&) const = default;

    /**
     * @brief The index of the chunk's first byte.
     */
    std::size_t begin{};

    /**
     * @brief The index after the chunk's last byte.
     */
    std::size_t end{};
};

/**
 * @brief An accepted marked string with a chunk between its anchors that is not accepted under the whole string's
 *        marking restricted to it, its anchors, and the chunks that fail.
 */
struct Chunk_dependence
{
    /**
     * @brief The accepted marked string, the shortest one.
     */
    Marked_string segmentation{};

    /**
     * @brief The anchors, the positions the windows' origins land on at every occurrence of every window of the
     *        inventory, ascending and without repeats, each a boundary of the segmentation.
     */
    std::vector<std::size_t> anchors{};

    /**
     * @brief The chunks between consecutive anchors, the first from position zero and the last to the end, that are
     *        not accepted under the segmentation's marks restricted to them, ascending and at least one.
     */
    std::vector<Chunk> failing{};
};

/**
 * @brief Decides the window certificate (window, origin): the token containing an occurrence's final byte begins at the
 *        occurrence's byte origin, at every occurrence in every accepted marked string.
 *
 * A breadth-first search over the product of the verifier with the window's matching progress, the boundary marks of
 * the last bytes read and a sticky flag raised at an occurrence whose covering boundary is elsewhere; the first
 * accepting product state with the flag raised gives the shortest miscovering.
 * @param verifier The verifier.
 * @param window The window, nonempty.
 * @param origin The certified origin, a byte index of the window.
 * @return std::nullopt when the certificate holds, otherwise the shortest accepted marked string with an occurrence
 *         whose covering boundary is not the origin.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it.
 */
[[nodiscard]] std::optional<Miscovering> miscovering(
        const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Decides whether the distance between consecutive boundaries is bounded over the accepted marked strings.
 *
 * The distance counts the bytes of a segment, the final one closed by the end of the input. A markless cycle gives the
 * lasso: the shortest stem from the start to a state on it, the cycle from that state, and the shortest suffix from it
 * to an accepting state. Without one the markless steps form an acyclic graph, and the supremum is the longest markless
 * path, one more when a marked step leaves its end and as is when its end accepts.
 * @param verifier The verifier, of a nonempty domain.
 * @return The supremum, or the lasso when the distance is unbounded.
 * @throws std::invalid_argument If the domain is empty.
 */
[[nodiscard]] Boundary_gap boundary_gap(const Verifier& verifier);

/**
 * @brief Decides whether generation restricted to completion-preserving steps never strands, and returns the mask.
 *
 * Every state of a trim verifier can complete but the start of an empty domain, so the mask admits every transition,
 * and from every state generation can stop at an accepting state or take an admitted step.
 * @param verifier The verifier.
 * @return std::nullopt when the domain is empty, otherwise each state's admitted steps.
 */
[[nodiscard]] std::optional<Mask> realizable(const Verifier& verifier);

/**
 * @brief Decides whether two verifiers accept the same marked strings, and returns a text they diverge on when not.
 *
 * The boundary half first, a breadth-first search over the product of the two verifiers reading one byte of the
 * alphabets' intersection under a marking each, latching once the markings differ; then the domain half, a
 * breadth-first search over the subset construction of each verifier over the union of the alphabets, the empty input
 * first. A verifier's alphabet is the bytes on its transitions. Both verifiers are functional, accepting one marking
 * per input of their domain, as armed_run() builds them and a caller's table must be; the two halves then decide
 * equality of the accepted marked languages.
 * @param a The first verifier, functional.
 * @param b The second verifier, functional.
 * @return std::nullopt when the two are equivalent, otherwise the shortest witness of the first half that has one.
 */
[[nodiscard]] std::optional<Divergence> divergence(const Verifier& a, const Verifier& b);

/**
 * @brief Decides whether every cut the window certificate (window, origin) certifies is independent: at every
 *        occurrence of the window in every accepted marked string, the prefix before the cut at the origin and the
 *        suffix from it are each accepted under the whole string's marking restricted to them.
 *
 * The certificate makes the cut a boundary of the whole string's segmentation; independence says that each side is
 * segmented alone as the whole string segments it, so that a scan may stop at the cut or begin there. A breadth-first
 * search over a product of the verifier in two phases. Before the cut the product carries the verifier's state and how
 * much of the window's first origin bytes the text's end matches, and the cut is placed on a marked byte where they
 * have just been read, the prefix's acceptance read off the state the cut byte reaches without the mark, the prefix's
 * own final symbol carrying no boundary, and latched as a flag rather than ending the path. After the cut a continuing
 * and a restarted copy of the verifier read the same marked suffix while the window's remaining bytes are confirmed to
 * follow at once. The first product state admitted with the occurrence confirmed, the continuing copy accepting and the
 * flag raised or the restarted copy not accepting gives the shortest dependence, the cut at position zero taken before
 * any byte when the origin is zero. The product has at most |Q| (origin + 1) states before the cut and 2 |Q| (|Q| + 1)
 * (|window| - origin + 1) after it, Q the verifier's states.
 * @param verifier The verifier, functional.
 * @param window The window, nonempty.
 * @param origin The certified origin, a byte index of the window.
 * @return std::nullopt when every certified cut is independent, otherwise the shortest accepted marked string with an
 *         occurrence whose cut is not, the occurrence, the cut and the sides that fail.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it, or the verifier does not certify
 *         the pair, so that no cut is certified.
 */
[[nodiscard]] std::optional<Dependence> dependence(
        const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Decides whether an inventory of certified pairs is chunk-independent: in every accepted marked string, cut at
 *        every anchor, the position a window's origin lands on at every occurrence of every window of the inventory,
 *        each chunk between consecutive cuts, the first from position zero and the last to the end, is accepted under
 *        the whole string's marking restricted to it.
 *
 * Cutting at the anchors of a chunk-independent inventory lets a scan take every chunk alone. Cut independence of every
 * pair by dependence() implies chunk independence, a middle chunk being the suffix side of a certified cut within the
 * prefix side of the next; the converse fails, since an inventory is chunk-independent when its strings are cut finely
 * enough that no chunk shows what a whole side would, by another pair's anchors or by a window's overlapping
 * occurrences. A breadth-first search over a product of a continuing copy of the verifier reading the string and a
 * restarted copy reading the current chunk, fed with a lag of h bytes for the longest window h, so that every
 * occurrence anchoring the position after a byte has completed when the byte reaches it. The product holds up to h
 * marked bytes the restarted copy has not read and the h - 2 bytes before them, where such an occurrence may begin, a
 * bit raised at the first commit, which places the anchor at position zero, and a flag latched when a chunk is refused.
 * At an anchor the restarted copy reads the committed byte without its mark, is accepted or refused, and restarts.
 * Every product state is a candidate end, where the held bytes are committed with no more to come; the first admitted
 * whose continuing copy accepts with a chunk refused gives the shortest witness. The product has at most
 * 4 |Q| (|Q| + 1) H(h) M(h) states, Q the verifier's states, H(h) the sum of |Sigma|^k for k up to max(h - 2, 0) and
 * M(h) the sum of (2 |Sigma|)^j for j up to h, Sigma the alphabet.
 * @param verifier The verifier, functional.
 * @param inventory The certified pairs, at least one.
 * @return std::nullopt when the inventory is chunk-independent, otherwise the shortest accepted marked string with a
 *         chunk that fails, its anchors and the chunks that fail.
 * @throws std::invalid_argument If the inventory is empty, a window is empty or its origin lies outside it, or the
 *         verifier does not certify a pair, so that its anchors are not boundaries.
 */
[[nodiscard]] std::optional<Chunk_dependence> chunk_dependence(
        const Verifier& verifier, std::span<const Certified_pair> inventory);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_DECISIONS_HPP

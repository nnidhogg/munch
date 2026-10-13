#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_K_COPY_SCAN_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_K_COPY_SCAN_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
/**
 * @brief The shortest window of a rank bound: the bound, the window and its origin, and the window's rank there, at
 *        most the bound.
 */
struct Fanout_window
{
    /**
     * @brief The rank bound.
     */
    std::size_t bound{};

    /**
     * @brief The window, the least by length and then by bytes among the windows of rank at most the bound.
     */
    std::string window{};

    /**
     * @brief The origin, the least among the window's origins of rank at most the bound.
     */
    std::size_t origin{};

    /**
     * @brief The window's rank at the origin, between one and the bound.
     */
    std::size_t rank{};
};

/**
 * @brief The relative fan-out of a pair: its relative rank, the number of classes of its configurations that the
 *        window's remaining bytes do not tell apart, and those classes.
 */
struct Relative_fanout
{
    /**
     * @brief The relative rank, the number of classes, at most the pair's rank.
     */
    std::size_t rank{};

    /**
     * @brief The classes, each the configurations fanout() returns that it holds, ascending, and the classes ordered
     *        by their least configuration.
     */
    std::vector<std::vector<Verifier::State_t>> classes{};
};

/**
 * @brief A pair whose occurrences cut a scan: a window and the origin at which its cut lies.
 */
struct Fanout_pair
{
    /**
     * @brief The window, nonempty.
     */
    std::string_view window{};

    /**
     * @brief The origin, a byte index of the window.
     */
    std::size_t origin{};
};

/**
 * @brief What a k-copy scan is asked for: the chunks aimed for, the pairs whose anchors may cut the input, and the
 *        bytes each worker reads past its chunk.
 */
struct Scan_request
{
    /**
     * @brief The number of chunks aimed for; fewer result where no anchor is found, and zero behaves as one.
     */
    std::size_t chunks{};

    /**
     * @brief The pairs whose anchors may cut the input.
     */
    std::span<const Fanout_pair> inventory{};

    /**
     * @brief The bytes a worker reads past the end of its chunk, fewer at the input's end.
     */
    std::size_t lookahead{};
};

/**
 * @brief One worker's chunk: its bytes, the copies it ran, and the cut at which the stitch fixed its marking.
 */
struct Scanned_chunk
{
    /**
     * @brief The index of the chunk's first byte, the previous chunk's end or zero.
     */
    std::size_t begin{};

    /**
     * @brief The index after the chunk's last byte, the next chunk's first or the input's size.
     */
    std::size_t end{};

    /**
     * @brief The copies the worker ran, one per configuration of the cut at its first byte, one for the first chunk.
     */
    std::size_t copies{};

    /**
     * @brief The first cut, from the chunk's end on, at which every chain the stitch keeps there passes through one
     *        exit of the chunk: its end when selection is pure there, a later cut, or the input's size when only the
     *        end resolves it.
     */
    std::size_t resolved{};
};

/**
 * @brief A k-copy scan: the input's segmentation and the chunks it was scanned in, in input order.
 */
struct K_copy_scan
{
    /**
     * @brief The input's one accepted marking.
     */
    Marked_string segmentation{};

    /**
     * @brief The chunks, the first beginning at zero, each beginning at the previous one's end, the last ending at the
     *        input's size; an empty input has one empty chunk.
     */
    std::vector<Scanned_chunk> chunks{};
};

/**
 * @brief Decides, for every rank bound k from the least rank of a window that occurs to the largest, the shortest
 *        window of rank at most k: the least pair (window, origin) by length, then by bytes and then by origin, whose
 *        rank as fanout() decides it lies between one and k.
 *
 * The rank of a pair (uv, |u|) depends only on the set of states u leads to under any marks from every state of the
 * minimal automaton and the set of states from which v can be read, by the two-sided formula of fanout(), so the least
 * rank and the shortest window of every bound are decided over two finite families of state sets with no bound on the
 * window's length: the forward images of the nonempty byte strings, with the states some byte enters standing for an
 * empty u at origin zero, and the backward domains of the nonempty byte strings, each set with the least of the
 * shortest strings that give it, closed by breadth-first search over the byte projection of the minimal automaton. A
 * shortest window of rank at most k is a shortest string of its forward set followed by a shortest string of its
 * backward set, since a shorter string of either set would shorten the window at the same rank, so the least pair is
 * the least u v over the pairs of members whose sets meet in between one and k states, its origin |u|. Over windows
 * that occur a bounded number of times the least rank can lie below the degree smallest_fanout() decides, which bounds
 * the windows that recur. Each family holds at most 2^|Q| sets, Q the states of the minimal automaton, each closed
 * under the |Sigma| bytes in O(|Q|^2) steps, and the pairs of members are compared in O(|F| |B| |Q|), F and B the two
 * families, so the search is exponential in the worst case, besides the minimization fanout() takes.
 * @param verifier The verifier, functional.
 * @return One window per bound from the least rank to the largest rank of a window that occurs, the first of rank the
 *         least, or none when no window occurs with its cut after position zero.
 */
[[nodiscard]] std::vector<Fanout_window> shortest_fanout_windows(const Verifier& verifier);

/**
 * @brief Decides the relative fan-out of the pair (window, origin): the classes of the configurations fanout() returns
 *        when two are identified that every marking of the window's bytes from the origin on takes to the same state of
 *        the minimal automaton or refuses from both.
 *
 * Configurations of one class give a worker equal copies on every chunk that begins at the cut and holds the window's
 * remaining bytes, since every marking of those bytes leads both to one state or refuses both and the rest of the chunk
 * is read from that state, so one copy per class suffices there. Two configurations of different classes differ on a
 * marking of those bytes after which one is at a state the other is not, and two states of the minimal automaton are
 * told apart by some marked continuation, so some chunk running to the end of an accepted marked string gives them
 * different copies, and the relative rank is the fewest copies a worker keyed by configurations runs on such chunks.
 * Rank and relative rank part where a token begun before the cut dies inside the window: under maximal munch over a, b
 * and aa the certified pair (b, 0) has rank 2 and relative rank 1, the b refusing the aa still armed after a lone a.
 * Each configuration is compared with the least of every class found so far by a walk over the pairs of states the
 * remaining bytes lead the two to under shared marks, a refusal counted as a state, at most (|Q| + 1)^2 pairs per byte,
 * Q the states of the minimal automaton, besides the minimization and the subset runs of fanout().
 * @param verifier The verifier, functional.
 * @param window The window, nonempty.
 * @param origin The origin, a byte index of the window.
 * @return The relative rank and the classes, none when the pair has rank zero.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it.
 */
[[nodiscard]] Relative_fanout relative_fanout(const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Scans an input over a verifier in chunks cut at anchors of an inventory, each worker running one copy of the
 *        scan per configuration its cut can carry, and stitches the copies into the input's segmentation.
 *
 * An anchor is a cut after position zero that the origin of a pair lands on at an occurrence of its window in the
 * input, and its configurations are those fanout() gives every pair anchoring it, the intersection of their
 * configurations. The cuts are sought as core::Lexer::chunk_boundaries() seeks them: each interior cut is the first
 * anchor at or after the later of its equal-division target and one past the previous cut, and the search ends at a
 * target with no anchor at or after it, the last chunk running to the input's end. Each chunk is scanned by a worker on
 * its own thread, the last on the calling thread, and a worker runs one copy per configuration of the cut its chunk
 * begins at, the start alone for the first chunk. A copy reads the chunk from its configuration under every marking the
 * minimal automaton admits and offers, for each configuration of the cut ending the chunk that it reaches, the one
 * marking that reaches it, and on the last chunk each accepting state it reaches. A worker reads the request's
 * lookahead past its chunk, fewer bytes at the input's end, and drops an exit from which those bytes cannot be read, or
 * from which they reach no accepting state when they run to the end. The stitch begins at the start state, lets each
 * chunk's exit select the next chunk's copy, keeps every exit a selected copy offers with a back-pointer to the exit
 * that selected it, and at the input's end follows the back-pointers from the one accepting exit.
 *
 * The contract is the k-copy chunk theorem. For every functional verifier, every inventory, every input in the domain
 * and every choice of cuts among the anchors: (1) the configuration the input's own segmentation reaches at each cut is
 * among the cut's configurations; (2) a copy reaches each exit under at most one marking of its chunk; (3) exactly one
 * chain of selected copies runs from the start through one exit of every chunk to an accepting state, and its markings
 * concatenate to the input's segmentation; (4) the worker of a cut runs as many copies as the cut has configurations,
 * at most the least rank of a pair anchoring it. An input outside the domain has no chain. Proof: (1) is the two-sided
 * formula fanout() decides, the configuration at an occurrence's cut being reached on the window's first origin bytes
 * from a state that can complete and leading on the window's remaining bytes to one, for every pair anchoring the cut.
 * (2) Two markings of a chunk joining two states that can complete would, with a marked prefix reaching the first and a
 * marked suffix completing from the second, give one input two accepted markings, which a functional verifier excludes.
 * (3) The input's accepted run passes through the configuration at every cut and gives a chain; any chain concatenates
 * to an accepted marking of the input, its segmentation by functionality, and the minimal automaton is deterministic
 * over marked symbols, so the chain is that run. (4) is the construction. This is the chunk theorem of the report
 * Finitely Verifiable Segmentation made rank-k: there each chunk is scanned alone from the start and must be segmented
 * alone as the whole input segments it, while a k-copy worker begins at every configuration its cut can carry, so no
 * independence condition is needed and the price is the fan-out, known from the pairs before any input is read; a
 * pumpable window's anchors recur, and a cut it anchors alone costs at least the degree smallest_fanout() decides.
 *
 * A selected copy offers several exits when the marking of its chunk depends on bytes past the cut ending it; the
 * stitch keeps them, and a later cut or the input's end resolves them. Selection is pure, each chunk's exit known at
 * its end so that the predecessor's exit alone selects the next copy, at every cut of one configuration, and at every
 * cut when the policy has delay d, its boundary bit after a byte fixed by the input's next d bytes, and the lookahead
 * is at least d: two exits of the selected copy that both read the lookahead are reached by two accepted strings
 * agreeing on the input through d bytes past the cut, whose bits up to the cut then agree, so the two are one. Maximal
 * munch over a token set whose longest token has l bytes, and two-mode WordPiece likewise, have delay l - 1, and
 * canonical byte-pair encoding over a table of byte merges, merge_tower(merges)'s, has delay at most
 * Q_start + 1 <= max(1, S) whatever its origin radius, Q_start its right start radius and S its merges' left parts
 * summed as rule_ceilings() reads them, since an appended string moves no token start further than Q_start bytes back
 * from the end. Over characters, merge_tower(merges, alphabet)'s, the delay counts bytes and the bound does not hold:
 * under the one merge (a, U+1F600) over a, U+1F600 and U+1F601, S is 1 and the boundary after a waits on the fourth
 * byte past it. Backward maximal munch and a key rule that breaks ties to the right have no bounded delay in general:
 * under backward maximal munch over a and aa a run of a's is cut by its parity counted from its right end, as under the
 * merge (a, a) applied at its rightmost occurrence, and with no lookahead every chunk of the run resolves only at the
 * input's end.
 *
 * Each pair's configurations are found by fanout() once and its anchors by one search of the input; a copy keeps one
 * state per marking alive, at most |Q| per byte, Q the states of the minimal automaton, and the exits that survive the
 * lookahead are found once per chunk by one walk back over its h bytes, so a worker costs O((1 + c |chunk| + h) |Q|)
 * steps and O((1 + c |chunk|) |Q|) memory for its c copies, and the stitch is linear in the exits kept, besides, per
 * cut, a walk back over the chains kept at earlier cuts to their nearest common ancestor.
 * @param verifier The verifier, functional.
 * @param input The input.
 * @param request The chunks aimed for, the inventory and the lookahead.
 * @return The input's segmentation and its chunks with the copies each worker ran and the cut at which each resolved,
 *         or std::nullopt when the input is outside the domain.
 * @throws std::invalid_argument If a window of the inventory is empty or its origin lies outside it.
 */
[[nodiscard]] std::optional<K_copy_scan> k_copy_scan(
        const Verifier& verifier, std::string_view input, const Scan_request& request);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_K_COPY_SCAN_HPP

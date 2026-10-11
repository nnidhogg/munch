#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_DECISIONS_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_DECISIONS_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "munch/verifier/verifier.hpp"

/**
 * @file
 * @brief The nine decisions over a verifier: miscovering(), boundary_gap(), realizable(), divergence(), dependence(),
 *        chunk_dependence(), right_context(), fanout() and smallest_fanout().
 *
 * Each is a search on a product with the verifier or on its minimal automaton, fanout() two subset runs on the latter,
 * and each witness is replayable through Verifier::step().
 */
namespace munch::verifier
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
     * @brief The accepted marked string, the shortest one when miscovering() decides on a verifier.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte at an occurrence whose covering boundary is not the origin.
     */
    std::size_t occurrence{};
};

/**
 * @brief Three marked strings such that stem, then loop repeated any number of times, then suffix is accepted.
 */
struct Lasso
{
    /**
     * @brief The marked string from the start to the loop.
     */
    Marked_string stem{};

    /**
     * @brief The cycle, nonempty.
     */
    Marked_string loop{};

    /**
     * @brief The marked string from the loop to an accepting state.
     */
    Marked_string suffix{};
};

/**
 * @brief The supremum of the distance between consecutive boundaries over the accepted marked strings, or the lasso
 *        whose pumping makes it unbounded, its loop carrying no boundary.
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
 * @brief An accepted marked string with an occurrence of a certified window and a right context too short at its cut,
 *        that occurrence, the cut and the context: the string's first bytes up to the cut plus the context, read as a
 *        whole input, are not accepted under the string's marks below the cut, with a boundary at the cut when the
 *        context is positive.
 */
struct Truncation
{
    /**
     * @brief The accepted marked string, longer than the cut plus the context.
     */
    Marked_string segmentation{};

    /**
     * @brief The index of the window's first byte at the occurrence.
     */
    std::size_t occurrence{};

    /**
     * @brief The cut, the occurrence plus the origin, a boundary of the segmentation after position zero.
     */
    std::size_t cut{};

    /**
     * @brief The right context that fails, the number of bytes read past the cut.
     */
    std::size_t context{};
};

/**
 * @brief A finite right context, the least from which every right context suffices, the least that suffices alone, and
 *        a truncation at every right context that fails.
 */
struct Finite_context
{
    /**
     * @brief The right context, one more than the largest that fails, zero when none does.
     */
    std::size_t context{};

    /**
     * @brief The least right context that fails at no occurrence, at most the right context.
     */
    std::size_t least{};

    /**
     * @brief A truncation at every right context that fails, ascending by context, the shortest one at its context.
     */
    std::vector<Truncation> failing{};
};

/**
 * @brief The witness of an unbounded right context: a lasso whose stem, then loop repeated n times, then suffix is
 *        accepted, its cut failing with the context plus n times the loop's length.
 */
struct Truncation_lasso
{
    /**
     * @brief The lasso, its stem from the start through the cut to the loop and its loop a cycle read a whole number of
     *        times.
     */
    Lasso lasso{};

    /**
     * @brief The index of the window's first byte at the occurrence, within the stem.
     */
    std::size_t occurrence{};

    /**
     * @brief The cut, the occurrence plus the origin, within the stem.
     */
    std::size_t cut{};

    /**
     * @brief The right context that fails with the loop read no times.
     */
    std::size_t context{};
};

/**
 * @brief An unbounded right context: the right contexts that fail, those below the threshold each with a truncation
 *        and those from it on the residues modulo the period that fail, each with a truncation lasso, and the least
 *        right context that suffices alone.
 */
struct Unbounded_context
{
    /**
     * @brief The threshold, the least from which the failing right contexts repeat with the period.
     */
    std::size_t threshold{};

    /**
     * @brief The period, the least with which the failing right contexts repeat from some point on.
     */
    std::size_t period{};

    /**
     * @brief The least right context that fails at no occurrence, none when every one fails at some occurrence.
     */
    std::optional<std::size_t> least{};

    /**
     * @brief A truncation at every right context below the threshold that fails, ascending by context, the shortest one
     *        at its context.
     */
    std::vector<Truncation> failing{};

    /**
     * @brief A truncation lasso for every residue modulo the period at which every right context from the threshold
     *        fails, ascending by context, its context at least the threshold and in the residue, and its loop's length
     *        a multiple of the period, so that every context it pumps lies in the residue.
     */
    std::vector<Truncation_lasso> residues{};
};

/**
 * @brief The right context of a certified pair with the right contexts that fail: finite with a truncation at each, or
 *        unbounded with the period from which they repeat, a truncation at each below its threshold and a truncation
 *        lasso per residue that fails from it.
 */
using Right_context = std::variant<Finite_context, Unbounded_context>;

/**
 * @brief The fan-out of a pair: its rank, the number of configurations its cut can carry, and those configurations.
 */
struct Fanout
{
    /**
     * @brief The rank, the number of configurations, zero when no occurrence has its cut after position zero.
     */
    std::size_t rank{};

    /**
     * @brief The configurations, each as the least verifier state of its class, ascending.
     */
    std::vector<Verifier::State_t> configurations{};
};

/**
 * @brief The smallest fan-out of a pumpable window, the degree, a window (ww, |w|) of that rank, and a lasso whose loop
 *        reads w.
 */
struct Smallest_fanout
{
    /**
     * @brief The degree, the least rank of a pumpable window.
     */
    std::size_t degree{};

    /**
     * @brief The window ww, its rank the degree at the origin.
     */
    std::string window{};

    /**
     * @brief The origin |w|, the cut between the two copies of w.
     */
    std::size_t origin{};

    /**
     * @brief The lasso, its loop the marked w, from a state of a configuration of the window back to one of the same
     *        configuration.
     */
    Lasso lasso{};
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

/**
 * @brief Decides the right context of a certified pair (window, origin) and every right context that fails: the right
 *        context is the least R such that at every occurrence of the window in every accepted marked string whose cut
 *        at the origin lies after position zero, and for every R' of at least R, the string's first bytes up to the
 *        cut plus R', read as a whole input, are segmented below the cut as the string is, with a boundary at the cut
 *        when R' is positive, and R' fails when at some such occurrence they are not.
 *
 * A worker scanning a chunk that ends at a certified cut reads R' bytes past it, the input ending there or at the
 * string's end, and keeps the tokens before the cut; it reproduces the string's segmentation of the chunk when its
 * input is accepted under the string's marks below the cut, a boundary at the cut when R' is positive, and none after
 * its last byte, which for a functional verifier is the input's one segmentation. That is the prefix side of a cut, and
 * dependence() is the case R' = 0 together with the suffix side. The cut at position zero has no byte below it and is
 * left out. A context that suffices need not make a larger one suffice, as under maximal munch over aa and b, where the
 * pair (ba, 1) needs none and every odd context cuts a run of a's at odd length, so the right context is one more than
 * the largest that fails at some occurrence, and unbounded when no largest exists; an inventory's is the largest of its
 * pairs', since its failing contexts are the union of theirs. The failing contexts are therefore returned whole, with
 * the least context that suffices alone, which can lie below the right context.
 *
 * A breadth-first search explores a product of the verifier in two phases. Before the cut it carries the verifier's
 * state and how much of the window's first origin bytes the text's end matches, and the cut is placed on a marked byte
 * where they have just been read, as dependence() places it. After the cut it carries the continuing state, the set of
 * states a worker's copy can be in under any marks of the bytes read since the cut, how much of the window's remaining
 * bytes have followed, and whether the worker's input ending at the last byte read is refused: at the cut when the cut
 * byte without its mark reaches no acceptance, and after it when no state of the set before the byte reaches acceptance
 * on the byte without its mark. A context R' fails at an occurrence exactly when the key R' bytes past its cut carries
 * the refusal and a step continues from it to a key with the window's remaining bytes read and the continuing state
 * accepting, so the failing contexts are the lengths past the cut of the product's paths from a key the cut places to a
 * key where a failing context ends. Their supremum is finite exactly when no cycle lies on such a path, since a cycle
 * pumps the length and a path longer than the number of keys repeats one; the decision is exact, and finite with the
 * keys, at most 1 + n (origin + 1) before the cut and n (2 + t 2^n) after it, for n = |Q| - 1, Q the verifier's states,
 * and t = |window| - origin. No step enters the start, so every key but the first holds one of n states. After the cut
 * the continuing state lies in the worker's set, so the cut places at most 2 n keys, the set holding that state alone,
 * and each of the t later counts of the window's bytes read past it holds at most n 2^(n - 1) pairs of a state and a
 * set, each with or without the refusal.
 *
 * Every key on such a path is relevant, a key after the cut from which a key where a failing context ends is reachable,
 * and every relevant key lies on one, being reached through a key the cut places. Layer L holds the relevant keys a
 * path of L steps from a key the cut places ends at, so layer zero holds the relevant keys the cut places, layer L + 1
 * the relevant keys a step leads to from layer L, and L fails exactly when layer L holds a key where a failing context
 * ends. Each layer is a function of the one before, so once layer b repeats an earlier layer a, layer L + b - a is
 * layer L for every L from a on, and the failing contexts repeat with period b - a from a. Two periods from a common
 * point give their greatest common divisor as one, so the least period p with which the failing contexts repeat from
 * some point on divides b - a, and it is the least divisor of b - a with which the failures of the layers from a to b
 * repeat; the threshold is the least point from which they repeat with period p, at most a. Below the threshold each
 * failing context has its shortest truncation, found by a breadth-first search over the keys paired with the steps
 * taken since the cut. From the threshold on, the failing contexts are those in the residues modulo p at which they
 * fail, and in each such residue the least context L that is at least both the threshold and the number N of relevant
 * keys has a shortest truncation whose path past the cut visits L + 1 > N keys and so repeats one: its first repeat
 * closes a cycle of c steps, and the lasso's stem is the truncation's string up to the repeat, its loop the cycle read
 * p / gcd(c, p) times, a multiple of p, and its suffix the rest of the string, so that every context the lasso pumps
 * lies in the residue. Without a cycle among the relevant keys every such path is shorter than N, so layer N is empty
 * and repeats at once, the period is one and no residue fails: the right context is the threshold, one more than the
 * largest failing context and zero when none fails. With one the failing contexts are unbounded, a residue failing from
 * the threshold on. The least context that suffices alone is the least that fails nowhere, found below the threshold
 * plus p. The layers are sets of relevant keys, so at most 2^N of them are explored, and N + 1 without a cycle; the
 * period divides the least common multiple, over the strongly connected components of the relevant keys, of the
 * greatest common divisor of each one's cycle lengths, which is at most its size, and the components are disjoint, so
 * the period is at most Landau's function of N, the largest least common multiple of numbers summing to N, and the
 * answer is superpolynomial in N in the worst case, while each truncation searches at most (L + 2) times the keys for
 * its context L.
 * @param verifier The verifier, functional.
 * @param window The window, nonempty.
 * @param origin The certified origin, a byte index of the window.
 * @return The finite right context with the least context that suffices alone and a truncation at every context that
 *         fails, or, when it is unbounded, the threshold, the period and the least context that suffices alone, with a
 *         truncation at every context below the threshold that fails and a truncation lasso per residue that fails
 *         from it.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it, or the verifier does not certify
 *         the pair, so that no cut is certified.
 */
[[nodiscard]] Right_context right_context(const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Decides the fan-out of the pair (window, origin): its rank, the number of configurations the cut at the origin
 *        can carry at an occurrence of the window in an accepted marked string, over every occurrence whose cut lies
 *        after position zero.
 *
 * A configuration is a state of the minimal automaton of the accepted marked strings, a class of verifier states that
 * no marked continuation tells apart, reached on the string's marked prefix before the cut. It is all a scan resumed at
 * the cut needs, so a worker beginning there runs one copy per configuration and the configuration the scan before it
 * ends in selects the copy; counting the verifier's own states could only split configurations the future does not
 * distinguish. The cut at position zero carries the start alone, known without selection, and is left out. The
 * configurations are the states the window's first origin bytes lead to under any marks from any state that can
 * complete, which can also read the remaining bytes under some marks to a state that can complete: a forward subset run
 * over the window's first origin bytes and a backward one over the rest, on the byte projection of the minimal
 * automaton. At origin zero the forward set is the states some byte enters, since the cut follows the byte before the
 * occurrence. A rank of one fixes the configuration at every such cut in advance, which is not certification: the token
 * covering the window may begin elsewhere at different occurrences, and a certified cut may carry several
 * configurations. Minimizing takes at most |Q| passes of Moore refinement over the 2 |Q| |Sigma| marked steps, Q the
 * verifier's states and Sigma the bytes on its transitions, and the two subset runs O(|window| |Q|) steps.
 * @param verifier The verifier, functional.
 * @param window The window, nonempty.
 * @param origin The origin, a byte index of the window.
 * @return The rank and the configurations.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it.
 */
[[nodiscard]] Fanout fanout(const Verifier& verifier, std::string_view window, std::size_t origin);

/**
 * @brief Decides the smallest fan-out of a pumpable window, one that occurs any number of times in some accepted marked
 *        strings: the degree, the fewest fixed points of a nonzero idempotent among the relations that nonempty byte
 *        strings induce on the states of the minimal automaton.
 *
 * Only a pumpable window occurs in some string more often than any bound, so the degree is the least number of copies a
 * worker must run at a cut that recurs. A byte string relates two states of the minimal automaton that can complete
 * when it can be read under some marks from one to the other, and these relations form a monoid, searched breadth first
 * from the bytes. Every pumpable window has rank at least the degree d at every origin, and for w the shortest byte
 * string, the least in byte order among the shortest, whose relation is an idempotent with d fixed points, the pair
 * (ww, |w|) has rank exactly d, its configurations the fixed points. Over windows that occur a bounded number of times
 * the least rank can lie below the degree, a window after the first token of a two-mode scan occurring once per string.
 * The witness pumps w at the least fixed point: the stem reaches it from the start, the loop reads w under the marks
 * that return to it, and the suffix completes. The search is exponential in the worst case: with m the states of the
 * minimal automaton the monoid N holds at most 2^(m^2) relations, each found once by the one search that finds the
 * degree and its string, multiplied by every byte in O(m^2), looked up in O(m^2 log |N|) and squared in O(m^3), so the
 * time is O(|N| (|Sigma| m^2 log |N| + m^3)) and the memory O(|N| m^2), Sigma the bytes on the verifier's transitions,
 * besides the minimization fanout() takes and the witness searches over the verifier, the loop one over at most |w| |Q|
 * keys, Q the verifier's states.
 * @param verifier The verifier, functional.
 * @return The degree, the window (ww, |w|) and its origin, and the stem, loop and suffix.
 * @throws std::invalid_argument If the domain is finite, so that no window recurs.
 */
[[nodiscard]] Smallest_fanout smallest_fanout(const Verifier& verifier);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_DECISIONS_HPP

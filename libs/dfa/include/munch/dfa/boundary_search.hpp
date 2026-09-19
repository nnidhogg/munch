#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_SEARCH_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_SEARCH_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/dfa/simulator.hpp"

/**
 * @brief The decisions that ask whether some completely tokenizable input makes an event happen, and the one search
 *        that answers them: rescue(), boundary_difference(), window_occurrence(), window_counterexample() and
 *        segmentation_difference().
 *
 * Each asks after an event, a scan rolling back, two token sets cutting a shared input apart, a window occurring, a
 * certificate being failed, two token sets segmenting an input differently, and all answer it the same way: the
 * input is built byte by byte, breadth first, while guessing where its tokens end, every closed segment's run kept
 * alive because a later accept on it proves the close was not the longest match. The guesses that survive are
 * exactly the maximal-munch segmentation, which is what makes guessing boundaries sound. A witness is therefore a
 * shortest input on which the event happens, and the cap is a ceiling on the states held rather than a budget spent
 * afterwards, so an answer is always one the cap paid for and a search the cap stops settles nothing, which every
 * result reports as exhaustive rather than leaving to be inferred from an empty witness.
 *
 * They are five instances of one search over one key, differing only in how many scans they walk, what raises the
 * key's mark and what else a branch's future depends on, which is why they are declared together here and
 * implemented in one unit, boundary_search.cpp: the search, its key and the moves over it are private to that file,
 * and a decision added later joins them there rather than being handed them across a header. The header reads as the
 * tree's classes do, the types first, the caps after them and the functions last, so that the five results stand side
 * by side, one shape with Separation adding the half, before the five contracts that produce them. The sixth
 * relative, anchor_free_span(), is not here: it walks the compiled machine rather than guessed inputs, building the
 * graph of positions the same guessed marking reaches and taking a longest anchor-free run or a cycle in it, so it has
 * neither witness nor cap and keeps its own header. The recovery queries in recovery.hpp walk the compiled machine
 * over a given tail and guess nothing.
 */
namespace munch::dfa
{
/**
 * @brief What the search for a rescue found.
 */
struct Rescue
{
    /**
     * @brief A completely tokenizable input holding a token whose scan read past the token's end before rolling
     *        back to it, the shortest such input; empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: rescue-free when the
     * search exhausted, and undetermined when the cap stopped it.
     */
    bool exhaustive{};
};

/**
 * @brief What a differential search over two token sets found.
 */
struct Difference
{
    /**
     * @brief An input both token sets tokenize completely and cut differently, empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: proved identical when
     * the search exhausted, and undetermined when the cap stopped it. A caller that treats the second as the first
     * would ship an unchecked assumption, which is the whole failure this decision exists to prevent.
     */
    bool exhaustive{};
};

/**
 * @brief What the search for an occurrence of a window found.
 */
struct Occurrence
{
    /**
     * @brief A nonempty completely tokenizable input containing the window, the shortest one; empty when none was
     *        found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: occurring in no nonempty
     * completely tokenizable input when the search exhausted, and undetermined when the cap stopped it. A caller
     * that treats the second as the first would call a certificate vacuous whose window may well occur.
     */
    bool exhaustive{};
};

/**
 * @brief What the search for a counterexample of a window certificate found.
 */
struct Counterexample
{
    /**
     * @brief A completely tokenizable input containing an occurrence of the window whose covering token begins
     *        elsewhere than the certified origin, the shortest one; empty when none was found.
     */
    std::string witness;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: the certificate proved
     * exact when the search exhausted, and undetermined when the cap stopped it. A caller that treats the second
     * as the first would cut at a window some input covers from elsewhere, which is the whole failure a
     * certificate exists to exclude.
     */
    bool exhaustive{};
};

/**
 * @brief The half of full equivalence a separating input falls in.
 *
 * Full equivalence has two halves, the domains coinciding and the segmentations agreeing on the common domain, and an
 * input separating two token sets fails exactly one of them: either one set tokenizes it completely and the other does
 * not, or both do and cut it apart. Which half is a fact about the input, read off the two scans of it.
 */
enum class Separation_half : std::size_t
{
    /**
     * @brief One token set tokenizes the input completely and the other does not.
     */
    domain,

    /**
     * @brief Both token sets tokenize the input completely and cut it into different tokens.
     */
    boundary
};

/**
 * @brief What the search for an input separating two token sets found.
 */
struct Separation
{
    /**
     * @brief An input the two token sets segment differently, the shortest one; empty when none was found.
     */
    std::string witness;

    /**
     * @brief The half the witness falls in; nothing when there is no witness.
     */
    std::optional<Separation_half> half;

    /**
     * @brief Whether the search settled the question, by exhausting its state space or by finding the witness,
     *        rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: proved the same segmentation
     * function when the search exhausted, and undetermined when the cap stopped it. A caller that treats the second as
     * the first would ship an unchecked assumption, which is the whole failure this decision exists to prevent.
     */
    bool exhaustive{};
};

/**
 * @brief The number of search states rescue() holds before giving up unless told otherwise, generous for the token
 *        sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t rescue_cap{1U << 20U};

/**
 * @brief The number of product states boundary_difference() holds before giving up unless told otherwise, generous
 *        for the token sets a lexer carries, where the worst case is exponential in both state counts.
 */
inline constexpr std::size_t difference_cap{1U << 20U};

/**
 * @brief The number of search states window_occurrence() holds before giving up unless told otherwise, generous for
 *        the token sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t occurrence_cap{1U << 20U};

/**
 * @brief The number of search states window_counterexample() holds before giving up unless told otherwise, generous for
 *        the token sets a lexer carries, where the worst case is exponential in the state count.
 */
inline constexpr std::size_t counterexample_cap{1U << 20U};

/**
 * @brief The number of search states segmentation_difference() holds before giving up unless told otherwise,
 *        generous for the token sets a lexer carries, where the worst case is exponential in both state counts.
 */
inline constexpr std::size_t segmentation_cap{1U << 20U};

/**
 * @brief Whether some completely tokenizable input makes the scan roll back, with a witness.
 *
 * A rescue is a rollback after a failed lookahead that lets the scan continue where a scheme restarting at
 * every accept would have declared the input malformed: a token of a completely tokenizable input whose scan
 * consumed at least one byte past the token's end, by a transition into a nonaccepting state, before dying
 * there, at a missing transition or the end of the input, and rolling back. On a rescue-free token set no such
 * token exists, so the two schemes emit the same tokens on every completely tokenizable input. Zero-lag sets
 * are rescue-free, and so is {a, abc, bc} with lag one: the stretch after the accepted a is entered on b, but
 * every completely tokenizable continuation begins with the token bc, whose c closes the longer token abc, so
 * the scan never rolls back to a.
 *
 * Decided exactly by the same boundary-guessing search as boundary_difference(): the input is built byte by
 * byte with every closed segment's run kept alive, a closed run that accepts abandoning the branch, so that
 * the surviving markings are the maximal-munch ones, and a closed run that survives a byte is the rollback
 * looked for. The search starts at the initial state and reaches every position a scan can stand in, so the
 * accepting states no input reaches never enter it.
 * @param simulator The compiled token set.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one
 *        the cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from
 *         an exhaustive search proves the token set rescue-free.
 */
[[nodiscard]] Rescue rescue(const Simulator& simulator, std::size_t cap = rescue_cap);

/**
 * @brief Whether two token sets cut some input they both tokenize into different tokens, with a witness.
 *
 * The question a tokenizer upgrade asks: does the new token set place a boundary the old one did not, on input
 * both accept? Answered from the two compiled tables alone, before any corpus exists, so a negative is a
 * statement about every input rather than about the ones a test suite happened to hold.
 *
 * The search walks both scans at once. Each side carries the run of the segment it is reading and the runs of
 * segments it has already closed; a closed run is kept alive because if it later accepts, the close was not the
 * longest match and the marking being explored is not the maximal-munch one, so the branch is abandoned. That is
 * what makes guessing boundaries sound: the guesses that survive are exactly the greedy segmentation. A byte at
 * which one side may close and the other may not sets the divergence flag, and the input is a witness when both
 * sides can close their last segment with the flag already set.
 * @param simulator The compiled token set the comparison starts from.
 * @param other The token set to compare against, compiled over the same byte alphabet.
 * @param cap The most product states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness and whether the search was exhaustive.
 */
[[nodiscard]] Difference boundary_difference(
        const Simulator& simulator, const Simulator& other, std::size_t cap = difference_cap);

/**
 * @brief Decides whether the given byte string occurs in some completely tokenizable input, returning one that
 *        contains it.
 *
 * The question a certificate leaves open. A certified split window (W, o) promises where the token covering the
 * occurrence's final byte begins in every completely tokenizable input containing W, and the promise is conditional
 * on occurrence: a window no completely tokenizable input contains satisfies it vacuously, so is_split_window() may
 * certify it and it anchors nothing. This decision splits the two readings of a certificate. A window
 * is_split_window() certifies is an occurring certificate when a witness comes back, and a vacuous one when an
 * exhaustive search finds none; the decision says nothing about the certificate itself, only whether it is about any
 * input. The decision is exact where the certificate is model-relative, so a window is_split_window() refuses may
 * occur or not, and this call answers that as well.
 *
 * Decided by the same boundary-guessing search as rescue() and boundary_difference(): the input is built byte by byte,
 * breadth first, with every closed segment's run kept alive, a closed run that accepts abandoning the branch, so that
 * the surviving markings are the maximal-munch ones. Beside the scan a window matcher guesses where the occurrence
 * begins, as the scan guesses where its tokens end, and reads the window byte by byte from there; the input is a
 * witness once the whole window has been read and the segment being read closes. A witness is therefore a shortest
 * completely tokenizable input containing the window, and never empty, so that an empty one means none: the question
 * is asked over nonempty inputs, the empty input, which every token set tokenizes completely and which contains the
 * empty window alone, being no input a cut could fall in. The empty window, contained in every input, has a shortest
 * token as its witness, and under a token set with no positive-width token, one accepting nothing or the empty
 * string alone, it occurs in no nonempty input and the answer is none. The search starts at the initial state and
 * reaches every position a scan can stand in, so the accepting states no input reaches never enter it, and a nullable
 * token set is decided through the positive-width equivalent the simulator compiled, as every decision here is.
 * @param simulator The compiled token set.
 * @param window The byte string to find.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from an
 *         exhaustive search proves that no nonempty completely tokenizable input contains the window.
 */
[[nodiscard]] Occurrence window_occurrence(
        const Simulator& simulator, std::string_view window, std::size_t cap = occurrence_cap);

/**
 * @brief Decides whether the window certificate (W, o) is failed by some completely tokenizable input, returning
 *        one that fails it.
 *
 * A certified split window (W, o) promises: in every completely tokenizable input and at every occurrence of W in it,
 * the token covering the occurrence's final byte begins exactly o bytes into the occurrence. A counterexample is a
 * completely tokenizable input holding an occurrence whose covering token begins elsewhere, and this decision searches
 * for one. The certificate quantifies over completed scans and nothing else, so an exhaustive search that finds no
 * counterexample proves the certificate exact over every completely tokenizable input, not relative to any model of the
 * scanner. is_split_window() decides the same certificate through the conservative cloud model, and the two stand in
 * one relation: a window it certifies at o has no counterexample at o, the model only ever over-approximating the scan,
 * while a window it refuses is refused relative to the model, and this decision settles it either way, with the
 * witness where the refusal was right and a proof where it was conservative. The relation to window_occurrence() is
 * the vacuous reading: a window no completely tokenizable input contains has no counterexample at any origin, its
 * certificate holding of no input, so a certificate proved exact here anchors something only where that decision
 * places the window.
 *
 * Decided by the same boundary-guessing search as rescue(), boundary_difference() and window_occurrence(): the input
 * is built byte by byte, breadth first, with every closed segment's run kept alive, a closed run that accepts
 * abandoning the branch, so that the surviving markings are the maximal-munch ones. Beside the scan the window
 * matcher of window_occurrence() guesses where the occurrence begins and reads the window byte by byte from there,
 * and beside the matcher one bit records whether the latest token start sits at the origin, set at every guessed
 * boundary. The occurrence fails the certificate when the window's final byte is read with the bit clear, and the
 * input is a witness once the segment being read then closes; an occurrence read through with the bit set conforms,
 * and its branch is dropped while the branches on which the matcher waits for a later occurrence carry on. A witness
 * is therefore a shortest failing input, and never empty. The search starts at the initial state and reaches every
 * position a scan can stand in, so the accepting states no input reaches never enter it, and a nullable token set is
 * decided through the positive-width equivalent the simulator compiled, as every decision here is.
 * @param simulator The compiled token set.
 * @param window The byte string of the certificate, non-empty.
 * @param origin The offset into the window at which the certificate places the covering token's start, inside it.
 * @param cap The most search states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, and whether the search settled the question; an empty witness from an
 *         exhaustive search proves that no completely tokenizable input fails the certificate.
 * @throws std::invalid_argument If the window is empty or the origin lies outside it, neither being a certificate.
 */
[[nodiscard]] Counterexample window_counterexample(
        const Simulator& simulator, std::string_view window, std::size_t origin, std::size_t cap = counterexample_cap);

/**
 * @brief Decides whether two token sets are the same segmentation function, returning an input they segment
 *        differently.
 *
 * Full equivalence, the whole of it: the two sets tokenize the same inputs completely, and cut every one of them
 * alike. A token set's segmentation function is the set of marked runs its scan accepts, a marked run being the
 * input's bytes each with or without a token boundary after it, and maximal munch assigns every input of the domain
 * exactly one marking, so the accepted marked runs are the function's graph and two token sets are fully equivalent
 * exactly when those languages are equal. Equal languages have the same byte projection, which is the domain, and one
 * marking each on a shared input, so the segmentations agree there; unequal languages carry a marked run only one side
 * accepts, whose bytes either leave one domain or are segmented apart. The decision is that language equality: both
 * sides are deterministic over the marked symbols, a byte with or without a close, and a breadth-first walk of their
 * product over the symmetric difference, each side completed with a dead state, finds the shortest marked run exactly
 * one side accepts or proves the languages equal. The witness is that run's bytes, and the half it falls in is a fact
 * about the witness, read off the two scans of it: a domain witness one set tokenizes completely and the other does
 * not, a boundary witness both do and cut apart.
 *
 * The relation to boundary_difference(), which decides the boundary half alone: an exhaustive negative here proves
 * the two sets cut every shared input alike, so it is an exhaustive negative there as well, and a boundary witness
 * here is an input both tokenize and cut apart, so it is a witness there. The witnesses need not agree: the
 * two-half route, the boundary half through boundary_difference() and the domain half beside it, may return another
 * input in another half than the shortest marked run only one side accepts. Over {a} against {aa} the shortest such
 * run is a with no boundary, which {a} accepts and {aa} does not, the domain witness a, while boundary_difference()
 * returns aa, one token against two.
 *
 * Decided by the same boundary-guessing search as rescue(), boundary_difference(), window_occurrence() and
 * window_counterexample(): the input is built byte by byte, breadth first, with every closed segment's run kept alive,
 * a closed run that accepts abandoning that side, so that the runs a side survives are exactly its maximal-munch
 * markings. Here the guessed marking is one and fed to both scans at once, a side that cannot read a marked symbol
 * dying rather than abandoning the branch, and the input is a witness at the first marked symbol after which exactly
 * one side accepts. The empty input is in every domain, so the witness is never empty and an empty one means none.
 * The search starts at the initial states and reaches every position a scan can stand in, so the accepting states no
 * input reaches never enter it, and a nullable token set is decided through the positive-width equivalent the
 * simulator compiled, as every decision here is.
 * @param simulator The compiled token set the comparison starts from.
 * @param other The token set to compare against, compiled over the same byte alphabet.
 * @param cap The most product states to hold at once, a ceiling rather than a budget spent afterwards: a state
 *        beyond it is never held, and the search gives up instead of admitting it, so an answer is always one the
 *        cap paid for. Zero holds nothing, not even the state the search starts in, and settles nothing.
 * @return The witness, the shortest one, the half it falls in, and whether the search settled the question; an empty
 *         witness from an exhaustive search proves the two token sets the same segmentation function.
 */
[[nodiscard]] Separation segmentation_difference(
        const Simulator& simulator, const Simulator& other, std::size_t cap = segmentation_cap);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_SEARCH_HPP

#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEGMENTATION_DIFFERENCE_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEGMENTATION_DIFFERENCE_HPP

#include <cstddef>
#include <optional>
#include <string>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
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
     * Reported rather than inferred because an empty witness means two different things: proved the same
     * segmentation function when the search exhausted, and undetermined when the cap stopped it. A caller that
     * treats the second as the first would ship an unchecked assumption, which is the whole failure this decision
     * exists to prevent.
     */
    bool exhaustive{};
};

/**
 * @brief The number of search states segmentation_difference() holds before giving up unless told otherwise,
 *        generous for the token sets a lexer carries, where the worst case is exponential in both state counts.
 */
inline constexpr std::size_t segmentation_cap{1U << 20U};

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
 * The relation to boundary_difference(), which decides the boundary half alone: an exhaustive negative here proves the
 * two sets cut every shared input alike, so it is an exhaustive negative there as well, and a boundary witness here is
 * an input both tokenize and cut apart, so it is a witness there. The witnesses need not agree: the two-half route,
 * the boundary half through boundary_difference() and the domain half beside it, may return another input in another
 * half than the shortest marked run only one side accepts. Over {a} against {aa} the shortest such run is a with no
 * boundary, which {a} accepts and {aa} does not, the domain witness a, while boundary_difference() returns aa, one
 * token against two.
 *
 * Decided by the same boundary-guessing search as rescue(), boundary_difference(), window_occurrence() and
 * window_violation(): the input is built byte by byte, breadth first, with every closed segment's run kept alive, a
 * closed run that accepts abandoning that side, so that the runs a side survives are exactly its maximal-munch
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

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEGMENTATION_DIFFERENCE_HPP

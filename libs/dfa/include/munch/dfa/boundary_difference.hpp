#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_DIFFERENCE_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_DIFFERENCE_HPP

#include <cstddef>
#include <string>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
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
     * @brief Whether the search covered its whole state space rather than stopping at the cap.
     *
     * Reported rather than inferred because an empty witness means two different things: proved identical when
     * the search exhausted, and undetermined when it did not. A caller that treats the second as the first would
     * ship an unchecked assumption, which is the whole failure this decision exists to prevent.
     */
    bool exhaustive{};
};

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
 * @param cap The largest number of product states to visit before giving up; the default is generous for the
 *        token sets a lexer carries and the worst case is exponential in both state counts.
 * @return The witness and whether the search was exhaustive.
 */
[[nodiscard]] Difference boundary_difference(
        const Simulator& simulator, const Simulator& other, std::size_t cap = 1U << 20U);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_BOUNDARY_DIFFERENCE_HPP

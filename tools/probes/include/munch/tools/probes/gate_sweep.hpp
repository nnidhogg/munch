#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SWEEP_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SWEEP_HPP

#include <cstddef>

#include "munch/tools/probes/gate_totals.hpp"

/**
 * @brief The window model against the shipped predicates over deterministic random grammars, Sweep and
 *        random_grammars.
 */
namespace munch::tools::probes
{
/**
 * @brief The counts of a random sweep.
 */
struct Sweep
{
    /**
     * @brief The grammars within the state limit whose trimmed automaton is not empty.
     */
    std::size_t usable{0};

    /**
     * @brief The grammars within the state limit with a token matching the empty string, decided through their
     *        positive-width equivalent.
     */
    std::size_t nullable{0};

    /**
     * @brief The usable grammars certifying at least one byte.
     */
    std::size_t with_certificate{0};

    /**
     * @brief The usable grammars certifying no byte for which the model finds a certified window.
     */
    std::size_t rescued{0};

    /**
     * @brief Those rescued grammars for which the bounded witness search finds an occurrence of a certified word.
     */
    std::size_t witnessed_rescued{0};

    /**
     * @brief The grammars certifying no byte whose search exhausted the quotient without a window.
     */
    std::size_t proved_none{0};

    /**
     * @brief The grammars certifying no byte whose search exceeded kSubsetBudget without a window.
     */
    std::size_t inconclusive{0};

    /**
     * @brief The disagreement instances: every byte on which the model and is_split_point() differ, and every backup
     *        input whose covering token begins elsewhere than the model's origin.
     */
    std::size_t disagreements{0};
};

/**
 * @brief Draws grammars of two to five random tokens over the alphabet {a, b, c} from a fixed seed and checks the
 *        model on each against the shipped scanner and predicates: the length-one certificate, the shortest window
 *        and its witness where no byte is certified, and the backup check over every certified two-byte window, each
 *        cross-checked against the shipped window decision. A grammar that exceeds the state limit of 400 is skipped.
 * @param totals The gate's totals, which count the cross-checks, the retained search keys and the witness rejections.
 * @param count The grammars drawn.
 * @return The sweep's counts.
 */
[[nodiscard]] Sweep random_grammars(Gate_totals& totals, std::size_t count);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SWEEP_HPP

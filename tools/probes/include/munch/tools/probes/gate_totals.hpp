#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_TOTALS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_TOTALS_HPP

#include <cstddef>
#include <optional>
#include <string>

#include "munch/core/lexer.hpp"

/**
 * @brief What every check of the window gate adds to the figures its summary pins, Gate_totals.
 */
namespace munch::tools::probes
{
/**
 * @brief The window gate's running totals: the shipped window decision's cross-checks against the model, the rewinding
 *        executions of the named rows, the quotient keys the random sweep walked, and the witness searches' origin
 *        disagreements.
 */
struct Gate_totals
{
    /**
     * @brief Asks the shipped core::Lexer::is_split_window() about a window and counts the check, and a disagreement
     *        when its answer differs from the model's.
     * @param lexer The lexer compiled from the grammar the model was asked about.
     * @param window The window.
     * @param model The model's answer: the certified origin, std::nullopt for a refusal.
     */
    void cross_check(const core::Lexer& lexer, const std::string& window, const std::optional<std::size_t>& model);

    /**
     * @brief The shipped window decision's answers compared with the model's.
     */
    std::size_t port_checks{0};

    /**
     * @brief The compared answers that differed.
     */
    std::size_t port_disagreements{0};

    /**
     * @brief The rewinding executions the named rows' backup checks ran.
     */
    std::size_t exercised_total{0};

    /**
     * @brief Those rewinding executions whose whole input tokenizes completely.
     */
    std::size_t exercised_tokenizable{0};

    /**
     * @brief The quotient keys the random sweep's shortest-window searches retained, summed over its grammars.
     */
    std::size_t visited_total{0};

    /**
     * @brief The most quotient keys one grammar's shortest-window search retained.
     */
    std::size_t visited_max{0};

    /**
     * @brief The completely tokenized witness candidates whose token covering the window's final byte began elsewhere
     *        than at the predicted origin.
     */
    std::size_t witness_disagreements{0};
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_TOTALS_HPP

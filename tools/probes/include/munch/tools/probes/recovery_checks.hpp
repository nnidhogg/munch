#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CHECKS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CHECKS_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <string>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

/**
 * @brief Every theorem and consistency assertion of the recovery study over one damaged trial, Cell, Trial,
 *        Campaign_totals and check_trial: each violation printed on standard error as it is found and counted.
 */
namespace munch::tools::probes
{
/**
 * @brief One cell of the campaign: a row damaged by one operation at one width under one seed.
 */
struct Cell
{
    /**
     * @brief The row.
     */
    const Row& row;

    /**
     * @brief The row's index among the rows, which salts the cell's streams.
     */
    std::size_t row_index{0};

    /**
     * @brief The damage operation.
     */
    Op op{Op::substitution};

    /**
     * @brief The operation's index in ops.
     */
    std::size_t op_index{0};

    /**
     * @brief The damage width.
     */
    std::size_t width{0};

    /**
     * @brief The width's index in widths.
     */
    std::size_t width_index{0};

    /**
     * @brief The seed index, from zero.
     */
    std::size_t seed{0};
};

/**
 * @brief One damaging trial of a cell: the damage, what the decider says at the blind anchor, and every arm's incident.
 */
struct Trial
{
    /**
     * @brief The cell the trial belongs to.
     */
    const Cell& cell;

    /**
     * @brief The trial's index within its cell.
     */
    std::size_t index{0};

    /**
     * @brief The damage position p.
     */
    std::size_t position{0};

    /**
     * @brief The serial scan's failure offset e on the damaged input, below its size.
     */
    std::size_t failure{0};

    /**
     * @brief The damaged input and its coordinate map.
     */
    Damage damaged{};

    /**
     * @brief The first image of a pristine boundary at or past the corruption end.
     */
    std::optional<std::size_t> first_true{};

    /**
     * @brief core::Lexer::minimal_repair() of the blind tail, the input from one past the failure: a repair exists when
     *        it answers.
     */
    std::optional<std::string> repair{};

    /**
     * @brief core::Lexer::next_anchored_start() at the blind anchor, in damaged coordinates.
     */
    std::optional<std::size_t> direct{};

    /**
     * @brief Every arm's incident, in arms order.
     */
    std::array<Incident, arms.size()> incidents{};
};

/**
 * @brief The campaign's running totals over every trial, which the closing summary prints.
 */
struct Campaign_totals
{
    /**
     * @brief The theorem and consistency violations.
     */
    std::size_t theorem_failures{0};

    /**
     * @brief The certified arm's first answers whose evidence begins at or past the corruption end.
     */
    std::size_t evidence_covered{0};

    /**
     * @brief The certified arm's moves over all incidents.
     */
    std::size_t certified_moves_total{0};

    /**
     * @brief Those moves whose evidence begins at or past the corruption end.
     */
    std::size_t certified_moves_covered{0};

    /**
     * @brief The certified arm's first answers whose evidence begins before the corruption end.
     */
    std::size_t evidence_uncovered{0};

    /**
     * @brief Those uncovered first answers that landed.
     */
    std::size_t evidence_uncovered_landed{0};

    /**
     * @brief The certified arm's first answers past the smallest answer a certificate from the same start yields.
     */
    std::size_t nonminimal_answers{0};

    /**
     * @brief The bytes by which those answers passed the smallest one, summed.
     */
    std::size_t nonminimal_bytes{0};

    /**
     * @brief The clean certified arm's first answers.
     */
    std::size_t clean_answers{0};

    /**
     * @brief The clean certified arm's first refusals.
     */
    std::size_t clean_refusals{0};

    /**
     * @brief The trials whose damage the grammar absorbed, the damaged input tokenizing completely.
     */
    std::size_t absorbed_total{0};

    /**
     * @brief The damaging trials whose blind tail has a repair.
     */
    std::size_t repairable_total{0};

    /**
     * @brief The damaging trials whose blind tail has none.
     */
    std::size_t unrepairable_total{0};

    /**
     * @brief The certified arm's first answers on unrepairable trials.
     */
    std::size_t vacuous_walk_answers{0};

    /**
     * @brief The exact arm's first answers.
     */
    std::size_t exact_answers_total{0};

    /**
     * @brief The bytes by which the direct decider answer precedes the certified arm's first answer, summed over the
     *        repairable trials where the exact arm, the certified arm and the direct decider all answered.
     */
    std::size_t exact_saved_bytes{0};

    /**
     * @brief The certified arm's first answer minus the exact arm's, summed over the trials where both answered.
     */
    std::ptrdiff_t exact_net_displacement{0};

    /**
     * @brief The trials where both the certified and the exact arm answered.
     */
    std::size_t exact_pairs{0};

    /**
     * @brief The sampled damage positions that repeat an earlier one of their cell.
     */
    std::size_t duplicate_positions{0};
};

/**
 * @brief Checks every assertion over one trial in a fixed order, printing each violation on standard error and counting
 *        it in theorem_failures, and adds the trial to the totals: the repair witness, the replica walk against the
 *        certified arm's first evidence, the transfer of the first answer, every certified move, the clean certified
 *        arm, the decider's consistency with the walk, the exact arm's pairing, the clean exact arm and the delimiter
 *        conventions.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_trial(const Trial& trial, Campaign_totals& totals);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_CHECKS_HPP

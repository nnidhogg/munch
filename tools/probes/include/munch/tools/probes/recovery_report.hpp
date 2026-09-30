#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_REPORT_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_REPORT_HPP

#include <array>
#include <cstddef>
#include <vector>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

/**
 * @brief The recovery study's tables, Tally, Row_tallies, tally_incident and the four printers: per row the stratified
 *        table, the pooled summary with Wilson intervals and the per-seed first landing, and the campaign's closing
 *        summary.
 */
namespace munch::tools::probes
{
/**
 * @brief One arm's accumulated incident outcomes over a stratum.
 */
struct Tally
{
    /**
     * @brief The damaging trials.
     */
    std::size_t trials{0};

    /**
     * @brief The incidents whose first move produced a resume position.
     */
    std::size_t answers{0};

    /**
     * @brief The incidents whose first move refused.
     */
    std::size_t refusals{0};

    /**
     * @brief The first answers that landed.
     */
    std::size_t first_landings{0};

    /**
     * @brief The terminal positions inside the input that landed.
     */
    std::size_t terminal_landings{0};

    /**
     * @brief The completed incidents.
     */
    std::size_t completions{0};

    /**
     * @brief The incidents that spent the attempt budget.
     */
    std::size_t capped{0};

    /**
     * @brief The moves over all incidents.
     */
    std::size_t attempts_sum{0};

    /**
     * @brief The convergence point minus the corruption end, summed over completed incidents.
     */
    std::ptrdiff_t conv_sum{0};

    /**
     * @brief The incidents that ended refused, at the first move or a later one.
     */
    std::size_t terminal_refused{0};

    /**
     * @brief The completed incidents, the convergence figures' denominator.
     */
    std::size_t conv_count{0};

    /**
     * @brief The lost boundaries, summed over completed incidents.
     */
    std::size_t lost_sum{0};

    /**
     * @brief The spurious starts, summed over completed incidents.
     */
    std::size_t spurious_sum{0};

    /**
     * @brief The first answer minus the first true boundary, summed over the answers where one exists.
     */
    std::ptrdiff_t overshoot_sum{0};

    /**
     * @brief The answers with a first true boundary, the overshoot's denominator.
     */
    std::size_t overshoot_count{0};

    /**
     * @brief The terminal positions strictly inside the input, the terminal landing's denominator.
     */
    std::size_t terminal_interior{0};
};

/**
 * @brief One row's tallies: one per operation, width and arm, pooled over seeds, and the first landing per seed and
 *        arm.
 */
struct Row_tallies
{
    /**
     * @brief The tallies, indexed by operation, width and arm in kOps, kWidths and kArms order.
     */
    std::array<std::array<std::array<Tally, kArms.size()>, kWidths.size()>, kOps.size()> cells{};

    /**
     * @brief The first answers per seed and arm.
     */
    std::vector<std::array<std::size_t, kArms.size()>> seed_answers;

    /**
     * @brief The first answers that landed, per seed and arm.
     */
    std::vector<std::array<std::size_t, kArms.size()>> seed_landings;
};

/**
 * @brief A row's tallies, every count zero.
 * @param seeds The seeds the row runs under.
 * @return The tallies, with a per-seed entry for each seed.
 */
[[nodiscard]] Row_tallies row_tallies(std::size_t seeds);

/**
 * @brief Adds one arm's incident of a damaging trial to the tally of its operation, width and arm, and its first
 *        answer to its seed's figures.
 * @param tallies The row's tallies.
 * @param trial The trial.
 * @param arm_index The arm's index in kArms.
 * @param incident The arm's incident.
 * @param score The incident's score.
 */
void tally_incident(
        Row_tallies& tallies, const Trial& trial, std::size_t arm_index, const Incident& incident, const Score& score);

/**
 * @brief Prints a row's stratified table: a header and one line per operation, width and arm with the answers,
 *        refusals, landing and completion rates, the capped count and the mean attempts, convergence, lost, spurious
 *        and overshoot figures.
 * @param tallies The row's tallies.
 */
void print_strata(const Row_tallies& tallies);

/**
 * @brief Prints a row's summary per arm, pooled over every operation, width and seed, with Wilson 95 percent intervals
 *        on first landing and completion.
 * @param tallies The row's tallies.
 */
void print_pooled(const Row_tallies& tallies);

/**
 * @brief Prints a row's first-landing rate per seed and arm, one line per seed.
 * @param tallies The row's tallies.
 */
void print_seeds(const Row_tallies& tallies);

/**
 * @brief Prints the campaign's closing summary: the absorbed trials, the covered and uncovered answers and moves, the
 *        nonminimal answers, the clean certified arm, the repairability strata, the exact arm's pairing and the
 *        duplicate positions.
 * @param totals The campaign's totals.
 */
void print_totals(const Campaign_totals& totals);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_REPORT_HPP

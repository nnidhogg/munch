#include "munch/tools/probes/recovery_report.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ranges>
#include <string>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief A confidence interval in percent.
 */
struct Interval
{
    /**
     * @brief The lower bound.
     */
    double low{0.0};

    /**
     * @brief The upper bound.
     */
    double high{0.0};
};

/**
 * @brief Returns a count as a percentage of a total.
 * @param hits The count.
 * @param total The total.
 * @return 100 hits / total, 0 for a zero total.
 */
double percent(const std::size_t hits, const std::size_t total)
{
    return total != 0 ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
}

/**
 * @brief Returns a sum's mean over a count.
 * @tparam Sum The sum's type.
 * @param sum The sum, signed or unsigned.
 * @param total The count.
 * @return sum / total, 0 for a zero count.
 */
template <typename Sum>
double mean(const Sum sum, const std::size_t total)
{
    return total != 0 ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
}

/**
 * @brief Returns the Wilson 95 percent score interval for successes out of a number of trials.
 * @param successes The successes.
 * @param trials The trials.
 * @return The lower and upper bounds in percent, both 0 for no trials.
 */
Interval wilson(const std::size_t successes, const std::size_t trials)
{
    if (trials == 0)
    {
        return Interval{.low = 0.0, .high = 0.0};
    }

    const auto total{static_cast<double>(trials)};

    const auto rate{static_cast<double>(successes) / total};

    constexpr double z_95{1.959963984540054};

    const auto denominator{1.0 + z_95 * z_95 / total};

    const auto center{rate + z_95 * z_95 / (2.0 * total)};

    const auto margin{z_95 * std::sqrt(rate * (1.0 - rate) / total + z_95 * z_95 / (4.0 * total * total))};

    const auto low{100.0 * (center - margin) / denominator};

    const auto high{100.0 * (center + margin) / denominator};

    return Interval{.low = low, .high = high};
}

/**
 * @brief Returns one arm's tallies pooled over every operation and width, in the fields the pooled summary prints.
 * @param tallies The row's tallies.
 * @param arm_index The arm's index in arms.
 * @return The pooled trials, answers, refusals, first landings, completions, capped and terminal refusals.
 */
Tally pooled_tally(const Row_tallies& tallies, const std::size_t arm_index)
{
    Tally pooled{};

    for (const auto& by_width : tallies.cells)
    {
        for (const auto& by_arm : by_width)
        {
            const auto& tally{by_arm[arm_index]};

            pooled.trials += tally.trials;

            pooled.answers += tally.answers;

            pooled.refusals += tally.refusals;

            pooled.first_landings += tally.first_landings;

            pooled.completions += tally.completions;

            pooled.capped += tally.capped;

            pooled.terminal_refused += tally.terminal_refused;
        }
    }

    return pooled;
}

} // namespace

Row_tallies row_tallies(const std::size_t seeds)
{
    Row_tallies tallies{};

    tallies.seed_answers.resize(seeds);

    tallies.seed_landings.resize(seeds);

    return tallies;
}

void tally_incident(
        Row_tallies& tallies, const Trial& trial, const std::size_t arm_index, const Incident& incident,
        const Score& score)
{
    const auto& cell{trial.cell};

    auto& tally{tallies.cells[cell.op_index][cell.width_index][arm_index]};

    ++tally.trials;

    tally.attempts_sum += incident.attempts;

    if (score.terminal_landed)
    {
        ++tally.terminal_interior;
    }

    if (incident.first)
    {
        ++tally.answers;

        ++tallies.seed_answers[cell.seed][arm_index];

        if (score.first_landed.value_or(false))
        {
            ++tally.first_landings;

            ++tallies.seed_landings[cell.seed][arm_index];
        }

        if (trial.first_true)
        {
            tally.overshoot_sum +=
                    static_cast<std::ptrdiff_t>(*incident.first) - static_cast<std::ptrdiff_t>(*trial.first_true);

            ++tally.overshoot_count;
        }
    }
    else
    {
        ++tally.refusals;
    }

    if (score.terminal_landed.value_or(false))
    {
        ++tally.terminal_landings;
    }

    switch (incident.outcome)
    {
    case Outcome::completed:
    {
        const auto& [at, lost, spurious]{*score.convergence};

        ++tally.completions;

        tally.convergence_sum += static_cast<std::ptrdiff_t>(at) - static_cast<std::ptrdiff_t>(trial.damaged.end);

        ++tally.convergence_count;

        tally.lost_sum += lost;

        tally.spurious_sum += spurious;

        break;
    }

    case Outcome::capped:
        ++tally.capped;

        break;

    case Outcome::refused:
        ++tally.terminal_refused;

        break;
    }
}

void print_strata(const Row_tallies& tallies)
{
    std::printf(
            "  %-11s %2s  %-15s %7s %7s %7s %7s %8s %8s %6s %8s %8s %6s %6s %9s\n", "op", "k", "strategy", "answers",
            "refuse", "t-ref", "f-land", "t-land", "complete", "capped", "attempts", "conv", "lost", "spur",
            "overshoot");

    const auto cells{std::views::cartesian_product(
            std::views::iota(std::size_t{0}, ops.size()), std::views::iota(std::size_t{0}, widths.size()),
            std::views::iota(std::size_t{0}, arms.size()))};

    for (const auto [op_index, width_index, arm_index] : cells)
    {
        const auto& tally{tallies.cells[op_index][width_index][arm_index]};

        const auto first_landing{percent(tally.first_landings, tally.answers)};

        const auto terminal_landing{percent(tally.terminal_landings, tally.terminal_interior)};

        const auto completion{percent(tally.completions, tally.trials)};

        const auto attempts{mean(tally.attempts_sum, tally.trials)};

        const auto convergence{mean(tally.convergence_sum, tally.convergence_count)};

        const auto lost{mean(tally.lost_sum, tally.convergence_count)};

        const auto spurious{mean(tally.spurious_sum, tally.convergence_count)};

        const auto overshoot{mean(tally.overshoot_sum, tally.overshoot_count)};

        std::printf(
                "  %-11s %2zu  %-15s %7zu %7zu %7zu %6.1f%% %7.1f%% %7.1f%% %6zu %8.2f %8.0f %6.2f "
                "%6.2f %9.1f\n",
                std::string{name(ops[op_index])}.c_str(), widths[width_index],
                std::string{arms[arm_index].name}.c_str(), tally.answers, tally.refusals, tally.terminal_refused,
                first_landing, terminal_landing, completion, tally.capped, attempts, convergence, lost, spurious,
                overshoot);
    }
}

void print_pooled(const Row_tallies& tallies)
{
    std::printf("\n  pooled over all cells and seeds, Wilson 95%% intervals\n");

    for (const auto& [arm_position, arm] : std::views::enumerate(arms))
    {
        const auto arm_index{static_cast<std::size_t>(arm_position)};

        const auto pooled{pooled_tally(tallies, arm_index)};

        const auto [land_low, land_high]{wilson(pooled.first_landings, pooled.answers)};

        const auto [complete_low, complete_high]{wilson(pooled.completions, pooled.trials)};

        std::printf(
                "  %-15s answers %6zu initial-refusals %5zu terminal-refused %5zu first-landing [%5.1f%%, "
                "%5.1f%%] completion [%5.1f%%, %5.1f%%] capped %zu\n",
                std::string{arm.name}.c_str(), pooled.answers, pooled.refusals, pooled.terminal_refused, land_low,
                land_high, complete_low, complete_high, pooled.capped);
    }
}

void print_seeds(const Row_tallies& tallies)
{
    const auto seeds{std::views::iota(std::size_t{0})};

    for (const auto& [seed, answers, landings] : std::views::zip(seeds, tallies.seed_answers, tallies.seed_landings))
    {
        std::printf("  seed %zu first-landing:", seed);

        for (const auto& [arm, answered, landed] : std::views::zip(arms, answers, landings))
        {
            const auto landing{percent(landed, answered)};

            std::printf(" %s %.1f%%", std::string{arm.name}.c_str(), landing);
        }

        std::printf("\n");
    }
}

void print_totals(const Campaign_totals& totals)
{
    std::printf("\ndamage absorbed by the grammar without a scan failure: %zu trials\n", totals.absorbed_total);

    std::printf(
            "evidence-covered first answers: %zu, all asserted to land; evidence-uncovered: %zu, of which %zu "
            "landed; certified moves in total: %zu, of which %zu covered, every covered move asserted to land\n",
            totals.evidence_covered, totals.evidence_uncovered, totals.evidence_uncovered_landed,
            totals.certified_moves_total, totals.certified_moves_covered);

    std::printf(
            "nonminimal answers: %zu, %zu extra bytes in total\n", totals.nonminimal_answers, totals.nonminimal_bytes);

    std::printf(
            "known-clean certified arm: %zu answers, every one asserted covered and landed; %zu refusals\n",
            totals.clean_answers, totals.clean_refusals);

    std::printf(
            "repairability at the blind anchor: %zu repairable, %zu unrepairable; the walk answered %zu of the "
            "unrepairable, the vacuous share its stratification labels\n",
            totals.repairable_total, totals.unrepairable_total, totals.vacuous_walk_answers);

    std::printf(
            "exact anchored arm: %zu answers, the decider asserted at or before the walk on every repairable "
            "trial where the walk answered and refusing every unrepairable one before the anchor advances; "
            "%zu paired answers, %zu bytes saved on repairable trials, net displacement %td bytes over all pairs\n",
            totals.exact_answers_total, totals.exact_pairs, totals.exact_saved_bytes, totals.exact_net_displacement);

    std::printf("duplicate sampled positions across all cells: %zu\n", totals.duplicate_positions);
}

} // namespace munch::tools::probes

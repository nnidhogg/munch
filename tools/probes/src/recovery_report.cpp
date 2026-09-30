#include "munch/tools/probes/recovery_report.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements recovery_report.hpp: the rates, the means and the Wilson interval are private to this unit.

/**
 * @brief A count as a percentage of a total.
 * @param hits The count.
 * @param total The total.
 * @return 100 hits / total, 0 for a zero total.
 */
double percent(const std::size_t hits, const std::size_t total)
{
    return total ? 100.0 * static_cast<double>(hits) / static_cast<double>(total) : 0.0;
}

/**
 * @brief A sum's mean over a count.
 * @param sum The sum, signed or unsigned.
 * @param total The count.
 * @return sum / total, 0 for a zero count.
 */
template <typename Sum_t>
double mean(const Sum_t sum, const std::size_t total)
{
    return total ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
}

/**
 * @brief The Wilson 95 percent score interval for successes out of n.
 * @param successes The successes.
 * @param n The trials.
 * @return The lower and upper bounds in percent, both 0 for no trials.
 */
std::pair<double, double> wilson(const std::size_t successes, const std::size_t n)
{
    if (n == 0)
    {
        return {0.0, 0.0};
    }

    const auto z{1.959963984540054};

    const auto total{static_cast<double>(n)};

    const auto rate{static_cast<double>(successes) / total};

    const auto denominator{1.0 + z * z / total};

    const auto center{rate + z * z / (2.0 * total)};

    const auto margin{z * std::sqrt(rate * (1.0 - rate) / total + z * z / (4.0 * total * total))};

    return {100.0 * (center - margin) / denominator, 100.0 * (center + margin) / denominator};
}

/**
 * @brief One arm's tallies pooled over every operation and width, in the fields the pooled summary prints.
 * @param tallies The row's tallies.
 * @param arm_index The arm's index in kArms.
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

    auto& tally{tallies.cells[cell.op_index][cell.k_index][arm_index]};

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

    if (incident.outcome == Outcome::Completed)
    {
        ++tally.completions;

        tally.conv_sum +=
                static_cast<std::ptrdiff_t>(score.convergence->at) - static_cast<std::ptrdiff_t>(trial.damaged.end);

        ++tally.conv_count;

        tally.lost_sum += score.convergence->lost;

        tally.spurious_sum += score.convergence->spurious;
    }
    else if (incident.outcome == Outcome::Capped)
    {
        ++tally.capped;
    }
    else
    {
        ++tally.terminal_refused;
    }
}

void print_strata(const Row_tallies& tallies)
{
    std::printf(
            "  %-11s %2s  %-15s %7s %7s %7s %7s %8s %8s %6s %8s %8s %6s %6s %9s\n", "op", "k", "strategy", "answers",
            "refuse", "t-ref", "f-land", "t-land", "complete", "capped", "attempts", "conv", "lost", "spur",
            "overshoot");

    for (std::size_t op_index{0}; op_index < kOps.size(); ++op_index)
    {
        for (std::size_t k_index{0}; k_index < kWidths.size(); ++k_index)
        {
            for (std::size_t s_index{0}; s_index < kArms.size(); ++s_index)
            {
                const auto& tally{tallies.cells[op_index][k_index][s_index]};

                std::printf(
                        "  %-11s %2zu  %-15s %7zu %7zu %7zu %6.1f%% %7.1f%% %7.1f%% %6zu %8.2f %8.0f %6.2f "
                        "%6.2f %9.1f\n",
                        std::string{name(kOps[op_index])}.c_str(), kWidths[k_index],
                        std::string{kArms[s_index].name}.c_str(), tally.answers, tally.refusals, tally.terminal_refused,
                        percent(tally.first_landings, tally.answers),
                        percent(tally.terminal_landings, tally.terminal_interior),
                        percent(tally.completions, tally.trials), tally.capped, mean(tally.attempts_sum, tally.trials),
                        mean(tally.conv_sum, tally.conv_count), mean(tally.lost_sum, tally.conv_count),
                        mean(tally.spurious_sum, tally.conv_count), mean(tally.overshoot_sum, tally.overshoot_count));
            }
        }
    }
}

void print_pooled(const Row_tallies& tallies)
{
    std::printf("\n  pooled over all cells and seeds, Wilson 95%% intervals\n");

    for (std::size_t s_index{0}; s_index < kArms.size(); ++s_index)
    {
        const auto pooled{pooled_tally(tallies, s_index)};

        const auto [land_low, land_high]{wilson(pooled.first_landings, pooled.answers)};

        const auto [complete_low, complete_high]{wilson(pooled.completions, pooled.trials)};

        std::printf(
                "  %-15s answers %6zu initial-refusals %5zu terminal-refused %5zu first-landing [%5.1f%%, "
                "%5.1f%%] completion [%5.1f%%, %5.1f%%] capped %zu\n",
                std::string{kArms[s_index].name}.c_str(), pooled.answers, pooled.refusals, pooled.terminal_refused,
                land_low, land_high, complete_low, complete_high, pooled.capped);
    }
}

void print_seeds(const Row_tallies& tallies)
{
    for (std::size_t seed{0}; seed < tallies.seed_answers.size(); ++seed)
    {
        std::printf("  seed %zu first-landing:", seed);

        for (std::size_t s_index{0}; s_index < kArms.size(); ++s_index)
        {
            std::printf(
                    " %s %.1f%%", std::string{kArms[s_index].name}.c_str(),
                    percent(tallies.seed_landings[seed][s_index], tallies.seed_answers[seed][s_index]));
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
            "trial where the walk answered and refusing every unrepairable one before the anchor advances; %zu paired "
            "answers, %zu bytes "
            "saved on repairable trials, net displacement %td bytes over all pairs\n",
            totals.exact_answers_total, totals.exact_pairs, totals.exact_saved_bytes, totals.exact_net_displacement);

    std::printf("duplicate sampled positions across all cells: %zu\n", totals.duplicate_positions);
}

} // namespace munch::tools::probes

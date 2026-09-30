#include "munch/tools/probes/recovery_checks.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements recovery_checks.hpp: one function per assertion family is private to this unit.

/**
 * @brief The first field in which the replica walk and the library's evidence differ, with both values.
 */
struct Mismatch
{
    /**
     * @brief The field's name: `existence`, `start`, `evidence_begin`, `evidence_end` or `class`.
     */
    std::string_view field;

    /**
     * @brief The library's value, -1 for a refusal.
     */
    std::ptrdiff_t library{-1};

    /**
     * @brief The replica's value, -1 for a refusal.
     */
    std::ptrdiff_t replica{-1};
};

/**
 * @brief Compares the certified arm's first evidence with the replica walk from one past the failure: first whether
 *        either found a certificate, then the answer, both evidence ends and the byte-or-window class.
 * @param trial The trial.
 * @return The first field that differs, std::nullopt when they agree.
 */
std::optional<Mismatch> replica_mismatch(const Trial& trial)
{
    const auto replica{evidence_of(trial.cell.row.lexer, trial.damaged.input, trial.failure + 1)};

    const auto& answer{trial.incidents[kCertifiedArm].evidence};

    // The replica in the library's terms: the answer it yields, and one past its evidence.
    const auto replica_start{replica ? replica->begin + (replica->byte ? 0 : replica->origin) : 0};

    const auto replica_end{replica ? replica->begin + replica->length : 0};

    if (replica.has_value() != answer.has_value())
    {
        return Mismatch{
                .field = "existence",
                .library = answer ? static_cast<std::ptrdiff_t>(answer->start) : -1,
                .replica = replica ? static_cast<std::ptrdiff_t>(replica_start) : -1};
    }

    if (!answer)
    {
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 4> names{"start", "evidence_begin", "evidence_end", "class"};

    const std::array<std::size_t, 4> library_fields{
            answer->start, answer->evidence_begin, answer->evidence_end, static_cast<std::size_t>(answer->window)};

    const std::array<std::size_t, 4> replica_fields{
            replica_start, replica->begin, replica_end, static_cast<std::size_t>(!replica->byte)};

    for (std::size_t f{0}; f < names.size(); ++f)
    {
        if (library_fields[f] != replica_fields[f])
        {
            return Mismatch{
                    .field = names[f],
                    .library = static_cast<std::ptrdiff_t>(library_fields[f]),
                    .replica = static_cast<std::ptrdiff_t>(replica_fields[f])};
        }
    }

    return std::nullopt;
}

/**
 * @brief Counts the trial as repairable or not and executes the repair witness: the repair prepended to the blind
 *        tail must scan to the end of input, or `REPAIR WITNESS VIOLATION` is reported.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_repair_witness(const Trial& trial, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    if (!trial.repair)
    {
        ++totals.unrepairable_total;

        return;
    }

    ++totals.repairable_total;

    const auto& input{trial.damaged.input};

    const auto witness{*trial.repair + input.substr(std::min(trial.failure + 1, input.size()))};

    if (failure_offset(row.lexer, witness) != witness.size())
    {
        std::fprintf(
                stderr, "REPAIR WITNESS VIOLATION: %s %s k=%zu p=%zu e=%zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure);

        ++totals.theorem_failures;
    }
}

/**
 * @brief Reports `EVIDENCE MISMATCH` naming the differing field when the replica walk and the certified arm's first
 *        evidence differ.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_replica(const Trial& trial, Campaign_totals& totals)
{
    const auto mismatch{replica_mismatch(trial)};

    if (!mismatch)
    {
        return;
    }

    const auto& row{trial.cell.row};

    std::fprintf(
            stderr, "EVIDENCE MISMATCH: %s %s k=%zu p=%zu e=%zu field %s library %td replica %td\n",
            std::string{row.label}.c_str(), std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position,
            trial.failure, std::string{mismatch->field}.c_str(), mismatch->library, mismatch->replica);

    ++totals.theorem_failures;
}

/**
 * @brief The transfer over the certified arm's first answer: an answer whose evidence begins at or past the
 *        corruption end must land (`EVIDENCE VIOLATION`), an answer at or past the corruption end plus three must land
 *        (`THEOREM VIOLATION`), and the covered, uncovered and nonminimal answers are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_first_transfer(const Trial& trial, Campaign_totals& totals)
{
    const auto& incident{trial.incidents[kCertifiedArm]};

    if (!incident.first)
    {
        return;
    }

    const auto& row{trial.cell.row};

    const auto& y{trial.damaged};

    const auto landed_first{is_landed(row.begins, y, *incident.first)};

    if (incident.evidence && incident.evidence->evidence_begin >= y.end)
    {
        ++totals.evidence_covered;

        if (!landed_first)
        {
            std::fprintf(
                    stderr, "EVIDENCE VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n",
                    std::string{row.label}.c_str(), std::string{name(trial.cell.op)}.c_str(), trial.cell.k,
                    trial.position, trial.failure, *incident.first);

            ++totals.theorem_failures;
        }
    }
    else
    {
        ++totals.evidence_uncovered;

        if (landed_first)
        {
            ++totals.evidence_uncovered_landed;
        }
    }

    if (*incident.first >= y.end + 3 && !landed_first)
    {
        std::fprintf(
                stderr, "THEOREM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure, *incident.first);

        ++totals.theorem_failures;
    }

    const auto minimal{minimal_answer(row.lexer, y.input, trial.failure + 1, *incident.first)};

    if (minimal < *incident.first)
    {
        ++totals.nonminimal_answers;

        totals.nonminimal_bytes += *incident.first - minimal;
    }
}

/**
 * @brief The transfer over every move of one certified arm: a move inside the input whose evidence begins at or past
 *        the corruption end must land (`MOVE EVIDENCE VIOLATION`); the certified arm's moves are counted.
 * @param trial The trial.
 * @param arm_index The arm, kCertifiedArm or kCertifiedCleanArm.
 * @param totals The campaign's totals.
 */
void check_moves(const Trial& trial, const std::size_t arm_index, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    const auto& y{trial.damaged};

    for (const auto& move : trial.incidents[arm_index].moves)
    {
        const auto move_at{move[0]};

        const auto move_evidence{move[1]};

        if (arm_index == kCertifiedArm)
        {
            ++totals.certified_moves_total;
        }

        if (move_evidence < y.end)
        {
            continue;
        }

        if (arm_index == kCertifiedArm)
        {
            ++totals.certified_moves_covered;
        }

        if (move_at < y.input.size() && !is_landed(row.begins, y, move_at))
        {
            std::fprintf(
                    stderr, "MOVE EVIDENCE VIOLATION: %s %s k=%zu p=%zu e=%zu at %zu\n", std::string{row.label}.c_str(),
                    std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure, move_at);

            ++totals.theorem_failures;
        }
    }
}

/**
 * @brief The clean certified arm's contract: every first answer rests on evidence at or past the corruption end and
 *        lands (`CLEAN-ARM VIOLATION`); its answers and refusals are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_clean(const Trial& trial, Campaign_totals& totals)
{
    const auto& incident{trial.incidents[kCertifiedCleanArm]};

    if (!incident.first)
    {
        ++totals.clean_refusals;

        return;
    }

    ++totals.clean_answers;

    const auto& row{trial.cell.row};

    const auto& y{trial.damaged};

    if (!incident.evidence || incident.evidence->evidence_begin < y.end || !is_landed(row.begins, y, *incident.first))
    {
        std::fprintf(
                stderr, "CLEAN-ARM VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure, *incident.first);

        ++totals.theorem_failures;
    }
}

/**
 * @brief The decider's consistency with the walk at the blind anchor: on a repairable trial a walk answer implies a
 *        direct answer at or before it (`EXACT ORDER VIOLATION`), and on an unrepairable trial the direct call refuses
 *        (`EXACT REFUSAL VIOLATION`); walk answers on unrepairable trials are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_consistency(const Trial& trial, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    const auto& first{trial.incidents[kCertifiedArm].first};

    if (trial.repair && first && (!trial.direct || *trial.direct > *first))
    {
        std::fprintf(
                stderr, "EXACT ORDER VIOLATION: %s %s k=%zu p=%zu e=%zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure);

        ++totals.theorem_failures;
    }

    if (!trial.repair && trial.direct)
    {
        std::fprintf(
                stderr, "EXACT REFUSAL VIOLATION: %s %s k=%zu p=%zu e=%zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure);

        ++totals.theorem_failures;
    }

    if (!trial.repair && first)
    {
        ++totals.vacuous_walk_answers;
    }
}

/**
 * @brief Counts the exact arm's answers and, where the certified arm answered too, the pair: the bytes the direct
 *        answer saves on a repairable trial and the signed displacement between the two arms' first answers.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void count_exact_pairs(const Trial& trial, Campaign_totals& totals)
{
    const auto& exact{trial.incidents[kExactArm].first};

    const auto& walk{trial.incidents[kCertifiedArm].first};

    if (!exact)
    {
        return;
    }

    ++totals.exact_answers_total;

    if (!walk)
    {
        return;
    }

    ++totals.exact_pairs;

    if (trial.repair && trial.direct)
    {
        totals.exact_saved_bytes += *walk - *trial.direct;
    }

    totals.exact_net_displacement += static_cast<std::ptrdiff_t>(*walk) - static_cast<std::ptrdiff_t>(*exact);
}

/**
 * @brief The clean exact arm's first answer inside the input lands (`EXACT-CLEAN VIOLATION`): the pristine prefix is a
 *        repair of what precedes the preserved suffix.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_exact_clean(const Trial& trial, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    const auto& first{trial.incidents[kExactCleanArm].first};

    const auto& y{trial.damaged};

    if (first && *first < y.input.size() && !is_landed(row.begins, y, *first))
    {
        std::fprintf(
                stderr, "EXACT-CLEAN VIOLATION: %s %s k=%zu p=%zu e=%zu answered %zu\n", std::string{row.label}.c_str(),
                std::string{name(trial.cell.op)}.c_str(), trial.cell.k, trial.position, trial.failure, *first);

        ++totals.theorem_failures;
    }
}

/**
 * @brief Two arms placing the same delimiter, one past it and one at it.
 */
struct Placement_pair
{
    /**
     * @brief The index in kArms of the arm resuming one past the delimiter.
     */
    std::size_t past;

    /**
     * @brief The index in kArms of the arm resuming at it.
     */
    std::size_t at;

    /**
     * @brief The delimiter.
     */
    char delimiter;
};

/**
 * @brief The delimiter conventions: the two placements of each delimiter answer together, and the past answer is the
 *        at answer plus one over the delimiter itself (`CONVENTION VIOLATION`).
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_conventions(const Trial& trial, Campaign_totals& totals)
{
    constexpr std::array<Placement_pair, 2> pairs{
            Placement_pair{.past = 5, .at = 6, .delimiter = '\n'},
            Placement_pair{.past = 7, .at = 8, .delimiter = ';'}};

    for (const auto& [past, at, delimiter] : pairs)
    {
        const auto& past_first{trial.incidents[past].first};

        const auto& at_first{trial.incidents[at].first};

        if (past_first.has_value() != at_first.has_value() ||
            (past_first && (*past_first != *at_first + 1 || trial.damaged.input[*at_first] != delimiter)))
        {
            std::fprintf(stderr, "CONVENTION VIOLATION\n");

            ++totals.theorem_failures;
        }
    }
}

} // namespace

void check_trial(const Trial& trial, Campaign_totals& totals)
{
    check_repair_witness(trial, totals);

    check_replica(trial, totals);

    check_first_transfer(trial, totals);

    check_moves(trial, kCertifiedArm, totals);

    check_moves(trial, kCertifiedCleanArm, totals);

    check_clean(trial, totals);

    check_consistency(trial, totals);

    count_exact_pairs(trial, totals);

    check_exact_clean(trial, totals);

    check_conventions(trial, totals);
}

} // namespace munch::tools::probes

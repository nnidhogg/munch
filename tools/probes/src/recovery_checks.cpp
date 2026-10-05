#include "munch/tools/probes/recovery_checks.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The width of the seam band [end, end + seam_band) past the corruption end, where a certified answer may rest
 *        on an occurrence straddling the seam: the longest window is four bytes with origin at most three.
 */
constexpr std::size_t seam_band{3};

/**
 * @brief The first field in which the replica walk and the library's evidence differ, with both values.
 */
struct Mismatch
{
    /**
     * @brief The field's name: `existence`, `start`, `evidence_begin`, `evidence_end` or `class`.
     */
    std::string_view field{};

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
 * @brief Two arms placing the same delimiter, one past it and one at it.
 */
struct Placement_pair
{
    /**
     * @brief The index in arms of the arm resuming one past the delimiter.
     */
    std::size_t past{0};

    /**
     * @brief The index in arms of the arm resuming at it.
     */
    std::size_t at{0};

    /**
     * @brief The delimiter.
     */
    char delimiter{'\0'};
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

    const auto& answer{trial.incidents[certified_arm].evidence};

    // The replica in the library's terms: the answer it yields, and one past its evidence.
    const auto origin_offset{replica && !replica->byte ? replica->origin : 0};

    const auto replica_start{replica ? replica->begin + origin_offset : 0};

    const auto replica_end{replica ? replica->begin + replica->length : 0};

    if (replica.has_value() != answer.has_value())
    {
        const auto library{answer ? static_cast<std::ptrdiff_t>(answer->start) : -1};

        const auto replica_answer{replica ? static_cast<std::ptrdiff_t>(replica_start) : -1};

        return Mismatch{.field = "existence", .library = library, .replica = replica_answer};
    }

    if (!answer)
    {
        return std::nullopt;
    }

    const std::array library_fields{
            answer->start, answer->evidence_begin, answer->evidence_end, static_cast<std::size_t>(answer->window)};

    const std::array replica_fields{
            replica_start, replica->begin, replica_end, static_cast<std::size_t>(!replica->byte)};

    constexpr std::array<std::string_view, 4> names{"start", "evidence_begin", "evidence_end", "class"};

    for (const auto& [field, library_value, replica_value] : std::views::zip(names, library_fields, replica_fields))
    {
        if (library_value != replica_value)
        {
            return Mismatch{
                    .field = field,
                    .library = static_cast<std::ptrdiff_t>(library_value),
                    .replica = static_cast<std::ptrdiff_t>(replica_value)};
        }
    }

    return std::nullopt;
}

/**
 * @brief Reports one violation on standard error, a heading, the trial's row, operation, width, damage position and
 *        failure offset, then a detail, and counts it in theorem_failures.
 * @param trial The trial.
 * @param heading The violation's name, such as `theorem violation`.
 * @param detail The text after the failure offset, empty or led by a space.
 * @param totals The campaign's totals.
 */
void report_violation(
        const Trial& trial, const std::string_view heading, const std::string_view detail, Campaign_totals& totals)
{
    const auto& [row, row_index, op, op_index, width, width_index, seed]{trial.cell};

    std::fprintf(
            stderr, "%s: %s %s k=%zu p=%zu e=%zu%s\n", std::string{heading}.c_str(), std::string{row.label}.c_str(),
            std::string{name(op)}.c_str(), width, trial.position, trial.failure, std::string{detail}.c_str());

    ++totals.theorem_failures;
}

/**
 * @brief Formats the answered position a violation names.
 * @param answer The position.
 * @return The detail ` answered ` and the position.
 */
std::string answered(const std::size_t answer)
{
    return std::format(" answered {}", answer);
}

/**
 * @brief Counts the trial as repairable or not and executes the repair witness: the repair prepended to the blind tail
 *        must scan to the end of input, or `repair witness violation` is reported.
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

    const auto anchor{std::min(trial.failure + 1, input.size())};

    const auto blind_tail{input.substr(anchor)};

    const auto witness{*trial.repair + blind_tail};

    const auto consumed{failure_offset(row.lexer, witness)};

    if (consumed != witness.size())
    {
        report_violation(trial, "repair witness violation", "", totals);
    }
}

/**
 * @brief Reports `evidence mismatch` naming the differing field when the replica walk and the certified arm's first
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

    const auto& [field, library, replica]{*mismatch};

    const auto detail{std::format(" field {} library {} replica {}", field, library, replica)};

    report_violation(trial, "evidence mismatch", detail, totals);
}

/**
 * @brief Checks the transfer over the certified arm's first answer: an answer whose evidence begins at or past the
 *        corruption end must land (`evidence violation`), an answer at or past the corruption end plus three must land
 *        (`theorem violation`), and the covered, uncovered and nonminimal answers are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_first_transfer(const Trial& trial, Campaign_totals& totals)
{
    const auto& incident{trial.incidents[certified_arm]};

    if (!incident.first)
    {
        return;
    }

    const auto& row{trial.cell.row};

    const auto& damaged{trial.damaged};

    const auto landed_first{is_landed(row.begins, damaged, *incident.first)};

    if (incident.evidence && incident.evidence->evidence_begin >= damaged.end)
    {
        ++totals.evidence_covered;

        if (!landed_first)
        {
            report_violation(trial, "evidence violation", answered(*incident.first), totals);
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

    if (*incident.first >= damaged.end + seam_band && !landed_first)
    {
        report_violation(trial, "theorem violation", answered(*incident.first), totals);
    }

    const auto minimal{minimal_answer(row.lexer, damaged.input, trial.failure + 1, *incident.first)};

    if (minimal < *incident.first)
    {
        ++totals.nonminimal_answers;

        totals.nonminimal_bytes += *incident.first - minimal;
    }
}

/**
 * @brief Checks the transfer over every move of one certified arm: a move inside the input whose evidence begins at or
 *        past the corruption end must land (`move evidence violation`); the certified arm's moves are counted.
 * @param trial The trial.
 * @param arm_index The arm, certified_arm or certified_clean_arm.
 * @param totals The campaign's totals.
 */
void check_moves(const Trial& trial, const std::size_t arm_index, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    const auto& damaged{trial.damaged};

    const auto& moves{trial.incidents[arm_index].moves};

    const auto counted{arm_index == certified_arm};

    if (counted)
    {
        totals.certified_moves_total += moves.size();
    }

    for (const auto& [move_at, move_evidence, move_end] : moves)
    {
        if (move_evidence < damaged.end)
        {
            continue;
        }

        if (counted)
        {
            ++totals.certified_moves_covered;
        }

        if (move_at < damaged.input.size() && !is_landed(row.begins, damaged, move_at))
        {
            const auto detail{std::format(" at {}", move_at)};

            report_violation(trial, "move evidence violation", detail, totals);
        }
    }
}

/**
 * @brief Checks the clean certified arm's contract: every first answer rests on evidence at or past the corruption end
 *        and lands (`clean-arm violation`); its answers and refusals are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_clean(const Trial& trial, Campaign_totals& totals)
{
    const auto& incident{trial.incidents[certified_clean_arm]};

    if (!incident.first)
    {
        ++totals.clean_refusals;

        return;
    }

    ++totals.clean_answers;

    const auto& row{trial.cell.row};

    const auto& damaged{trial.damaged};

    if (!incident.evidence || incident.evidence->evidence_begin < damaged.end ||
        !is_landed(row.begins, damaged, *incident.first))
    {
        report_violation(trial, "clean-arm violation", answered(*incident.first), totals);
    }
}

/**
 * @brief Checks the decider's consistency with the walk at the blind anchor: on a repairable trial a walk answer
 *        implies a direct answer at or before it (`exact order violation`), and on an unrepairable trial the direct
 *        call refuses (`exact refusal violation`); walk answers on unrepairable trials are counted.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_consistency(const Trial& trial, Campaign_totals& totals)
{
    const auto& first{trial.incidents[certified_arm].first};

    if (trial.repair && first && (!trial.direct || *trial.direct > *first))
    {
        report_violation(trial, "exact order violation", "", totals);
    }

    if (!trial.repair && trial.direct)
    {
        report_violation(trial, "exact refusal violation", "", totals);
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
    const auto& exact{trial.incidents[exact_arm].first};

    const auto& walk{trial.incidents[certified_arm].first};

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
 * @brief Checks the clean exact arm's first answer inside the input lands (`exact-clean violation`): the pristine
 *        prefix is a repair of what precedes the preserved suffix.
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_exact_clean(const Trial& trial, Campaign_totals& totals)
{
    const auto& row{trial.cell.row};

    const auto& first{trial.incidents[exact_clean_arm].first};

    const auto& damaged{trial.damaged};

    if (first && *first < damaged.input.size() && !is_landed(row.begins, damaged, *first))
    {
        report_violation(trial, "exact-clean violation", answered(*first), totals);
    }
}

/**
 * @brief Checks the delimiter conventions: the two placements of each delimiter answer together, and the past answer is
 *        the at answer plus one over the delimiter itself (`convention violation`).
 * @param trial The trial.
 * @param totals The campaign's totals.
 */
void check_conventions(const Trial& trial, Campaign_totals& totals)
{
    constexpr std::array pairs{
            Placement_pair{.past = newline_arm, .at = newline_at_arm, .delimiter = '\n'},
            Placement_pair{.past = semicolon_arm, .at = semicolon_at_arm, .delimiter = ';'}};

    for (const auto& [past, at, delimiter] : pairs)
    {
        const auto& past_first{trial.incidents[past].first};

        const auto& at_first{trial.incidents[at].first};

        const auto answered_together{past_first.has_value() == at_first.has_value()};

        const auto one_past_delimiter{
                !past_first || !at_first ||
                (*past_first == *at_first + 1 && trial.damaged.input[*at_first] == delimiter)};

        if (!answered_together || !one_past_delimiter)
        {
            std::fprintf(stderr, "convention violation\n");

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

    check_moves(trial, certified_arm, totals);

    check_moves(trial, certified_clean_arm, totals);

    check_clean(trial, totals);

    check_consistency(trial, totals);

    count_exact_pairs(trial, totals);

    check_exact_clean(trial, totals);

    check_conventions(trial, totals);
}

} // namespace munch::tools::probes

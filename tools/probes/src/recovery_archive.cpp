#include "munch/tools/probes/recovery_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The certified moves of an incident whose evidence begins at or past the corruption end, and those among them
 *        inside the input that landed.
 */
struct Covered_moves
{
    /**
     * @brief The covered moves.
     */
    std::size_t covered{0};

    /**
     * @brief Those that landed.
     */
    std::size_t landed{0};
};

/**
 * @brief Writes a count, or nothing when it is absent, which leaves its field blank.
 * @param stream The archive's stream.
 * @param value The count.
 */
void write_count(std::FILE* stream, const std::optional<std::size_t>& value)
{
    if (value)
    {
        std::fprintf(stream, "%zu", *value);
    }
}

/**
 * @brief Writes a flag as 1 or 0, or nothing when it is absent, which leaves its field blank.
 * @param stream The archive's stream.
 * @param value The flag.
 */
void write_flag(std::FILE* stream, const std::optional<bool>& value)
{
    if (value)
    {
        std::fprintf(stream, "%d", *value ? 1 : 0);
    }
}

/**
 * @brief Writes every generated row's corpus to corpus_path(), each file closed and checked; the first that fails stops
 *        the writing after printing `corpus write failed: <path>` on standard error.
 * @param csv_path The archive's path.
 * @param rows The rows; those not generated are skipped.
 * @return True when every corpus was written.
 */
bool write_corpora(const std::string_view csv_path, const std::vector<Row>& rows)
{
    for (const auto& [label, lexer, corpus, begins, generated] : rows)
    {
        if (!generated)
        {
            continue;
        }

        const auto corpus_file{corpus_path(csv_path, label)};

        std::ofstream out{corpus_file, std::ios::binary};

        out.write(corpus.data(), static_cast<std::streamsize>(corpus.size()));

        out.close();

        if (!out)
        {
            std::fprintf(stderr, "corpus write failed: %s\n", corpus_file.c_str());

            return false;
        }
    }

    return true;
}

/**
 * @brief Writes the fields that name a cell, the grammar, the operation, the width and the seed, each followed by its
 *        comma.
 * @param stream The archive's stream.
 * @param cell The cell.
 */
void write_cell_fields(std::FILE* stream, const Cell& cell)
{
    std::fprintf(
            stream, "%s,%s,%zu,%zu,", std::string{cell.row.label}.c_str(), std::string{name(cell.op)}.c_str(),
            cell.width, cell.seed);
}

/**
 * @brief Writes the fields every arm's row of a trial shares, from the grammar to the direct decider answer, each
 *        followed by its comma.
 * @param stream The archive's stream.
 * @param trial The trial.
 */
void write_trial_fields(std::FILE* stream, const Trial& trial)
{
    write_cell_fields(stream, trial.cell);

    std::fprintf(stream, "%zu,%zu,%zu,%zu,", trial.index, trial.position, trial.failure, trial.damaged.end);

    write_count(stream, trial.first_true);

    std::fprintf(stream, ",%d,", trial.repair ? 1 : 0);

    const auto length_of{[](const std::string& repair) { return repair.size(); }};

    const auto repair_length{trial.repair.transform(length_of)};

    write_count(stream, repair_length);

    std::fprintf(stream, ",");

    write_count(stream, trial.direct);
}

/**
 * @brief Writes an arm's answer fields, from the strategy to the smallest answer, the answer's certified evidence and
 *        the smallest answer from the arm's search start left blank when it has none.
 * @param stream The archive's stream.
 * @param trial The trial.
 * @param arm The arm.
 * @param incident The arm's incident.
 * @param score The incident's score.
 */
void write_answer_fields(
        std::FILE* stream, const Trial& trial, const Arm& arm, const Incident& incident, const Score& score)
{
    std::fprintf(stream, ",%s,", std::string{arm.name}.c_str());

    write_count(stream, incident.first);

    std::fprintf(stream, ",");

    write_flag(stream, score.first_landed);

    if (!incident.evidence)
    {
        std::fprintf(stream, ",,,,");

        return;
    }

    const auto& [start, evidence_begin, evidence_end, window]{*incident.evidence};

    std::fprintf(stream, ",%zu,%zu,%s,", evidence_begin, evidence_end, window ? "window" : "byte");

    const auto& damaged{trial.damaged};

    const auto from{search_start(arm, trial.failure, damaged.end)};

    const auto smallest{minimal_answer(trial.cell.row.lexer, damaged.input, from, *incident.first)};

    std::fprintf(stream, "%zu", smallest);
}

/**
 * @brief Counts an incident's covered moves and those that landed.
 * @param trial The trial.
 * @param incident The incident.
 * @return The two counts.
 */
Covered_moves covered_moves(const Trial& trial, const Incident& incident)
{
    const auto& damaged{trial.damaged};

    Covered_moves counts{};

    for (const auto& [answer, evidence_begin, evidence_end] : incident.moves)
    {
        if (evidence_begin < damaged.end)
        {
            continue;
        }

        ++counts.covered;

        if (answer < damaged.input.size() && is_landed(trial.cell.row.begins, damaged, answer))
        {
            ++counts.landed;
        }
    }

    return counts;
}

/**
 * @brief Writes an arm's outcome fields, from the terminal position to the convergence, ending the row.
 * @param stream The archive's stream.
 * @param trial The trial.
 * @param incident The arm's incident.
 * @param score The incident's score.
 */
void write_outcome_fields(std::FILE* stream, const Trial& trial, const Incident& incident, const Score& score)
{
    std::fprintf(stream, ",");

    write_count(stream, incident.terminal);

    std::fprintf(stream, ",");

    write_flag(stream, score.terminal_landed);

    std::fprintf(stream, ",%s,%zu,", std::string{outcome_name(incident.outcome)}.c_str(), incident.attempts);

    if (incident.moves.empty())
    {
        std::fprintf(stream, ",,");
    }
    else
    {
        const auto [covered, landed]{covered_moves(trial, incident)};

        std::fprintf(stream, "%zu,%zu,", covered, landed);
    }

    if (!score.convergence)
    {
        std::fprintf(stream, ",,\n");

        return;
    }

    const auto& [at, lost, spurious]{*score.convergence};

    std::fprintf(stream, "%zu,%zu,%zu\n", at, lost, spurious);
}

} // namespace

std::string corpus_path(const std::string_view csv_path, const std::string_view label)
{
    std::string slug{label};

    const auto is_not_alphanumeric{[](const char byte) { return std::isalnum(static_cast<unsigned char>(byte)) == 0; }};

    std::ranges::replace_if(slug, is_not_alphanumeric, '-');

    return std::format("{}.corpus-{}.bin", csv_path, slug);
}

bool Archive::open(const std::string_view csv_path, const std::vector<Row>& rows)
{
    campaign_ = Output_file{std::filesystem::path{csv_path}};

    if (!campaign_.is_open())
    {
        std::fprintf(stderr, "cannot open archive %s\n", std::string{csv_path}.c_str());

        return false;
    }

    const auto moves_path{std::format("{}.moves.csv", csv_path)};

    moves_ = Output_file{std::filesystem::path{moves_path}};

    if (!moves_.is_open())
    {
        std::fprintf(stderr, "cannot open moves archive %s\n", moves_path.c_str());

        return false;
    }

    std::fprintf(moves_.stream(), "grammar,op,k,seed,trial,strategy,move,answer,evidence_begin,evidence_end\n");

    if (!write_corpora(csv_path, rows))
    {
        return false;
    }

    std::fprintf(
            campaign_.stream(),
            "grammar,op,k,seed,trial,p,failure_offset,corruption_end,first_true,repairable,minimal_repair,"
            "exact_at_anchor,strategy,first,first_landed,evidence_begin,evidence_end,evidence_kind,minimal,"
            "terminal,terminal_landed,outcome,attempts,moves_covered,moves_covered_landed,converged,lost,"
            "spurious\n");

    return true;
}

void Archive::absorbed_row(const Cell& cell, const std::size_t trial, const std::size_t position, const std::size_t end)
{
    if (!campaign_.is_open())
    {
        return;
    }

    write_cell_fields(campaign_.stream(), cell);

    std::fprintf(campaign_.stream(), "%zu,%zu,,%zu,,,,,absorbed,,,,,,,,,,,,,,,\n", trial, position, end);
}

void Archive::incident_row(const Trial& trial, const Arm& arm, const Incident& incident, const Score& score)
{
    if (!campaign_.is_open())
    {
        return;
    }

    write_trial_fields(campaign_.stream(), trial);

    write_answer_fields(campaign_.stream(), trial, arm, incident, score);

    write_outcome_fields(campaign_.stream(), trial, incident, score);
}

void Archive::move_rows(const Trial& trial, const Arm& arm, const Incident& incident)
{
    if (!moves_.is_open())
    {
        return;
    }

    for (const auto& [move_index, move] : std::views::enumerate(incident.moves))
    {
        const auto& [answer, evidence_begin, evidence_end]{move};

        const auto index{static_cast<std::size_t>(move_index)};

        write_cell_fields(moves_.stream(), trial.cell);

        std::fprintf(
                moves_.stream(), "%zu,%s,%zu,%zu,%zu,%zu\n", trial.index, std::string{arm.name}.c_str(), index, answer,
                evidence_begin, evidence_end);
    }
}

bool Archive::close()
{
    if (campaign_.is_open() && !campaign_.close())
    {
        std::fprintf(stderr, "csv write or close failed\n");

        return false;
    }

    if (moves_.is_open() && !moves_.close())
    {
        std::fprintf(stderr, "moves csv write or close failed\n");

        return false;
    }

    return true;
}

} // namespace munch::tools::probes

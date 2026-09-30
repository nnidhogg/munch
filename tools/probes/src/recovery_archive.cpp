#include "munch/tools/probes/recovery_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
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
// Implements recovery_archive.hpp: the corpus writing and the field groups of a row are private to this unit.

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
 * @brief Counts an incident's covered moves and those that landed.
 * @param trial The trial.
 * @param incident The incident.
 * @return The two counts.
 */
Covered_moves covered_moves(const Trial& trial, const Incident& incident)
{
    const auto& y{trial.damaged};

    Covered_moves counts{};

    for (const auto& move : incident.moves)
    {
        if (move[1] < y.end)
        {
            continue;
        }

        ++counts.covered;

        if (move[0] < y.input.size() && is_landed(trial.cell.row.begins, y, move[0]))
        {
            ++counts.landed;
        }
    }

    return counts;
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
    for (const auto& row : rows)
    {
        if (!row.generated)
        {
            continue;
        }

        const auto corpus_file{corpus_path(csv_path, row.label)};

        std::ofstream out{corpus_file, std::ios::binary};

        out.write(row.corpus.data(), static_cast<std::streamsize>(row.corpus.size()));
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
 * @brief Writes the fields every arm's row of a trial shares, from the grammar to the direct decider answer, each
 *        followed by its comma.
 * @param stream The archive's stream.
 * @param trial The trial.
 */
void write_trial_fields(std::FILE* stream, const Trial& trial)
{
    const auto& cell{trial.cell};

    std::fprintf(
            stream, "%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,", std::string{cell.row.label}.c_str(),
            std::string{name(cell.op)}.c_str(), cell.k, cell.seed, trial.index, trial.position, trial.failure,
            trial.damaged.end);

    write_count(stream, trial.first_true);

    std::fprintf(stream, ",%d,", trial.repair ? 1 : 0);

    write_count(stream, trial.repair ? std::optional{trial.repair->size()} : std::nullopt);

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

    std::fprintf(
            stream, ",%zu,%zu,%s,", incident.evidence->evidence_begin, incident.evidence->evidence_end,
            incident.evidence->window ? "window" : "byte");

    const auto& y{trial.damaged};

    const auto from{arm.clean ? std::max(y.end, trial.failure + 1) : trial.failure + 1};

    std::fprintf(stream, "%zu", minimal_answer(trial.cell.row.lexer, y.input, from, *incident.first));
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

    if (!incident.moves.empty())
    {
        const auto counts{covered_moves(trial, incident)};

        std::fprintf(stream, "%zu,%zu,", counts.covered, counts.landed);
    }
    else
    {
        std::fprintf(stream, ",,");
    }

    if (score.convergence)
    {
        std::fprintf(
                stream, "%zu,%zu,%zu\n", score.convergence->at, score.convergence->lost, score.convergence->spurious);
    }
    else
    {
        std::fprintf(stream, ",,\n");
    }
}

} // namespace

std::string corpus_path(const std::string_view csv_path, const std::string_view label)
{
    std::string slug{label};

    for (auto& byte : slug)
    {
        byte = static_cast<char>(std::isalnum(static_cast<unsigned char>(byte)) != 0 ? byte : '-');
    }

    return std::string{csv_path} + ".corpus-" + slug + ".bin";
}

bool Archive::open(const std::string_view csv_path, const std::vector<Row>& rows)
{
    campaign_ = Output_file{std::filesystem::path{csv_path}};

    if (!campaign_.is_open())
    {
        std::fprintf(stderr, "cannot open archive %s\n", std::string{csv_path}.c_str());

        return false;
    }

    const auto moves_path{std::string{csv_path} + ".moves.csv"};

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

    std::fprintf(
            campaign_.stream(), "%s,%s,%zu,%zu,%zu,%zu,,%zu,,,,,absorbed,,,,,,,,,,,,,,,\n",
            std::string{cell.row.label}.c_str(), std::string{name(cell.op)}.c_str(), cell.k, cell.seed, trial, position,
            end);
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

    const auto& cell{trial.cell};

    for (std::size_t move_index{0}; move_index < incident.moves.size(); ++move_index)
    {
        const auto& move{incident.moves[move_index]};

        std::fprintf(
                moves_.stream(), "%s,%s,%zu,%zu,%zu,%s,%zu,%zu,%zu,%zu\n", std::string{cell.row.label}.c_str(),
                std::string{name(cell.op)}.c_str(), cell.k, cell.seed, trial.index, std::string{arm.name}.c_str(),
                move_index, move[0], move[1], move[2]);
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

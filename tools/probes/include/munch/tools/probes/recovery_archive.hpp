#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARCHIVE_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARCHIVE_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_checks.hpp"
#include "munch/tools/probes/recovery_oracle.hpp"

/**
 * @brief The recovery campaign's archive, corpus_path and Archive: one CSV row per arm per damaging trial and one per
 *        absorbed trial, one sidecar row per certified move, and the generated corpora beside them.
 */
namespace munch::tools::probes
{
/**
 * @brief Returns the path a generated corpus is written to beside the archive, so two campaigns in one directory keep
 *        their corpora apart.
 * @param csv_path The archive's path.
 * @param label The row's label.
 * @return `<csv_path>.corpus-<slug>.bin`, the slug being the label with every byte that is not alphanumeric replaced by
 *         a dash.
 */
[[nodiscard]] std::string corpus_path(std::string_view csv_path, std::string_view label);

/**
 * @brief The campaign archive and its moves sidecar, opened together by open(); a row written to a file that is not
 *        open is dropped.
 */
class Archive
{
public:
    /**
     * @brief Opens the archive at a path and the sidecar at the path plus `.moves.csv`, writes the sidecar's header,
     *        every generated row's corpus to corpus_path(), and the archive's header, in that order. A file that cannot
     *        be opened or written stops the opening after printing `cannot open archive <path>`, `cannot open moves
     *        archive <path>` or `corpus write failed: <path>` on standard error.
     * @param csv_path The archive's path.
     * @param rows The rows; the generated ones have their corpora written.
     * @return True when both files are open and every corpus was written.
     */
    [[nodiscard]] bool open(std::string_view csv_path, const std::vector<Row>& rows);

    /**
     * @brief Writes the row of a trial whose damage the grammar absorbed: its cell, index, position and corruption end,
     *        the strategy `absorbed`, and every other field blank.
     * @param cell The trial's cell.
     * @param trial The trial's index within its cell.
     * @param position The damage position.
     * @param end The corruption end.
     */
    void absorbed_row(const Cell& cell, std::size_t trial, std::size_t position, std::size_t end);

    /**
     * @brief Writes one arm's row of a damaging trial: the trial's fields, the arm's first answer, its evidence and the
     *        smallest answer from the arm's search start, its terminal position, outcome and attempts, the covered
     *        moves and those among them that landed, and the convergence; a field that does not apply is blank.
     * @param trial The trial.
     * @param arm The arm.
     * @param incident The arm's incident.
     * @param score The incident's score.
     */
    void incident_row(const Trial& trial, const Arm& arm, const Incident& incident, const Score& score);

    /**
     * @brief Writes one sidecar row per certified move of an arm's incident: the trial's cell and index, the arm, the
     *        move's index, its answer and its evidence interval.
     * @param trial The trial.
     * @param arm The arm.
     * @param incident The arm's incident.
     */
    void move_rows(const Trial& trial, const Arm& arm, const Incident& incident);

    /**
     * @brief Closes the archive, then the sidecar, each checked for a failed write or close; the first failure stops
     *        the closing after printing `csv write or close failed` or `moves csv write or close failed` on standard
     *        error.
     * @return True when both closed cleanly or neither was open.
     */
    [[nodiscard]] bool close();

private:
    /**
     * @brief The archive, one row per arm per damaging trial and one per absorbed trial.
     */
    Output_file campaign_{};

    /**
     * @brief The sidecar, one row per certified move.
     */
    Output_file moves_{};
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ARCHIVE_HPP

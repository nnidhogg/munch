#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ORACLE_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ORACLE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_damage.hpp"

/**
 * @brief The recovery study's mapped pristine oracle, Row, Evidence, Convergence, Score, evidence_of, minimal_answer,
 *        pristine_oracle, converge and score_of: where an answer lands, what evidence a walk answer rests on, and where
 *        a resumed stream converges to the mapped pristine one.
 */
namespace munch::tools::probes
{
/**
 * @brief One grammar row of the study: its lexer, its pristine corpus and that corpus's boundaries.
 */
struct Row
{
    /**
     * @brief The row's label, as the tables and the archive print it.
     */
    std::string_view label{};

    /**
     * @brief The row's lexer.
     */
    core::Lexer lexer;

    /**
     * @brief The pristine corpus, completely tokenizable by the lexer.
     */
    std::string corpus{};

    /**
     * @brief The corpus's token starts, ascending.
     */
    std::vector<std::size_t> begins{};

    /**
     * @brief Whether the corpus was generated, and so is written beside the archive; a real document is not.
     */
    bool generated{true};
};

/**
 * @brief The evidence behind a walk answer, found by a replica of the certificate walk coded apart from the library's
 *        search: the certified byte, or the occurrence of the certified window.
 */
struct Evidence
{
    /**
     * @brief The certified byte's position or the window occurrence's start.
     */
    std::size_t begin{0};

    /**
     * @brief Whether the evidence is a certified byte rather than a window.
     */
    bool byte{false};

    /**
     * @brief The evidence's length: 1 for a byte, 2 to 4 for a window.
     */
    std::size_t length{0};

    /**
     * @brief The window's certified origin within the occurrence, 0 for a byte.
     */
    std::size_t origin{0};
};

/**
 * @brief Where a resumed boundary stream and the mapped pristine stream agree forever after, and what the region before
 *        it cost.
 */
struct Convergence
{
    /**
     * @brief The convergence point: the smallest emitted start from which the two suffixes coincide, the floor when
     *        they coincide down to it, the input's size when they share no suffix.
     */
    std::size_t at{0};

    /**
     * @brief The mapped pristine boundaries from the corruption end to the convergence point, never recovered.
     */
    std::size_t lost{0};

    /**
     * @brief The emitted starts from the corruption end to the convergence point that land on no mapped boundary.
     */
    std::size_t spurious{0};
};

/**
 * @brief One incident scored against the mapped pristine oracle.
 */
struct Score
{
    /**
     * @brief Whether the first answer landed, for a first answer inside the input.
     */
    std::optional<bool> first_landed{};

    /**
     * @brief Whether the terminal position landed, for a terminal position inside the input.
     */
    std::optional<bool> terminal_landed{};

    /**
     * @brief Where the resumed stream converged, for a completed incident.
     */
    std::optional<Convergence> convergence{};
};

/**
 * @brief Walks the input from an offset for the first certificate in evidence order: at each position a certified byte,
 *        then a certified window of length 2 to 4 starting there, each decided by the library's split-point and
 *        split-window predicates.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param from The offset the walk starts at.
 * @return The first certificate's evidence, std::nullopt when none occurs.
 */
[[nodiscard]] std::optional<Evidence> evidence_of(const core::Lexer& lexer, std::string_view input, std::size_t from);

/**
 * @brief Returns the smallest answer any certificate at or after an offset yields, for the nonminimality figure;
 *        evidence beginning past the walk's answer cannot yield a smaller one, so the scan stops there.
 * @param lexer The row's lexer.
 * @param input The damaged input.
 * @param from The offset the scan starts at.
 * @param answer The walk's answer.
 * @return The smallest certified position found, the answer when none is smaller.
 */
[[nodiscard]] std::size_t minimal_answer(
        const core::Lexer& lexer, std::string_view input, std::size_t from, std::size_t answer);

/**
 * @brief Runs the hard oracle on a pristine corpus: from offsets sampled by unbiased rejection sampling over [0, size -
 *        2] with the stream seeded 0x5EED0003, every core::Lexer::next_certified_start() answer must be a boundary at
 *        or past its offset. Each violation prints `pristine oracle violation: <row> from <offset> answered <answer>`
 *        on standard error.
 * @param row The row.
 * @param samples The offsets sampled.
 * @return The violations.
 */
[[nodiscard]] std::size_t pristine_oracle(const Row& row, std::size_t samples);

/**
 * @brief Finds where a resumed stream converges: the emitted starts and the mapped pristine boundaries at or above the
 *        floor are walked backward from their ends, in step, to their first disagreement. A convergence at or before
 *        the corruption end with a lost or spurious start ends the program with exit status one after printing
 *        `convergence region violation` on standard error.
 *
 * Both counts range over the divergence region, from the corruption end to the convergence point, so the initial jump's
 * skipped boundaries count as lost and emitted starts before the corruption end never count.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param starts The emitted starts, ascending.
 * @param floor The first resume position, below which emitted starts are not matched.
 * @return The convergence point and the lost and spurious counts.
 */
[[nodiscard]] Convergence converge(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::vector<std::size_t>& starts,
        std::size_t floor);

/**
 * @brief Scores an incident: its first and terminal positions against the oracle, and for a completed incident its
 *        convergence from the first answer, or at the input's size with nothing lost when it had no answer.
 * @param row The row.
 * @param damaged The damaged input and its coordinate map.
 * @param incident The incident.
 * @return The score.
 */
[[nodiscard]] Score score_of(const Row& row, const Damage& damaged, const Incident& incident);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_ORACLE_HPP

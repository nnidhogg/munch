#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_OBSTRUCTION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_OBSTRUCTION_HPP

#include <cstddef>
#include <optional>
#include <string>

#include "munch/core/lexer.hpp"
#include "munch/tools/audit/report.hpp"

/**
 * @brief What the verifier's relation monoid names as the obstruction where a token set certifies no byte, Obstruction,
 *        with the group of its least idempotent, Phase_group, found by obstruction() within the relation_limit; and the
 *        obstruction rendered as JSON, obstruction_json(), and as the report's own section, obstruction_section().
 */
namespace munch::tools::audit
{
/**
 * @brief The most relations of nonempty strings the monoid is searched for; past it the search stops and the section
 *        says so, since a verifier of m states can carry up to 2^(m^2) relations.
 */
constexpr std::size_t relation_limit{1U << 17U};

/**
 * @brief The group of an idempotent of the fewest fixed points: their number, the shortest string whose relation is
 *        that idempotent, the group's order, and a shortest string whose relation generates it.
 */
struct Phase_group
{
    /**
     * @brief The degree, the idempotent's number of fixed points.
     */
    std::size_t degree{};

    /**
     * @brief The shortest string whose relation is the idempotent, the first in breadth-first order over the classes.
     */
    std::string idempotent{};

    /**
     * @brief The group's order, the number of permutations of the fixed points it holds.
     */
    std::size_t order{};

    /**
     * @brief A shortest string whose relation, multiplied by the idempotent on both sides, generates the group;
     *        std::nullopt when the group is trivial or not cyclic.
     */
    std::optional<std::string> generator{};
};

/**
 * @brief What the relation monoid of a token set's verifier names as the obstruction to a certified byte: its size, a
 *        relation of rank one when one exists, and the group of its least idempotent.
 */
struct Obstruction
{
    /**
     * @brief The number of byte classes the strings are spelled over.
     */
    std::size_t class_count{};

    /**
     * @brief The number of states of the verifier's minimal automaton the relations join.
     */
    std::size_t state_count{};

    /**
     * @brief The number of nonzero relations of nonempty strings; std::nullopt when they are more than the
     *        relation_limit, where nothing else was decided.
     */
    std::optional<std::size_t> relation_count{};

    /**
     * @brief The shortest string whose relation has rank one, a nonzero rectangle; std::nullopt when none has.
     */
    std::optional<std::string> rank_one{};

    /**
     * @brief The group of the least idempotent; std::nullopt when no relation of a nonempty string is a nonzero
     *        idempotent, so that no string recurs.
     */
    std::optional<Phase_group> group{};
};

/**
 * @brief Returns what the relation monoid of a compiled token set's verifier names as the obstruction to a certified
 *        byte, an experimental diagnostic.
 *
 * The verifier is the automaton of maximal munch over the set's marked strings, a byte with whether a token boundary
 * follows it, built here over the public Simulator interface as an armed-run exploration: the run of the current token
 * and the runs of the tokens that ended at earlier boundaries, a step refused where one of those accepts, since the
 * earlier token would then have been longer. It is trimmed to the states that can complete and minimized by Moore's
 * refinement. A byte class relates two states when one of its bytes leads from one to the other under some mark, and
 * the relations of the nonempty strings are searched breadth first over the classes in the order of their lowest bytes,
 * each kept where first reached, so that each carries its shortest string. Of them the first of rank one is named, and
 * the first nonzero idempotent of the fewest fixed points, whose group is the permutations of those points that the
 * relations e m e induce, Berstel, Perrin and Reutenauer's group of an idempotent, and its degree is the one
 * verifier::smallest_fanout() returns. For a finite prefix code a certified window exists exactly when a relation of
 * rank one does; for other sets that is a conjecture, and where no relation has rank one the group is the phase a
 * parity argument counts, the parity of a run of a's under maximal munch over aa and a. The search stops past the
 * relation_limit, and then names nothing.
 * @param report The set's report, whose byte classes the strings are spelled over.
 * @param lexer The compiled set.
 * @return The obstruction.
 */
[[nodiscard]] Obstruction obstruction(const Report& report, const core::Lexer& lexer);

/**
 * @brief Renders the obstruction as the JSON object of its account: the states, the relations and the limit, the
 *        rank-one string, and the degree, idempotent, order and generator of the group, null where not found.
 * @param found The obstruction found.
 * @return The JSON text, one line.
 */
[[nodiscard]] std::string obstruction_json(const Obstruction& found);

/**
 * @brief Renders the obstruction as the report's own section: a heading that calls it a diagnostic, and rows for the
 *        relations, the rank-one string, the degree, the group and the phase it counts, or one row saying that the
 *        relations passed the limit.
 * @param found The obstruction found.
 * @return The section, a blank line before its heading and a newline after every row.
 */
[[nodiscard]] std::string obstruction_section(const Obstruction& found);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_OBSTRUCTION_HPP

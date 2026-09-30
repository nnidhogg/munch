#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_EVIDENCE_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_EVIDENCE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/gate_totals.hpp"
#include "munch/tools/probes/window_model.hpp"

/**
 * @brief What the shipped scanner does with the inputs the window model speaks about, Backup, backup_disagreements,
 *        find_witness and covering_violations: rewinds, covering tokens and occurrence witnesses.
 */
namespace munch::tools::probes
{
/**
 * @brief The backup check of a set of windows against the scanner.
 */
struct Backup
{
    /**
     * @brief The scanned inputs whose token covering the window's final byte did not begin at the expected origin.
     */
    std::size_t disagreements{0};

    /**
     * @brief The scanned inputs whose full scan rewinds at least once.
     */
    std::size_t exercised{0};

    /**
     * @brief Those rewinding inputs that tokenize completely.
     */
    std::size_t tokenizable{0};

    /**
     * @brief The prefixes each window was preceded by.
     */
    std::size_t prefixes{0};
};

/**
 * @brief Scans every window the model certifies behind a family of prefixes and before a set of fixed tails, and
 *        counts the inputs whose token covering the window's final byte does not begin at the model's origin, or at
 *        the forced origin when one is given. The prefixes are the shortest words reaching each live (state,
 *        distance past the last accepting position) pair with distance at most 9, each optionally preceded by one of
 *        up to five accepted words; an input whose scan stops before the window ends is skipped.
 * @param dfa The automaton the model walks.
 * @param lexer The lexer compiled from the same grammar.
 * @param live The automaton's trim states.
 * @param windows The windows; those the model refuses are skipped.
 * @param force_origin The origin every window is held to in place of the model's, std::nullopt for the model's.
 * @return The disagreements, the rewinding executions, those among them that tokenize completely, and the prefix
 *         count.
 */
[[nodiscard]] Backup backup_disagreements(
        const dfa::Dfa& dfa, const core::Lexer& lexer, const States_t& live, const std::vector<std::string>& windows,
        std::optional<std::size_t> force_origin = std::nullopt);

/**
 * @brief Searches a bounded family for a completely tokenizable input containing one of the certified windows with its
 *        covering token at the window's origin: heads are the empty word followed by the shortest word reaching each
 *        live (state, distance) pair with distance at most 6, the initial pair's word empty again, so the empty head
 *        is tried twice; tails are the window's shortest token completion, the empty tail, up to seven of the
 *        grammar's accepted words and five generic tails. Counts, into the totals, every completely tokenized
 *        candidate whose covering token begins elsewhere.
 * @param totals The totals the rejected candidates are counted into.
 * @param dfa The automaton.
 * @param lexer The lexer compiled from the same grammar.
 * @param live The automaton's trim states.
 * @param words The certified windows with their origins, tried in order.
 * @return The first witness input and its window, std::nullopt when the bounded family holds none.
 */
[[nodiscard]] std::optional<std::pair<std::string, std::string>> find_witness(
        Gate_totals& totals, const dfa::Dfa& dfa, const core::Lexer& lexer, const States_t& live,
        const std::vector<Certified_window>& words);

/**
 * @brief Enumerates every input over an alphabet from the window's length up to a bound, and over those that tokenize
 *        completely counts the window's occurrences and those whose covering token, the token containing the window's
 *        final byte, does not begin at the window's position plus the origin.
 * @param lexer The lexer.
 * @param window The window.
 * @param origin The claimed origin inside the window.
 * @param alphabet The bytes the inputs are drawn from.
 * @param max_length The longest input.
 * @return The occurrences and the violations.
 */
[[nodiscard]] std::pair<std::size_t, std::size_t> covering_violations(
        const core::Lexer& lexer, const std::string& window, std::size_t origin, const std::string& alphabet,
        std::size_t max_length);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_EVIDENCE_HPP

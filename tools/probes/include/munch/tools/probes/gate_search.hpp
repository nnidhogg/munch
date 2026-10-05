#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SEARCH_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SEARCH_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/window_model.hpp"

/**
 * @brief The window model decided exactly over its finite quotient, subset_budget, kept_windows, Search,
 *        shortest_windows and certified_words_upto: the shortest certified windows and the certified words up to a
 *        bound.
 */
namespace munch::tools::probes
{
/**
 * @brief The quotient keys a search may retain; a search that exceeds it is reported inconclusive rather than negative.
 */
inline constexpr std::size_t subset_budget{200'000};

static_assert(subset_budget == 200'000, "the paper states a fixed safety threshold of 200,000 keys");

/**
 * @brief The most certified windows a shortest-window search keeps, and the most words the sweep's witness search
 *        collects.
 */
inline constexpr std::size_t kept_windows{400};

/**
 * @brief What a breadth-first search for the shortest certified windows found.
 */
struct Search
{
    /**
     * @brief The length of the shortest certified window, 0 when the walk found none.
     */
    std::size_t shortest{0};

    /**
     * @brief Certified windows of the shortest length, in the order the walk met them, at most kept_windows.
     */
    std::vector<std::string> found{};

    /**
     * @brief Whether the walk exhausted the quotient without exceeding subset_budget, which makes a negative conclusive
     *        under the model.
     */
    bool exhausted{true};

    /**
     * @brief The quotient keys the walk retained.
     */
    std::size_t visited{0};
};

/**
 * @brief Walks the model's quotient breadth first from the maximum uncertainty, one key per class, and stops each
 *        branch at the first length that certified a window or once more than subset_budget keys are retained.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @return The shortest certified length, up to kept_windows windows of that length, whether the walk exhausted the
 *         quotient, and the keys it retained.
 */
[[nodiscard]] Search shortest_windows(const dfa::Dfa& dfa, const States_t& live);

/**
 * @brief Collects certified words up to a length, continuing past certified clouds so a longer word is found where a
 *        shorter one certifies too; one representative word per fresh quotient key, stopping at the cap or once more
 *        than subset_budget keys are retained.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @param max_length The longest word collected.
 * @param cap The most words collected.
 * @return The certified words with their origins, shortest first.
 */
[[nodiscard]] std::vector<Certified_window> certified_words_upto(
        const dfa::Dfa& dfa, const States_t& live, bool reentrant, std::size_t max_length, std::size_t cap);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GATE_SEARCH_HPP

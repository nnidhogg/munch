#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief What shortest_split_window() found: a shortest certified window, a proof that none exists, or an exhausted
 *        budget.
 */
struct Shortest_window
{
    /**
     * @brief How the search ended.
     */
    enum class Outcome : std::uint8_t
    {
        /**
         * @brief A shortest certified window was found.
         */
        found,

        /**
         * @brief No window of any length is certified.
         */
        none,

        /**
         * @brief The budget ran out before either was established.
         */
        budget
    };

    /**
     * @brief How the search ended.
     */
    Outcome outcome{Outcome::none};

    /**
     * @brief A shortest window is_split_window() certifies, when one was found; its bytes are one representative per
     *        class of bytes the tables do not tell apart.
     */
    std::string window{};

    /**
     * @brief Its certified origin.
     */
    std::size_t origin{};
};

/**
 * @brief The number of search nodes shortest_split_window() visits before giving up unless told otherwise, a few
 *        hundred megabytes at a few thousand states.
 */
inline constexpr std::size_t shortest_window_budget{1U << 20U};

/**
 * @brief Decides whether the given byte string is a certified split window, returning the covering origin.
 *
 * A certified split window (W, o) promises: in every completely tokenizable input containing W, the token covering the
 * occurrence's final byte begins exactly o bytes into the occurrence. The certificate is conditional on occurrence; a
 * window no completely tokenizable input contains satisfies it vacuously, and this decision does not establish that an
 * occurrence exists. A caller that has just found W in its input at hand holds that occurrence, and on completely
 * tokenizable input the promise applies to it directly; on malformed input the promise carries nothing at all.
 *
 * The decision runs the conservative cloud model over the compiled tables, a set of hypotheses about where the scan
 * could be that only ever over-approximates the true state: every live state starts as a hypothesis whose token began
 * before the window, each byte advances hypotheses deterministically, a fresh token may begin exactly where some
 * represented history just ended one, and reading from a non-re-entrant initial state begins a token at that offset.
 * The window is certified when every surviving hypothesis agrees on one in-window origin. A refusal is model-relative:
 * the model deliberately refuses some windows a greedy scanner would allow, and refusal never proves that no
 * certificate exists semantically. On non-empty token sets this coincides at length one with is_split_point(), a
 * nullable set being decided through the positive-width equivalent the simulator compiled, and an empty token set
 * refuses everything on both sides.
 * @param simulator The compiled token set.
 * @param window The byte string to decide.
 * @return The in-window origin every covering token begins at, or std::nullopt when the window is refused.
 */
[[nodiscard]] std::optional<std::size_t> is_split_window(const Simulator& simulator, std::string_view window);

/**
 * @brief Finds, by an exact search rather than by trying candidates, a shortest window is_split_window() certifies.
 *
 * The search follows one origin from the moment a token may begin, the support of every other hypothesis beside it, and
 * before any origin is chosen the support of all of them: the cloud of is_split_window() with the origins forgotten
 * except the one that is to survive. A window is certified exactly when that origin outlives every other hypothesis,
 * and two hypotheses that share a state never separate again, so a collision ends the path. The nodes are at most
 * (n + 2) 2^(n - 1) for n live states, and a shortest window is shorter than that. The question is PSPACE-complete, so
 * the budget bounds the nodes visited, and running out is reported as such, never as an answer. A node costs about a
 * state count over eight bytes, so the default budget holds a few hundred megabytes at a few thousand states; a caller
 * with less to spare passes less. A window found is as is_split_window() would certify it, conditional on occurrence
 * like every window certificate.
 * @param simulator The compiled token set.
 * @param budget The most search nodes visited before the search gives up; zero visits none.
 * @return A shortest certified window with its origin, Outcome::none when no window of any length is certified, or
 *         Outcome::budget when the search stopped first.
 */
[[nodiscard]] Shortest_window shortest_split_window(
        const Simulator& simulator, std::size_t budget = shortest_window_budget);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SPLIT_WINDOW_HPP

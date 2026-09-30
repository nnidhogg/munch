#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"

/**
 * @brief The conservative window model window_gate.cpp states and proves in its header comment, States_t, kBefore,
 *        Trajectory_t, Cloud_t, Certified_window, live_states, is_init_reentrant, step_cloud, is_certified,
 *        unknown_cloud, predicted and certified_pairs: the cloud of token-start hypotheses a worker cutting blind
 *        carries through a window, and the origin it certifies.
 */
namespace munch::tools::probes
{
/**
 * @brief A set of automaton states.
 */
using States_t = std::set<dfa::Dfa::State_t>;

/**
 * @brief A token started before the window, so its boundary is unknown to a worker cutting here.
 */
inline constexpr std::size_t kBefore{static_cast<std::size_t>(-1)};

/**
 * @brief One final-token-prefix hypothesis: a state, and the offset at which its current token began.
 */
using Trajectory_t = std::pair<dfa::Dfa::State_t, std::size_t>;

/**
 * @brief Every hypothesis a worker carries at one offset of a window.
 */
using Cloud_t = std::set<Trajectory_t>;

/**
 * @brief One certified window and the offset inside it at which a token begins.
 */
struct Certified_window
{
    /**
     * @brief The window's bytes.
     */
    std::string window;

    /**
     * @brief The offset inside the window at which the model says a token begins.
     */
    std::size_t origin{0};
};

/**
 * @brief The live states: the trim states, reachable from the initial state and able to still reach an accepting one.
 * @param dfa The automaton.
 * @return The trim states.
 */
[[nodiscard]] States_t live_states(const dfa::Dfa& dfa);

/**
 * @brief Whether any live transition re-enters the initial state. Where one does, arriving at the initial state no
 *        longer proves the scan is between tokens, so the model does not rename a trajectory's origin on reaching it.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @return True when a transition from a live state enters the initial state.
 */
[[nodiscard]] bool is_init_reentrant(const dfa::Dfa& dfa, const States_t& live);

/**
 * @brief Advances a cloud by one byte of the window: every trajectory whose state consumes the byte into a live state
 *        moves on, its origin renamed to this offset when it leaves the initial state and the initial state is not
 *        re-entered, and one fresh trajectory begins at this offset where the initial state consumes the byte into a
 *        live state and some state of the cloud accepts.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param from The cloud before the byte.
 * @param symbol The byte.
 * @param at The byte's offset inside the window.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The cloud after the byte, std::nullopt when no trajectory survives it.
 */
[[nodiscard]] std::optional<Cloud_t> step_cloud(
        const dfa::Dfa& dfa, const States_t& live, const Cloud_t& from, char symbol, std::size_t at, bool reentrant);

/**
 * @brief Whether every trajectory agrees the current token began at the same offset inside the window.
 * @param cloud The cloud at the window's end, not empty.
 * @return True when every trajectory carries one origin and it lies inside the window.
 */
[[nodiscard]] bool is_certified(const Cloud_t& cloud);

/**
 * @brief The maximum uncertainty: every trim state, each mid-token from before the window.
 * @param live The automaton's trim states.
 * @return The cloud a worker starts a window with.
 */
[[nodiscard]] Cloud_t unknown_cloud(const States_t& live);

/**
 * @brief The offset at which the model says a token begins, walking the window from the maximum uncertainty.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param window The window's bytes.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The certified origin, std::nullopt when the window is not certified.
 */
[[nodiscard]] std::optional<std::size_t> predicted(
        const dfa::Dfa& dfa, const States_t& live, const std::string& window, bool reentrant);

/**
 * @brief Every certified two-byte window, in the order of its first byte and then its second.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The certified windows with their origins.
 */
[[nodiscard]] std::vector<Certified_window> certified_pairs(const dfa::Dfa& dfa, const States_t& live, bool reentrant);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP

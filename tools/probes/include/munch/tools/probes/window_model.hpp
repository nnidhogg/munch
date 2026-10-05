#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP

#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"

/**
 * @brief The conservative window model window_gate.cpp states and proves in its header comment, byte_count, every_byte,
 *        States_t, before_window, Trajectory_t, Cloud_t, Certified_window, live_states, is_init_reentrant, step_cloud,
 *        is_certified, unknown_cloud, predicted and certified_pairs: the cloud of token-start hypotheses a worker
 *        cutting blind carries through a window, and the origin it certifies.
 */
namespace munch::tools::probes
{
/**
 * @brief The number of byte values an automaton advances on.
 */
inline constexpr int byte_count{256};

/**
 * @brief Returns every byte value, in ascending order of its unsigned value, as the char an automaton advances on.
 * @return The view of the byte_count bytes.
 */
[[nodiscard]] constexpr auto every_byte()
{
    const auto as_char{[](const int value) { return static_cast<char>(value); }};

    return std::views::iota(0, byte_count) | std::views::transform(as_char);
}

/**
 * @brief A set of automaton states.
 */
using States_t = std::set<dfa::Dfa::State_t>;

/**
 * @brief A token started before the window, so its boundary is unknown to a worker cutting here.
 */
inline constexpr std::size_t before_window{std::numeric_limits<std::size_t>::max()};

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
    std::string window{};

    /**
     * @brief The offset inside the window at which the model says a token begins.
     */
    std::size_t origin{0};
};

/**
 * @brief Returns the live states: the trim states, reachable from the initial state and able to still reach an
 *        accepting one.
 * @param dfa The automaton.
 * @return The trim states.
 */
[[nodiscard]] States_t live_states(const dfa::Dfa& dfa);

/**
 * @brief Returns whether any live transition re-enters the initial state. Where one does, arriving at the initial state
 *        no longer proves the scan is between tokens, so the model does not rename a trajectory's origin on reaching
 *        it.
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
 *
 * A token can only end where the automaton accepted, which is why the fresh trajectory needs an accepting state in the
 * cloud before the byte. Reading from the initial state begins a token, so the origin becomes this offset rather than
 * whatever the trajectory carried in; that holds only while nothing re-enters the initial state, since arriving there
 * would then no longer prove the scan is between tokens, and the shipped predicate withdraws its own exemption for the
 * same reason, so the model does too rather than certify bytes the library rejects. A trajectory that cannot consume
 * the byte is an impossible history and ends without a restart. The window's first byte needs no special case: the
 * initial cloud is every live state and so contains an accepting one, and the transition is the same at every offset.
 * The final segmentation's actual token-prefix history is then contained in the cloud, alongside conservative
 * hypotheses, so backup never has to be simulated.
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
 * @brief Returns whether every trajectory agrees the current token began at the same offset inside the window.
 * @param cloud The cloud at the window's end, not empty.
 * @return True when every trajectory carries one origin and it lies inside the window.
 */
[[nodiscard]] bool is_certified(const Cloud_t& cloud);

/**
 * @brief Returns the maximum uncertainty: every trim state, each mid-token from before the window.
 * @param live The automaton's trim states.
 * @return The cloud a worker starts a window with.
 */
[[nodiscard]] Cloud_t unknown_cloud(const States_t& live);

/**
 * @brief Returns the offset at which the model says a token begins, walking the window from the maximum uncertainty.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param window The window's bytes.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The certified origin, std::nullopt when the window is not certified.
 */
[[nodiscard]] std::optional<std::size_t> predicted(
        const dfa::Dfa& dfa, const States_t& live, const std::string& window, bool reentrant);

/**
 * @brief Returns every certified two-byte window, in the order of its first byte and then its second.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The certified windows with their origins.
 */
[[nodiscard]] std::vector<Certified_window> certified_pairs(const dfa::Dfa& dfa, const States_t& live, bool reentrant);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WINDOW_MODEL_HPP

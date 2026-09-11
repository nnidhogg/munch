#include "munch/dfa/split_window.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace munch::dfa
{
std::optional<std::size_t> is_split_window(const Simulator& simulator, const std::string_view window)
{
    // The empty window certifies nothing.
    if (window.empty())
    {
        return std::nullopt;
    }

    // The pre-window origin: a hypothesis whose token began before the window. Any value no in-window offset can
    // take serves as the marker.
    constexpr std::size_t before{std::numeric_limits<std::size_t>::max()};

    // The cloud of hypotheses (state, origin). A set, exactly as the proof's model: the seed and the rename can
    // propose the identical pair and must coalesce.
    std::set<std::pair<std::size_t, std::size_t>> cloud;

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (simulator.is_live(state))
        {
            cloud.emplace(state, before);
        }
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        // A token can only end where the automaton accepted, so a boundary before this byte is possible exactly
        // where some tracked state accepts. The test runs on the cloud as it stands, ahead of the step.
        const auto accepting{std::ranges::any_of(cloud | std::views::keys, [&simulator](const std::size_t state) {
            return simulator.is_accepting(state);
        })};

        std::set<std::pair<std::size_t, std::size_t>> next;

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{simulator.step(state, byte)}; to && simulator.is_live(*to))
            {
                // Reading from the initial state begins a token here, so the origin is this offset rather than
                // whatever the hypothesis carried in; valid only while nothing re-enters the initial state. A
                // hypothesis that cannot consume the byte is an impossible history and is dropped, never
                // restarted.
                const auto begins{state == simulator.init_state() && !simulator.init_reentrant()};

                next.emplace(*to, begins ? at : origin);
            }
        }

        // One fresh hypothesis wherever the automaton had just accepted, the only place a token can begin. The
        // initial cloud contains an accepting live state whenever the grammar is usable, so no first-byte special
        // case exists.
        if (accepting)
        {
            if (const auto to{simulator.step(simulator.init_state(), byte)}; to && simulator.is_live(*to))
            {
                next.emplace(*to, at);
            }
        }

        // The empty cloud is absorbing: no live history crosses this window, and the walk refuses.
        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    // Certified exactly when every surviving hypothesis agrees on one in-window origin; unanimity at the
    // pre-window marker means the window never resolves where the covering token began.
    const auto origin{cloud.begin()->second};

    if (!std::ranges::all_of(cloud | std::views::values, [origin](const std::size_t at) { return at == origin; }))
    {
        return std::nullopt;
    }

    return origin == before ? std::nullopt : std::optional{origin};
}

} // namespace munch::dfa

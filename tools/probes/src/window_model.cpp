#include "munch/tools/probes/window_model.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "munch/dfa/dfa.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements window_model.hpp over the automaton type, named Dfa in this unit.

using dfa::Dfa;
} // namespace

States_t live_states(const Dfa& dfa)
{
    States_t reachable{dfa.init_state()};

    std::deque<Dfa::State_t> pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && !reachable.contains(*next))
            {
                reachable.insert(*next);

                pending.push_back(*next);
            }
        }
    }

    States_t co_accessible;

    for (const auto state : reachable)
    {
        if (dfa.has_accept_token(state))
        {
            co_accessible.insert(state);
        }
    }

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state))
            {
                continue;
            }

            for (int symbol{0}; symbol < 256; ++symbol)
            {
                if (const auto next{dfa.advance(state, static_cast<char>(symbol))};
                    next && co_accessible.contains(*next))
                {
                    co_accessible.insert(state);

                    grew = true;

                    break;
                }
            }
        }
    }

    return co_accessible;
}

bool is_init_reentrant(const Dfa& dfa, const States_t& live)
{
    for (const auto state : live)
    {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && *next == dfa.init_state())
            {
                return true;
            }
        }
    }

    return false;
}

std::optional<Cloud_t> step_cloud(
        const Dfa& dfa, const States_t& live, const Cloud_t& from, const char symbol, const std::size_t at,
        const bool reentrant)
{
    const auto restart{dfa.advance(dfa.init_state(), symbol)};

    const auto restart_ok{restart && live.contains(*restart)};

    // A token can only end where the automaton accepted, so a boundary before this byte is possible exactly where
    // some tracked state accepts. Hence the test runs on the cloud as it stands, ahead of the step.
    auto accepting{false};

    for (const auto& [state, origin] : from)
    {
        accepting = accepting || dfa.has_accept_token(state);
    }

    Cloud_t next;

    for (const auto& [state, origin] : from)
    {
        if (const auto direct{dfa.advance(state, symbol)}; direct && live.contains(*direct))
        {
            // Reading from the initial state begins a token here, so the origin is this offset rather than whatever
            // the trajectory carried in. That holds only while nothing re-enters the initial state, since arriving
            // there would then no longer prove the scan is between tokens. The shipped predicate withdraws its own
            // exemption for the same reason, so the model must too or it certifies bytes the library correctly
            // rejects.
            const auto begins{state == dfa.init_state() && !reentrant};

            next.emplace(*direct, begins ? at : origin);
        }

        // No restart when a trajectory dies. A state that cannot consume the byte is an impossible history, not a
        // token boundary; the discarded variant that restarted here is refuted by the executable legacy regression in
        // window_gate.cpp's main(): over {a, abc, bx, x} and "abx" it certifies origin 2 where the scanner cuts at 0
        // and 1.
    }

    // One fresh trajectory wherever the automaton had just accepted, which is the only place a token can begin.
    // No special case for the window's first byte: the initial cloud is every live state and so contains an
    // accepting one, making the guard true there anyway, and the transition stays the same at every offset. The
    // final segmentation's actual token-prefix history is then contained in the cloud, alongside conservative
    // hypotheses, so backup never has to be simulated.
    if (restart_ok && accepting)
    {
        next.emplace(*restart, at);
    }

    return next.empty() ? std::nullopt : std::optional{next};
}

bool is_certified(const Cloud_t& cloud)
{
    const auto origin{cloud.begin()->second};

    if (origin == kBefore)
    {
        return false;
    }

    for (const auto& [state, at] : cloud)
    {
        if (at != origin)
        {
            return false;
        }
    }

    return true;
}

Cloud_t unknown_cloud(const States_t& live)
{
    Cloud_t cloud;

    for (const auto state : live)
    {
        cloud.emplace(state, kBefore);
    }

    return cloud;
}

std::optional<std::size_t> predicted(
        const Dfa& dfa, const States_t& live, const std::string& window, const bool reentrant)
{
    auto cloud{unknown_cloud(live)};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto next{step_cloud(dfa, live, cloud, window[at], at, reentrant)};

        if (!next)
        {
            return std::nullopt;
        }

        cloud = *next;
    }

    return is_certified(cloud) ? std::optional{cloud.begin()->second} : std::nullopt;
}

std::vector<Certified_window> certified_pairs(const Dfa& dfa, const States_t& live, const bool reentrant)
{
    std::vector<Certified_window> windows;

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const std::string pair{static_cast<char>(first), static_cast<char>(second)};

            if (const auto at{predicted(dfa, live, pair, reentrant)})
            {
                windows.push_back({.window = pair, .origin = *at});
            }
        }
    }

    return windows;
}

} // namespace munch::tools::probes

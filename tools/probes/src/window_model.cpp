#include "munch/tools/probes/window_model.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief Returns the predicate on an automaton's states that holds where a state accepts.
 * @param dfa The automaton, which outlives the predicate.
 * @return The predicate, true for a state that carries an accepted token.
 */
auto accepting_in(const dfa::Dfa& dfa)
{
    return [&dfa](const dfa::Dfa::State_t state) { return dfa.has_accept_token(state).has_value(); };
}

} // namespace

States_t live_states(const dfa::Dfa& dfa)
{
    States_t reachable{dfa.init_state()};

    std::deque<dfa::Dfa::State_t> pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (const auto symbol : every_byte())
        {
            if (const auto next{dfa.advance(state, symbol)}; next && !reachable.contains(*next))
            {
                reachable.insert(*next);

                pending.push_back(*next);
            }
        }
    }

    States_t co_accessible{};

    const auto accepts{accepting_in(dfa)};

    std::ranges::copy_if(reachable, std::inserter(co_accessible, co_accessible.end()), accepts);

    const auto reaches_accepting{[&dfa, &co_accessible](const dfa::Dfa::State_t state) {
        const auto leads_to_accepting{[&dfa, &co_accessible, state](const char symbol) {
            const auto next{dfa.advance(state, symbol)};

            return next && co_accessible.contains(*next);
        }};

        return std::ranges::any_of(every_byte(), leads_to_accepting);
    }};

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state) || !reaches_accepting(state))
            {
                continue;
            }

            co_accessible.insert(state);

            grew = true;
        }
    }

    return co_accessible;
}

bool is_init_reentrant(const dfa::Dfa& dfa, const States_t& live)
{
    const auto re_enters{[&dfa](const dfa::Dfa::State_t state) {
        const auto enters_init{[&dfa, state](const char symbol) {
            const auto next{dfa.advance(state, symbol)};

            return next && *next == dfa.init_state();
        }};

        return std::ranges::any_of(every_byte(), enters_init);
    }};

    return std::ranges::any_of(live, re_enters);
}

std::optional<Cloud_t> step_cloud(
        const dfa::Dfa& dfa, const States_t& live, const Cloud_t& from, const char symbol, const std::size_t at,
        const bool reentrant)
{
    const auto restart{dfa.advance(dfa.init_state(), symbol)};

    const auto restart_ok{restart && live.contains(*restart)};

    // A boundary before this byte is possible exactly where some tracked state accepts, so the test runs on the cloud
    // as it stands, ahead of the step.
    const auto accepts{accepting_in(dfa)};

    const auto accepting{std::ranges::any_of(from | std::views::keys, accepts)};

    Cloud_t next{};

    for (const auto& [state, origin] : from)
    {
        const auto direct{dfa.advance(state, symbol)};

        // A trajectory that cannot consume the byte is an impossible history, not a token boundary, and ends here.
        if (!direct || !live.contains(*direct))
        {
            continue;
        }

        // Reading from the initial state begins a token at this offset, unless the initial state is re-entered.
        const auto begins{state == dfa.init_state() && !reentrant};

        next.emplace(*direct, begins ? at : origin);
    }

    // One fresh trajectory wherever the automaton had just accepted, the only place a token can begin.
    if (restart_ok && accepting)
    {
        next.emplace(*restart, at);
    }

    if (next.empty())
    {
        return std::nullopt;
    }

    return next;
}

bool is_certified(const Cloud_t& cloud)
{
    const auto& [first_state, origin]{*cloud.begin()};

    if (origin == before_window)
    {
        return false;
    }

    const auto at_origin{[origin](const std::size_t at) { return at == origin; }};

    return std::ranges::all_of(cloud | std::views::values, at_origin);
}

Cloud_t unknown_cloud(const States_t& live)
{
    Cloud_t cloud{};

    const auto from_before{[](const dfa::Dfa::State_t state) { return Trajectory_t{state, before_window}; }};

    std::ranges::transform(live, std::inserter(cloud, cloud.end()), from_before);

    return cloud;
}

std::optional<std::size_t> predicted(
        const dfa::Dfa& dfa, const States_t& live, const std::string& window, const bool reentrant)
{
    auto cloud{unknown_cloud(live)};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        auto next{step_cloud(dfa, live, cloud, window[at], at, reentrant)};

        if (!next)
        {
            return std::nullopt;
        }

        cloud = std::move(*next);
    }

    if (!is_certified(cloud))
    {
        return std::nullopt;
    }

    const auto& [state, origin]{*cloud.begin()};

    return origin;
}

std::vector<Certified_window> certified_pairs(const dfa::Dfa& dfa, const States_t& live, const bool reentrant)
{
    std::vector<Certified_window> windows{};

    for (const auto first : every_byte())
    {
        for (const auto second : every_byte())
        {
            const std::string pair{first, second};

            if (const auto at{predicted(dfa, live, pair, reentrant)})
            {
                windows.push_back({.window = pair, .origin = *at});
            }
        }
    }

    return windows;
}

} // namespace munch::tools::probes

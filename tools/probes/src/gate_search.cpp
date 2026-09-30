#include "munch/tools/probes/gate_search.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements gate_search.hpp: the quotient the walks deduplicate on is private to this unit.

using dfa::Dfa;

/**
 * @brief The finite quotient the search deduplicates on: which states carry the pre-window origin, and how many
 *        distinct in-window origins each state carries, saturated at two.
 *
 * Every in-window origin of a reachable cloud occupies at most one state: it is seeded once into a single state, the
 * deterministic step moves it to at most one successor, and a dead origin occupies none. Under that invariant,
 * saturating at two is exact for certification, since two origins meeting in one deterministic state follow identical
 * futures and never separate again, and summing counts across predecessors is exact, since origins arriving from
 * different predecessors are distinct. The pre-window origin is held apart in its own support set. The quotient is
 * therefore a transition congruence over reachable clouds, and a walk over it decides exactly whether a window exists
 * under the model and what its minimum length is; prefixes reaching the same key collapse to one representative, so
 * its witnesses are examples rather than the full set. It has at most 6^|Q+| configurations.
 */
using Quotient_t = std::pair<std::set<Dfa::State_t>, std::map<Dfa::State_t, int>>;

/**
 * @brief A cloud's quotient key.
 * @param cloud The cloud.
 * @return The states carrying the pre-window origin, and per state its in-window origins counted up to two.
 */
Quotient_t quotient(const Cloud_t& cloud)
{
    Quotient_t out{};

    for (const auto& [state, origin] : cloud)
    {
        if (origin == kBefore)
        {
            out.first.insert(state);
        }
        else
        {
            auto& count{out.second[state]};

            count = std::min(count + 1, 2);
        }
    }

    return out;
}
} // namespace

Search shortest_windows(const Dfa& dfa, const States_t& live)
{
    Search search{};

    constexpr std::size_t kKeep{400};

    const auto reentrant{is_init_reentrant(dfa, live)};

    std::map<Quotient_t, std::string> seen{};

    std::deque<std::pair<Cloud_t, std::string>> queue{{unknown_cloud(live), ""}};

    seen[quotient(unknown_cloud(live))] = "";

    while (!queue.empty())
    {
        const auto [current, word]{queue.front()};

        queue.pop_front();

        // Once a length has produced certified windows, deeper ones cannot be shorter.
        if ((search.shortest != 0 && word.size() >= search.shortest) || seen.size() > kSubsetBudget)
        {
            search.exhausted = search.exhausted && seen.size() <= kSubsetBudget;

            continue;
        }

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{step_cloud(dfa, live, current, static_cast<char>(symbol), word.size(), reentrant)};

            if (!next)
            {
                continue;
            }

            const auto extended{word + static_cast<char>(symbol)};

            if (is_certified(*next))
            {
                search.shortest = search.shortest != 0 ? search.shortest : extended.size();

                if (search.found.size() < kKeep)
                {
                    search.found.push_back(extended);
                }

                continue;
            }

            if (const auto key{quotient(*next)}; !seen.contains(key))
            {
                seen[key] = extended;

                queue.emplace_back(*next, extended);
            }
        }
    }

    search.visited = seen.size();

    return search;
}

std::vector<Certified_window> certified_words_upto(
        const Dfa& dfa, const States_t& live, const bool reentrant, const std::size_t max_length, const std::size_t cap)
{
    std::vector<Certified_window> words{};

    std::map<Quotient_t, std::string> seen{};

    std::deque<std::pair<Cloud_t, std::string>> queue{{unknown_cloud(live), ""}};

    seen[quotient(unknown_cloud(live))] = "";

    while (!queue.empty() && words.size() < cap && seen.size() <= kSubsetBudget)
    {
        const auto [current, word]{queue.front()};

        queue.pop_front();

        if (word.size() >= max_length)
        {
            continue;
        }

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{step_cloud(dfa, live, current, static_cast<char>(symbol), word.size(), reentrant)};

            if (!next)
            {
                continue;
            }

            const auto extended{word + static_cast<char>(symbol)};

            if (is_certified(*next) && words.size() < cap)
            {
                words.push_back({.window = extended, .origin = next->begin()->second});
            }

            // Certified clouds are stepped onward too, so a certified word's key does not swallow its extensions.
            if (const auto key{quotient(*next)}; !seen.contains(key))
            {
                seen[key] = extended;

                queue.emplace_back(*next, extended);
            }
        }
    }

    return words;
}

} // namespace munch::tools::probes

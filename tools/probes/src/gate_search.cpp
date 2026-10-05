#include "munch/tools/probes/gate_search.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace munch::tools::probes
{
namespace
{
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
 * under the model and what its minimum length is; prefixes reaching the same key collapse to one representative, so its
 * witnesses are examples rather than the full set. It has at most 6^|Q+| configurations.
 */
struct Quotient
{
    /**
     * @brief Orders by the pre-window states, then by the counts.
     */
    auto operator<=>(const Quotient&) const = default;

    /**
     * @brief The states carrying the pre-window origin.
     */
    std::set<dfa::Dfa::State_t> before{};

    /**
     * @brief Per state, its in-window origins counted up to two.
     */
    std::map<dfa::Dfa::State_t, int> counts{};
};

/**
 * @brief Returns a cloud's quotient key.
 * @param cloud The cloud.
 * @return The states carrying the pre-window origin, and per state its in-window origins counted up to two.
 */
Quotient quotient(const Cloud_t& cloud)
{
    Quotient out{};

    auto& [before, counts]{out};

    for (const auto& [state, origin] : cloud)
    {
        if (origin == before_window)
        {
            before.insert(state);

            continue;
        }

        auto& count{counts[state]};

        count = std::min(count + 1, 2);
    }

    return out;
}

/**
 * @brief Walks the model's quotient breadth first from the maximum uncertainty, one key per class: each dequeued word
 *        is stepped by every byte in order, and each surviving extension whose visit asks for it is enqueued once per
 *        fresh key.
 * @tparam Expands The type of the dequeued word's expansion test.
 * @tparam Visit The type of the extension visitor.
 * @tparam Keep_walking The type of the walk's continuation test.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @param expands Tells, from a dequeued word and the keys retained, whether the word is stepped onward.
 * @param visit Receives each surviving extension's cloud and word, and tells whether the extension is stepped onward.
 * @param keep_walking Tells, from the keys retained, whether the walk dequeues another word; by default it always does,
 *        each branch stopping on its own.
 * @return The quotient keys the walk retained.
 */
template <typename Expands, typename Visit, typename Keep_walking = decltype([](std::size_t) { return true; })>
std::size_t walk_quotient(
        const dfa::Dfa& dfa, const States_t& live, const bool reentrant, const Expands& expands, const Visit& visit,
        const Keep_walking& keep_walking = {})
{
    std::map<Quotient, std::string> seen{};

    const auto start{unknown_cloud(live)};

    std::deque<std::pair<Cloud_t, std::string>> queue{{start, ""}};

    seen[quotient(start)] = "";

    while (!queue.empty() && keep_walking(seen.size()))
    {
        const auto [current, word]{queue.front()};

        queue.pop_front();

        if (!expands(word, seen.size()))
        {
            continue;
        }

        for (const auto symbol : every_byte())
        {
            const auto next{step_cloud(dfa, live, current, symbol, word.size(), reentrant)};

            if (!next)
            {
                continue;
            }

            const auto extended{word + symbol};

            if (!visit(*next, extended))
            {
                continue;
            }

            if (const auto key{quotient(*next)}; !seen.contains(key))
            {
                seen[key] = extended;

                queue.emplace_back(*next, extended);
            }
        }
    }

    return seen.size();
}

} // namespace

Search shortest_windows(const dfa::Dfa& dfa, const States_t& live)
{
    Search search{};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto record{[&search](const std::string& window) {
        if (search.shortest == 0)
        {
            search.shortest = window.size();
        }

        if (search.found.size() < kept_windows)
        {
            search.found.push_back(window);
        }
    }};

    const auto expands{[&search](const std::string& word, const std::size_t retained) {
        const auto past_shortest{search.shortest != 0 && word.size() >= search.shortest};

        return !past_shortest && retained <= subset_budget;
    }};

    const auto visit{[&record](const Cloud_t& next, const std::string& extended) {
        if (is_certified(next))
        {
            record(extended);

            return false;
        }

        return true;
    }};

    search.visited = walk_quotient(dfa, live, reentrant, expands, visit);

    // Every fresh key is enqueued, so a walk that retained more than subset_budget keys dequeued a word past it.
    search.exhausted = search.visited <= subset_budget;

    return search;
}

std::vector<Certified_window> certified_words_upto(
        const dfa::Dfa& dfa, const States_t& live, const bool reentrant, const std::size_t max_length,
        const std::size_t cap)
{
    std::vector<Certified_window> words{};

    const auto keep_walking{
            [&words, cap](const std::size_t retained) { return words.size() < cap && retained <= subset_budget; }};

    const auto expands{[max_length](const std::string& word, const std::size_t) { return word.size() < max_length; }};

    const auto visit{[&words, cap](const Cloud_t& next, const std::string& extended) {
        if (is_certified(next) && words.size() < cap)
        {
            const auto& [state, origin]{*next.begin()};

            words.push_back({.window = extended, .origin = origin});
        }

        return true;
    }};

    std::ignore = walk_quotient(dfa, live, reentrant, expands, visit, keep_walking);

    return words;
}

} // namespace munch::tools::probes

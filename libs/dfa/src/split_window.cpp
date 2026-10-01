#include "munch/dfa/split_window.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace munch::dfa
{
std::optional<std::size_t> is_split_window(const Simulator& simulator, const std::string_view window)
{
    // The empty window certifies nothing.
    if (window.empty())
    {
        return std::nullopt;
    }

    // The pre-window origin: a hypothesis whose token began before the window. Any value no in-window offset
    // can take serves as the marker.
    constexpr std::size_t before{std::numeric_limits<std::size_t>::max()};

    // The cloud of hypotheses (state, origin). A set, exactly as the proof's model: the seed and the rename can
    // propose the identical pair and must coalesce. Held as a sorted vector without duplicates, the same set without a
    // node allocated per hypothesis per byte; the next cloud's storage is kept across bytes for the same reason.
    std::vector<std::pair<std::size_t, std::size_t>> cloud;

    std::vector<std::pair<std::size_t, std::size_t>> next;

    cloud.reserve(simulator.state_count());

    next.reserve(simulator.state_count() + 1);

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (simulator.is_live(state))
        {
            cloud.emplace_back(state, before);
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

        next.clear();

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{simulator.step(state, byte)}; to && simulator.is_live(*to))
            {
                // Reading from the initial state begins a token here, so the origin is this offset rather than
                // whatever the hypothesis carried in; valid only while nothing re-enters the initial state. A
                // hypothesis that cannot consume the byte is an impossible history and is dropped, never restarted.
                const auto begins{state == simulator.init_state() && !simulator.init_reentrant()};

                next.emplace_back(*to, begins ? at : origin);
            }
        }

        // One fresh hypothesis wherever the automaton had just accepted, the only place a token can begin. The initial
        // cloud contains an accepting live state whenever the grammar is usable, so no first-byte special case exists.
        if (accepting)
        {
            if (const auto to{simulator.step(simulator.init_state(), byte)}; to && simulator.is_live(*to))
            {
                next.emplace_back(*to, at);
            }
        }

        // The empty cloud is absorbing: no live history crosses this window, and the walk refuses.
        if (next.empty())
        {
            return std::nullopt;
        }

        std::ranges::sort(next);

        const auto duplicates{std::ranges::unique(next)};

        next.erase(duplicates.begin(), duplicates.end());

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

namespace
{
/**
 * @brief A set of states packed one bit each, so a search node costs a state count over eight bytes and no more.
 */
using States_t = std::vector<std::uint64_t>;

/**
 * @brief A node of the shortest-window search: before an origin is chosen, the states every hypothesis occupies;
 *        after, the chosen origin's state and the states every other one occupies.
 */
struct Search_node
{
    bool operator==(const Search_node&) const = default;

    /**
     * @brief The chosen origin's state, or nothing before it is chosen.
     */
    std::optional<std::size_t> chosen;

    /**
     * @brief The states the other hypotheses occupy.
     */
    States_t others;
};

/**
 * @brief Whether a state is in the set.
 * @param states The set.
 * @param state The state.
 * @return True when it is.
 */
bool contains(const States_t& states, const std::size_t state)
{
    return ((states[state >> 6U] >> (state & 63U)) & 1U) != 0;
}

/**
 * @brief Adds a state to the set.
 * @param states The set.
 * @param state The state.
 */
void insert(States_t& states, const std::size_t state)
{
    states[state >> 6U] |= std::uint64_t{1} << (state & 63U);
}

/**
 * @brief Whether the set holds no state.
 * @param states The set.
 * @return True when it is empty.
 */
bool empty(const States_t& states)
{
    return std::ranges::all_of(states, [](const std::uint64_t word) { return word == 0; });
}

/**
 * @brief Hashes a node for the set of nodes already reached.
 */
struct Node_hash
{
    /** @brief Mixes the chosen state and the packed states. */
    std::size_t operator()(const Search_node& node) const noexcept
    {
        std::size_t hash{node.chosen ? *node.chosen + 1 : 0};

        for (const auto word : node.others)
        {
            hash = hash * 0x9E3779B97F4A7C15U + static_cast<std::size_t>(word ^ (word >> 29U));
        }

        return hash;
    }
};

/**
 * @brief One byte per class of bytes the tables do not tell apart, so one edge stands for the class.
 * @param simulator The compiled token set.
 * @return The representatives, ascending.
 */
std::vector<unsigned char> byte_classes(const Simulator& simulator)
{
    std::set<std::vector<std::size_t>> signatures;

    std::vector<unsigned char> bytes;

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        std::vector<std::size_t> signature;

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, byte).value_or(simulator.state_count()));
        }

        if (signatures.insert(std::move(signature)).second)
        {
            bytes.push_back(byte);
        }
    }

    return bytes;
}

/**
 * @brief The search nodes one byte leads to, as is_split_window() steps its cloud.
 *
 * Every hypothesis advances or dies. A token begins at this byte where some hypothesis had just accepted, or where the
 * initial state is read and nothing re-enters it; that fresh origin is the one a node before the choice may choose.
 * After the choice the fresh origin is just another hypothesis, and the chosen one must not share a state with any.
 * @param simulator The compiled token set.
 * @param node The node advanced from.
 * @param byte The byte read.
 * @return The nodes reached, each with whether its origin was chosen at this byte.
 */
std::vector<std::pair<Search_node, bool>> advance(
        const Simulator& simulator, const Search_node& node, const unsigned char byte)
{
    const auto live_step{[&simulator, byte](const std::size_t state) -> std::optional<std::size_t> {
        const auto to{simulator.step(state, byte)};

        return to && simulator.is_live(*to) ? to : std::nullopt;
    }};

    const auto states{simulator.state_count()};

    auto accepting{node.chosen && simulator.is_accepting(*node.chosen)};

    const auto init{simulator.init_state()};

    const auto renamed{!simulator.init_reentrant() && !node.chosen && contains(node.others, init)};

    States_t moved(node.others.size(), 0);

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!contains(node.others, state))
        {
            continue;
        }

        accepting = accepting || simulator.is_accepting(state);

        if (state == init && renamed)
        {
            continue; // it begins the fresh token here, which is where its origin now lies
        }

        if (const auto to{live_step(state)})
        {
            insert(moved, *to);
        }
    }

    const auto fresh{accepting || renamed ? live_step(init) : std::nullopt};

    std::vector<std::pair<Search_node, bool>> out;

    if (node.chosen)
    {
        const auto chosen{live_step(*node.chosen)};

        if (!chosen)
        {
            return out;
        }

        if (fresh)
        {
            insert(moved, *fresh);
        }

        if (!contains(moved, *chosen))
        {
            out.emplace_back(Search_node{.chosen = chosen, .others = std::move(moved)}, false);
        }

        return out;
    }

    if (fresh && !contains(moved, *fresh))
    {
        out.emplace_back(Search_node{.chosen = fresh, .others = moved}, true);
    }

    if (fresh)
    {
        insert(moved, *fresh);
    }

    if (!empty(moved))
    {
        out.emplace_back(Search_node{.chosen = std::nullopt, .others = std::move(moved)}, false);
    }

    return out;
}
} // namespace

Shortest_window shortest_split_window(const Simulator& simulator, const std::size_t budget)
{
    Search_node start{.chosen = std::nullopt, .others = States_t((simulator.state_count() + 63) / 64, 0)};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (simulator.is_live(state))
        {
            insert(start.others, state);
        }
    }

    if (empty(start.others))
    {
        return {};
    }

    if (budget == 0)
    {
        return {.outcome = Shortest_window::Outcome::budget, .window = {}, .origin = 0};
    }

    // Each node is held once, in the set of nodes reached; the queue and the trail back point into it. A trail entry
    // remembers the node it was reached from, the byte read, and whether the origin was chosen there, so a window is
    // read back from its last node, the origin being the depth at which it was chosen.
    struct Reached
    {
        /**
         * @brief The node it was reached from.
         */
        std::size_t parent;

        /**
         * @brief The byte read on the way.
         */
        unsigned char byte;

        /**
         * @brief Whether the origin was chosen on that byte.
         */
        bool chose;
    };

    const auto bytes{byte_classes(simulator)};

    std::unordered_map<Search_node, std::size_t, Node_hash> index;

    std::vector<const Search_node*> nodes{&index.emplace(std::move(start), 0).first->first};

    std::vector<Reached> reached{{.parent = 0, .byte = 0, .chose = false}};

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        for (const auto byte : bytes)
        {
            for (auto& [next, chose] : advance(simulator, *nodes[at], byte))
            {
                if (index.contains(next))
                {
                    continue;
                }

                if (nodes.size() >= budget)
                {
                    return {.outcome = Shortest_window::Outcome::budget, .window = {}, .origin = 0};
                }

                const auto certified{next.chosen && empty(next.others)};

                nodes.push_back(&index.emplace(std::move(next), nodes.size()).first->first);

                reached.push_back({.parent = at, .byte = byte, .chose = chose});

                if (certified)
                {
                    std::string window;

                    std::size_t chosen_at{0};

                    for (auto walk{nodes.size() - 1}; walk != 0; walk = reached[walk].parent)
                    {
                        window.push_back(static_cast<char>(reached[walk].byte));

                        if (reached[walk].chose)
                        {
                            chosen_at = window.size();
                        }
                    }

                    std::ranges::reverse(window);

                    const auto origin{window.size() - chosen_at};

                    return {.outcome = Shortest_window::Outcome::found, .window = std::move(window), .origin = origin};
                }
            }
        }
    }

    return {};
}

} // namespace munch::dfa

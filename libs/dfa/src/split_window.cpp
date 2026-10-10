#include "munch/dfa/split_window.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace munch::dfa
{
namespace
{
/**
 * @brief A set of states packed one bit each, so a search node costs a state count over eight bytes and no more.
 */
using States_t = std::vector<std::uint64_t>;

/**
 * @brief The number of states one word of a state set holds.
 */
constexpr std::size_t word_bits{64};

/**
 * @brief The origin that marks a hypothesis whose token began before the window, a value no in-window offset can take.
 */
constexpr std::size_t before_window{std::numeric_limits<std::size_t>::max()};

/**
 * @brief A node of the shortest-window search: before an origin is chosen, the states every hypothesis occupies; after,
 *        the chosen origin's state and the states every other one occupies.
 */
struct Search_node
{
    /**
     * @brief Equal when the chosen state and the other states match.
     */
    bool operator==(const Search_node&) const = default;

    /**
     * @brief The chosen origin's state, or nothing before it is chosen.
     */
    std::optional<std::size_t> chosen{};

    /**
     * @brief The states the other hypotheses occupy.
     */
    States_t others{};
};

/**
 * @brief Hashes a node for the set of nodes already reached.
 */
struct Node_hash
{
    /**
     * @brief Combines the hashes of the packed states and of the chosen state.
     * @param node The node hashed.
     * @return Its hash.
     */
    std::size_t operator()(const Search_node& node) const noexcept
    {
        auto seed{boost::hash_range(node.others.cbegin(), node.others.cend())};

        boost::hash_combine(seed, node.chosen);

        return seed;
    }
};

/**
 * @brief Hashes a reached node by its number, or a node by value, alike, so the set of reached nodes holds numbers and
 *        is searched by a node's value before that node has a number.
 */
struct Number_hash
{
    /**
     * @brief Admits lookups by a node's value beside lookups by number.
     */
    using is_transparent = void;

    /**
     * @brief Returns the hash of the node with this number.
     * @param number The node's number.
     * @return The hash of the node it names.
     */
    std::size_t operator()(const std::size_t number) const noexcept { return Node_hash{}(nodes[number]); }

    /**
     * @brief Returns the hash of a node by value.
     * @param node The node.
     * @return Its hash.
     */
    std::size_t operator()(const Search_node& node) const noexcept { return Node_hash{}(node); }

    /**
     * @brief The nodes reached, numbered by position.
     */
    const std::vector<Search_node>& nodes;
};

/**
 * @brief Compares reached nodes by number with each other and with a node by value.
 */
struct Number_equal
{
    /**
     * @brief Admits comparisons with a node's value beside comparisons of numbers.
     */
    using is_transparent = void;

    /**
     * @brief Returns whether the nodes with these numbers are equal.
     * @param left The first node's number.
     * @param right The second node's number.
     * @return True when the nodes they name are equal.
     */
    bool operator()(const std::size_t left, const std::size_t right) const { return nodes[left] == nodes[right]; }

    /**
     * @brief Returns whether the node with this number equals a node by value.
     * @param number The reached node's number.
     * @param node The node by value.
     * @return True when they are equal.
     */
    bool operator()(const std::size_t number, const Search_node& node) const { return nodes[number] == node; }

    /**
     * @brief Returns whether a node by value equals the node with this number.
     * @param node The node by value.
     * @param number The reached node's number.
     * @return True when they are equal.
     */
    bool operator()(const Search_node& node, const std::size_t number) const { return node == nodes[number]; }

    /**
     * @brief The nodes reached, numbered by position.
     */
    const std::vector<Search_node>& nodes;
};

/**
 * @brief A trail entry of the shortest-window search: how a reached node was reached, so a window is read back from its
 *        last node, the origin being the depth at which it was chosen.
 */
struct Reached
{
    /**
     * @brief The number of the node it was reached from.
     */
    std::size_t parent{};

    /**
     * @brief The byte read on the way.
     */
    unsigned char byte{};

    /**
     * @brief Whether the origin was chosen on that byte.
     */
    bool chose{};
};

/**
 * @brief Returns whether a state is in the set.
 * @param states The set.
 * @param state The state.
 * @return True when it is.
 */
[[nodiscard]] bool contains(const States_t& states, const std::size_t state)
{
    return ((states[state / word_bits] >> (state % word_bits)) & 1U) != 0;
}

/**
 * @brief Adds a state to the set.
 * @param states The set.
 * @param state The state.
 */
void insert(States_t& states, const std::size_t state)
{
    states[state / word_bits] |= std::uint64_t{1} << (state % word_bits);
}

/**
 * @brief Returns whether the set holds no state.
 * @param states The set.
 * @return True when it is empty.
 */
[[nodiscard]] bool empty(const States_t& states)
{
    return std::ranges::all_of(states, std::logical_not{});
}

/**
 * @brief Returns the search nodes one byte leads to, as is_split_window() steps its cloud.
 *
 * Every hypothesis advances or dies. A token begins at this byte where some hypothesis had just accepted, or where the
 * initial state is read and nothing re-enters it; that fresh origin is the one a node before the choice may choose.
 * After the choice the fresh origin is just another hypothesis, and the chosen one must not share a state with any.
 * @param simulator The compiled token set.
 * @param node The node advanced from.
 * @param byte The byte read.
 * @return The nodes reached, each with whether its origin was chosen at this byte.
 */
[[nodiscard]] std::vector<std::pair<Search_node, bool>> advance(
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

        // The initial state begins the fresh token here, which is where its origin lies.
        if (state == init && renamed)
        {
            continue;
        }

        if (const auto to{live_step(state)})
        {
            insert(moved, *to);
        }
    }

    const auto fresh{accepting || renamed ? live_step(init) : std::nullopt};

    std::vector<std::pair<Search_node, bool>> out{};

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

std::optional<std::size_t> is_split_window(const Simulator& simulator, const std::string_view window)
{
    // The empty window certifies nothing.
    if (window.empty())
    {
        return std::nullopt;
    }

    // The cloud of hypotheses (state, origin). A set, exactly as the proof's model: the seed and the rename can propose
    // the identical pair and must coalesce. Held as a sorted vector without duplicates, the same set without a node
    // allocated per hypothesis per byte; the next cloud's storage is kept across bytes for the same reason.
    std::vector<std::pair<std::size_t, std::size_t>> cloud{};

    std::vector<std::pair<std::size_t, std::size_t>> next{};

    cloud.reserve(simulator.state_count());

    next.reserve(simulator.state_count() + 1);

    const auto is_live{[&simulator](const std::size_t state) { return simulator.is_live(state); }};

    const auto is_accepting{[&simulator](const std::size_t state) { return simulator.is_accepting(state); }};

    const auto states{std::views::iota(std::size_t{0}, simulator.state_count())};

    for (const auto state : states | std::views::filter(is_live))
    {
        cloud.emplace_back(state, before_window);
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        // A token can only end where the automaton accepted, so a boundary before this byte is possible exactly where
        // some tracked state accepts. The test runs on the cloud as it stands, ahead of the step.
        const auto accepting{std::ranges::any_of(cloud | std::views::keys, is_accepting)};

        next.clear();

        for (const auto& [state, origin] : cloud)
        {
            const auto to{simulator.step(state, byte)};

            // A hypothesis that cannot consume the byte is an impossible history and is dropped, never restarted.
            if (!to || !simulator.is_live(*to))
            {
                continue;
            }

            // Reading from the initial state begins a token here, so the origin is this offset rather than whatever the
            // hypothesis carried in; valid only while nothing re-enters the initial state.
            const auto begins{state == simulator.init_state() && !simulator.init_reentrant()};

            const auto stepped_origin{begins ? at : origin};

            next.emplace_back(*to, stepped_origin);
        }

        // One fresh hypothesis wherever the automaton had just accepted, the only place a token can begin. The initial
        // cloud contains an accepting live state whenever the grammar is usable, so no first-byte special case exists.
        const auto fresh{accepting ? simulator.step(simulator.init_state(), byte) : std::nullopt};

        if (fresh && simulator.is_live(*fresh))
        {
            next.emplace_back(*fresh, at);
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

    // Certified exactly when every surviving hypothesis agrees on one in-window origin; unanimity at the pre-window
    // marker means the window never resolves where the covering token began.
    const auto& [first_state, origin]{cloud.front()};

    const auto agrees{[origin](const std::size_t at) { return at == origin; }};

    if (!std::ranges::all_of(cloud | std::views::values, agrees))
    {
        return std::nullopt;
    }

    if (origin == before_window)
    {
        return std::nullopt;
    }

    return origin;
}

Shortest_window shortest_split_window(const Simulator& simulator, const std::size_t budget)
{
    const auto words{(simulator.state_count() + word_bits - 1) / word_bits};

    Search_node start{.chosen = std::nullopt, .others = States_t(words, 0)};

    const auto is_live{[&simulator](const std::size_t state) { return simulator.is_live(state); }};

    const auto states{std::views::iota(std::size_t{0}, simulator.state_count())};

    for (const auto state : states | std::views::filter(is_live))
    {
        insert(start.others, state);
    }

    if (empty(start.others))
    {
        return {};
    }

    if (budget == 0)
    {
        return {.outcome = Shortest_window::Outcome::budget, .window = {}, .origin = 0};
    }

    // One edge per class of bytes the tables do not tell apart, the class's lowest byte standing for it.
    std::vector<unsigned char> bytes{};

    for (const auto& symbol_class : simulator.symbol_classes())
    {
        bytes.push_back(symbol_class.front());
    }

    // Each node is held once, in the list of nodes reached in the order they were reached, which is also the queue; the
    // set of reached nodes holds their numbers in that list, and a number always names the same node.
    std::vector<Search_node> nodes{};

    nodes.push_back(std::move(start));

    std::unordered_set<std::size_t, Number_hash, Number_equal> index{
            {0},
            1,
            Number_hash{.nodes = nodes},
            Number_equal{.nodes = nodes}};

    std::vector<Reached> reached{{.parent = 0, .byte = 0, .chose = false}};

    const auto window_to{[&reached](const std::size_t last) -> Shortest_window {
        std::string window{};

        std::size_t chosen_at{0};

        for (auto walk{last}; walk != 0;)
        {
            const auto [parent, byte, chose]{reached[walk]};

            window.push_back(static_cast<char>(byte));

            if (chose)
            {
                chosen_at = window.size();
            }

            walk = parent;
        }

        std::ranges::reverse(window);

        const auto origin{window.size() - chosen_at};

        return {.outcome = Shortest_window::Outcome::found, .window = std::move(window), .origin = origin};
    }};

    const auto expand_byte{[&](const std::size_t at, const unsigned char byte) -> std::optional<Shortest_window> {
        for (auto& [next, chose] : advance(simulator, nodes[at], byte))
        {
            if (index.contains(next))
            {
                continue;
            }

            if (nodes.size() >= budget)
            {
                return Shortest_window{.outcome = Shortest_window::Outcome::budget, .window = {}, .origin = 0};
            }

            const auto certified{next.chosen && empty(next.others)};

            nodes.push_back(std::move(next));

            const auto number{nodes.size() - 1};

            index.insert(number);

            reached.push_back({.parent = at, .byte = byte, .chose = chose});

            if (certified)
            {
                return window_to(number);
            }
        }

        return std::nullopt;
    }};

    const auto expand{[&](const std::size_t at) -> std::optional<Shortest_window> {
        for (const auto byte : bytes)
        {
            if (auto finished{expand_byte(at, byte)})
            {
                return finished;
            }
        }

        return std::nullopt;
    }};

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        if (auto finished{expand(at)})
        {
            return std::move(*finished);
        }
    }

    return {};
}

} // namespace munch::dfa

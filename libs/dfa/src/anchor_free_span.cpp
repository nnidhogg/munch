#include "munch/dfa/anchor_free_span.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/split_window.hpp"

namespace munch::dfa
{
namespace
{
/**
 * @brief A state as the walk stores it: narrowed, since a node holds a set of them and the graph holds many nodes.
 */
using State_t = std::uint32_t;

/**
 * @brief The walk behind anchor_free_span(): a guessed marking of the input read one byte at a time, beside a guessed
 *        stretch that no window may anchor.
 *
 * The marking is the two-part position the other decisions carry, the run of the segment being read and the runs of
 * segments already closed, kept so that a later accept on one refutes the guessed close. A window anchors a position
 * inside its occurrence, at the origin, rather than at the byte just read, so it is seen only once its last byte
 * arrives, at most the longest window's width less one bytes after the position it anchors. So the walk guesses where
 * the stretch begins and ends and refuses any window that anchors a position inside it: it needs to know only how far
 * back the stretch began and ended, both capped at that width, and which windows can still complete, the longest
 * suffix of the input that begins one. No byte is kept. The byte case is this with a width of one.
 */
class Span_walk
{
public:
    /**
     * @brief Takes a certified inventory, checking it against the simulator it is for.
     *
     * The walk keeps the inventory as its own copy and nothing else of what it is handed: the simulator comes back
     * with decide(), so the walk owns nothing it could outlive.
     * @param simulator The simulator the inventory was certified by.
     * @param inventory The certified windows and their origins.
     * @throws std::invalid_argument If a window is empty or uncertified at its stated origin.
     */
    Span_walk(const Simulator& simulator, std::span<const std::pair<std::string_view, std::size_t>> inventory);

    /**
     * @brief Decides the anchor-free span.
     *
     * A token of unbounded length settles it alone: no certified origin falls inside a token, since the input's own
     * boundaries would contradict the certificate there, so such a token carries anchor-free runs as long as it is.
     * Otherwise every token is bounded and the product graph is built, where it stays bounded too. A node from
     * which no input can be completed is dropped first, since a stretch that never finishes is not a stretch of any
     * input; a cycle of anchor-free exits among the nodes that remain is exactly an unbounded answer; and otherwise
     * the longest run is a fixed point over an acyclic graph.
     * @param simulator The simulator whose tables the walk reads, the one the inventory was checked against.
     * @return The exact supremum, or std::nullopt when it is unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> decide(const Simulator& simulator) const;

private:
    /**
     * @brief Where the input read so far stands to the guessed stretch.
     */
    enum class Phase : std::uint8_t
    {
        before,
        inside,
        after,
        settled
    };

    /**
     * @brief A node of the product graph: the marking's state and the stretch's.
     */
    struct Node
    {
        auto operator<=>(const Node&) const = default;

        /**
         * @brief The state of the segment being read.
         */
        std::size_t reading{};

        /**
         * @brief The states of the closed segments still alive, sorted and distinct.
         */
        std::vector<State_t> closed{};

        /**
         * @brief The longest suffix of the input that begins some window, as an index into the walk's prefixes.
         */
        std::uint32_t prefix{};

        /**
         * @brief Where the stretch stands.
         */
        Phase phase{Phase::before};

        /**
         * @brief How far back the stretch began, capped at the reach; meaningful inside it and after it.
         */
        std::uint32_t since_start{};

        /**
         * @brief How far back the stretch ended, below the reach; meaningful after it.
         */
        std::uint32_t since_end{};
    };

    /**
     * @brief What the prefix tree does on a byte: the prefix it moves to, and how far back from the byte each window
     *        completed by it anchors, ascending.
     */
    struct Move
    {
        /**
         * @brief The prefix moved to, as an index into the walk's prefixes.
         */
        std::uint32_t prefix{};

        /**
         * @brief How far back each completed window anchors, ascending.
         */
        std::vector<std::size_t> ages;
    };

    /**
     * @brief A node's index in the product graph, which the edges name nodes by.
     */
    using Index_t = std::uint32_t;

    /**
     * @brief The product graph: the nodes in the order found, the start node first, and each node's out-edges as the
     *        node entered and whether the byte read lies in the stretch, one edge per class of bytes the walk cannot
     *        tell apart.
     */
    struct Graph
    {
        /**
         * @brief The nodes, by index.
         */
        std::vector<Node> nodes;

        /**
         * @brief Each node's out-edges.
         */
        std::vector<std::vector<std::pair<Index_t, bool>>> edges;
    };

    /**
     * @brief Whether the token set matches any positive-length input at all.
     *
     * With no window every position is anchor-free, so the span is as long as inputs can be: unbounded whenever a
     * token matches, since a match repeats, and zero when nothing does.
     * @param simulator The simulator whose tables the walk reads.
     * @return True when some reachable state has a transition into an accepting one.
     */
    [[nodiscard]] static bool matches_any_token(const Simulator& simulator);

    /**
     * @brief Whether some token is arbitrarily long: a cycle among the live states reachable from the start, found
     *        depth first with an explicit stack.
     * @param simulator The simulator whose tables the walk reads.
     * @return True when such a cycle exists.
     */
    [[nodiscard]] static bool unbounded_token(const Simulator& simulator);

    /**
     * @brief The node every walk starts from: the initial state, nothing closed, the empty prefix, no stretch yet.
     * @return That node.
     */
    [[nodiscard]] static Node start(const Simulator& simulator);

    /**
     * @brief One byte per class of bytes the walk cannot tell apart: the same transition from every state and the
     *        same folded byte, so one edge stands for the class.
     * @param simulator The simulator whose tables the walk reads.
     * @return The representatives, ascending.
     */
    [[nodiscard]] std::vector<unsigned char> representatives(const Simulator& simulator) const;

    /**
     * @brief Builds the product graph breadth first from the start node.
     * @param simulator The simulator whose tables the walk reads.
     * @return Every reachable node with its out-edges.
     */
    [[nodiscard]] Graph explore(const Simulator& simulator) const;

    /**
     * @brief Advances the marking by one byte, optionally closing the segment being read first.
     *
     * Closing first is what puts a window's origin at the start of a token, which is what the certificate says
     * about it. A closed run that reaches acceptance refutes the guess and kills the node; one with no
     * transition is simply forgotten.
     * @param simulator The simulator whose tables the walk reads.
     * @param node The node advanced from.
     * @param mark Whether the segment being read closes before the byte.
     * @param byte The byte read.
     * @return The advanced node, or std::nullopt where the reading run has no transition or a guess is refuted.
     */
    [[nodiscard]] static std::optional<Node> read(const Simulator& simulator, Node node, bool mark, unsigned char byte);

    /**
     * @brief The stretch's next phases on a byte whose completed windows anchor the given ages back.
     *
     * Before the stretch, the byte may begin it unless a window anchors the byte itself. Inside, the byte may extend
     * it unless a window anchors a position from its start to the byte, or be the first byte after it unless one
     * anchors a position inside it. After it, the walk refuses any window that anchors inside it, until no window can
     * reach back that far.
     * @param node The node advanced from.
     * @param ages How far back from the byte the windows completed by it anchor, ascending.
     * @return Each next node's stretch fields, with whether the byte lies in the stretch.
     */
    [[nodiscard]] std::vector<std::pair<Node, bool>> stretch(
            const Node& node, const std::vector<std::size_t>& ages) const;

    /**
     * @brief The nodes an input can be finished from: those that reach a node whose reading run accepts, found by
     *        walking the edges backwards from the accepting ones.
     * @param simulator The simulator whose tables the walk reads.
     * @param graph The product graph.
     * @return One flag per node, by index.
     */
    [[nodiscard]] static std::vector<bool> endable(const Simulator& simulator, const Graph& graph);

    /**
     * @brief Whether the endable nodes carry a cycle of bytes inside the stretch, which is a stretch with no end.
     *
     * Walked with an explicit stack rather than by recursion, since the product graph is as deep as it is wide.
     * @param graph The product graph.
     * @param finishing The endable nodes.
     * @return True when such a cycle exists.
     */
    [[nodiscard]] static bool has_inside_cycle(const Graph& graph, const std::vector<bool>& finishing);

    /**
     * @brief The longest stretch: the most edges inside it along a path among the endable nodes, relaxed to a fixed
     *        point over those edges, which have no cycle by then.
     * @param graph The product graph.
     * @param finishing The endable nodes.
     * @return The supremum.
     */
    [[nodiscard]] static std::size_t longest_stretch(const Graph& graph, const std::vector<bool>& finishing);

    /**
     * @brief The prefix tree's move from a prefix on a byte.
     * @param prefix The prefix's index.
     * @param byte The byte read.
     * @return The move.
     */
    [[nodiscard]] const Move& move(std::uint32_t prefix, unsigned char byte) const;

    /**
     * @brief The certified windows and their origins, the walk's own copy, folded like the bytes read.
     */
    std::vector<std::pair<std::string, std::size_t>> inventory_;

    /**
     * @brief The longest window's width less one: how far back from its last byte a window can anchor.
     */
    std::size_t reach_{0};

    /**
     * @brief Each byte as the prefix tree reads it: itself when some window contains it, else one stand-in from
     *        outside the windows' alphabet, so the walk does not multiply its nodes by bytes no window can see.
     */
    std::array<char, Simulator::symbol_count> fold_{};

    /**
     * @brief Every prefix of every window, the empty one first, which the nodes name by index.
     */
    std::vector<std::string> prefixes_;

    /**
     * @brief The prefix tree's move from each prefix on each folded byte, computed as the walk first needs it.
     */
    mutable std::map<std::pair<std::uint32_t, char>, Move> moves_;
};

Span_walk::Span_walk(
        const Simulator& simulator, const std::span<const std::pair<std::string_view, std::size_t>> inventory)
{
    std::set<char> present;

    for (const auto& [window, origin] : inventory)
    {
        if (window.empty() || origin >= window.size())
        {
            throw std::invalid_argument{"anchor_free_span: a window is empty or its origin lies outside it"};
        }

        if (is_split_window(simulator, window) != std::optional<std::size_t>{origin})
        {
            throw std::invalid_argument{"anchor_free_span: a window is not certified at the stated origin"};
        }

        inventory_.emplace_back(window, origin);

        reach_ = std::max(reach_, window.size() - 1);

        present.insert(window.begin(), window.end());
    }

    std::optional<char> stand_in;

    for (std::size_t value{0}; value < Simulator::symbol_count && !stand_in; ++value)
    {
        if (!present.contains(static_cast<char>(value)))
        {
            stand_in = static_cast<char>(value);
        }
    }

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<char>(value)};

        fold_[value] = stand_in && !present.contains(byte) ? *stand_in : byte;
    }

    std::set<std::string> prefixes{std::string{}};

    for (const auto& window : inventory_ | std::views::keys)
    {
        for (std::size_t length{1}; length <= window.size(); ++length)
        {
            prefixes.insert(window.substr(0, length));
        }
    }

    prefixes_.assign(prefixes.begin(), prefixes.end());
}

const Span_walk::Move& Span_walk::move(const std::uint32_t prefix, const unsigned char byte) const
{
    const auto key{std::pair{prefix, fold_[byte]}};

    if (const auto found{moves_.find(key)}; found != moves_.end())
    {
        return found->second;
    }

    const auto seen{prefixes_[prefix] + fold_[byte]};

    Move result;

    for (const auto& [window, origin] : inventory_)
    {
        if (seen.size() >= window.size() && seen.compare(seen.size() - window.size(), window.size(), window) == 0)
        {
            result.ages.push_back(window.size() - 1 - origin);
        }
    }

    std::ranges::sort(result.ages);

    // The longest suffix of what was read that begins a window: every window ending later passes through it.
    for (std::size_t drop{0}; drop <= seen.size(); ++drop)
    {
        const auto suffix{seen.substr(drop)};

        if (const auto at{std::ranges::lower_bound(prefixes_, suffix)}; at != prefixes_.end() && *at == suffix)
        {
            result.prefix = static_cast<std::uint32_t>(at - prefixes_.begin());

            break;
        }
    }

    return moves_.emplace(key, std::move(result)).first->second;
}

std::optional<std::size_t> Span_walk::decide(const Simulator& simulator) const
{
    if (inventory_.empty())
    {
        return matches_any_token(simulator) ? std::nullopt : std::optional<std::size_t>{0};
    }

    if (unbounded_token(simulator))
    {
        return std::nullopt;
    }

    const auto graph{explore(simulator)};

    const auto finishing{endable(simulator, graph)};

    if (!finishing.front())
    {
        return 0; // no input is tokenizable at all, so no stretch exists
    }

    if (has_inside_cycle(graph, finishing))
    {
        return std::nullopt;
    }

    return longest_stretch(graph, finishing);
}

bool Span_walk::matches_any_token(const Simulator& simulator)
{
    std::vector<bool> seen(simulator.state_count(), false);

    std::deque<std::size_t> pending{simulator.init_state()};

    seen[simulator.init_state()] = true;

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
        {
            const auto next{simulator.step(at, static_cast<unsigned char>(value))};

            if (!next)
            {
                continue;
            }

            if (simulator.is_accepting(*next))
            {
                return true;
            }

            if (!seen[*next])
            {
                seen[*next] = true;

                pending.push_back(*next);
            }
        }
    }

    return false;
}

bool Span_walk::unbounded_token(const Simulator& simulator)
{
    std::vector<std::uint8_t> colour(simulator.state_count(), 0);

    std::vector<std::pair<std::size_t, std::size_t>> stack{{simulator.init_state(), 0}};

    colour[simulator.init_state()] = 1;

    while (!stack.empty())
    {
        auto& [state, value]{stack.back()};

        if (value == Simulator::symbol_count)
        {
            colour[state] = 2;

            stack.pop_back();

            continue;
        }

        const auto next{simulator.step(state, static_cast<unsigned char>(value++))};

        if (!next || !simulator.is_live(*next))
        {
            continue;
        }

        if (colour[*next] == 1)
        {
            return true;
        }

        if (colour[*next] == 0)
        {
            colour[*next] = 1;

            stack.emplace_back(*next, 0);
        }
    }

    return false;
}

Span_walk::Node Span_walk::start(const Simulator& simulator)
{
    return {.reading = simulator.init_state(), .closed = {}};
}

std::vector<unsigned char> Span_walk::representatives(const Simulator& simulator) const
{
    std::set<std::vector<std::size_t>> signatures;

    std::vector<unsigned char> bytes;

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        std::vector<std::size_t> signature;

        signature.reserve(simulator.state_count() + 1);

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, byte).value_or(simulator.state_count()));
        }

        signature.push_back(static_cast<unsigned char>(fold_[value]));

        if (signatures.insert(std::move(signature)).second)
        {
            bytes.push_back(byte);
        }
    }

    return bytes;
}

Span_walk::Graph Span_walk::explore(const Simulator& simulator) const
{
    const auto bytes{representatives(simulator)};

    Graph graph{.nodes = {start(simulator)}, .edges = {{}}};

    std::map<Node, Index_t> index{{start(simulator), 0}};

    for (Index_t at{0}; at < graph.nodes.size(); ++at)
    {
        const auto node{graph.nodes[at]};

        for (const auto byte : bytes)
        {
            for (const auto mark : {false, true})
            {
                if (mark && !simulator.is_accepting(node.reading))
                {
                    continue;
                }

                auto next{read(simulator, node, mark, byte)};

                if (!next)
                {
                    continue;
                }

                const auto& moved{move(node.prefix, byte)};

                next->prefix = moved.prefix;

                for (auto [target, inside] : stretch(*next, moved.ages))
                {
                    const auto [entry, added]{
                            index.try_emplace(std::move(target), static_cast<Index_t>(graph.nodes.size()))};

                    if (added)
                    {
                        graph.nodes.push_back(entry->first);

                        graph.edges.emplace_back();
                    }

                    graph.edges[at].emplace_back(entry->second, inside);
                }
            }
        }
    }

    return graph;
}

std::optional<Span_walk::Node> Span_walk::read(
        const Simulator& simulator, Node node, const bool mark, const unsigned char byte)
{
    if (mark)
    {
        node.closed.push_back(static_cast<State_t>(node.reading));

        node.reading = simulator.init_state();
    }

    const auto next{simulator.step(node.reading, byte)};

    if (!next)
    {
        return std::nullopt;
    }

    node.reading = *next;

    std::vector<State_t> survived;

    for (const auto state : node.closed)
    {
        const auto moved{simulator.step(state, byte)};

        if (!moved)
        {
            continue;
        }

        if (simulator.is_accepting(*moved))
        {
            return std::nullopt;
        }

        survived.push_back(static_cast<State_t>(*moved));
    }

    std::ranges::sort(survived);

    const auto duplicates{std::ranges::unique(survived)};

    survived.erase(duplicates.begin(), duplicates.end());

    node.closed = std::move(survived);

    return node;
}

std::vector<std::pair<Span_walk::Node, bool>> Span_walk::stretch(
        const Node& node, const std::vector<std::size_t>& ages) const
{
    const auto cap{static_cast<std::uint32_t>(reach_)};

    // Whether a window completed by this byte anchors a position from `low` to `high` bytes back, inclusive.
    const auto anchors{[&ages](const std::size_t low, const std::size_t high) {
        return std::ranges::any_of(ages, [&](const std::size_t age) { return low <= age && age <= high; });
    }};

    const auto with{[&node](const Phase phase, const std::uint32_t since_start, const std::uint32_t since_end) {
        auto next{node};

        next.phase = phase;
        next.since_start = since_start;
        next.since_end = since_end;

        return next;
    }};

    std::vector<std::pair<Node, bool>> out;

    switch (node.phase)
    {
    case Phase::before:
        out.emplace_back(with(Phase::before, 0, 0), false);

        if (!anchors(0, 0))
        {
            out.emplace_back(with(Phase::inside, 0, 0), true);
        }

        break;

    case Phase::inside:
    {
        const auto since_start{std::min(node.since_start + 1, cap)};

        if (!anchors(0, since_start))
        {
            out.emplace_back(with(Phase::inside, since_start, 0), true);
        }

        if (!anchors(1, since_start))
        {
            out.emplace_back(cap > 0 ? with(Phase::after, since_start, 0) : with(Phase::settled, 0, 0), false);
        }

        break;
    }

    case Phase::after:
    {
        const auto since_start{std::min(node.since_start + 1, cap)};

        const auto since_end{node.since_end + 1};

        if (!anchors(since_end + 1, since_start))
        {
            out.emplace_back(
                    since_end < cap ? with(Phase::after, since_start, since_end) : with(Phase::settled, 0, 0), false);
        }

        break;
    }

    case Phase::settled:
        out.emplace_back(with(Phase::settled, 0, 0), false);

        break;
    }

    return out;
}

std::vector<bool> Span_walk::endable(const Simulator& simulator, const Graph& graph)
{
    std::vector<std::vector<Index_t>> into(graph.nodes.size());

    for (Index_t at{0}; at < graph.nodes.size(); ++at)
    {
        for (const auto target : graph.edges[at] | std::views::keys)
        {
            into[target].push_back(at);
        }
    }

    std::vector<bool> finishing(graph.nodes.size(), false);

    std::deque<Index_t> pending;

    for (Index_t at{0}; at < graph.nodes.size(); ++at)
    {
        if (simulator.is_accepting(graph.nodes[at].reading))
        {
            finishing[at] = true;

            pending.push_back(at);
        }
    }

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        for (const auto source : into[at])
        {
            if (!finishing[source])
            {
                finishing[source] = true;

                pending.push_back(source);
            }
        }
    }

    return finishing;
}

bool Span_walk::has_inside_cycle(const Graph& graph, const std::vector<bool>& finishing)
{
    std::vector<std::uint8_t> colour(graph.nodes.size(), 0);

    for (Index_t root{0}; root < graph.nodes.size(); ++root)
    {
        if (!finishing[root] || colour[root] != 0)
        {
            continue;
        }

        std::vector<std::pair<Index_t, std::size_t>> stack{{root, 0}};

        colour[root] = 1;

        while (!stack.empty())
        {
            auto& [node, next]{stack.back()};

            const auto& out{graph.edges[node]};

            while (next < out.size() && (!out[next].second || !finishing[out[next].first]))
            {
                ++next;
            }

            if (next == out.size())
            {
                colour[node] = 2;

                stack.pop_back();

                continue;
            }

            const auto target{out[next++].first};

            if (colour[target] == 1)
            {
                return true;
            }

            if (colour[target] == 0)
            {
                colour[target] = 1;

                stack.emplace_back(target, 0);
            }
        }
    }

    return false;
}

std::size_t Span_walk::longest_stretch(const Graph& graph, const std::vector<bool>& finishing)
{
    std::vector<std::size_t> run(graph.nodes.size(), 0);

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (Index_t at{0}; at < graph.nodes.size(); ++at)
        {
            if (!finishing[at])
            {
                continue;
            }

            for (const auto& [target, inside] : graph.edges[at])
            {
                if (inside && finishing[target] && run[at] + 1 > run[target])
                {
                    run[target] = run[at] + 1;

                    changed = true;
                }
            }
        }
    }

    return std::ranges::max(run);
}

} // namespace

std::optional<std::size_t> anchor_free_span(const Simulator& simulator)
{
    // The certified bytes as width-one windows at origin zero, which is what the general walk reduces to.
    std::vector<std::string> bytes;

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        if (simulator.is_split_point(static_cast<char>(value)))
        {
            bytes.emplace_back(1, static_cast<char>(value));
        }
    }

    std::vector<std::pair<std::string_view, std::size_t>> inventory;

    inventory.reserve(bytes.size());

    for (const auto& window : bytes)
    {
        inventory.emplace_back(window, 0);
    }

    return Span_walk{simulator, inventory}.decide(simulator);
}

std::optional<std::size_t> anchor_free_span(
        const Simulator& simulator, const std::span<const std::pair<std::string_view, std::size_t>> inventory)
{
    return Span_walk{simulator, inventory}.decide(simulator);
}

} // namespace munch::dfa

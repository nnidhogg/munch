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
 * back the stretch began and ended, both capped at that width, and which windows can still complete, the longest suffix
 * of the input that begins one. No byte is kept. The byte case is this with a width of one.
 */
class Span_walk
{
public:
    /**
     * @brief Takes a certified inventory, checking it against the simulator it is for.
     *
     * The walk keeps the inventory as its own copy and nothing else of what it is handed: the simulator comes back with
     * decide(), so the walk owns nothing it could outlive.
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
     * Otherwise every token is bounded and the product graph is built, where it stays bounded too. A node from which no
     * input can be completed is dropped first, since a stretch that never finishes is not a stretch of any input; a
     * cycle of anchor-free exits among the nodes that remain is exactly an unbounded answer; and otherwise the longest
     * run is a fixed point over an acyclic graph.
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
        /**
         * @brief The stretch has not begun.
         */
        before,

        /**
         * @brief The byte read lies in the stretch.
         */
        inside,

        /**
         * @brief The stretch has ended, and a window completing later can still anchor inside it.
         */
        after,

        /**
         * @brief The stretch has ended and no window can reach back into it.
         */
        settled
    };

    /**
     * @brief Where a depth-first search stands with a vertex.
     */
    enum class Colour : std::uint8_t
    {
        /**
         * @brief Not reached yet.
         */
        unseen,

        /**
         * @brief On the search's stack, its successors still being read.
         */
        open,

        /**
         * @brief Every successor read and the vertex left.
         */
        done
    };

    /**
     * @brief A node of the product graph: the marking's state and the stretch's.
     */
    struct Node
    {
        /**
         * @brief Ordered member by member, so nodes key the map of nodes found.
         */
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
        std::vector<std::size_t> ages{};
    };

    /**
     * @brief A node's index in the product graph, which the edges name nodes by.
     */
    using Index_t = std::uint32_t;

    /**
     * @brief An out-edge of the product graph.
     */
    struct Edge
    {
        /**
         * @brief The node entered.
         */
        Index_t target{};

        /**
         * @brief Whether the byte read lies in the stretch.
         */
        bool inside{};
    };

    /**
     * @brief The product graph: the nodes in the order found, the start node first, and each node's out-edges, one edge
     *        per class of bytes the walk cannot tell apart.
     */
    struct Graph
    {
        /**
         * @brief The nodes, by index.
         */
        std::vector<Node> nodes{};

        /**
         * @brief Each node's out-edges.
         */
        std::vector<std::vector<Edge>> edges{};
    };

    /**
     * @brief Returns whether the token set matches any positive-length input at all.
     *
     * With no window every position is anchor-free, so the span is as long as inputs can be: unbounded whenever a token
     * matches, since a match repeats, and zero when nothing does.
     * @param simulator The simulator whose tables the walk reads.
     * @return True when some reachable state has a transition into an accepting one.
     */
    [[nodiscard]] static bool matches_any_token(const Simulator& simulator);

    /**
     * @brief Returns whether some token is arbitrarily long: a cycle among the live states reachable from the start,
     *        found depth first with an explicit stack.
     * @param simulator The simulator whose tables the walk reads.
     * @return True when such a cycle exists.
     */
    [[nodiscard]] static bool unbounded_token(const Simulator& simulator);

    /**
     * @brief Builds the product graph breadth first from the start node.
     * @param simulator The simulator whose tables the walk reads.
     * @return Every reachable node with its out-edges.
     */
    [[nodiscard]] Graph explore(const Simulator& simulator) const;

    /**
     * @brief Picks one byte per class of bytes the walk cannot tell apart: the same transition from every state and the
     *        same folded byte, so one edge stands for the class.
     * @param simulator The simulator whose tables the walk reads.
     * @return The representatives, ascending.
     */
    [[nodiscard]] std::vector<unsigned char> representatives(const Simulator& simulator) const;

    /**
     * @brief Returns the node every walk starts from: the initial state, nothing closed, the empty prefix, no stretch
     *        yet.
     * @param simulator The simulator whose tables the walk reads.
     * @return That node.
     */
    [[nodiscard]] static Node start(const Simulator& simulator);

    /**
     * @brief Advances the marking by one byte, optionally closing the segment being read first.
     *
     * Closing first is what puts a window's origin at the start of a token, which is what the certificate says about
     * it. A closed run that reaches acceptance refutes the guess and kills the node; one with no transition is simply
     * forgotten.
     * @param simulator The simulator whose tables the walk reads.
     * @param node The node advanced from.
     * @param mark Whether the segment being read closes before the byte.
     * @param byte The byte read.
     * @return The advanced node, or std::nullopt where the reading run has no transition or a guess is refuted.
     */
    [[nodiscard]] static std::optional<Node> read(const Simulator& simulator, Node node, bool mark, unsigned char byte);

    /**
     * @brief Returns the prefix tree's move from a prefix on a byte.
     * @param prefix The prefix's index.
     * @param byte The byte read.
     * @return The move.
     */
    [[nodiscard]] const Move& move(std::uint32_t prefix, unsigned char byte) const;

    /**
     * @brief Returns the stretch's next phases on a byte whose completed windows anchor the given ages back.
     *
     * Before the stretch, the byte may begin it unless a window anchors the byte itself. Inside, the byte may extend it
     * unless a window anchors a position from its start to the byte, or be the first byte after it unless one anchors a
     * position inside it. After it, the walk refuses any window that anchors inside it, until no window can reach back
     * that far.
     * @param node The node advanced from.
     * @param ages How far back from the byte the windows completed by it anchor, ascending.
     * @return Each next node's stretch fields, with whether the byte lies in the stretch.
     */
    [[nodiscard]] std::vector<std::pair<Node, bool>> stretch(
            const Node& node, const std::vector<std::size_t>& ages) const;

    /**
     * @brief Returns the nodes an input can be finished from: those that reach a node whose reading run accepts, found
     *        by walking the edges backwards from the accepting ones.
     * @param simulator The simulator whose tables the walk reads.
     * @param graph The product graph.
     * @return One flag per node, by index.
     */
    [[nodiscard]] static std::vector<bool> endable(const Simulator& simulator, const Graph& graph);

    /**
     * @brief Returns whether the endable nodes carry a cycle of bytes inside the stretch, which is a stretch with no
     *        end.
     *
     * Walked with an explicit stack rather than by recursion, since the product graph is as deep as it is wide.
     * @param graph The product graph.
     * @param finishing The endable nodes.
     * @return True when such a cycle exists.
     */
    [[nodiscard]] static bool has_inside_cycle(const Graph& graph, const std::vector<bool>& finishing);

    /**
     * @brief Returns the longest stretch: the most edges inside it along a path among the endable nodes, relaxed to a
     *        fixed point over those edges, which have no cycle by then.
     * @param graph The product graph.
     * @param finishing The endable nodes.
     * @return The supremum.
     */
    [[nodiscard]] static std::size_t longest_stretch(const Graph& graph, const std::vector<bool>& finishing);

    /**
     * @brief The certified windows and their origins, the walk's own copy, folded like the bytes read.
     */
    std::vector<std::pair<std::string, std::size_t>> inventory_{};

    /**
     * @brief The longest window's width less one: how far back from its last byte a window can anchor.
     */
    std::size_t reach_{0};

    /**
     * @brief Each byte as the prefix tree reads it: itself when some window contains it, else one stand-in from outside
     *        the windows' alphabet, so the walk does not multiply its nodes by bytes no window can see.
     */
    std::array<char, Simulator::symbol_count> fold_{};

    /**
     * @brief Every prefix of every window, the empty one first, which the nodes name by index.
     */
    std::vector<std::string> prefixes_{};

    /**
     * @brief The prefix tree's move from each prefix on each folded byte, computed as the walk first needs it.
     */
    mutable std::map<std::pair<std::uint32_t, char>, Move> moves_{};
};

Span_walk::Span_walk(
        const Simulator& simulator, const std::span<const std::pair<std::string_view, std::size_t>> inventory)
{
    std::set<char> present{};

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

    const auto absent{[&present](const std::size_t value) { return !present.contains(static_cast<char>(value)); }};

    const auto values{std::views::iota(std::size_t{0}, Simulator::symbol_count)};

    const auto first_absent{std::ranges::find_if(values, absent)};

    const auto stand_in{first_absent == values.end() ? std::nullopt : std::optional{static_cast<char>(*first_absent)}};

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

std::optional<std::size_t> Span_walk::decide(const Simulator& simulator) const
{
    if (inventory_.empty())
    {
        if (matches_any_token(simulator))
        {
            return std::nullopt;
        }

        return 0;
    }

    if (unbounded_token(simulator))
    {
        return std::nullopt;
    }

    const auto graph{explore(simulator)};

    const auto finishing{endable(simulator, graph)};

    // No input is tokenizable at all, so no stretch exists.
    if (!finishing.front())
    {
        return 0;
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

            if (seen[*next])
            {
                continue;
            }

            seen[*next] = true;

            pending.push_back(*next);
        }
    }

    return false;
}

bool Span_walk::unbounded_token(const Simulator& simulator)
{
    std::vector<Colour> colour(simulator.state_count(), Colour::unseen);

    std::vector<std::pair<std::size_t, std::size_t>> stack{{simulator.init_state(), 0}};

    colour[simulator.init_state()] = Colour::open;

    while (!stack.empty())
    {
        auto& [state, value]{stack.back()};

        if (value == Simulator::symbol_count)
        {
            colour[state] = Colour::done;

            stack.pop_back();

            continue;
        }

        const auto byte{static_cast<unsigned char>(value)};

        ++value;

        const auto next{simulator.step(state, byte)};

        if (!next || !simulator.is_live(*next))
        {
            continue;
        }

        if (colour[*next] == Colour::open)
        {
            return true;
        }

        if (colour[*next] != Colour::unseen)
        {
            continue;
        }

        colour[*next] = Colour::open;

        stack.emplace_back(*next, 0);
    }

    return false;
}

Span_walk::Graph Span_walk::explore(const Simulator& simulator) const
{
    const auto bytes{representatives(simulator)};

    Graph graph{.nodes = {start(simulator)}, .edges = {{}}};

    std::map<Node, Index_t> index{{start(simulator), 0}};

    const auto add_edge{[&graph, &index](const Index_t at, Node target, const bool inside) {
        const auto number{static_cast<Index_t>(graph.nodes.size())};

        const auto [entry, added]{index.try_emplace(std::move(target), number)};

        const auto& [node, entered]{*entry};

        if (added)
        {
            graph.nodes.push_back(node);

            graph.edges.emplace_back();
        }

        graph.edges[at].push_back(Edge{.target = entered, .inside = inside});
    }};

    const auto follow{[&](const Index_t at, const Node& node, const unsigned char byte, const bool mark) {
        if (mark && !simulator.is_accepting(node.reading))
        {
            return;
        }

        auto next{read(simulator, node, mark, byte)};

        if (!next)
        {
            return;
        }

        const auto& [prefix, ages]{move(node.prefix, byte)};

        next->prefix = prefix;

        for (auto [target, inside] : stretch(*next, ages))
        {
            add_edge(at, std::move(target), inside);
        }
    }};

    for (Index_t at{0}; at < graph.nodes.size(); ++at)
    {
        const auto node{graph.nodes[at]};

        for (const auto byte : bytes)
        {
            for (const auto mark : {false, true})
            {
                follow(at, node, byte, mark);
            }
        }
    }

    return graph;
}

std::vector<unsigned char> Span_walk::representatives(const Simulator& simulator) const
{
    std::set<std::vector<std::size_t>> signatures{};

    std::vector<unsigned char> bytes{};

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        std::vector<std::size_t> signature{};

        signature.reserve(simulator.state_count() + 1);

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            const auto successor{simulator.step(state, byte).value_or(simulator.state_count())};

            signature.push_back(successor);
        }

        const auto folded{static_cast<unsigned char>(fold_[value])};

        signature.push_back(folded);

        const auto [position, inserted]{signatures.insert(std::move(signature))};

        if (!inserted)
        {
            continue;
        }

        bytes.push_back(byte);
    }

    return bytes;
}

Span_walk::Node Span_walk::start(const Simulator& simulator)
{
    return {.reading = simulator.init_state(), .closed = {}};
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

    std::vector<State_t> survived{};

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

const Span_walk::Move& Span_walk::move(const std::uint32_t prefix, const unsigned char byte) const
{
    const auto key{std::pair{prefix, fold_[byte]}};

    if (const auto found{moves_.find(key)}; found != moves_.end())
    {
        const auto& [found_key, found_move]{*found};

        return found_move;
    }

    const auto seen{prefixes_[prefix] + fold_[byte]};

    Move result{};

    for (const auto& [window, origin] : inventory_)
    {
        if (seen.ends_with(window))
        {
            const auto age{window.size() - 1 - origin};

            result.ages.push_back(age);
        }
    }

    std::ranges::sort(result.ages);

    // The longest suffix of what was read that begins a window: every window ending later passes through it.
    for (std::size_t drop{0}; drop <= seen.size(); ++drop)
    {
        const auto suffix{seen.substr(drop)};

        const auto at{std::ranges::lower_bound(prefixes_, suffix)};

        if (at == prefixes_.end() || *at != suffix)
        {
            continue;
        }

        const auto position{std::ranges::distance(prefixes_.begin(), at)};

        result.prefix = static_cast<std::uint32_t>(position);

        break;
    }

    const auto [entry, added]{moves_.emplace(key, std::move(result))};

    const auto& [entry_key, entry_move]{*entry};

    return entry_move;
}

std::vector<std::pair<Span_walk::Node, bool>> Span_walk::stretch(
        const Node& node, const std::vector<std::size_t>& ages) const
{
    const auto cap{static_cast<std::uint32_t>(reach_)};

    const auto anchors{[&ages](const std::size_t low, const std::size_t high) {
        const auto in_range{[low, high](const std::size_t age) { return low <= age && age <= high; }};

        return std::ranges::any_of(ages, in_range);
    }};

    const auto with{[&node](const Phase phase, const std::uint32_t since_start, const std::uint32_t since_end) {
        auto next{node};

        next.phase = phase;

        next.since_start = since_start;

        next.since_end = since_end;

        return next;
    }};

    std::vector<std::pair<Node, bool>> out{};

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
            const auto ended{cap > 0 ? with(Phase::after, since_start, 0) : with(Phase::settled, 0, 0)};

            out.emplace_back(ended, false);
        }

        break;
    }

    case Phase::after:
    {
        const auto since_start{std::min(node.since_start + 1, cap)};

        const auto since_end{node.since_end + 1};

        if (!anchors(since_end + 1, since_start))
        {
            const auto later{since_end < cap ? with(Phase::after, since_start, since_end) : with(Phase::settled, 0, 0)};

            out.emplace_back(later, false);
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
        for (const auto& [target, inside] : graph.edges[at])
        {
            into[target].push_back(at);
        }
    }

    std::vector<bool> finishing(graph.nodes.size(), false);

    std::deque<Index_t> pending{};

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
            if (finishing[source])
            {
                continue;
            }

            finishing[source] = true;

            pending.push_back(source);
        }
    }

    return finishing;
}

bool Span_walk::has_inside_cycle(const Graph& graph, const std::vector<bool>& finishing)
{
    std::vector<Colour> colour(graph.nodes.size(), Colour::unseen);

    const auto inside_endable{[&finishing](const Edge& edge) { return edge.inside && finishing[edge.target]; }};

    for (Index_t root{0}; root < graph.nodes.size(); ++root)
    {
        if (!finishing[root] || colour[root] != Colour::unseen)
        {
            continue;
        }

        std::vector<std::pair<Index_t, std::size_t>> stack{{root, 0}};

        colour[root] = Colour::open;

        while (!stack.empty())
        {
            auto& [node, next]{stack.back()};

            const auto& out{graph.edges[node]};

            while (next < out.size() && !inside_endable(out[next]))
            {
                ++next;
            }

            if (next == out.size())
            {
                colour[node] = Colour::done;

                stack.pop_back();

                continue;
            }

            const auto [target, inside]{out[next]};

            ++next;

            if (colour[target] == Colour::open)
            {
                return true;
            }

            if (colour[target] != Colour::unseen)
            {
                continue;
            }

            colour[target] = Colour::open;

            stack.emplace_back(target, 0);
        }
    }

    return false;
}

std::size_t Span_walk::longest_stretch(const Graph& graph, const std::vector<bool>& finishing)
{
    std::vector<std::size_t> run(graph.nodes.size(), 0);

    const auto relax{[&graph, &finishing, &run](const Index_t at) {
        auto grew{false};

        for (const auto& [target, inside] : graph.edges[at])
        {
            if (!inside || !finishing[target] || run[at] + 1 <= run[target])
            {
                continue;
            }

            run[target] = run[at] + 1;

            grew = true;
        }

        return grew;
    }};

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (Index_t at{0}; at < graph.nodes.size(); ++at)
        {
            if (!finishing[at])
            {
                continue;
            }

            const auto grew{relax(at)};

            changed = changed || grew;
        }
    }

    return std::ranges::max(run);
}

} // namespace

std::optional<std::size_t> anchor_free_span(const Simulator& simulator)
{
    // The certified bytes as width-one windows at origin zero, which is what the general walk reduces to.
    std::vector<std::string> bytes{};

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<char>(value)};

        if (simulator.is_split_point(byte))
        {
            bytes.emplace_back(1, byte);
        }
    }

    std::vector<std::pair<std::string_view, std::size_t>> inventory{};

    inventory.reserve(bytes.size());

    for (const auto& window : bytes)
    {
        inventory.emplace_back(window, 0);
    }

    const Span_walk walk{simulator, inventory};

    return walk.decide(simulator);
}

std::optional<std::size_t> anchor_free_span(
        const Simulator& simulator, const std::span<const std::pair<std::string_view, std::size_t>> inventory)
{
    const Span_walk walk{simulator, inventory};

    return walk.decide(simulator);
}

} // namespace munch::dfa

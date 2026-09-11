#include "munch/dfa/anchor_free_span.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
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
 * @brief The walk behind anchor_free_span(): a guessed marking of the input read one byte at a time, beside a buffer
 *        of the positions no window has settled yet.
 *
 * The marking is the two-part position the other decisions carry, the run of the segment being read and the runs of
 * segments already closed, kept so that a later accept on one refutes the guessed close. A window anchors a position
 * inside its occurrence, at the origin, rather than at the byte just read, so a position's status is only settled
 * once the rest of the window has arrived; the buffer holds the last few bytes and a flag per position still
 * waiting, and a position leaves it anchored or not once no window can still reach back to it. The byte case is
 * this with a buffer of nothing.
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
     * @throws std::invalid_argument If a window is empty, uncertified at its stated origin, or too long to buffer.
     */
    Span_walk(const Simulator& simulator, std::span<const std::pair<std::string_view, std::size_t>> inventory);

    /**
     * @brief Decides the anchor-free span.
     *
     * A node from which no input can be completed is dropped first, since a stretch that never finishes is not a
     * stretch of any input; a cycle of anchor-free exits among the nodes that remain is exactly an unbounded
     * answer; and otherwise the longest run is a fixed point over an acyclic graph.
     * @param simulator The simulator whose tables the walk reads, the one the inventory was checked against.
     * @return The exact supremum, or std::nullopt when it is unbounded.
     */
    [[nodiscard]] std::optional<std::size_t> decide(const Simulator& simulator) const;

private:
    /**
     * @brief A node of the product graph: the marking's state and the buffer's.
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
         * @brief The last bytes read, as many as the longest window minus one, folded to the windows' alphabet.
         */
        std::string recent{};

        /**
         * @brief One bit per buffered position, set once some window anchors it; bit zero is the newest.
         */
        std::uint64_t flags{};

        /**
         * @brief How many positions the buffer holds, short of full only during warm-up.
         */
        std::uint8_t filled{};
    };

    /**
     * @brief What the oldest buffered position was when it left the buffer on an edge; nothing during warm-up.
     */
    enum class Exit : std::uint8_t
    {
        none,
        anchored,
        free
    };

    /**
     * @brief The product graph: each node's out-edges with the exit each carries.
     */
    using Edges_t = std::map<Node, std::vector<std::pair<Node, Exit>>>;

    /**
     * @brief Whether the token set matches any positive-length input at all.
     *
     * With no window every position is anchor-free, so the span is as long as inputs can be: unbounded whenever a
     * token matches, since a match repeats, and zero when nothing does.
     * @return True when some reachable state has a transition into an accepting one.
     */
    [[nodiscard]] static bool matches_any_token(const Simulator& simulator);

    /**
     * @brief The node every walk starts from: the initial state, nothing closed, an empty buffer.
     * @return That node.
     */
    [[nodiscard]] static Node start(const Simulator& simulator);

    /**
     * @brief Builds the product graph breadth first from the start node.
     * @return Every reachable node with its out-edges.
     */
    [[nodiscard]] Edges_t explore(const Simulator& simulator) const;

    /**
     * @brief Advances the marking by one byte, optionally closing the segment being read first.
     *
     * Closing first is what puts a window's origin at the start of a token, which is what the certificate says
     * about it. A closed run that reaches acceptance refutes the guess and kills the node; one with no transition
     * is simply forgotten.
     * @param node The node advanced from.
     * @param mark Whether the segment being read closes before the byte.
     * @param byte The byte read.
     * @return The advanced node, or std::nullopt where the reading run has no transition or a guess is refuted.
     */
    [[nodiscard]] static std::optional<Node> read(const Simulator& simulator, Node node, bool mark, unsigned char byte);

    /**
     * @brief Records the byte in the buffer, flags the positions the windows now anchor, and retires the oldest.
     * @param node The node just advanced, whose buffer still precedes the byte.
     * @param byte The byte read.
     * @return How the retired position left, or Exit::none while the buffer is still filling.
     */
    [[nodiscard]] Exit buffer(Node& node, unsigned char byte) const;

    /**
     * @brief The nodes an input can be finished from: those that reach a node whose reading run accepts.
     * @param edges The product graph.
     * @return Those nodes.
     */
    [[nodiscard]] static std::set<Node> endable(const Simulator& simulator, const Edges_t& edges);

    /**
     * @brief Whether the endable nodes carry a cycle of anchor-free exits, which is a stretch with no end.
     *
     * Walked with an explicit stack rather than by recursion, since the product graph is as deep as it is wide.
     * @param edges The product graph.
     * @param finishing The endable nodes.
     * @return True when such a cycle exists.
     */
    [[nodiscard]] static bool has_free_cycle(const Edges_t& edges, const std::set<Node>& finishing);

    /**
     * @brief The longest anchor-free run over an acyclic graph, relaxed to a fixed point.
     *
     * A warm-up edge retires nothing and carries the run along; an anchored exit resets it; a free one extends it.
     * A stretch may still be inside the buffer when the input ends, so a node whose reading run accepts also counts
     * what it holds: the anchor-free positions oldest inward, continuing the run that arrived, and the longest run
     * wholly inside.
     * @param edges The product graph.
     * @param finishing The endable nodes.
     * @return The supremum.
     */
    [[nodiscard]] static std::size_t longest_run(
            const Simulator& simulator, const Edges_t& edges, const std::set<Node>& finishing);

    /**
     * @brief What a node's buffer holds when the input ends there.
     * @param node The node.
     * @return The anchor-free positions from the oldest inward, and the longest anchor-free run wholly inside.
     */
    [[nodiscard]] static std::pair<std::size_t, std::size_t> held(const Node& node);

    /**
     * @brief The certified windows and their origins, the walk's own copy.
     */
    std::vector<std::pair<std::string, std::size_t>> inventory_;

    /**
     * @brief The longest window's width, which is one more than the buffer holds.
     */
    std::size_t longest_{0};

    /**
     * @brief Each byte as the buffer records it: itself when some window contains it, else one stand-in from
     *        outside the windows' alphabet, so the walk does not multiply its nodes by bytes no window can see.
     */
    std::array<char, Simulator::symbol_count> fold_{};
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

        longest_ = std::max(longest_, window.size());

        present.insert(window.begin(), window.end());
    }

    // The per-position flags ride in one word, so the buffer holds as many positions as it has bits.
    if (longest_ > std::numeric_limits<std::uint64_t>::digits)
    {
        throw std::invalid_argument{"anchor_free_span: a window is longer than this walk can buffer"};
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
}

std::optional<std::size_t> Span_walk::decide(const Simulator& simulator) const
{
    if (inventory_.empty())
    {
        return matches_any_token(simulator) ? std::nullopt : std::optional<std::size_t>{0};
    }

    const auto edges{explore(simulator)};

    const auto finishing{endable(simulator, edges)};

    if (!finishing.contains(start(simulator)))
    {
        return 0; // no input is tokenizable at all, so no stretch exists
    }

    if (has_free_cycle(edges, finishing))
    {
        return std::nullopt;
    }

    return longest_run(simulator, edges, finishing);
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

Span_walk::Node Span_walk::start(const Simulator& simulator)
{
    return {.reading = simulator.init_state(), .closed = {}, .recent = {}, .flags = 0, .filled = 0};
}

Span_walk::Edges_t Span_walk::explore(const Simulator& simulator) const
{
    Edges_t edges{{start(simulator), {}}};

    std::deque<Node> frontier{start(simulator)};

    while (!frontier.empty())
    {
        const auto at{frontier.front()};

        frontier.pop_front();

        for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
        {
            for (const auto mark : {false, true})
            {
                if (mark && !simulator.is_accepting(at.reading))
                {
                    continue;
                }

                auto next{read(simulator, at, mark, static_cast<unsigned char>(value))};

                if (!next)
                {
                    continue;
                }

                const auto exit{buffer(*next, static_cast<unsigned char>(value))};

                edges[at].emplace_back(*next, exit);

                if (const auto [entry, added]{edges.try_emplace(*next)}; added)
                {
                    frontier.push_back(*next);
                }
            }
        }
    }

    return edges;
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

Span_walk::Exit Span_walk::buffer(Node& node, const unsigned char byte) const
{
    const auto seen{node.recent + fold_[byte]};

    node.flags <<= 1U;

    ++node.filled;

    for (const auto& [window, origin] : inventory_)
    {
        if (seen.size() >= window.size() && seen.compare(seen.size() - window.size(), window.size(), window) == 0)
        {
            if (const auto age{window.size() - 1 - origin}; age < node.filled)
            {
                node.flags |= std::uint64_t{1} << age;
            }
        }
    }

    auto exit{Exit::none};

    if (node.filled > longest_ - 1)
    {
        const auto oldest{static_cast<std::uint64_t>(node.filled) - 1};

        exit = ((node.flags >> oldest) & 1U) != 0 ? Exit::anchored : Exit::free;

        node.flags &= ~(std::uint64_t{1} << oldest);

        --node.filled;
    }

    node.recent = longest_ > 1 ? seen.substr(seen.size() - std::min(seen.size(), longest_ - 1)) : std::string{};

    return exit;
}

std::set<Span_walk::Node> Span_walk::endable(const Simulator& simulator, const Edges_t& edges)
{
    std::set<Node> finishing;

    for (const auto& node : edges | std::views::keys)
    {
        if (simulator.is_accepting(node.reading))
        {
            finishing.insert(node);
        }
    }

    for (bool growing{true}; growing;)
    {
        growing = false;

        for (const auto& [node, out] : edges)
        {
            if (finishing.contains(node))
            {
                continue;
            }

            if (std::ranges::any_of(out, [&finishing](const auto& edge) { return finishing.contains(edge.first); }))
            {
                finishing.insert(node);

                growing = true;
            }
        }
    }

    return finishing;
}

bool Span_walk::has_free_cycle(const Edges_t& edges, const std::set<Node>& finishing)
{
    std::map<Node, int> colour;

    for (const auto& root : finishing)
    {
        if (colour[root] != 0)
        {
            continue;
        }

        std::vector<std::pair<Node, std::size_t>> stack{{root, 0}};

        colour[root] = 1;

        while (!stack.empty())
        {
            auto& [node, next]{stack.back()};

            const auto& out{edges.at(node)};

            while (next < out.size() && (out[next].second != Exit::free || !finishing.contains(out[next].first)))
            {
                ++next;
            }

            if (next == out.size())
            {
                colour[node] = 2;

                stack.pop_back();

                continue;
            }

            const auto& target{out[next++].first};

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

std::size_t Span_walk::longest_run(const Simulator& simulator, const Edges_t& edges, const std::set<Node>& finishing)
{
    std::map<Node, std::size_t> run;

    for (const auto& node : finishing)
    {
        run[node] = 0;
    }

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (const auto& node : finishing)
        {
            for (const auto& [target, exit] : edges.at(node))
            {
                if (!finishing.contains(target))
                {
                    continue;
                }

                const auto value{
                        exit == Exit::anchored ? std::size_t{0} :
                        exit == Exit::none     ? run[node] :
                                                 run[node] + 1};

                if (value > run[target])
                {
                    run[target] = value;

                    changed = true;
                }
            }
        }
    }

    auto best{std::ranges::max(run | std::views::values)};

    for (const auto& node : finishing)
    {
        if (!simulator.is_accepting(node.reading))
        {
            continue;
        }

        const auto [oldest_contiguous, inside]{held(node)};

        best = std::max({best, run.at(node) + oldest_contiguous, inside});
    }

    return best;
}

std::pair<std::size_t, std::size_t> Span_walk::held(const Node& node)
{
    std::size_t oldest_contiguous{0};

    for (auto age{node.filled}; age > 0; --age)
    {
        if (((node.flags >> (age - 1)) & 1U) != 0)
        {
            break;
        }

        ++oldest_contiguous;
    }

    std::size_t inside{0};

    std::size_t current{0};

    for (std::uint8_t age{0}; age < node.filled; ++age)
    {
        current = ((node.flags >> age) & 1U) != 0 ? 0 : current + 1;

        inside = std::max(inside, current);
    }

    return {oldest_contiguous, inside};
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

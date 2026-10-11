#include "munch/verifier/trie_chain.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::verifier
{
namespace
{
/**
 * @brief A node of a trie, by its index; the root is zero.
 */
using Node_t = std::size_t;

/**
 * @brief The mode of a run: which trie it reads.
 */
enum class Mode : std::uint8_t
{
    /**
     * @brief The initial tokens, the first segment's.
     */
    initial,

    /**
     * @brief The continuation tokens, every later segment's.
     */
    continuation
};

/**
 * @brief A run of the scan: the node of its mode's trie it has reached.
 */
struct Run
{
    /**
     * @brief Ordered member by member, so states key the map of interned ones.
     */
    auto operator<=>(const Run&) const = default;

    /**
     * @brief The mode, which trie the run reads.
     */
    Mode mode{};

    /**
     * @brief The node reached.
     */
    Node_t node{};
};

/**
 * @brief One boundary of a tail's chain: its depth inside the tail, the mode of the run begun there, and the node the
 *        rest of the tail reads to in that mode, if the trie carries it.
 */
struct Link
{
    /**
     * @brief The depth inside the tail.
     */
    std::size_t boundary{};

    /**
     * @brief The mode of the run begun at the boundary.
     */
    Mode mode{};

    /**
     * @brief The node the rest of the tail reads to from the mode's root, or std::nullopt when a byte has no child.
     */
    std::optional<Node_t> node{};
};

/**
 * @brief A node of a trie: its children by byte, whether it ends a token, the bytes read to it, and its chain when it
 *        is a pending tail, a node other than the root with a child.
 */
struct Node
{
    /**
     * @brief The children, by the byte read to each.
     */
    std::map<unsigned char, Node_t> children{};

    /**
     * @brief Whether the node ends a token.
     */
    bool accepting{};

    /**
     * @brief The bytes read from the root to the node.
     */
    std::string spelling{};

    /**
     * @brief The chain of the node as a tail: the scanner's reading of its spelling, one link per boundary, the first
     *        at depth zero.
     */
    std::vector<Link> chain{};
};

/**
 * @brief The trie of one mode's tokens, the root first.
 */
using Trie = std::vector<Node>;

/**
 * @brief The two tries, the initial tokens' then the continuation tokens'.
 */
using Tries = std::array<Trie, 2>;

/**
 * @brief A state of the verifier: the oldest armed run, whose node spells the tail, and the depth inside the tail at
 *        which the unarmed run began, zero when nothing is armed and the run is the unarmed run itself.
 */
struct State
{
    /**
     * @brief Ordered member by member, so states key the map of interned ones.
     */
    auto operator<=>(const State&) const = default;

    /**
     * @brief The oldest armed run, or the unarmed run when nothing is armed.
     */
    Run tail{};

    /**
     * @brief The depth inside the tail at which the unarmed run began.
     */
    std::size_t began{};
};

/**
 * @brief A state read as the runs it carries: the armed runs, oldest first, and the unarmed run.
 */
struct Runs
{
    /**
     * @brief The armed runs, oldest first.
     */
    std::vector<Run> armed{};

    /**
     * @brief The unarmed run.
     */
    Run unarmed{};
};

/**
 * @brief Returns the child of a node on a byte.
 * @param node The node.
 * @param byte The byte.
 * @return The child, or std::nullopt when the node has none on the byte.
 */
[[nodiscard]] std::optional<Node_t> child_of(const Node& node, const unsigned char byte)
{
    const auto found{node.children.find(byte)};

    if (found == node.children.cend())
    {
        return std::nullopt;
    }

    return found->second;
}

/**
 * @brief Adds a token to a trie, a node per byte of it past the nodes already there.
 * @param trie The trie, extended in place.
 * @param token The token.
 */
void add_token(Trie& trie, const std::string_view token)
{
    Node_t node{0};

    for (const auto byte : token)
    {
        const auto [edge, inserted]{trie[node].children.try_emplace(static_cast<unsigned char>(byte), trie.size())};

        const auto child{edge->second};

        if (inserted)
        {
            trie.push_back({.spelling = trie[node].spelling + byte});
        }

        node = child;
    }

    trie[node].accepting = true;
}

/**
 * @brief Builds the trie of one mode's tokens.
 * @param tokens The tokens.
 * @param mode_name The mode's name, for the refusal.
 * @return The trie.
 * @throws std::invalid_argument If a token is empty.
 */
[[nodiscard]] Trie build_trie(const std::span<const std::string> tokens, const std::string_view mode_name)
{
    Trie trie{Node{}};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        if (tokens[index].empty())
        {
            throw std::invalid_argument{std::format("trie_chain: the {} token at index {} is empty", mode_name, index)};
        }

        add_token(trie, tokens[index]);
    }

    return trie;
}

/**
 * @brief Returns the node a text reads to from the root of a trie.
 * @param trie The trie.
 * @param text The text.
 * @return The node, or std::nullopt when a byte has no child.
 */
[[nodiscard]] std::optional<Node_t> walk(const Trie& trie, const std::string_view text)
{
    std::optional<Node_t> node{0};

    for (std::size_t index{0}; index < text.size() && node; ++index)
    {
        node = child_of(trie[*node], static_cast<unsigned char>(text[index]));
    }

    return node;
}

/**
 * @brief Returns the length of the longest token of a trie that is a prefix of a text.
 * @param trie The trie.
 * @param text The text.
 * @return The length, zero when no token is a prefix.
 */
[[nodiscard]] std::size_t longest(const Trie& trie, const std::string_view text)
{
    std::size_t best{0};

    std::optional<Node_t> node{0};

    for (std::size_t index{0}; index < text.size(); ++index)
    {
        node = child_of(trie[*node], static_cast<unsigned char>(text[index]));

        if (!node)
        {
            return best;
        }

        if (trie[*node].accepting)
        {
            best = index + 1;
        }
    }

    return best;
}

/**
 * @brief The trie chain under exploration: the two tries, the state each verifier state reads as, the transitions and
 *        accepting states found so far, and the interned states with their verifier states.
 */
class Exploration
{
public:
    /**
     * @brief Builds the two tries and reads the chain of every pending tail, nothing explored yet.
     * @param initial The initial tokens.
     * @param continuation The continuation tokens.
     * @throws std::invalid_argument If a token is empty.
     */
    Exploration(std::span<const std::string> initial, std::span<const std::string> continuation);

    /**
     * @brief Explores every state reachable from the initial trie's root and returns the verifier they make.
     * @return The verifier, its start the initial trie's root.
     */
    [[nodiscard]] Verifier run() &&;

private:
    /**
     * @brief Reads the chain of every pending tail of both tries, a node other than the root with a child.
     */
    void read_chains();

    /**
     * @brief Reads a tail as the scanner reads it: a link at depth zero in the tail's mode, then one at the longest
     *        token of the mode from there, in continuation mode from the second link on, until no token matches.
     * @param mode The tail's mode.
     * @param tail The tail.
     * @return The chain.
     */
    [[nodiscard]] std::vector<Link> chain_of(Mode mode, std::string_view tail) const;

    /**
     * @brief Returns the trie a mode reads.
     * @param mode The mode.
     * @return The mode's trie.
     */
    [[nodiscard]] const Trie& trie_of(Mode mode) const noexcept;

    /**
     * @brief Returns the verifier state of a state, interning it as a new one when it is first found.
     * @param state The state.
     * @return The verifier state.
     */
    [[nodiscard]] Verifier::State_t intern(const State& state);

    /**
     * @brief Returns the runs a state carries: with nothing armed the unarmed run alone, and otherwise the chain's runs
     *        before the unarmed run's boundary that are alive and may grow, armed, with the run begun at that boundary.
     * @param state The state.
     * @return The runs.
     */
    [[nodiscard]] Runs runs_of(const State& state) const;

    /**
     * @brief Returns the node of a run.
     * @param run The run.
     * @return The node.
     */
    [[nodiscard]] const Node& node_of(Run run) const noexcept;

    /**
     * @brief Reads every byte the unarmed run has a child on from one verifier state, with and without a boundary after
     *        it, interning the states reached.
     * @param from The verifier state read from.
     */
    void expand(Verifier::State_t from);

    /**
     * @brief Reads a byte into every run: the unarmed run steps to its child, an armed run steps to its child or drops.
     * @param from The runs read from.
     * @param byte The byte.
     * @return The runs after the byte, or std::nullopt when the unarmed run has no child or an armed run reaches a
     *         token.
     */
    [[nodiscard]] std::optional<Runs> read(const Runs& from, unsigned char byte) const;

    /**
     * @brief Records a transition from a verifier state on a marked symbol to the state the runs make.
     * @param from The verifier state read from.
     * @param symbol The marked symbol.
     * @param to The runs reached.
     */
    void record(Verifier::State_t from, Marked symbol, const Runs& to);

    /**
     * @brief Returns the state the runs make: the oldest armed run with the depth of the unarmed run's start inside it,
     *        or the unarmed run alone.
     * @param runs The runs.
     * @return The state.
     */
    [[nodiscard]] State state_of(const Runs& runs) const;

    /**
     * @brief Closes the unarmed run at a boundary: it joins the armed runs when a longer token extends it, and a fresh
     *        unarmed run begins at the continuation trie's root.
     * @param runs The runs, closed in a copy.
     * @return The runs after the boundary, or std::nullopt when the unarmed run is not at a token.
     */
    [[nodiscard]] std::optional<Runs> close(Runs runs) const;

    /**
     * @brief The two tries.
     */
    Tries tries_{};

    /**
     * @brief The state each verifier state reads as, by verifier state.
     */
    std::vector<State> sources_{};

    /**
     * @brief The transitions found so far.
     */
    Verifier::Transitions_t transitions_{};

    /**
     * @brief The accepting states found so far.
     */
    Verifier::Accept_states_t accept_states_{};

    /**
     * @brief The interned states with their verifier states.
     */
    std::map<State, Verifier::State_t> interned_{};
};

Exploration::Exploration(const std::span<const std::string> initial, const std::span<const std::string> continuation)
    : tries_{build_trie(initial, "initial"), build_trie(continuation, "continuation")}
{
    read_chains();
}

void Exploration::read_chains()
{
    for (const auto mode : {Mode::initial, Mode::continuation})
    {
        auto& trie{tries_[std::to_underlying(mode)]};

        for (Node_t node{1}; node < trie.size(); ++node)
        {
            if (trie[node].children.empty())
            {
                continue;
            }

            trie[node].chain = chain_of(mode, trie[node].spelling);
        }
    }
}

std::vector<Link> Exploration::chain_of(const Mode mode, const std::string_view tail) const
{
    const auto& first{trie_of(mode)};

    std::vector<Link> chain{{.boundary = 0, .mode = mode, .node = walk(first, tail)}};

    const auto& trie{trie_of(Mode::continuation)};

    for (auto step{longest(first, tail)}; step != 0; step = longest(trie, tail.substr(chain.back().boundary)))
    {
        const auto boundary{chain.back().boundary + step};

        chain.push_back({.boundary = boundary, .mode = Mode::continuation, .node = walk(trie, tail.substr(boundary))});
    }

    return chain;
}

const Trie& Exploration::trie_of(const Mode mode) const noexcept
{
    return tries_[std::to_underlying(mode)];
}

Verifier Exploration::run() &&
{
    const auto start{intern({.tail = {.mode = Mode::initial, .node = 0}, .began = 0})};

    // The start accepts the empty input, which the scan consumes whole; no step returns to it.
    accept_states_.insert(start);

    for (auto state{start}; state < sources_.size(); ++state)
    {
        expand(state);
    }

    return Verifier{start, std::move(transitions_), std::move(accept_states_)};
}

Verifier::State_t Exploration::intern(const State& state)
{
    const auto [entry, inserted]{interned_.try_emplace(state, sources_.size())};

    const auto& [key, number]{*entry};

    if (!inserted)
    {
        return number;
    }

    if (node_of(runs_of(state).unarmed).accepting)
    {
        accept_states_.insert(number);
    }

    sources_.push_back(state);

    return number;
}

Runs Exploration::runs_of(const State& state) const
{
    if (state.began == 0)
    {
        return {.armed = {}, .unarmed = state.tail};
    }

    const auto& chain{node_of(state.tail).chain};

    const auto reached{std::ranges::find(chain, state.began, &Link::boundary)};

    Runs runs{.armed = {}, .unarmed = {.mode = reached->mode, .node = *reached->node}};

    for (const auto& link : std::ranges::subrange(chain.cbegin(), reached))
    {
        if (link.node && !trie_of(link.mode)[*link.node].children.empty())
        {
            runs.armed.push_back({.mode = link.mode, .node = *link.node});
        }
    }

    return runs;
}

const Node& Exploration::node_of(const Run run) const noexcept
{
    return trie_of(run.mode)[run.node];
}

void Exploration::expand(const Verifier::State_t from)
{
    const auto runs{runs_of(sources_[from])};

    for (const auto byte : std::views::keys(node_of(runs.unarmed).children))
    {
        const auto unmarked{read(runs, byte)};

        if (!unmarked)
        {
            continue;
        }

        record(from, {.byte = byte, .boundary_after = false}, *unmarked);

        const auto marked{close(*unmarked)};

        if (!marked)
        {
            continue;
        }

        record(from, {.byte = byte, .boundary_after = true}, *marked);
    }
}

std::optional<Runs> Exploration::read(const Runs& from, const unsigned char byte) const
{
    const auto unarmed{child_of(node_of(from.unarmed), byte)};

    if (!unarmed)
    {
        return std::nullopt;
    }

    Runs runs{.armed = {}, .unarmed = {.mode = from.unarmed.mode, .node = *unarmed}};

    for (const auto& run : from.armed)
    {
        const auto stepped{child_of(node_of(run), byte)};

        if (!stepped)
        {
            continue;
        }

        if (trie_of(run.mode)[*stepped].accepting)
        {
            return std::nullopt;
        }

        runs.armed.push_back({.mode = run.mode, .node = *stepped});
    }

    return runs;
}

void Exploration::record(const Verifier::State_t from, const Marked symbol, const Runs& to)
{
    const auto target{intern(state_of(to))};

    transitions_.emplace(Verifier::Key_t{from, symbol}, target);
}

State Exploration::state_of(const Runs& runs) const
{
    if (runs.armed.empty())
    {
        return {.tail = runs.unarmed, .began = 0};
    }

    const auto& oldest{runs.armed.front()};

    const auto began{node_of(oldest).spelling.size() - node_of(runs.unarmed).spelling.size()};

    return {.tail = oldest, .began = began};
}

std::optional<Runs> Exploration::close(Runs runs) const
{
    const auto& node{node_of(runs.unarmed)};

    if (!node.accepting)
    {
        return std::nullopt;
    }

    if (!node.children.empty())
    {
        runs.armed.push_back(runs.unarmed);
    }

    runs.unarmed = {.mode = Mode::continuation, .node = 0};

    return runs;
}

} // namespace

Verifier trie_chain(const std::span<const std::string> initial, const std::span<const std::string> continuation)
{
    Exploration exploration{initial, continuation};

    return std::move(exploration).run();
}

} // namespace munch::verifier

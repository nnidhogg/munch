/*
 * The smaller searches of paper/split-windows/split-windows.tex, run over the same token sets as the window gate whose
 * retained keys the paper's window table quotes, so the new columns of that table are computed beside the old one
 * rather than transcribed. Three things are decided here, and every figure the paper quotes is asserted:
 *
 *   1. The three-status quotient of the cloud: a live state is empty, single (exactly one in-window origin) or blocked
 *      (the pre-window origin or two in-window origins). Its breadth-first search retains the keys this program counts,
 *      under the gate's own stopping discipline, and its shortest length must equal the gate's.
 *   2. The chosen-origin search, the search munch 2.2 ships as shortest_split_window(): one origin is followed from its
 *      birth beside the support of every other hypothesis, and a window is certified when that origin outlives them
 *      all. The nodes it holds when its shortest window appears are the table's other new column.
 *   3. The long-window family of the paper's complexity section: m cycle sources of lengths 1 to m behind the
 *      reduction's header, whose shortest certified window is lcm(1..m) + m + 2 bytes long. The minima and the
 *      nodes the search holds when each appears are asserted, and each window is replayed through the plain cloud at
 *      the claimed origin and scanned by maximal munch to show that it is its own completely tokenizable occurrence.
 *
 * A fourth check decides the greedy-agreement class of the paper's structural section on its examples: {a, ab} and ab*
 * lie in the class, {a, ab, bb} does not, and the obstruction the test returns is replayed through the scan.
 */

#include <algorithm>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/label.hpp"
#include "munch/dfa/token.hpp"
#include "munch/dfa/unroll_start.hpp"

namespace
{
using namespace figures;

using munch::dfa::Dfa;

using State_t = Dfa::State_t;

/**
 * @brief A set of live states packed one bit each.
 */
using Bits_t = std::vector<std::uint64_t>;

/**
 * @brief The bits in one word of a state set.
 */
constexpr std::size_t word_bits{64};

/**
 * @brief The origin of a hypothesis that began before the window.
 */
constexpr std::size_t before_window{std::numeric_limits<std::size_t>::max()};

/**
 * @brief The first byte escaped() prints as itself.
 */
constexpr unsigned first_printable{32};

/**
 * @brief The last byte escaped() prints as itself.
 */
constexpr unsigned last_printable{126};

/**
 * @brief The marker a row's line ends with when a figure no longer matches the paper.
 */
constexpr std::string_view moved_marker{"   <- moved"};

/**
 * @brief The live automaton the searches run over: the trim states renumbered densely, transitions between live states
 *        only, one representative byte per class of bytes the live tables do not tell apart.
 */
struct Live
{
    /**
     * @brief Returns the live state a live transition leads to.
     * @param state The state.
     * @param byte The byte.
     * @return The target, or std::nullopt where no live transition leaves the state on the byte.
     */
    [[nodiscard]] std::optional<std::size_t> step(const std::size_t state, const unsigned char byte) const
    {
        const auto to{delta[state * byte_values + byte]};

        if (to == n)
        {
            return std::nullopt;
        }

        return to;
    }

    /**
     * @brief Returns the empty state set.
     * @return A set with room for every live state and none in it.
     */
    [[nodiscard]] Bits_t none() const
    {
        const auto words{(n + word_bits - 1) / word_bits};

        return Bits_t(words, 0);
    }

    /**
     * @brief Returns the set of every live state.
     * @return The set.
     */
    [[nodiscard]] Bits_t all() const;

    /**
     * @brief The live state count n.
     */
    std::size_t n{0};

    /**
     * @brief The dense index of the initial state.
     */
    std::size_t init{0};

    /**
     * @brief Whether a live transition re-enters the initial state, which disables the rename.
     */
    bool reentrant{false};

    /**
     * @brief Whether each live state accepts.
     */
    std::vector<bool> accepting{};

    /**
     * @brief The live transition table, n rows of 256 entries, n meaning none.
     */
    std::vector<std::size_t> delta{};

    /**
     * @brief One byte per class.
     */
    std::vector<unsigned char> bytes{};
};

/**
 * @brief What a search found: the shortest certified length, or zero when the quotient was exhausted, with the keys or
 *        nodes it held, and for the chosen-origin search the window and its origin.
 */
struct Found
{
    /**
     * @brief The shortest certified length, zero when none was found.
     */
    std::size_t shortest{0};

    /**
     * @brief The keys or nodes the search held.
     */
    std::size_t held{0};

    /**
     * @brief The shortest window, for the chosen-origin search.
     */
    std::string window{};

    /**
     * @brief The offset in the window of the origin it certifies.
     */
    std::size_t origin{0};
};

/**
 * @brief A key of the three-status quotient: the single states, then the blocked states.
 */
using Key_t = std::pair<Bits_t, Bits_t>;

/**
 * @brief A node of the chosen-origin search: the chosen origin's state once chosen, and the other hypotheses' states.
 */
struct Node
{
    /**
     * @brief Equal when the chosen state and the other states match.
     */
    bool operator==(const Node&) const = default;

    /**
     * @brief The chosen origin's state, or std::nullopt before the choice.
     */
    std::optional<std::size_t> chosen{};

    /**
     * @brief The states of every other hypothesis.
     */
    Bits_t others{};
};

/**
 * @brief Hashes a chosen-origin node for the search's index.
 */
struct Node_hash
{
    /**
     * @brief Combines the hashes of the other states' words and of the chosen state.
     * @param node The node.
     * @return The hash.
     */
    std::size_t operator()(const Node& node) const noexcept
    {
        auto seed{boost::hash_range(node.others.cbegin(), node.others.cend())};

        boost::hash_combine(seed, node.chosen);

        return seed;
    }
};

/**
 * @brief How the chosen-origin search reached a node.
 */
struct Reached
{
    /**
     * @brief The node it was reached from.
     */
    std::size_t parent{0};

    /**
     * @brief The byte read.
     */
    unsigned char byte{0};

    /**
     * @brief Whether the step chose the fresh origin.
     */
    bool chose{false};
};

/**
 * @brief A state of the greedy-agreement product: the first run's state, the second run's state, and whether a byte has
 *        been read, so a root is distinct from the same pair reached again after a nonempty path.
 */
struct Product_state
{
    /**
     * @brief Orders states member by member, so that they can key a map.
     */
    auto operator<=>(const Product_state&) const = default;

    /**
     * @brief The first run's state, continuing the token automaton from an accepting state.
     */
    std::size_t first{0};

    /**
     * @brief The second run's state, parsing a prefix of X*.
     */
    std::size_t second{0};

    /**
     * @brief Whether a byte has been read.
     */
    bool started{false};
};

/**
 * @brief How the greedy-agreement product reached a state.
 */
struct Product_parent
{
    /**
     * @brief The state it was reached from.
     */
    Product_state from{};

    /**
     * @brief The byte read.
     */
    char byte{0};
};

/**
 * @brief A greedy-agreement obstruction: an accepted word whose displayed factorization begins with an accepted u.
 */
struct Obstruction
{
    /**
     * @brief The factorization's first factor u.
     */
    std::string first_factor{};

    /**
     * @brief The obstruction urz.
     */
    std::string word{};
};

/**
 * @brief A row of the paper's window table: the gate's shortest length, and the two counts this program pins.
 */
struct Row
{
    /**
     * @brief The row's name.
     */
    std::string_view name{};

    /**
     * @brief The gate's shortest certified length.
     */
    std::size_t shortest{0};

    /**
     * @brief The three-status quotient's retained keys.
     */
    std::size_t keys{0};

    /**
     * @brief The chosen-origin search's held nodes.
     */
    std::size_t nodes{0};
};

/**
 * @brief Returns whether a state is in a set.
 * @param bits The set.
 * @param at The state.
 * @return Whether its bit is set.
 */
bool contains(const Bits_t& bits, const std::size_t at)
{
    const auto word{bits[at / word_bits]};

    const auto shifted{word >> (at % word_bits)};

    return (shifted & 1U) != 0;
}

/**
 * @brief Adds a state to a set.
 * @param bits The set.
 * @param at The state.
 */
void insert(Bits_t& bits, const std::size_t at)
{
    const auto bit{std::uint64_t{1} << (at % word_bits)};

    bits[at / word_bits] |= bit;
}

/**
 * @brief Returns whether a set holds no state.
 * @param bits The set.
 * @return Whether every word is zero.
 */
bool empty(const Bits_t& bits)
{
    return std::ranges::all_of(bits, [](const std::uint64_t word) { return word == 0; });
}

/**
 * @brief Counts the states in a set.
 * @param bits The set.
 * @return The states in it.
 */
std::size_t count(const Bits_t& bits)
{
    std::size_t total{0};

    for (const auto word : bits)
    {
        total += static_cast<std::size_t>(std::popcount(word));
    }

    return total;
}

Bits_t Live::all() const
{
    auto bits{none()};

    for (std::size_t state{0}; state < n; ++state)
    {
        insert(bits, state);
    }

    return bits;
}

/**
 * @brief Collects the states reachable from the initial state, breadth first.
 * @param dfa The automaton.
 * @return The reachable states.
 */
std::set<State_t> reachable_states(const Dfa& dfa)
{
    std::set reachable{dfa.init_state()};

    std::deque pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < byte_values; ++symbol)
        {
            const auto next{dfa.advance(state, static_cast<char>(symbol))};

            if (!next || reachable.contains(*next))
            {
                continue;
            }

            reachable.insert(*next);

            pending.push_back(*next);
        }
    }

    return reachable;
}

/**
 * @brief Collects the reachable states from which acceptance is reachable, by closing the accepting ones backwards.
 * @param dfa The automaton.
 * @param reachable The reachable states.
 * @return The live states.
 */
std::set<State_t> co_accessible_states(const Dfa& dfa, const std::set<State_t>& reachable)
{
    std::set<State_t> co_accessible{};

    for (const auto state : reachable)
    {
        if (dfa.has_accept_token(state))
        {
            co_accessible.insert(state);
        }
    }

    const auto steps_into_co_accessible{[&](const State_t state) {
        for (int symbol{0}; symbol < byte_values; ++symbol)
        {
            const auto next{dfa.advance(state, static_cast<char>(symbol))};

            if (next && co_accessible.contains(*next))
            {
                return true;
            }
        }

        return false;
    }};

    for (auto grown{true}; grown;)
    {
        grown = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state) || !steps_into_co_accessible(state))
            {
                continue;
            }

            co_accessible.insert(state);

            grown = true;
        }
    }

    return co_accessible;
}

/**
 * @brief Trims the automaton to its live states, reachable from the initial state and co-accessible to acceptance.
 * @param dfa The automaton.
 * @return The live automaton, empty when the initial state is not live.
 */
Live live_of(const Dfa& dfa)
{
    const auto reachable{reachable_states(dfa)};

    const auto co_accessible{co_accessible_states(dfa, reachable)};

    Live live{};

    std::map<State_t, std::size_t> index{};

    for (const auto state : co_accessible)
    {
        index[state] = live.n;

        ++live.n;
    }

    if (!index.contains(dfa.init_state()))
    {
        return Live{};
    }

    live.init = index.at(dfa.init_state());

    live.accepting.assign(live.n, false);

    live.delta.assign(live.n * byte_values, live.n);

    for (const auto& [state, at] : index)
    {
        live.accepting[at] = dfa.has_accept_token(state).has_value();

        for (int symbol{0}; symbol < byte_values; ++symbol)
        {
            const auto next{dfa.advance(state, static_cast<char>(symbol))};

            if (!next || !index.contains(*next))
            {
                continue;
            }

            const auto target{index.at(*next)};

            live.delta[at * byte_values + static_cast<std::size_t>(symbol)] = target;

            live.reentrant = live.reentrant || target == live.init;
        }
    }

    std::set<std::vector<std::size_t>> signatures{};

    for (int symbol{0}; symbol < byte_values; ++symbol)
    {
        std::vector<std::size_t> signature{};

        for (std::size_t state{0}; state < live.n; ++state)
        {
            signature.push_back(live.delta[state * byte_values + static_cast<std::size_t>(symbol)]);
        }

        const auto [slot, inserted]{signatures.insert(signature)};

        if (inserted)
        {
            live.bytes.push_back(static_cast<unsigned char>(symbol));
        }
    }

    return live;
}

/**
 * @brief Runs the plain cloud of the paper's model over a window and returns the origin every surviving hypothesis
 *        agrees on.
 * @param live The live automaton.
 * @param window The window.
 * @return The agreed in-window origin, or std::nullopt when the hypotheses die out or disagree.
 */
std::optional<std::size_t> cloud_origin(const Live& live, const std::string& window)
{
    using Hypothesis_t = std::pair<std::size_t, std::size_t>;

    std::set<Hypothesis_t> cloud{};

    for (std::size_t state{0}; state < live.n; ++state)
    {
        cloud.emplace(state, before_window);
    }

    const auto is_accepting{[&live](const Hypothesis_t& hypothesis) {
        const auto& [state, origin]{hypothesis};

        return live.accepting[state];
    }};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        const auto accepting{std::ranges::any_of(cloud, is_accepting)};

        std::set<Hypothesis_t> next{};

        for (const auto& [state, origin] : cloud)
        {
            const auto to{live.step(state, byte)};

            if (!to)
            {
                continue;
            }

            const auto renamed{state == live.init && !live.reentrant};

            next.emplace(*to, renamed ? at : origin);
        }

        const auto fresh{accepting ? live.step(live.init, byte) : std::nullopt};

        if (fresh)
        {
            next.emplace(*fresh, at);
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    const auto& [first_state, origin]{*cloud.begin()};

    const auto shares_origin{[origin](const Hypothesis_t& hypothesis) {
        const auto& [state, other_origin]{hypothesis};

        return other_origin == origin;
    }};

    const auto unanimous{std::ranges::all_of(cloud, shares_origin)};

    if (!unanimous || origin == before_window)
    {
        return std::nullopt;
    }

    return origin;
}

/**
 * @brief Runs the maximal-munch scan over an input.
 * @param live The live automaton.
 * @param input The input.
 * @return The token starts, or std::nullopt when the input is not completely tokenizable.
 */
std::optional<std::vector<std::size_t>> token_starts(const Live& live, const std::string& input)
{
    std::vector<std::size_t> starts{};

    const auto longest_from{[&](const std::size_t at) {
        std::optional<std::size_t> last{};

        auto state{live.init};

        for (auto read{at}; read < input.size(); ++read)
        {
            const auto to{live.step(state, static_cast<unsigned char>(input[read]))};

            if (!to)
            {
                break;
            }

            state = *to;

            if (live.accepting[state])
            {
                last = read + 1;
            }
        }

        return last;
    }};

    for (std::size_t at{0}; at < input.size();)
    {
        starts.push_back(at);

        const auto last{longest_from(at)};

        if (!last)
        {
            return std::nullopt;
        }

        at = *last;
    }

    return starts;
}

/**
 * @brief Steps a key of the three-status quotient on one byte.
 *
 * Every live state moves on, the initial state too unless it is renamed; a blocked state's successor is blocked, and a
 * state reached from two single states is blocked too. Where the key accepts or renames, a fresh origin starts at the
 * initial state.
 * @param live The live automaton.
 * @param key The key.
 * @param byte The byte.
 * @return The successor key.
 */
Key_t next_key(const Live& live, const Key_t& key, const unsigned char byte)
{
    const auto& [single, blocked]{key};

    const auto accepting_member{[&](const std::size_t state) {
        const auto member{contains(single, state) || contains(blocked, state)};

        return member && live.accepting[state];
    }};

    const auto states{std::views::iota(std::size_t{0}, live.n)};

    const auto accepting{std::ranges::any_of(states, accepting_member)};

    const auto renamed{!live.reentrant && contains(blocked, live.init)};

    std::vector<unsigned> arrivals(live.n, 0);

    std::vector<bool> blocked_arrival(live.n, false);

    for (std::size_t state{0}; state < live.n; ++state)
    {
        if (state == live.init && renamed)
        {
            continue;
        }

        const auto to{live.step(state, byte)};

        if (!to)
        {
            continue;
        }

        if (contains(blocked, state))
        {
            blocked_arrival[*to] = true;

            continue;
        }

        if (contains(single, state))
        {
            ++arrivals[*to];
        }
    }

    const auto fresh{accepting || renamed ? live.step(live.init, byte) : std::nullopt};

    if (fresh)
    {
        ++arrivals[*fresh];
    }

    Key_t next{live.none(), live.none()};

    auto& [next_single, next_blocked]{next};

    for (std::size_t state{0}; state < live.n; ++state)
    {
        if (blocked_arrival[state] || arrivals[state] >= 2)
        {
            insert(next_blocked, state);

            continue;
        }

        if (arrivals[state] == 1)
        {
            insert(next_single, state);
        }
    }

    return next;
}

/**
 * @brief Searches the three-status quotient breadth first.
 *
 * The stopping discipline is the gate's: once a length has certified, deeper words are not expanded, a certified
 * successor is never retained as a key, and the keys held when the queue drains are the count. The initial key is every
 * live state blocked, since every one carries the pre-window origin.
 * @param live The live automaton.
 * @return The shortest certified length and the keys held.
 */
Found three_status(const Live& live)
{
    Found found{};

    std::map<Key_t, std::size_t> seen{};

    std::deque<std::pair<Key_t, std::size_t>> queue{};

    const Key_t start{live.none(), live.all()};

    seen.emplace(start, 0);

    queue.emplace_back(start, 0);

    while (!queue.empty())
    {
        const auto [key, depth]{queue.front()};

        queue.pop_front();

        if (found.shortest != 0 && depth >= found.shortest)
        {
            continue;
        }

        for (const auto byte : live.bytes)
        {
            const auto next{next_key(live, key, byte)};

            const auto& [single, blocked]{next};

            if (empty(single) && empty(blocked))
            {
                continue;
            }

            if (empty(blocked) && count(single) == 1)
            {
                if (found.shortest == 0)
                {
                    found.shortest = depth + 1;
                }

                continue;
            }

            if (seen.contains(next))
            {
                continue;
            }

            seen.emplace(next, depth + 1);

            queue.emplace_back(next, depth + 1);
        }
    }

    found.held = seen.size();

    return found;
}

/**
 * @brief Lists the nodes one byte leads to, as shortest_split_window() advances them: before the choice the fresh
 *        origin may be chosen or declined; after it the chosen origin must survive and share a state with no other.
 * @param live The live automaton.
 * @param node The node.
 * @param byte The byte.
 * @return The successors, each with whether its step chose the fresh origin.
 */
std::vector<std::pair<Node, bool>> successors(const Live& live, const Node& node, const unsigned char byte)
{
    auto accepting{node.chosen && live.accepting[*node.chosen]};

    const auto renamed{!live.reentrant && !node.chosen && contains(node.others, live.init)};

    auto moved{live.none()};

    for (std::size_t state{0}; state < live.n; ++state)
    {
        if (!contains(node.others, state))
        {
            continue;
        }

        accepting = accepting || live.accepting[state];

        if (state == live.init && renamed)
        {
            continue;
        }

        const auto to{live.step(state, byte)};

        if (to)
        {
            insert(moved, *to);
        }
    }

    const auto fresh{accepting || renamed ? live.step(live.init, byte) : std::nullopt};

    std::vector<std::pair<Node, bool>> out{};

    if (node.chosen)
    {
        const auto chosen{live.step(*node.chosen, byte)};

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
            out.emplace_back(Node{.chosen = chosen, .others = std::move(moved)}, false);
        }

        return out;
    }

    if (fresh && !contains(moved, *fresh))
    {
        out.emplace_back(Node{.chosen = fresh, .others = moved}, true);
    }

    if (fresh)
    {
        insert(moved, *fresh);
    }

    if (!empty(moved))
    {
        out.emplace_back(Node{.chosen = std::nullopt, .others = std::move(moved)}, false);
    }

    return out;
}

/**
 * @brief Runs the chosen-origin search breadth first, returning at the first certified node with the nodes held then.
 * @param live The live automaton.
 * @return The shortest window, its origin and the nodes held, or the nodes held when the search is exhausted.
 */
Found chosen_origin(const Live& live)
{
    std::unordered_map<Node, std::size_t, Node_hash> index{};

    std::vector<Node> nodes{};

    std::vector<Reached> reached{};

    const Node start{.chosen = std::nullopt, .others = live.all()};

    index.emplace(start, 0);

    nodes.push_back(start);

    reached.push_back(Reached{.parent = 0, .byte = 0, .chose = false});

    // Returns whether the node is new and certified.
    const auto add{[&](Node next, const std::size_t at, const unsigned char byte, const bool chose) {
        if (index.contains(next))
        {
            return false;
        }

        const auto certified{next.chosen && empty(next.others)};

        index.emplace(next, nodes.size());

        nodes.push_back(std::move(next));

        reached.push_back(Reached{.parent = at, .byte = byte, .chose = chose});

        return certified;
    }};

    const auto found_at_last{[&] {
        Found found{};

        std::size_t chosen_at{0};

        for (auto walk{nodes.size() - 1}; walk != 0; walk = reached[walk].parent)
        {
            const auto& [parent, byte, chose]{reached[walk]};

            found.window.push_back(static_cast<char>(byte));

            if (chose)
            {
                chosen_at = found.window.size();
            }
        }

        std::ranges::reverse(found.window);

        found.shortest = found.window.size();

        found.origin = found.window.size() - chosen_at;

        found.held = nodes.size();

        return found;
    }};

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        for (const auto byte : live.bytes)
        {
            for (auto& [next, chose] : successors(live, nodes[at], byte))
            {
                if (add(std::move(next), at, byte, chose))
                {
                    return found_at_last();
                }
            }
        }
    }

    return Found{.shortest = 0, .held = nodes.size(), .window = {}, .origin = 0};
}

/**
 * @brief Finds a shortest word from the initial state to each live state, breadth first.
 * @param live The live automaton.
 * @return Per state, the word, or std::nullopt for none.
 */
std::vector<std::optional<std::string>> shortest_from_init(const Live& live)
{
    std::vector<std::optional<std::string>> from_init(live.n);

    from_init[live.init] = "";

    std::deque pending{live.init};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (const auto byte : live.bytes)
        {
            const auto to{live.step(state, byte)};

            if (!to || from_init[*to])
            {
                continue;
            }

            from_init[*to] = *from_init[state] + static_cast<char>(byte);

            pending.push_back(*to);
        }
    }

    return from_init;
}

/**
 * @brief Finds a shortest accepting completion from each live state, by relaxing until nothing shortens.
 * @param live The live automaton.
 * @return Per state, the completion, or std::nullopt for none.
 */
std::vector<std::optional<std::string>> shortest_to_accept(const Live& live)
{
    std::vector<std::optional<std::string>> to_accept(live.n);

    for (std::size_t state{0}; state < live.n; ++state)
    {
        if (live.accepting[state])
        {
            to_accept[state] = "";
        }
    }

    for (auto grown{true}; grown;)
    {
        grown = false;

        for (std::size_t state{0}; state < live.n; ++state)
        {
            for (const auto byte : live.bytes)
            {
                const auto to{live.step(state, byte)};

                if (!to || !to_accept[*to])
                {
                    continue;
                }

                const auto candidate{static_cast<char>(byte) + *to_accept[*to]};

                if (to_accept[state] && candidate.size() >= to_accept[state]->size())
                {
                    continue;
                }

                to_accept[state] = candidate;

                grown = true;
            }
        }
    }

    return to_accept;
}

/**
 * @brief Runs the greedy-agreement test: X meets X Pref+(X*) exactly when some accepted word u extends to an accepted
 *        ur with r a nonempty prefix of a factorable word.
 *
 * A product of two runs: the first continues the token automaton from an accepting state q, reached by a shortest
 * accepted u; the second parses r as a prefix of X*, restarting at the initial state after any accepted factor, the
 * reset chosen before each byte wherever it stands at an accepting state. A nonempty product path ending with the first
 * run accepting is an obstruction.
 * @param live The live automaton.
 * @return The obstruction urz with its factorization's first factor u, or std::nullopt when X lies in the class.
 */
std::optional<Obstruction> greedy_obstruction(const Live& live)
{
    const auto from_init{shortest_from_init(live)};

    const auto to_accept{shortest_to_accept(live)};

    std::map<Product_state, Product_parent> parent{};

    std::deque<Product_state> pending{};

    for (std::size_t q{0}; q < live.n; ++q)
    {
        if (live.accepting[q] && from_init[q])
        {
            pending.push_back(Product_state{.first = q, .second = live.init, .started = false});
        }
    }

    const auto obstruction_at{[&](const Product_state& end) {
        std::string r{};

        auto walk{end};

        while (walk.started)
        {
            const auto& [from, byte]{parent.at(walk)};

            r.push_back(byte);

            walk = from;
        }

        std::ranges::reverse(r);

        const auto u{*from_init[walk.first]};

        return Obstruction{.first_factor = u, .word = u + r + *to_accept[end.second]};
    }};

    const auto step_on{[&](const Product_state& at, const unsigned char byte) {
        const auto first{live.step(at.first, byte)};

        if (!first)
        {
            return;
        }

        std::vector<std::size_t> seconds{};

        const auto continued{live.step(at.second, byte)};

        if (continued)
        {
            seconds.push_back(*continued);
        }

        const auto restarted{live.accepting[at.second] ? live.step(live.init, byte) : std::nullopt};

        if (restarted)
        {
            seconds.push_back(*restarted);
        }

        for (const auto second : seconds)
        {
            const Product_state next{.first = *first, .second = second, .started = true};

            if (parent.contains(next))
            {
                continue;
            }

            parent.emplace(next, Product_parent{.from = at, .byte = static_cast<char>(byte)});

            pending.push_back(next);
        }
    }};

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        if (at.started && live.accepting[at.first])
        {
            return obstruction_at(at);
        }

        for (const auto byte : live.bytes)
        {
            step_on(at, byte);
        }
    }

    return std::nullopt;
}

/**
 * @brief Renders a window for printing: newline, tab and the quote escaped, other bytes outside printable ASCII as
 *        \\xHH.
 * @param window The window.
 * @return The rendering.
 */
std::string escaped(const std::string& window)
{
    std::string out{};

    for (const auto byte : window)
    {
        const auto value{static_cast<unsigned char>(byte)};

        switch (byte)
        {
        case '\n':
            out += R"(\n)";
            break;
        case '\t':
            out += R"(\t)";
            break;
        case '"':
            out += R"(\")";
            break;
        default:
            if (value < first_printable || value > last_printable)
            {
                out += std::format(R"(\x{:02x})", value);

                break;
            }

            out.push_back(byte);

            break;
        }
    }

    return out;
}

/**
 * @brief Searches one row of the window table both ways, prints its line and checks it against the paper.
 * @param row The row as the paper quotes it.
 * @param dfa The row's automaton.
 * @return Whether both searches agree with the gate's shortest length, both counts match and the chosen window replays.
 */
bool run(const Row& row, const Dfa& dfa)
{
    const auto live{live_of(dfa)};

    const auto quotient{three_status(live)};

    const auto chosen{chosen_origin(live)};

    const auto exhausted{chosen.shortest == 0};

    const auto replayed{exhausted ? std::nullopt : cloud_origin(live, chosen.window)};

    const auto lengths_agree{quotient.shortest == row.shortest && chosen.shortest == row.shortest};

    const auto counts_agree{quotient.held == row.keys && chosen.held == row.nodes};

    const auto replays{exhausted || replayed == std::optional{chosen.origin}};

    const auto ok{lengths_agree && counts_agree && replays};

    const auto shown{
            exhausted ? std::string{"exhausted"} : std::format("{} at {}", escaped(chosen.window), chosen.origin)};

    std::cout << std::format(
            "  {:<32} n {:>3}  shortest {} / {}  three-status keys {:>4}  chosen-origin nodes {:>4}  {}{}\n", row.name,
            live.n, quotient.shortest, chosen.shortest, quotient.held, chosen.held, shown, ok ? "" : moved_marker);

    return ok;
}

/**
 * @brief Builds the long-window family: the reduction's header before m cycle sources of lengths 1 to m, every source
 *        letter advancing each cycle, the j-th accepting at its last state, so a common accepted length is one less
 *        than a multiple of lcm(1..m).
 * @param m The number of cycle sources.
 * @return The automaton.
 */
Dfa lcm_family(const std::size_t m)
{
    munch::dfa::Builder builder{};

    const auto root{builder.init_state()};

    const auto error{builder.next_state()};

    const auto guard{builder.next_state()};

    const auto result{builder.next_state()};

    std::vector<State_t> timer(m + 1);

    std::vector<State_t> header(m + 1);

    std::vector<std::vector<State_t>> source(m);

    for (auto& state : timer)
    {
        state = builder.next_state();
    }

    for (auto& state : header)
    {
        state = builder.next_state();
    }

    for (std::size_t j{0}; j < m; ++j)
    {
        source[j].resize(j + 1);

        for (auto& state : source[j])
        {
            state = builder.next_state();
        }
    }

    const auto edge{[&builder](const State_t from, const char symbol, const State_t to) {
        const munch::dfa::Label label(symbol);

        builder.add_transition(from, label, to);
    }};

    const auto accept_resetting{
            [&](const State_t state, const State_t on_r, const State_t on_t, const bool letters_reset) {
                builder.add_accept_state(state, munch::dfa::Token{1});

                edge(state, 'r', on_r);

                edge(state, 't', on_t);

                if (!letters_reset)
                {
                    return;
                }

                edge(state, 'a', error);

                edge(state, 'b', error);
            }};

    edge(root, 'r', header[0]);

    edge(root, '#', result);

    for (const auto state : {error, result})
    {
        accept_resetting(state, timer[m], error, true);

        edge(state, '#', error);
    }

    accept_resetting(guard, timer[m], error, false);

    edge(guard, 'a', guard);

    edge(guard, 'b', guard);

    for (std::size_t j{1}; j <= m; ++j)
    {
        accept_resetting(timer[j], timer[j - 1], error, true);

        edge(timer[j], '#', error);
    }

    accept_resetting(timer[0], timer[0], guard, true);

    edge(timer[0], '#', error);

    for (std::size_t j{0}; j < m; ++j)
    {
        accept_resetting(header[j], header[j + 1], source[j][0], true);

        edge(header[j], '#', error);
    }

    accept_resetting(header[m], header[m], guard, true);

    edge(header[m], '#', error);

    for (std::size_t j{0}; j < m; ++j)
    {
        for (std::size_t k{0}; k <= j; ++k)
        {
            const auto next{source[j][(k + 1) % (j + 1)]};

            accept_resetting(source[j][k], timer[m], error, false);

            edge(source[j][k], 'a', next);

            edge(source[j][k], 'b', next);

            // The accepting state alone dies on #.
            if (k != j)
            {
                edge(source[j][k], '#', error);
            }
        }
    }

    return builder.build();
}

/**
 * @brief Compiles a builder's token set to the automaton the searches read, its start state unrolled.
 * @param builder The builder.
 * @return The automaton.
 */
Dfa unrolled(const Builder_dbg& builder)
{
    const auto dfa{builder.dfa()};

    return munch::dfa::unroll_start(dfa);
}

/**
 * @brief Builds a literal token set over its words, every word its own kind.
 * @param words The words.
 * @return The automaton.
 */
Dfa literal_set(const std::vector<std::string>& words)
{
    Builder_dbg builder{};

    std::size_t kind{0};

    for (const auto& word : words)
    {
        builder.add_token(text(word), static_cast<Token>(kind), 1);

        ++kind;
    }

    return unrolled(builder);
}

/**
 * @brief Searches the eight rows of the window table, the gate's shortest lengths repeated here as the agreement the
 *        program asserts, the two counts pinned as the table quotes them.
 * @return Whether every row agrees.
 */
bool window_table_rows()
{
    auto ok{true};

    Builder_dbg strings{};

    c_like(strings, false);

    strings.add_token(string_literal(), Token::string, 2);

    ok = run({.name = "C-like + string literals", .shortest = 2, .keys = 20, .nodes = 17}, unrolled(strings)) && ok;

    Builder_dbg line_comments{};

    c_like(line_comments, false);

    line_comments.add_token(line_comment(), Token::line_comment, 1);

    ok = run({.name = "C-like + // line comments", .shortest = 2, .keys = 13, .nodes = 14}, unrolled(line_comments)) &&
         ok;

    Builder_dbg block_comments{};

    c_like(block_comments, false);

    block_comments.add_token(block_comment(), Token::block_comment, 1);

    ok = run({.name = "C-like + block comments", .shortest = 4, .keys = 30, .nodes = 30}, unrolled(block_comments)) &&
         ok;

    Builder_dbg conventional{};

    c_like_with_comments(conventional, false);

    ok = run({.name = "C-like conventional", .shortest = 2, .keys = 22, .nodes = 16}, unrolled(conventional)) && ok;

    Builder_dbg split_friendly{};

    c_like_with_block_comments(split_friendly, true);

    ok = run({.name = "split-friendly + block comments", .shortest = 4, .keys = 103, .nodes = 81},
             unrolled(split_friendly)) &&
         ok;

    Builder_dbg cumulative{};

    c_like_with_block_comments(cumulative, false);

    ok = run({.name = "C-like cumulative (new here)", .shortest = 4, .keys = 102, .nodes = 79}, unrolled(cumulative)) &&
         ok;

    Builder_dbg json_row{};

    json(json_row);

    ok = run({.name = "JSON, RFC 8259", .shortest = 2, .keys = 52, .nodes = 29}, unrolled(json_row)) && ok;

    Builder_dbg negative{};

    negative.add_token(plus(any_of(Set{'a'})), Token::identifier, 1);

    ok = run({.name = "a+ (negative row)", .shortest = 0, .keys = 2, .nodes = 2}, unrolled(negative)) && ok;

    return ok;
}

/**
 * @brief Searches the long-window family's exact minima, each the family's lcm(1..m) + m + 2, each window certified by
 *        the plain cloud at its last byte and its own completely tokenizable occurrence under maximal munch, and holds
 *        the nodes the search retains when each appears to the paper's table.
 * @return Whether every member agrees.
 */
bool long_window_rows()
{
    std::cout << "\nthe long-window family, m sources of cycle lengths 1 to m\n";

    const std::vector<std::size_t> minima{4, 6, 11, 18, 67, 68, 429, 850, 2531};

    const std::vector<std::size_t> nodes_held{18, 41, 110, 253, 1226, 1781, 11038, 27017, 87518};

    auto ok{true};

    for (std::size_t m{1}; m <= minima.size(); ++m)
    {
        const auto family{lcm_family(m)};

        const auto live{live_of(family)};

        const auto found{chosen_origin(live)};

        std::size_t lcm{1};

        for (std::size_t i{2}; i <= m; ++i)
        {
            lcm = std::lcm(lcm, i);
        }

        const auto starts{token_starts(live, found.window)};

        const auto replayed{cloud_origin(live, found.window)};

        const auto shortest_agrees{found.shortest == minima[m - 1] && found.shortest == lcm + m + 2};

        const auto held_agrees{found.held == nodes_held[m - 1]};

        const auto states_agree{live.n == (m * m + 5 * m + 12) / 2};

        const auto origin_agrees{found.origin == found.shortest - 1 && replayed == std::optional{found.origin}};

        const auto two_tokens{
                starts && !starts->empty() && starts->size() == 2 && starts->back() == found.shortest - 1};

        const auto row_ok{shortest_agrees && held_agrees && states_agree && origin_agrees && two_tokens};

        const auto tokens{starts ? starts->size() : 0};

        std::cout << std::format(
                "  m {}  states {:>3}  lcm {:>4}  shortest window {:>4} at {:>4}  nodes held {:>7}  tokens {}{}\n", m,
                live.n, lcm, found.shortest, found.origin, found.held, tokens, row_ok ? "" : moved_marker);

        ok = row_ok && ok;
    }

    return ok;
}

/**
 * @brief Decides one example's greedy-agreement class, prints its line and checks it against the paper, replaying any
 *        obstruction.
 *
 * The obstruction's displayed factorization begins with u; the scan either fails on the word or begins with a longer
 * first token, so it is not that factorization.
 * @param name The example's name.
 * @param dfa The example's automaton.
 * @param expected_in Whether the paper places it in the class.
 * @return Whether the test sorts it as the paper does and any obstruction replays.
 */
bool greedy_row(const std::string_view name, const Dfa& dfa, const bool expected_in)
{
    const auto live{live_of(dfa)};

    const auto obstruction{greedy_obstruction(live)};

    const auto replays{[&live](const Obstruction& found) {
        const auto& [first_factor, word]{found};

        const auto starts{token_starts(live, word)};

        if (!starts)
        {
            return true;
        }

        const auto longer_second{starts->size() > 1 && (*starts)[1] != first_factor.size()};

        const auto longer_only{starts->size() == 1 && first_factor.size() < word.size()};

        return longer_second || longer_only;
    }};

    const auto replay_ok{!obstruction || replays(*obstruction)};

    const auto row_ok{obstruction.has_value() != expected_in && replay_ok};

    const auto verdict{obstruction ? "out, obstruction " : "in"};

    const auto shown{
            obstruction ?
                    std::format(
                            "{} with first factor {}", escaped(obstruction->word), escaped(obstruction->first_factor)) :
                    std::string{}};

    std::cout << std::format("  {:<20} {}{}{}\n", name, verdict, shown, row_ok ? "" : moved_marker);

    return row_ok;
}

/**
 * @brief Decides the greedy-agreement class on the structural section's examples: in, in, out, with the obstruction
 *        replayed.
 * @return Whether every example is sorted as the paper states.
 */
bool greedy_rows()
{
    std::cout << "\nthe greedy-agreement test\n";

    auto ok{true};

    ok = greedy_row("{a, ab}", literal_set({"a", "ab"}), true) && ok;

    ok = greedy_row("{a, ab, bb}", literal_set({"a", "ab", "bb"}), false) && ok;

    ok = greedy_row("{a, ab, b}", literal_set({"a", "ab", "b"}), false) && ok;

    ok = greedy_row("{0, 00, 01}", literal_set({"0", "00", "01"}), false) && ok;

    Builder_dbg star{};

    star.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), Token::identifier, 1);

    ok = greedy_row("ab*", unrolled(star), true) && ok;

    Builder_dbg conventional{};

    c_like_with_comments(conventional, false);

    ok = greedy_row("C-like conventional", unrolled(conventional), false) && ok;

    return ok;
}

} // namespace

/**
 * @brief Runs the window table's searches, the long-window family and the greedy-agreement test, then prints the
 *        verdict.
 * @return EXIT_SUCCESS when every figure agrees with the paper, EXIT_FAILURE otherwise.
 */
int main()
{
    std::cout << "the smaller searches over the window table's token sets, and the long-window family\n";

    const auto rows_ok{window_table_rows()};

    const auto family_ok{long_window_rows()};

    const auto greedy_ok{greedy_rows()};

    const auto ok{rows_ok && family_ok && greedy_ok};

    const auto verdict{
            ok ? "The three-status quotient and the chosen-origin search agree with the gate's shortest lengths on "
                 "every row, the family's minima are lcm(1..m) + m + 2 with the nodes held that the table pins, and "
                 "the greedy-agreement test sorts the examples as the paper states." :
                 "A measurement moved. The window figures must be re-derived before being relied on."};

    std::cout << std::format("\n{}\n", verdict);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * The smaller searches of paper/split-windows/split-windows.tex, run over the same token sets as the window gate
 * whose retained keys the paper's window table quotes, so the new columns of that table are computed beside the old
 * one rather than transcribed. Three things are decided here, and every figure the paper quotes is asserted:
 *
 *   1. The three-status quotient of the cloud: a live state is empty, single (exactly one in-window origin) or blocked
 *      (the pre-window origin or two in-window origins). Its breadth-first search retains the keys this program
 *      counts, under the gate's own stopping discipline, and its shortest length must equal the gate's.
 *   2. The chosen-origin search, the search munch 2.2 ships as shortest_split_window(): one origin is followed from
 *      its birth beside the support of every other hypothesis, and a window is certified when that origin outlives
 *      them all. The nodes it holds when its shortest window appears are the table's other new column.
 *   3. The long-window family of the paper's complexity section: m cycle sources of lengths 1 to m behind the
 *      reduction's header, whose shortest certified window is lcm(1..m) + m + 2 bytes long. The minima are searched
 *      exactly, replayed through the plain cloud at the claimed origin, and scanned by maximal munch to show that the
 *      window is its own completely tokenizable occurrence.
 *
 * A fourth check decides the greedy-agreement class of the paper's structural section on its examples: {a, ab} and
 * ab* lie in the class, {a, ab, bb} does not, and the obstruction the test returns is replayed through the scan.
 */

#include <algorithm>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
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
using namespace munch::regex;
using namespace figures;

using munch::dfa::Dfa;

using State_t = Dfa::State_t;

/**
 * @brief A Builder exposing its compiled automaton, the way the unit tests expose it.
 */
class Exposed final : public munch::core::Builder
{
public:
    using Builder::dfa;
};

/**
 * @brief A set of live states packed one bit each.
 */
using Bits_t = std::vector<std::uint64_t>;

bool contains(const Bits_t& bits, const std::size_t at)
{
    return ((bits[at >> 6U] >> (at & 63U)) & 1U) != 0;
}

void insert(Bits_t& bits, const std::size_t at)
{
    bits[at >> 6U] |= std::uint64_t{1} << (at & 63U);
}

bool empty(const Bits_t& bits)
{
    return std::ranges::all_of(bits, [](const std::uint64_t word) { return word == 0; });
}

std::size_t count(const Bits_t& bits)
{
    std::size_t total{0};

    for (const auto word : bits)
    {
        total += static_cast<std::size_t>(std::popcount(word));
    }

    return total;
}

/**
 * @brief The live automaton the searches run over: the trim states renumbered densely, transitions between live
 *        states only, one representative byte per class of bytes the live tables do not tell apart.
 */
struct Live
{
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
    std::vector<bool> accepting;

    /**
     * @brief The live transition table, n rows of 256 entries, n meaning none.
     */
    std::vector<std::size_t> delta;

    /**
     * @brief One byte per class.
     */
    std::vector<unsigned char> bytes;

    [[nodiscard]] std::optional<std::size_t> step(const std::size_t state, const unsigned char byte) const
    {
        const auto to{delta[state * 256 + byte]};

        return to == n ? std::nullopt : std::optional{to};
    }

    [[nodiscard]] Bits_t none() const { return Bits_t((n + 63) / 64, 0); }

    [[nodiscard]] Bits_t all() const
    {
        auto bits{none()};

        for (std::size_t state{0}; state < n; ++state)
        {
            insert(bits, state);
        }

        return bits;
    }
};

/**
 * @brief Trims the automaton to its live states, reachable from the initial state and co-accessible to acceptance.
 */
Live live_of(const Dfa& dfa)
{
    std::set<State_t> reachable{dfa.init_state()};

    std::deque<State_t> pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && !reachable.contains(*next))
            {
                reachable.insert(*next);

                pending.push_back(*next);
            }
        }
    }

    std::set<State_t> co_accessible;

    for (const auto state : reachable)
    {
        if (dfa.has_accept_token(state))
        {
            co_accessible.insert(state);
        }
    }

    for (auto grown{true}; grown;)
    {
        grown = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state))
            {
                continue;
            }

            for (int symbol{0}; symbol < 256; ++symbol)
            {
                if (const auto next{dfa.advance(state, static_cast<char>(symbol))};
                    next && co_accessible.contains(*next))
                {
                    co_accessible.insert(state);

                    grown = true;

                    break;
                }
            }
        }
    }

    Live live;

    std::map<State_t, std::size_t> index;

    for (const auto state : co_accessible)
    {
        index[state] = live.n++;
    }

    if (!index.contains(dfa.init_state()))
    {
        return Live{};
    }

    live.init = index.at(dfa.init_state());

    live.accepting.assign(live.n, false);

    live.delta.assign(live.n * 256, live.n);

    for (const auto& [state, at] : index)
    {
        live.accepting[at] = dfa.has_accept_token(state).has_value();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && index.contains(*next))
            {
                live.delta[at * 256 + static_cast<std::size_t>(symbol)] = index.at(*next);

                live.reentrant = live.reentrant || index.at(*next) == live.init;
            }
        }
    }

    std::set<std::vector<std::size_t>> signatures;

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        std::vector<std::size_t> signature;

        for (std::size_t state{0}; state < live.n; ++state)
        {
            signature.push_back(live.delta[state * 256 + static_cast<std::size_t>(symbol)]);
        }

        if (signatures.insert(signature).second)
        {
            live.bytes.push_back(static_cast<unsigned char>(symbol));
        }
    }

    return live;
}

/**
 * @brief The plain cloud of the paper's model: the origin every surviving hypothesis agrees on, or nothing.
 */
std::optional<std::size_t> cloud_origin(const Live& live, const std::string& window)
{
    constexpr std::size_t before{static_cast<std::size_t>(-1)};

    std::set<std::pair<std::size_t, std::size_t>> cloud;

    for (std::size_t state{0}; state < live.n; ++state)
    {
        cloud.emplace(state, before);
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        const auto accepting{
                std::ranges::any_of(cloud, [&live](const auto& pair) { return live.accepting[pair.first]; })};

        std::set<std::pair<std::size_t, std::size_t>> next;

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{live.step(state, byte)})
            {
                next.emplace(*to, state == live.init && !live.reentrant ? at : origin);
            }
        }

        if (accepting)
        {
            if (const auto to{live.step(live.init, byte)})
            {
                next.emplace(*to, at);
            }
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    const auto origin{cloud.begin()->second};

    const auto unanimous{std::ranges::all_of(cloud, [origin](const auto& pair) { return pair.second == origin; })};

    return unanimous && origin != before ? std::optional{origin} : std::nullopt;
}

/**
 * @brief The maximal-munch scan: the token starts, or nothing when the input is not completely tokenizable.
 */
std::optional<std::vector<std::size_t>> scan(const Live& live, const std::string& input)
{
    std::vector<std::size_t> starts;

    for (std::size_t at{0}; at < input.size();)
    {
        starts.push_back(at);

        std::optional<std::size_t> last;

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

        if (!last)
        {
            return std::nullopt;
        }

        at = *last;
    }

    return starts;
}

/**
 * @brief What a search found: the shortest certified length, or zero when the quotient was exhausted, with the keys
 *        or nodes it held, and for the chosen-origin search the window and its origin.
 */
struct Found
{
    std::size_t shortest{0};

    std::size_t held{0};

    std::string window;

    std::size_t origin{0};
};

/**
 * @brief Breadth-first search over the three-status quotient: single states, blocked states.
 *
 * The stopping discipline is the gate's: once a length has certified, deeper words are not expanded, a certified
 * successor is never retained as a key, and the keys held when the queue drains are the count. The initial key is
 * every live state blocked, since every one carries the pre-window origin.
 */
Found three_status(const Live& live)
{
    using Key = std::pair<Bits_t, Bits_t>;

    Found found;

    std::map<Key, std::size_t> seen;

    std::deque<std::pair<Key, std::size_t>> queue;

    const Key start{live.none(), live.all()};

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

        const auto& [single, blocked]{key};

        auto accepting{false};

        for (std::size_t state{0}; state < live.n; ++state)
        {
            accepting = accepting || ((contains(single, state) || contains(blocked, state)) && live.accepting[state]);
        }

        const auto renamed{!live.reentrant && contains(blocked, live.init)};

        for (const auto byte : live.bytes)
        {
            std::vector<unsigned> arrivals(live.n, 0);

            std::vector<bool> blocked_arrival(live.n, false);

            for (std::size_t state{0}; state < live.n; ++state)
            {
                if (state == live.init && renamed)
                {
                    continue;
                }

                if (contains(blocked, state))
                {
                    if (const auto to{live.step(state, byte)})
                    {
                        blocked_arrival[*to] = true;
                    }
                }
                else if (contains(single, state))
                {
                    if (const auto to{live.step(state, byte)})
                    {
                        ++arrivals[*to];
                    }
                }
            }

            if (accepting || renamed)
            {
                if (const auto to{live.step(live.init, byte)})
                {
                    ++arrivals[*to];
                }
            }

            Key next{live.none(), live.none()};

            for (std::size_t state{0}; state < live.n; ++state)
            {
                if (blocked_arrival[state] || arrivals[state] >= 2)
                {
                    insert(next.second, state);
                }
                else if (arrivals[state] == 1)
                {
                    insert(next.first, state);
                }
            }

            if (empty(next.first) && empty(next.second))
            {
                continue;
            }

            if (empty(next.second) && count(next.first) == 1)
            {
                found.shortest = found.shortest != 0 ? found.shortest : depth + 1;

                continue;
            }

            if (!seen.contains(next))
            {
                seen.emplace(next, depth + 1);

                queue.emplace_back(next, depth + 1);
            }
        }
    }

    found.held = seen.size();

    return found;
}

/**
 * @brief A node of the chosen-origin search: the chosen origin's state once chosen, and the other hypotheses' states.
 */
struct Node
{
    bool operator==(const Node&) const = default;

    std::optional<std::size_t> chosen;

    Bits_t others;
};

struct Node_hash
{
    std::size_t operator()(const Node& node) const noexcept
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
 * @brief The nodes one byte leads to, as shortest_split_window() advances them: before the choice the fresh origin
 *        may be chosen or declined; after it the chosen origin must survive and share a state with no other.
 */
std::vector<std::pair<Node, bool>> advance(const Live& live, const Node& node, const unsigned char byte)
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

        if (const auto to{live.step(state, byte)})
        {
            insert(moved, *to);
        }
    }

    const auto fresh{accepting || renamed ? live.step(live.init, byte) : std::nullopt};

    std::vector<std::pair<Node, bool>> out;

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
 * @brief The chosen-origin search, breadth first, returning at the first certified node with the nodes held then.
 */
Found chosen_origin(const Live& live)
{
    struct Reached
    {
        std::size_t parent;

        unsigned char byte;

        bool chose;
    };

    Found found;

    std::unordered_map<Node, std::size_t, Node_hash> index;

    std::vector<const Node*> nodes{&index.emplace(Node{.chosen = std::nullopt, .others = live.all()}, 0).first->first};

    std::vector<Reached> reached{{.parent = 0, .byte = 0, .chose = false}};

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        for (const auto byte : live.bytes)
        {
            for (auto& [next, chose] : advance(live, *nodes[at], byte))
            {
                if (index.contains(next))
                {
                    continue;
                }

                const auto certified{next.chosen && empty(next.others)};

                nodes.push_back(&index.emplace(std::move(next), nodes.size()).first->first);

                reached.push_back({.parent = at, .byte = byte, .chose = chose});

                if (certified)
                {
                    std::size_t chosen_at{0};

                    for (auto walk{nodes.size() - 1}; walk != 0; walk = reached[walk].parent)
                    {
                        found.window.push_back(static_cast<char>(reached[walk].byte));

                        if (reached[walk].chose)
                        {
                            chosen_at = found.window.size();
                        }
                    }

                    std::ranges::reverse(found.window);

                    found.shortest = found.window.size();

                    found.origin = found.window.size() - chosen_at;

                    found.held = nodes.size();

                    return found;
                }
            }
        }
    }

    found.held = nodes.size();

    return found;
}

/**
 * @brief The greedy-agreement test: X meets X Pref+(X*) exactly when some accepted word u extends to an accepted ur
 *        with r a nonempty prefix of a factorable word; the obstruction urz is returned, with its factorization's
 *        first factor u.
 *
 * A product of two runs: the first continues the token automaton from an accepting state q, reached by a shortest
 * accepted u; the second parses r as a prefix of X*, restarting at the initial state after any accepted factor. A
 * nonempty product path ending with the first run accepting is an obstruction.
 */
std::optional<std::pair<std::string, std::string>> greedy_obstruction(const Live& live)
{
    // Shortest word from the initial state to each live state, by breadth-first search.
    std::vector<std::optional<std::string>> from_init(live.n);

    from_init[live.init] = "";

    for (std::deque<std::size_t> pending{live.init}; !pending.empty(); pending.pop_front())
    {
        const auto state{pending.front()};

        for (const auto byte : live.bytes)
        {
            if (const auto to{live.step(state, byte)}; to && !from_init[*to])
            {
                from_init[*to] = *from_init[state] + static_cast<char>(byte);

                pending.push_back(*to);
            }
        }
    }

    // Shortest accepting completion from each live state.
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

                if (!to_accept[state] || candidate.size() < to_accept[state]->size())
                {
                    to_accept[state] = candidate;

                    grown = true;
                }
            }
        }
    }

    // The product: the first run's state, the second run's state, and whether a byte has been read, so a root is
    // distinct from the same pair reached again after a nonempty path. The second run's reset is chosen before each
    // byte, wherever it stands at an accepting state.
    struct Pair
    {
        auto operator<=>(const Pair&) const = default;

        std::size_t first;

        std::size_t second;

        bool started;
    };

    std::map<Pair, std::pair<Pair, char>> parent;

    std::deque<Pair> pending;

    for (std::size_t q{0}; q < live.n; ++q)
    {
        if (live.accepting[q] && from_init[q])
        {
            pending.push_back({.first = q, .second = live.init, .started = false});
        }
    }

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        if (at.started && live.accepting[at.first])
        {
            std::string r;

            auto walk{at};

            while (walk.started)
            {
                const auto& [from, byte]{parent.at(walk)};

                r.push_back(byte);

                walk = from;
            }

            std::ranges::reverse(r);

            const auto u{*from_init[walk.first]};

            return std::pair{u, u + r + *to_accept[at.second]};
        }

        for (const auto byte : live.bytes)
        {
            const auto first{live.step(at.first, byte)};

            if (!first)
            {
                continue;
            }

            std::vector<std::size_t> seconds;

            if (const auto second{live.step(at.second, byte)})
            {
                seconds.push_back(*second);
            }

            if (live.accepting[at.second])
            {
                if (const auto second{live.step(live.init, byte)})
                {
                    seconds.push_back(*second);
                }
            }

            for (const auto second : seconds)
            {
                const Pair next{.first = *first, .second = second, .started = true};

                if (!parent.contains(next))
                {
                    parent[next] = {at, static_cast<char>(byte)};

                    pending.push_back(next);
                }
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief A row of the paper's window table: the gate's shortest length, and the two counts this program pins.
 */
struct Row
{
    std::string_view name;

    std::size_t shortest;

    std::size_t keys;

    std::size_t nodes;
};

std::string escaped(const std::string& window)
{
    std::string out;

    for (const auto byte : window)
    {
        if (byte == '\n')
        {
            out += "\\n";
        }
        else if (byte == '\t')
        {
            out += "\\t";
        }
        else if (byte == '"')
        {
            out += "\\\"";
        }
        else if (static_cast<unsigned char>(byte) < 32 || static_cast<unsigned char>(byte) > 126)
        {
            char buffer[8];

            std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned char>(byte));

            out += buffer;
        }
        else
        {
            out.push_back(byte);
        }
    }

    return out;
}

bool run(const Row& row, const Dfa& dfa)
{
    const auto live{live_of(dfa)};

    const auto quotient{three_status(live)};

    const auto chosen{chosen_origin(live)};

    const auto replayed{chosen.shortest == 0 ? std::nullopt : cloud_origin(live, chosen.window)};

    const auto ok{
            quotient.shortest == row.shortest && chosen.shortest == row.shortest && quotient.held == row.keys &&
            chosen.held == row.nodes && (chosen.shortest == 0 || replayed == std::optional{chosen.origin})};

    std::printf(
            "  %-32s n %3zu  shortest %zu / %zu  three-status keys %4zu  chosen-origin nodes %4zu  %s%s\n",
            std::string{row.name}.c_str(), live.n, quotient.shortest, chosen.shortest, quotient.held, chosen.held,
            chosen.shortest == 0 ? "exhausted" :
                                   (escaped(chosen.window) + " at " + std::to_string(chosen.origin)).c_str(),
            ok ? "" : "   <- MOVED");

    return ok;
}

/**
 * @brief The long-window family: the reduction's header before m cycle sources of lengths 1 to m, every source letter
 *        advancing each cycle, the j-th accepting at its last state, so a common accepted length is one less than a
 *        multiple of lcm(1..m).
 */
Dfa lcm_family(const std::size_t m)
{
    munch::dfa::Builder builder;

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

    const auto edge{[&builder](const auto from, const char symbol, const auto to) {
        builder.add_transition(from, munch::dfa::Label(symbol), to);
    }};

    // Every state but the root accepts, all with the one kind; a letter that would end the countdown resets it.
    const auto accept_resetting{[&](const auto state, const auto on_r, const auto on_t, const bool letters_reset) {
        builder.add_accept_state(state, munch::dfa::Token{1});
        edge(state, 'r', on_r);
        edge(state, 't', on_t);

        if (letters_reset)
        {
            edge(state, 'a', error);
            edge(state, 'b', error);
        }
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

            if (k != j)
            {
                edge(source[j][k], '#', error); // the accepting state alone dies on #
            }
        }
    }

    return builder.build();
}

/**
 * @brief A literal token set over its words, every word its own kind.
 */
Dfa literal_set(const std::vector<std::string>& words)
{
    Exposed builder;

    std::size_t kind{0};

    for (const auto& word : words)
    {
        builder.add_token(text(word), static_cast<Token>(kind++), 1);
    }

    return munch::dfa::unroll_start(builder.dfa());
}
} // namespace

int main()
{
    std::printf("the smaller searches over the window table's token sets, and the long-window family\n");

    auto ok{true};

    // The eight rows of the window table, the gate's shortest lengths repeated here as the agreement the program
    // asserts, the two counts pinned as the table quotes them.
    {
        Exposed b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        ok = run({.name = "C-like + string literals", .shortest = 2, .keys = 20, .nodes = 17},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        c_like(b, false);
        b.add_token(line_comment(), Token::LineComment, 1);
        ok = run({.name = "C-like + // line comments", .shortest = 2, .keys = 13, .nodes = 14},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        c_like(b, false);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like + block comments", .shortest = 4, .keys = 30, .nodes = 30},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        ok = run({.name = "C-like conventional", .shortest = 2, .keys = 22, .nodes = 16},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        c_like(b, true);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "split-friendly + block comments", .shortest = 4, .keys = 103, .nodes = 81},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        b.add_token(block_comment(), Token::BlockComment, 1);
        ok = run({.name = "C-like cumulative (new here)", .shortest = 4, .keys = 102, .nodes = 79},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        json(b);
        ok = run({.name = "JSON, RFC 8259", .shortest = 2, .keys = 52, .nodes = 29},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }
    {
        Exposed b;
        b.add_token(plus(any_of(Set{'a'})), Token::Identifier, 1);
        ok = run({.name = "a+ (negative row)", .shortest = 0, .keys = 2, .nodes = 2},
                 munch::dfa::unroll_start(b.dfa())) &&
             ok;
    }

    // The long-window family: the exact minima, each the family's lcm(1..m) + m + 2, each window certified by the
    // plain cloud at its last byte and its own completely tokenizable occurrence under maximal munch.
    std::printf("\nthe long-window family, m sources of cycle lengths 1 to m\n");

    const std::vector<std::size_t> minima{4, 6, 11, 18, 67, 68, 429, 850, 2531};

    for (std::size_t m{1}; m <= minima.size(); ++m)
    {
        const auto live{live_of(lcm_family(m))};

        const auto found{chosen_origin(live)};

        std::size_t lcm{1};

        for (std::size_t i{2}; i <= m; ++i)
        {
            lcm = std::lcm(lcm, i);
        }

        const auto starts{scan(live, found.window)};

        const auto replayed{cloud_origin(live, found.window)};

        const auto row_ok{
                found.shortest == minima[m - 1] && found.shortest == lcm + m + 2 &&
                live.n == (m * m + 5 * m + 12) / 2 && found.origin == found.shortest - 1 &&
                replayed == std::optional{found.origin} && starts && !starts->empty() && starts->size() == 2 &&
                starts->back() == found.shortest - 1};

        std::printf(
                "  m %zu  states %3zu  lcm %4zu  shortest window %4zu at %4zu  nodes held %7zu  tokens %zu%s\n", m,
                live.n, lcm, found.shortest, found.origin, found.held, starts ? starts->size() : 0,
                row_ok ? "" : "   <- MOVED");

        ok = row_ok && ok;
    }

    // The greedy-agreement class on the structural section's examples: in, in, out, with the obstruction replayed.
    std::printf("\nthe greedy-agreement test\n");

    const auto check_class{[&ok](const std::string_view name, const Dfa& dfa, const bool expected_in) {
        const auto live{live_of(dfa)};

        const auto obstruction{greedy_obstruction(live)};

        auto replay_ok{true};

        if (obstruction)
        {
            // The obstruction's displayed factorization begins with u; the scan either fails on the word or begins
            // with a longer first token, so it is not that factorization.
            const auto starts{scan(live, obstruction->second)};

            replay_ok = !starts || (starts->size() > 1 && (*starts)[1] != obstruction->first.size()) ||
                        (starts->size() == 1 && obstruction->first.size() < obstruction->second.size());
        }

        const auto row_ok{obstruction.has_value() != expected_in && replay_ok};

        std::printf(
                "  %-20s %s%s%s\n", std::string{name}.c_str(), obstruction ? "out, obstruction " : "in",
                obstruction ?
                        (escaped(obstruction->second) + " with first factor " + escaped(obstruction->first)).c_str() :
                        "",
                row_ok ? "" : "   <- MOVED");

        ok = row_ok && ok;
    }};

    check_class("{a, ab}", literal_set({"a", "ab"}), true);
    check_class("{a, ab, bb}", literal_set({"a", "ab", "bb"}), false);
    check_class("{a, ab, b}", literal_set({"a", "ab", "b"}), false);
    check_class("{0, 00, 01}", literal_set({"0", "00", "01"}), false);

    {
        Exposed b;
        b.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), Token::Identifier, 1);
        check_class("ab*", munch::dfa::unroll_start(b.dfa()), true);
    }
    {
        Exposed b;
        c_like(b, false);
        b.add_token(string_literal(), Token::String, 2);
        b.add_token(line_comment(), Token::LineComment, 1);
        check_class("C-like conventional", munch::dfa::unroll_start(b.dfa()), false);
    }

    std::printf(
            "\n%s\n", ok ? "The three-status quotient and the chosen-origin search agree with the gate's shortest "
                           "lengths on every row, the family's minima are lcm(1..m) + m + 2, and the "
                           "greedy-agreement test sorts the examples as the paper states." :
                           "A measurement moved. The window figures must be re-derived before being relied on.");

    return ok ? 0 : 1;
}

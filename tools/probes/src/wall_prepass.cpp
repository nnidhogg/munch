// Synthesizes carry prepasses from the compiled tables and holds every product to the serial scan.
//
// Some token sets refuse every split certificate for a structural reason: two or more readings of any
// factor survive forever, none ever eliminated, so no byte and no window can pin a boundary. This probe
// derives, from the tables alone, the machinery that resolves such walls, and pins the whole pipeline:
//
//   - the zero-lag premise, decided on a (state, seen-accept) product of the live tables, with the end
//     of input counted as a death available at every position: once a run accepts, every later state
//     accepts, so every rollback, byte or end-of-input, is zero bytes wide; grammars that fail the
//     premise are refused with a witness, never measured approximately;
//   - the wall verdict from that subset graph, state-granular and therefore a lower bound on
//     origin-distinguished readings: a floor of at least two is an absolute wall and licenses the
//     synthesis, while a smaller floor refuses without concluding the absence of an origin-level wall;
//   - the carry: on an absolute wall every byte acts on the kernel's readings as a bijection, a flavor
//     labeling with one permutation of flavors per byte is searched exactly, the permutations generate
//     the carry group, a factor's carry is the ordered product of its bytes' permutations, and the true
//     scan's flavor after any prefix is that product applied to a derived boundary seed, checked here
//     against the real maximal-munch loop at every position of a deterministic corpus;
//   - certificates behind the resolved flavor: the window decision's cloud walk started from one
//     flavor's states alone certifies where the unconditional wall never can, the same soundness
//     argument once the carry names the true reading's flavor; planning driven by prefix carries
//     recovers every target on generated corpora, every cut on the serial segmentation, every spliced
//     boundary sequence equal to the serial one, while the unconditional walk certifies zero windows
//     across the same campaign and deliberately wrong flavors cut wrongly every time;
//   - the price tower: positional control (log2 of the seed's orbit), compositional control (log2 of
//     the group order), and the exact summary (log2 of the kernel's transfer semigroup over nonempty
//     factors), with a hazard witness for every ordered flavor pair: a concrete input whose wrong-flavor
//     cut leaves the serial segmentation. The control prices are the storage of the conditioned-cut
//     carry; the semigroup price calibrates the stronger full-transfer interface, which the cut service
//     never needs. A scheme may pay work instead of bits (rescanning the prefix realizes the same cuts),
//     so scheme-wide bounds await an explicit one-pass compositional model with common-context fooling
//     pairs. The two-string row separates all three prices strictly.
//
// The guarantees are conditional exactly as the window certificate's own: completely tokenizable input.
// On malformed input the carry names nothing past the failure, and the splice hazard is the documented
// one, no worse. Everything below is deterministic; every count is pinned; a drifted number fails.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"

namespace
{
using namespace munch::regex;
using munch::dfa::Dfa;

class Builder_dbg : public munch::core::Builder
{
public:
    using Builder::dfa;
};

std::size_t failures{0};

void expect(const bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;

        std::cout << "FAIL: " << what << "\n";
    }
}

constexpr int dead{-1};

struct Table
{
    std::size_t states{};
    std::size_t init{};
    std::vector<std::array<int, 256>> next;
    std::vector<char> accept;
};

Table extract(const Dfa& dfa)
{
    std::map<Dfa::State_t, std::size_t> index;

    std::vector<Dfa::State_t> order;

    const auto intern{[&](const Dfa::State_t state) {
        if (const auto found{index.find(state)}; found != index.end())
        {
            return found->second;
        }

        index.emplace(state, order.size());

        order.push_back(state);

        return order.size() - 1;
    }};

    intern(dfa.init_state());

    std::vector<std::array<int, 256>> raw;

    for (std::size_t at{0}; at < order.size(); ++at)
    {
        raw.emplace_back();

        raw.back().fill(dead);

        for (int byte{0}; byte < 256; ++byte)
        {
            if (const auto to{dfa.advance(order[at], static_cast<char>(byte))})
            {
                raw[at][static_cast<std::size_t>(byte)] = static_cast<int>(intern(*to));
            }
        }
    }

    std::vector<char> accept(order.size(), 0);

    for (std::size_t at{0}; at < order.size(); ++at)
    {
        accept[at] = dfa.has_accept_token(order[at]) ? 1 : 0;
    }

    std::vector<char> co{accept};

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (std::size_t at{0}; at < raw.size(); ++at)
        {
            if (co[at])
            {
                continue;
            }

            for (int byte{0}; byte < 256 && !co[at]; ++byte)
            {
                if (const auto to{raw[at][static_cast<std::size_t>(byte)]};
                    to != dead && co[static_cast<std::size_t>(to)])
                {
                    co[at] = 1;

                    grew = true;
                }
            }
        }
    }

    Table table{.states = raw.size(), .init = 0, .next = std::move(raw), .accept = std::move(accept)};

    for (auto& row : table.next)
    {
        for (auto& to : row)
        {
            if (to != dead && !co[static_cast<std::size_t>(to)])
            {
                to = dead;
            }
        }
    }

    return table;
}

bool zero_lag_holds(const Table& table)
{
    std::vector<std::array<char, 2>> visited(table.states, {0, 0});

    std::deque<std::pair<std::size_t, int>> pending{{table.init, 0}};

    visited[table.init][0] = 1;

    while (!pending.empty())
    {
        const auto [state, seen]{pending.front()};

        pending.pop_front();

        // End-of-input is a death available at every position, so a stale accept is a violation the
        // moment a seen run occupies a non-accepting state, byte-death or not.
        if (seen != 0 && table.accept[state] == 0)
        {
            return false;
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto to{table.next[state][static_cast<std::size_t>(byte)]};

            if (to == dead)
            {
                continue;
            }

            const auto target{static_cast<std::size_t>(to)};

            const auto next_seen{seen != 0 || table.accept[target] != 0 ? 1 : 0};

            if (visited[target][static_cast<std::size_t>(next_seen)] == 0)
            {
                visited[target][static_cast<std::size_t>(next_seen)] = 1;

                pending.emplace_back(target, next_seen);
            }
        }
    }

    return true;
}

// The synthesized carry: the kernel, its flavor labeling, the per-byte permutations, the group, and the
// boundary seed. An empty group signals a refusal, with the reason printed by the synthesizer.
struct Carry
{
    std::size_t width{};
    std::size_t kernel_subsets{};
    std::size_t semigroup{};
    std::vector<int> state_flavor;
    std::array<std::vector<int>, 256> sigma;
    std::set<std::vector<int>> group;
    int seed{-1};
};

std::optional<Carry> synthesize(const Table& table)
{
    // The shipped window certificate refuses nullable token sets wholesale; the synthesis mirrors that
    // scope rather than answering beyond it.
    if (table.accept[table.init] != 0)
    {
        std::cout << "refused: the token set is nullable\n";

        return std::nullopt;
    }

    if (!zero_lag_holds(table))
    {
        std::cout << "refused: the zero-lag premise fails\n";

        return std::nullopt;
    }

    // The subset walk under the zero-lag licence: bare live states, a dying accepting member restarting
    // at the current byte, a dying non-accepting member eliminated; every edge's member map is recorded.
    std::vector<std::size_t> start;

    for (std::size_t state{0}; state < table.states; ++state)
    {
        auto live{table.accept[state] != 0};

        for (int byte{0}; byte < 256 && !live; ++byte)
        {
            live = table.next[state][static_cast<std::size_t>(byte)] != dead;
        }

        if (live)
        {
            start.push_back(state);
        }
    }

    std::map<std::vector<std::size_t>, std::size_t> ids;

    std::vector<std::vector<std::size_t>> subsets;

    std::vector<std::array<std::size_t, 256>> successor;

    const auto intern{[&](std::vector<std::size_t> subset) {
        if (const auto found{ids.find(subset)}; found != ids.end())
        {
            return found->second;
        }

        ids.emplace(subset, subsets.size());

        subsets.push_back(std::move(subset));

        successor.emplace_back();

        return subsets.size() - 1;
    }};

    intern(start);

    for (std::size_t at{0}; at < subsets.size(); ++at)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            std::set<std::size_t> stepped;

            auto restart{false};

            for (const auto state : subsets[at])
            {
                if (const auto to{table.next[state][static_cast<std::size_t>(byte)]}; to != dead)
                {
                    stepped.insert(static_cast<std::size_t>(to));
                }
                else if (table.accept[state] != 0)
                {
                    restart = true;
                }
            }

            if (restart)
            {
                if (const auto to{table.next[table.init][static_cast<std::size_t>(byte)]}; to != dead)
                {
                    stepped.insert(static_cast<std::size_t>(to));
                }
            }

            const auto target{intern({stepped.begin(), stepped.end()})};

            successor[at][static_cast<std::size_t>(byte)] = target;

            if (subsets.size() > 4096)
            {
                std::cout << "refused: the subset graph exceeds its budget\n";

                return std::nullopt;
            }
        }
    }

    const auto count{subsets.size()};

    // Floors by ascending-width reverse propagation, single-pass by construction: processing nodes in
    // increasing width order means the first touch a node receives carries the smallest width its
    // forward closure reaches, so every node and reverse edge is visited once and an adversarial chain
    // costs the same as a friendly graph.
    std::vector<std::vector<std::size_t>> predecessors(count);

    for (std::size_t node{0}; node < count; ++node)
    {
        for (const auto to : successor[node])
        {
            predecessors[to].push_back(node);
        }
    }

    constexpr auto untouched{std::numeric_limits<std::size_t>::max()};

    std::vector<std::size_t> floor(count, untouched);

    std::vector<std::size_t> order(count);

    for (std::size_t node{0}; node < count; ++node)
    {
        order[node] = node;
    }

    std::ranges::sort(
            order, [&](const auto left, const auto right) { return subsets[left].size() < subsets[right].size(); });

    for (const auto origin : order)
    {
        if (floor[origin] != untouched)
        {
            continue;
        }

        floor[origin] = subsets[origin].size();

        std::vector<std::size_t> pending{origin};

        while (!pending.empty())
        {
            const auto node{pending.back()};

            pending.pop_back();

            for (const auto from : predecessors[node])
            {
                if (floor[from] == untouched)
                {
                    floor[from] = floor[origin];

                    pending.push_back(from);
                }
            }
        }
    }

    std::size_t wall{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        wall = std::max(wall, floor[node]);
    }

    if (wall < 2)
    {
        std::cout << "refused: state-granular wall floor " << wall
                  << " is below two; the origin-level verdict is unavailable\n";

        return std::nullopt;
    }

    // The kernel: constant-width nodes at the wall floor, required to be successor-closed.
    std::vector<char> kernel(count, 0);

    for (std::size_t node{0}; node < count; ++node)
    {
        kernel[node] = floor[node] == wall && subsets[node].size() == wall ? 1 : 0;
    }

    for (std::size_t node{0}; node < count; ++node)
    {
        if (kernel[node] == 0)
        {
            continue;
        }

        for (const auto to : successor[node])
        {
            if (kernel[to] == 0)
            {
                std::cout << "refused: the kernel is not successor-closed\n";

                return std::nullopt;
            }
        }
    }

    // Member maps on kernel edges: position i of the source subset maps to a position of the target, a
    // dying accepting member continuing as the restart. Any elimination or merge refuses.
    const auto member_map{[&](const std::size_t node, const int byte) -> std::optional<std::vector<std::size_t>> {
        const auto& source{subsets[node]};

        const auto& target{subsets[successor[node][static_cast<std::size_t>(byte)]]};

        std::vector<std::size_t> mapped(source.size());

        std::set<std::size_t> hit;

        for (std::size_t at{0}; at < source.size(); ++at)
        {
            auto to{table.next[source[at]][static_cast<std::size_t>(byte)]};

            if (to == dead)
            {
                if (table.accept[source[at]] == 0)
                {
                    return std::nullopt;
                }

                to = table.next[table.init][static_cast<std::size_t>(byte)];

                if (to == dead)
                {
                    return std::nullopt;
                }
            }

            const auto found{std::ranges::lower_bound(target, static_cast<std::size_t>(to))};

            mapped[at] = static_cast<std::size_t>(found - target.begin());

            if (!hit.insert(mapped[at]).second)
            {
                return std::nullopt;
            }
        }

        return mapped;
    }};

    std::size_t base{count};

    for (std::size_t node{0}; node < count && base == count; ++node)
    {
        if (kernel[node] != 0)
        {
            base = node;
        }
    }

    // The kernel's edges, gathered once: source node, byte, target node, member map.
    std::vector<std::size_t> nodes;

    for (std::size_t node{0}; node < count; ++node)
    {
        if (kernel[node] != 0)
        {
            nodes.push_back(node);
        }
    }

    std::map<std::size_t, std::size_t> dense;

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        dense.emplace(nodes[at], at);
    }

    // Resource caps run before any factorial or closure work, and the closure itself carries a hard
    // element budget below: dimensional caps alone cannot bound it, since six width-six subsets admit
    // wreath-product closures beyond any enumeration.
    // The labeling search is exhaustive over
    // wall-factorial permutations and the summary closure grows with the kernel, so both are bounded
    // here, refused fail-closed beyond.
    if (nodes.size() > 6)
    {
        std::cout << "refused: the kernel has too many subsets for the labeling search\n";

        return std::nullopt;
    }

    if (wall > 6)
    {
        std::cout << "refused: the wall is too wide for the labeling search\n";

        return std::nullopt;
    }

    struct Edge
    {
        std::size_t source{};
        int byte{};
        std::size_t target{};
        std::vector<std::size_t> map;
    };

    std::vector<Edge> edges;

    for (const auto node : nodes)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            const auto mapped{member_map(node, byte)};

            if (!mapped)
            {
                std::cout << "refused: a kernel byte action eliminates or merges\n";

                return std::nullopt;
            }

            edges.push_back(
                    {.source = dense.at(node),
                     .byte = byte,
                     .target = dense.at(successor[node][static_cast<std::size_t>(byte)]),
                     .map = *mapped});
        }
    }

    // The exact chunk summary first, for calibration: the transfer semigroup of the kernel over
    // nonempty factors, whose elements map each kernel subset to a target subset and a member bijection.
    // Planning composes nonempty chunks only, so the empty word's identity is deliberately outside the
    // count; a model admitting empty chunks would add it and could round the price up one bit.
    using Summary_t = std::vector<std::pair<std::size_t, std::vector<std::size_t>>>;

    std::map<int, Summary_t> letters;

    for (const auto& edge : edges)
    {
        auto& letter{letters[edge.byte]};

        letter.resize(nodes.size());

        letter[edge.source] = {edge.target, edge.map};
    }

    std::set<Summary_t> semigroup;

    std::deque<Summary_t> queue;

    for (const auto& [byte, letter] : letters)
    {
        if (semigroup.insert(letter).second)
        {
            queue.push_back(letter);
        }
    }

    while (!queue.empty())
    {
        const auto element{queue.front()};

        queue.pop_front();

        for (const auto& [byte, letter] : letters)
        {
            Summary_t composed(nodes.size());

            for (std::size_t at{0}; at < nodes.size(); ++at)
            {
                const auto& [middle, first]{element[at]};

                const auto& [end, second]{letter[middle]};

                composed[at].first = end;

                composed[at].second.resize(wall);

                for (std::size_t member{0}; member < wall; ++member)
                {
                    composed[at].second[member] = second[first[member]];
                }
            }

            if (semigroup.insert(composed).second)
            {
                queue.push_back(composed);
            }

            if (semigroup.size() > 4096)
            {
                std::cout << "refused: the transfer closure exceeds the budget\n";

                return std::nullopt;
            }
        }
    }

    // The flavor structure, searched exactly: the base keeps the identity labeling, every other kernel
    // subset tries each permutation of flavors, and an assignment wins when one permutation per byte
    // explains every edge. Kernel graphs measure a handful of subsets, so the search is a formality, and
    // a failed search is a refusal, not an error: the exact summary above still stands.

    // The labeling search examines wall-factorial assignments per non-base subset, and dimensional caps
    // cannot bound that product: a table with a tiny closure can still demand ten-to-the-fourteenth
    // assignments. The budget refuses before any permutation is materialized. The division test is
    // exact, not approximate: the floor of budget over factorial is precisely the largest count whose
    // product stays within budget, so the flag fires exactly when the true power exceeds it, verified
    // against exact arithmetic on all thirty reachable width-and-count pairs; no intermediate can wrap,
    // since a retained value is at most the budget times the largest admitted factorial, far inside the
    // type even after one further multiply. The six-block witness in the battery
    // pins this guard: a 217-element closure beside six-factorial to the fifth power assignments.
    {
        std::size_t factorial{1};

        for (std::size_t at{2}; at <= wall; ++at)
        {
            factorial *= at;
        }

        std::size_t assignments{1};

        auto overflowed{false};

        for (std::size_t at{1}; at < nodes.size() && !overflowed; ++at)
        {
            overflowed = assignments > 100000 / factorial;

            assignments *= factorial;
        }

        if (overflowed || assignments > 100000)
        {
            std::cout << "refused: the labeling search exceeds its budget\n";

            return std::nullopt;
        }
    }

    const auto base_dense{dense.at(base)};

    std::vector<std::vector<int>> permutations;

    {
        std::vector<int> identity(wall);

        for (std::size_t at{0}; at < wall; ++at)
        {
            identity[at] = static_cast<int>(at);
        }

        auto current{identity};

        do
        {
            permutations.push_back(current);
        } while (std::ranges::next_permutation(current).found);
    }

    std::vector<std::vector<int>> flavor_of(nodes.size());

    std::array<std::vector<int>, 256> sigma{};

    auto solved{false};

    std::vector<std::size_t> choice(nodes.size(), 0);

    for (auto searching{true}; searching && !solved;)
    {
        for (std::size_t at{0}; at < nodes.size(); ++at)
        {
            flavor_of[at] = permutations[at == base_dense ? 0 : choice[at]];
        }

        std::array<std::vector<int>, 256> trial{};

        auto consistent{true};

        for (const auto& edge : edges)
        {
            std::vector<int> derived(wall, -1);

            for (std::size_t member{0}; member < wall; ++member)
            {
                derived[static_cast<std::size_t>(flavor_of[edge.source][member])] =
                        flavor_of[edge.target][edge.map[member]];
            }

            auto& slot{trial[static_cast<std::size_t>(edge.byte)]};

            if (slot.empty())
            {
                slot = derived;
            }
            else if (slot != derived)
            {
                consistent = false;

                break;
            }
        }

        if (consistent)
        {
            sigma = trial;

            solved = true;

            break;
        }

        // Advance the mixed-radix choice vector, skipping the base.
        searching = false;

        for (std::size_t at{0}; at < nodes.size(); ++at)
        {
            if (at == base_dense)
            {
                continue;
            }

            if (++choice[at] < permutations.size())
            {
                searching = true;

                break;
            }

            choice[at] = 0;
        }
    }

    if (!solved)
    {
        std::cout << "refused: no flavor labeling makes the byte actions source-independent; exact summary count "
                  << semigroup.size() << "\n";

        return std::nullopt;
    }

    // State flavors must be consistent wherever a state appears across kernel subsets, and every restart
    // entry from the initial state must land on a flavored state.
    std::vector<int> state_flavor(table.states, -1);

    for (std::size_t at{0}; at < nodes.size(); ++at)
    {
        for (std::size_t member{0}; member < wall; ++member)
        {
            const auto state{subsets[nodes[at]][member]};

            if (state_flavor[state] == -1)
            {
                state_flavor[state] = flavor_of[at][member];
            }
            else if (state_flavor[state] != flavor_of[at][member])
            {
                std::cout << "refused: a state carries two flavors across kernel subsets\n";

                return std::nullopt;
            }
        }
    }

    // The boundary seed: the initial state's outgoing images must name one flavor once each byte's
    // permutation is undone; byte-independence is the seed's own consistency check. A live image the
    // labeling does not cover is a refusal, never a skip: the true scan can occupy it while the carry
    // claims a flavor, and the conditioned cloud would exclude the true reading.
    int seed{-1};

    for (int byte{0}; byte < 256; ++byte)
    {
        const auto to{table.next[table.init][static_cast<std::size_t>(byte)]};

        if (to == dead)
        {
            continue;
        }

        if (state_flavor[static_cast<std::size_t>(to)] == -1)
        {
            std::cout << "refused: a live initial-state image is unflavored\n";

            return std::nullopt;
        }

        const auto& perm{sigma[static_cast<std::size_t>(byte)]};

        int undone{-1};

        for (std::size_t at{0}; at < wall; ++at)
        {
            if (perm[at] == state_flavor[static_cast<std::size_t>(to)])
            {
                undone = static_cast<int>(at);
            }
        }

        if (seed == -1)
        {
            seed = undone;
        }
        else if (seed != undone)
        {
            std::cout << "refused: the boundary seed is byte-dependent\n";

            return std::nullopt;
        }
    }

    if (seed == -1)
    {
        std::cout << "refused: no restart entry reaches a flavored state\n";

        return std::nullopt;
    }

    // The carry group: closure of the byte permutations under composition.
    std::set<std::vector<int>> group;

    std::vector<int> identity(wall);

    for (std::size_t at{0}; at < wall; ++at)
    {
        identity[at] = static_cast<int>(at);
    }

    group.insert(identity);

    std::deque<std::vector<int>> pending;

    for (int byte{0}; byte < 256; ++byte)
    {
        if (group.insert(sigma[static_cast<std::size_t>(byte)]).second)
        {
            pending.push_back(sigma[static_cast<std::size_t>(byte)]);
        }
    }

    std::set<std::vector<int>> generators{group};

    while (!pending.empty())
    {
        const auto element{pending.front()};

        pending.pop_front();

        for (const auto& generator : generators)
        {
            std::vector<int> composed(wall);

            for (std::size_t at{0}; at < wall; ++at)
            {
                composed[at] = generator[static_cast<std::size_t>(element[at])];
            }

            if (group.insert(composed).second)
            {
                pending.push_back(composed);
            }
        }
    }

    std::size_t kernel_count{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        kernel_count += kernel[node] != 0 ? 1 : 0;
    }

    return Carry{
            .width = wall,
            .kernel_subsets = kernel_count,
            .semigroup = semigroup.size(),
            .state_flavor = std::move(state_flavor),
            .sigma = sigma,
            .group = std::move(group),
            .seed = seed};
}

// Liveness, re-entrancy, the window walk with a chosen starting cloud, and the serial boundary scan.
std::vector<std::size_t> live_states(const Table& table)
{
    std::vector<std::size_t> states;

    for (std::size_t state{0}; state < table.states; ++state)
    {
        auto live{table.accept[state] != 0};

        for (int byte{0}; byte < 256 && !live; ++byte)
        {
            live = table.next[state][static_cast<std::size_t>(byte)] != dead;
        }

        if (live)
        {
            states.push_back(state);
        }
    }

    return states;
}

bool init_reentrant(const Table& table)
{
    for (std::size_t state{0}; state < table.states; ++state)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            if (table.next[state][static_cast<std::size_t>(byte)] == static_cast<int>(table.init))
            {
                return true;
            }
        }
    }

    return false;
}

// The shipped window walk with a chosen starting cloud: flavor -1 seats every live state, the
// unconditional decision; a real flavor seats that flavor's states alone, the conditional one. The
// stepping, seeding, rename, and unanimity rules mirror the shipped decision exactly.
std::optional<std::size_t> window_walk(
        const Table& table, const Carry& carry, const bool reentrant, const std::string_view window, const int flavor)
{
    // The shipped decision's own scope, mirrored: the empty window certifies nothing, and a nullable
    // token set is refused wholesale.
    if (window.empty() || table.accept[table.init] != 0)
    {
        return std::nullopt;
    }

    constexpr auto before{std::numeric_limits<std::size_t>::max()};

    std::set<std::pair<std::size_t, std::size_t>> cloud;

    for (const auto state : live_states(table))
    {
        if (flavor < 0 || carry.state_flavor[state] == flavor)
        {
            cloud.emplace(state, before);
        }
    }

    if (cloud.empty())
    {
        return std::nullopt;
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        auto accepting{false};

        for (const auto& [state, origin] : cloud)
        {
            accepting = accepting || table.accept[state] != 0;
        }

        std::set<std::pair<std::size_t, std::size_t>> next;

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{table.next[state][byte]}; to != dead)
            {
                const auto begins{state == table.init && !reentrant};

                next.emplace(static_cast<std::size_t>(to), begins ? at : origin);
            }
        }

        if (accepting)
        {
            if (const auto to{table.next[table.init][byte]}; to != dead)
            {
                next.emplace(static_cast<std::size_t>(to), at);
            }
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    const auto origin{cloud.begin()->second};

    for (const auto& [state, at] : cloud)
    {
        if (at != origin)
        {
            return std::nullopt;
        }
    }

    return origin == before ? std::nullopt : std::optional{origin};
}

// The serial maximal-munch scan of a tokenizable input, boundary positions out; zero-lag makes the
// restart-at-the-stuck-byte loop the whole story.
std::optional<std::vector<std::size_t>> serial_boundaries(const Table& table, const std::string_view input)
{
    std::vector<std::size_t> starts{0};

    std::size_t state{table.init};

    auto consumed_first{false};

    for (std::size_t at{0}; at < input.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(input[at])};

        if (const auto to{table.next[state][byte]}; to != dead)
        {
            state = static_cast<std::size_t>(to);
        }
        else
        {
            if (table.accept[state] == 0)
            {
                return std::nullopt;
            }

            starts.push_back(at);

            const auto restart{table.next[table.init][byte]};

            if (restart == dead)
            {
                return std::nullopt;
            }

            state = static_cast<std::size_t>(restart);
        }

        consumed_first = true;
    }

    if (consumed_first && table.accept[state] == 0)
    {
        return std::nullopt;
    }

    starts.push_back(input.size());

    return starts;
}

struct Premise
{
    bool holds{true};
    std::size_t witness_state{};
    int witness_byte{};
};

// The zero-lag premise on the (state, seen-accept) product: seen records an accept at or before the
// current position, so a death at a non-accepting seen state is exactly a stale rollback.
Premise zero_lag(const Table& table)
{
    std::vector<std::array<char, 2>> visited(table.states, {0, 0});

    std::deque<std::pair<std::size_t, int>> pending{{table.init, 0}};

    visited[table.init][0] = 1;

    Premise premise{};

    while (!pending.empty())
    {
        const auto [state, seen]{pending.front()};

        pending.pop_front();

        // End-of-input is a death available at every position; a seen run at a non-accepting state is
        // the violation itself, with the end of input as its killing "byte".
        if (seen != 0 && table.accept[state] == 0 && premise.holds)
        {
            premise = {.holds = false, .witness_state = state, .witness_byte = 256};
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto to{table.next[state][static_cast<std::size_t>(byte)]};

            if (to == dead)
            {
                continue;
            }

            const auto target{static_cast<std::size_t>(to)};

            const auto next_seen{seen != 0 || table.accept[target] != 0 ? 1 : 0};

            if (visited[target][static_cast<std::size_t>(next_seen)] == 0)
            {
                visited[target][static_cast<std::size_t>(next_seen)] = 1;

                pending.emplace_back(target, next_seen);
            }
        }
    }

    return premise;
}

struct Verdict
{
    std::size_t nodes{};
    std::size_t sustained{};
    std::size_t floor_start{};
    std::size_t wall_floor{};
    bool bounded{};
};

// The subset decider under the bare-state dynamics the zero-lag lemma licenses: members advance through
// live transitions, a dying accepting member restarts at the current byte through the initial state, a
// dying non-accepting member is an eliminated hypothesis. The quantities are state-granular: a direct
// arrival and a restart arrival at the same state merge although their current tokens began at different
// places, so sustained width and floors are lower bounds on origin-distinguished readings. The licence
// direction is unaffected, a floor of two or more is a real wall, while a smaller floor does not conclude
// the absence of an origin-level wall. Sustained width is the largest subset on a cycle, the floor of a
// node is the least width its forward closure reaches, and the wall floor is the largest floor any
// reachable node carries.
Verdict decide(const Table& table)
{
    std::vector<std::size_t> start;

    for (std::size_t state{0}; state < table.states; ++state)
    {
        auto live{table.accept[state] != 0};

        for (int byte{0}; byte < 256 && !live; ++byte)
        {
            live = table.next[state][static_cast<std::size_t>(byte)] != dead;
        }

        if (live)
        {
            start.push_back(state);
        }
    }

    std::map<std::vector<std::size_t>, std::size_t> ids;

    std::vector<std::vector<std::size_t>> subsets;

    std::vector<std::set<std::size_t>> successors;

    const auto intern{[&](std::vector<std::size_t> subset) {
        if (const auto found{ids.find(subset)}; found != ids.end())
        {
            return found->second;
        }

        ids.emplace(subset, subsets.size());

        subsets.push_back(std::move(subset));

        successors.emplace_back();

        return subsets.size() - 1;
    }};

    const auto origin{intern(start)};

    for (std::size_t at{0}; at < subsets.size(); ++at)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            std::set<std::size_t> stepped;

            auto restart{false};

            for (const auto state : subsets[at])
            {
                if (const auto to{table.next[state][static_cast<std::size_t>(byte)]}; to != dead)
                {
                    stepped.insert(static_cast<std::size_t>(to));
                }
                else if (table.accept[state] != 0)
                {
                    restart = true;
                }
            }

            if (restart)
            {
                if (const auto to{table.next[table.init][static_cast<std::size_t>(byte)]}; to != dead)
                {
                    stepped.insert(static_cast<std::size_t>(to));
                }
            }

            // Interning may grow the vectors, so the target is taken before the source's edge set is
            // addressed; the sanitizer caught the elided order as a use after free.
            const auto target{intern({stepped.begin(), stepped.end()})};

            successors[at].insert(target);

            // The decider's bound is far tighter than the synthesizer's: the sustained-width phase below
            // is quadratic in the node count, so a graph sitting exactly at a large threshold would slip
            // a matching cap and crawl. Every legitimate diagnostic caller measures a few dozen nodes.
            if (subsets.size() > 128)
            {
                return {.bounded = true};
            }
        }
    }

    // A node lies on a cycle exactly when it reaches itself; the graphs measure tiny, so the quadratic
    // reachability is the simple honest choice.
    const auto count{subsets.size()};

    std::vector<std::vector<char>> reaches(count, std::vector<char>(count, 0));

    for (std::size_t from{0}; from < count; ++from)
    {
        std::deque<std::size_t> pending{from};

        while (!pending.empty())
        {
            const auto node{pending.front()};

            pending.pop_front();

            for (const auto to : successors[node])
            {
                if (reaches[from][to] == 0)
                {
                    reaches[from][to] = 1;

                    pending.push_back(to);
                }
            }
        }
    }

    std::size_t sustained{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (reaches[node][node] != 0)
        {
            sustained = std::max(sustained, subsets[node].size());
        }
    }

    // Floors by the same ascending-width reverse propagation as the synthesizer: single-pass by
    // construction, so an adversarial chain cannot make the floor propagation quadratic. The sustained
    // phase above remains quadratic by design and is why this walker's bound is the tight one.
    std::vector<std::vector<std::size_t>> predecessors_of(count);

    for (std::size_t node{0}; node < count; ++node)
    {
        for (const auto to : successors[node])
        {
            predecessors_of[to].push_back(node);
        }
    }

    constexpr auto untouched{std::numeric_limits<std::size_t>::max()};

    std::vector<std::size_t> floor(count, untouched);

    std::vector<std::size_t> order(count);

    for (std::size_t node{0}; node < count; ++node)
    {
        order[node] = node;
    }

    std::ranges::sort(
            order, [&](const auto left, const auto right) { return subsets[left].size() < subsets[right].size(); });

    for (const auto origin : order)
    {
        if (floor[origin] != untouched)
        {
            continue;
        }

        floor[origin] = subsets[origin].size();

        std::vector<std::size_t> propagation{origin};

        while (!propagation.empty())
        {
            const auto node{propagation.back()};

            propagation.pop_back();

            for (const auto from : predecessors_of[node])
            {
                if (floor[from] == untouched)
                {
                    floor[from] = floor[origin];

                    propagation.push_back(from);
                }
            }
        }
    }

    std::size_t wall{0};

    for (std::size_t node{0}; node < count; ++node)
    {
        if (reaches[origin][node] != 0 || node == origin)
        {
            wall = std::max(wall, floor[node]);
        }
    }

    return {.nodes = count, .sustained = sustained, .floor_start = floor[origin], .wall_floor = wall};
}

// The empirical side of the theorem: the true maximal-munch scan, restarts included, must sit at the
// flavor the homomorphism predicts after every byte of a deterministic random corpus.
std::size_t check_theorem(const Table& table, const Carry& carry, const std::string& alphabet)
{
    std::uint64_t lcg{0x5eed5eed5eed5eedULL};

    const auto draw{[&lcg](const std::size_t bound) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;

        return static_cast<std::size_t>((lcg >> 33U) % bound);
    }};

    std::size_t checked{0};

    for (std::size_t trial{0}; trial < 200; ++trial)
    {
        std::size_t state{table.init};

        auto predicted{carry.seed};

        for (std::size_t at{0}; at < 400; ++at)
        {
            const auto byte{static_cast<unsigned char>(alphabet[draw(alphabet.size())])};

            auto to{table.next[state][byte]};

            if (to == dead)
            {
                if (table.accept[state] == 0)
                {
                    break;
                }

                to = table.next[table.init][byte];

                if (to == dead)
                {
                    break;
                }
            }

            state = static_cast<std::size_t>(to);

            predicted = carry.sigma[byte][static_cast<std::size_t>(predicted)];

            if (carry.state_flavor[state] != predicted)
            {
                return checked;
            }

            ++checked;
        }
    }

    return checked;
}

// Carry-driven planning: prefix flavors by the homomorphism, then from every equal-division target the
// first position whose two-to-four-byte window certifies under the position's flavor, the cut at the
// occurrence plus the reported origin, strictly advancing. The census counts every unconditional try so
// the wall's total refusal is asserted, not assumed.
struct Plan
{
    std::vector<std::size_t> cuts;
    std::size_t unconditional_certificates{};
};

Plan plan(
        const Table& table, const Carry& carry, const bool reentrant, const std::string_view input,
        const std::size_t chunks, const bool flip)
{
    std::vector<int> flavor_at(input.size() + 1);

    flavor_at[0] = carry.seed;

    for (std::size_t at{0}; at < input.size(); ++at)
    {
        flavor_at[at + 1] = carry.sigma[static_cast<unsigned char>(input[at])][static_cast<std::size_t>(flavor_at[at])];
    }

    const auto flavors{static_cast<int>(carry.group.begin()->size())};

    Plan result;

    std::size_t last{0};

    for (std::size_t target{1}; target < chunks; ++target)
    {
        const auto aim{target * input.size() / chunks};

        auto cut{std::optional<std::size_t>{}};

        for (auto at{std::max(aim, last + 1)}; at + 2 <= input.size() && !cut; ++at)
        {
            for (std::size_t length{2}; length <= 4 && at + length <= input.size() && !cut; ++length)
            {
                const auto window{input.substr(at, length)};

                if (window_walk(table, carry, reentrant, window, -1))
                {
                    ++result.unconditional_certificates;
                }

                const auto asked{flip ? (flavor_at[at] + 1) % flavors : flavor_at[at]};

                if (const auto origin{window_walk(table, carry, reentrant, window, asked)})
                {
                    if (at + *origin > last && at + *origin < input.size())
                    {
                        cut = at + *origin;
                    }
                }
            }
        }

        if (cut)
        {
            result.cuts.push_back(*cut);

            last = *cut;
        }
    }

    return result;
}

// Deterministic tokenizable corpora: bare runs and strings, the two-string flavor mixing each string
// type's delimiter into the other's content, the crux the carry exists to resolve.
std::string corpus(const std::size_t trial, const bool ticks)
{
    std::uint64_t lcg{0x5eed0000ULL + trial * 2654435761ULL};

    const auto draw{[&lcg](const std::size_t bound) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;

        return static_cast<std::size_t>((lcg >> 33U) % bound);
    }};

    std::string text;

    while (text.size() < 1200)
    {
        if (draw(10) < 6)
        {
            const auto length{1 + draw(8)};

            for (std::size_t at{0}; at < length; ++at)
            {
                text += static_cast<char>('a' + draw(6));
            }
        }
        else
        {
            const auto tick{ticks && draw(2) == 0};

            const auto delimiter{tick ? '`' : '"'};

            text += delimiter;

            const auto length{draw(12)};

            for (std::size_t at{0}; at < length; ++at)
            {
                const auto roll{draw(8)};

                if (roll == 0)
                {
                    text += tick ? '"' : (ticks ? '`' : ' ');
                }
                else
                {
                    text += static_cast<char>('a' + draw(6));
                }
            }

            text += delimiter;
        }
    }

    return text;
}

struct Tally
{
    std::size_t cuts{};
    std::size_t off_boundary{};
    std::size_t splice_mismatches{};
    std::size_t unconditional{};
    std::size_t teeth_bad{};
};

Tally run(const Table& table, const Carry& carry, const bool ticks)
{
    const auto reentrant{init_reentrant(table)};

    Tally tally;

    for (std::size_t trial{0}; trial < 30; ++trial)
    {
        const auto text{corpus(trial, ticks)};

        const auto serial{serial_boundaries(table, text)};

        if (!serial)
        {
            ++tally.splice_mismatches;

            continue;
        }

        const std::set<std::size_t> boundaries(serial->begin(), serial->end());

        const auto planned{plan(table, carry, reentrant, text, 8, false)};

        tally.cuts += planned.cuts.size();

        tally.unconditional += planned.unconditional_certificates;

        for (const auto cut : planned.cuts)
        {
            tally.off_boundary += boundaries.contains(cut) ? 0 : 1;
        }

        // The splice: every chunk scanned alone must consume completely, and the reassembled boundary
        // sequence must equal the serial one.
        std::vector<std::size_t> edges{0};

        edges.insert(edges.end(), planned.cuts.begin(), planned.cuts.end());

        edges.push_back(text.size());

        std::vector<std::size_t> spliced;

        auto broken{false};

        for (std::size_t piece{0}; piece + 1 < edges.size() && !broken; ++piece)
        {
            const auto local{serial_boundaries(
                    table, std::string_view{text}.substr(edges[piece], edges[piece + 1] - edges[piece]))};

            if (!local)
            {
                broken = true;

                break;
            }

            for (std::size_t at{0}; at + 1 < local->size(); ++at)
            {
                spliced.push_back(edges[piece] + (*local)[at]);
            }
        }

        spliced.push_back(text.size());

        if (broken || spliced != *serial)
        {
            ++tally.splice_mismatches;
        }

        const auto wrong{plan(table, carry, reentrant, text, 8, true)};

        for (const auto cut : wrong.cuts)
        {
            tally.teeth_bad += boundaries.contains(cut) ? 0 : 1;
        }
    }

    return tally;
}

std::size_t bits_for(const std::size_t values)
{
    std::size_t bits{0};

    while ((std::size_t{1} << bits) < values)
    {
        ++bits;
    }

    return bits;
}

// One maximal-munch step with the zero-lag restart; nullopt is a hard scan failure.
std::optional<std::size_t> scan_step(const Table& table, const std::size_t state, const unsigned char byte)
{
    if (const auto to{table.next[state][byte]}; to != dead)
    {
        return static_cast<std::size_t>(to);
    }

    if (table.accept[state] == 0)
    {
        return std::nullopt;
    }

    if (const auto to{table.next[table.init][byte]}; to != dead)
    {
        return static_cast<std::size_t>(to);
    }

    return std::nullopt;
}

std::optional<std::size_t> scan_word(const Table& table, std::size_t state, const std::string_view word)
{
    for (const auto letter : word)
    {
        const auto next{scan_step(table, state, static_cast<unsigned char>(letter))};

        if (!next)
        {
            return std::nullopt;
        }

        state = *next;
    }

    return state;
}

int carry_word(const Carry& carry, int flavor, const std::string_view word)
{
    for (const auto letter : word)
    {
        flavor = carry.sigma[static_cast<unsigned char>(letter)][static_cast<std::size_t>(flavor)];
    }

    return flavor;
}

std::vector<std::string> words_up_to(const std::string& alphabet, const std::size_t longest)
{
    std::vector<std::string> words{""};

    for (std::size_t from{0}, length{0}; length < longest; ++length)
    {
        const auto until{words.size()};

        for (; from < until; ++from)
        {
            for (const auto letter : alphabet)
            {
                words.push_back(words[from] + letter);
            }
        }
    }

    return words;
}

// The hazard witness for an ordered flavor pair: a tokenizable input carrying the true
// flavor at an occurrence whose assumed-flavor certificate cuts off the serial segmentation.
struct Witness
{
    std::string input;
    std::size_t cut{};
};

std::optional<Witness> find_witness(
        const Table& table, const Carry& carry, const bool reentrant, const std::string& alphabet, const int truth,
        const int assumed)
{
    const auto windows{words_up_to(alphabet, 3)};

    const auto prefixes{words_up_to(alphabet, 6)};

    const auto suffixes{words_up_to(alphabet, 2)};

    for (const auto& window : windows)
    {
        if (window.size() < 2)
        {
            continue;
        }

        const auto origin{window_walk(table, carry, reentrant, window, assumed)};

        if (!origin)
        {
            continue;
        }

        for (const auto& prefix : prefixes)
        {
            if (carry_word(carry, carry.seed, prefix) != truth)
            {
                continue;
            }

            const auto entered{scan_word(table, table.init, prefix)};

            if (!entered)
            {
                continue;
            }

            const auto crossed{scan_word(table, *entered, window)};

            if (!crossed)
            {
                continue;
            }

            for (const auto& suffix : suffixes)
            {
                const auto finished{scan_word(table, *crossed, suffix)};

                if (!finished || table.accept[*finished] == 0)
                {
                    continue;
                }

                const auto input{prefix + window + suffix};

                const auto cut{prefix.size() + *origin};

                if (cut == 0 || cut >= input.size())
                {
                    continue;
                }

                const auto serial{serial_boundaries(table, input)};

                if (!serial)
                {
                    continue;
                }

                if (!std::ranges::binary_search(*serial, cut))
                {
                    return Witness{.input = input, .cut = cut};
                }
            }
        }
    }

    return std::nullopt;
}

struct Prices
{
    std::size_t orbit{};
    std::size_t positional{};
    std::size_t compositional{};
    std::size_t summary{};
    std::size_t witnesses{};
};

Prices price(const std::string& name, const Table& table, const Carry& carry, const std::string& alphabet)
{
    const auto reentrant{init_reentrant(table)};

    // The seed's orbit under the group; transitivity means every flavor occurs as a true prefix carry.
    std::set<int> orbit;

    for (const auto& element : carry.group)
    {
        orbit.insert(element[static_cast<std::size_t>(carry.seed)]);
    }

    const auto width{carry.group.begin()->size()};

    expect(orbit.size() == width, name + ": the seed's orbit does not reach every flavor");

    // Faithfulness holds by construction: group elements are stored as permutations, so distinct
    // elements differ on some flavor. What the witnesses below establish is a hazard relation:
    // conditioning on the wrong flavor licenses a cut off the serial segmentation. The three prices
    // bind three distinct services, named exactly: the orbit prices the conditioned flavor choice at a
    // position; the group prices composable flavor transfer, owed only by a service required to compose
    // arbitrary factors; the semigroup prices exact kernel transfer, a stronger service the cut
    // machinery never needs. None of the three binds every scheme providing the same cuts: a serial
    // flavor prepass realizes them without composing anything, and rescanning the raw prefix from the
    // initial state at each query carries zero bits, paying work instead. A scheme-wide bit bound needs
    // an explicit one-pass compositional interface and common-context fooling pairs, which live with
    // the width program's summary model, not here.

    Prices prices{
            .orbit = orbit.size(),
            .positional = bits_for(orbit.size()),
            .compositional = bits_for(carry.group.size()),
            .summary = bits_for(carry.semigroup)};

    for (const auto truth : orbit)
    {
        for (const auto assumed : orbit)
        {
            if (truth == assumed)
            {
                continue;
            }

            const auto witness{find_witness(table, carry, reentrant, alphabet, truth, assumed)};

            expect(witness.has_value(), name + ": no witness separates flavors " + std::to_string(truth) + " and " +
                                                std::to_string(assumed));

            if (witness)
            {
                ++prices.witnesses;

                const auto serial{serial_boundaries(table, witness->input)};

                expect(serial && !std::ranges::binary_search(*serial, witness->cut),
                       name + ": a recorded witness cut lies on the serial segmentation after all");
            }
        }
    }

    std::cout << name << ": orbit " << prices.orbit << ", positional bits " << prices.positional
              << ", compositional bits " << prices.compositional << " (group " << carry.group.size()
              << "), summary bits " << prices.summary << " (semigroup " << carry.semigroup << "), witnesses "
              << prices.witnesses << "\n";

    return prices;
}

enum class Local : std::size_t
{
    Str,
    Tick,
    Chunk,
    Quoted,
    Bare,
    Comma,
    Newline,
    A,
    Abc,
    B,
    X,
};

// The absolute-wall pair: strings over an outside that accepts every byte, so no elimination evidence
// exists and the quote parity survives forever; the two-string variant mixes each delimiter into the
// other's content and its carry group is the non-abelian S3.
Table gadget()
{
    Builder_dbg builder;

    builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"'})), Local::Chunk, 2);

    return extract(builder.dfa());
}

Table two_string()
{
    Builder_dbg builder;

    builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

    builder.add_token(concat(text("`"), kleene(any_of(Set::all() - Set{'`'})), text("`")), Local::Tick, 1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', '`'})), Local::Chunk, 2);

    return extract(builder.dfa());
}

// RFC 4180 CSV: the doubled-quote continuation re-enters the interior after an accept, so the row fails
// the end-of-input premise and every decider verdict for it is approximation-scoped; its parity remains
// grammar-resolvable in the state-granular sense, a corpus question, never a licence.
Table csv()
{
    Builder_dbg builder;

    builder.add_token(
            concat(text("\""), kleene(choice(any_of(Set::all() - Set{'"'}), text("\"\""))), text("\"")), Local::Quoted,
            1);

    builder.add_token(plus(any_of(Set::all() - Set{'"', ',', '\n', '\r'})), Local::Bare, 2);

    builder.add_token(text(","), Local::Comma, 2);

    builder.add_token(concat(optional(text("\r")), text("\n")), Local::Newline, 2);

    return extract(builder.dfa());
}

// Two premise refusals: RFC 8259 numbers accept at "12", continue non-accepting through "12.", and die
// on a non-digit, a stale rollback; the {a, ab*c, b, x} family is the classic rollback shape.
Table json_strict()
{
    Builder_dbg builder;

    figures::json(builder);

    return extract(builder.dfa());
}

Table c_like()
{
    Builder_dbg builder;

    figures::c_like(builder, false);

    return extract(builder.dfa());
}

Table rollback_family()
{
    Builder_dbg builder;

    builder.add_token(text("a"), Local::A, 1);

    builder.add_token(concat(text("a"), concat(kleene(text("b")), text("c"))), Local::Abc, 1);

    builder.add_token(text("b"), Local::B, 1);

    builder.add_token(text("x"), Local::X, 1);

    return extract(builder.dfa());
}

} // namespace

int main()
{
    // The wall verdicts: state-granular lower bounds under the premise, refused without it.
    {
        const auto verdict{decide(gadget())};

        std::cout << "parity gadget: nodes " << verdict.nodes << ", sustained " << verdict.sustained << ", wall floor "
                  << verdict.wall_floor << "\n";

        expect(zero_lag(gadget()).holds, "the parity gadget fails the zero-lag premise");

        expect(verdict.nodes == 3 && verdict.sustained == 2 && verdict.wall_floor == 2,
               "the parity gadget's absolute wall drifted from 3 nodes, sustained 2, floor 2");

        const auto two{decide(two_string())};

        std::cout << "two-string gadget: nodes " << two.nodes << ", sustained " << two.sustained << ", wall floor "
                  << two.wall_floor << "\n";

        expect(zero_lag(two_string()).holds, "the two-string gadget fails the zero-lag premise");

        expect(two.nodes == 4 && two.sustained == 3 && two.wall_floor == 3,
               "the two-string absolute wall drifted from 4 nodes, sustained 3, floor 3");

        // Under the end-of-input-aware premise the CSV row fails honestly: the doubled-quote
        // continuation re-enters the string interior after the closed quote accepted, so an unterminated
        // tail rolls back with lag. Its decider verdicts are approximation-scoped and refused.
        expect(!zero_lag(csv()).holds, "the csv row passes the premise it must fail at end of input");

        const auto conventional{decide(c_like())};

        expect(zero_lag(c_like()).holds, "the c-like row fails the zero-lag premise");

        expect(conventional.wall_floor == 0 && conventional.floor_start == 0,
               "the c-like row does not reach total cloud death");

        const auto strict{zero_lag(json_strict())};

        expect(!strict.holds && strict.witness_state == 12,
               "the strict-number row's premise refusal moved off the number state");

        expect(!zero_lag(rollback_family()).holds, "the rollback family passes the premise it must fail");
    }

    // The synthesizer must refuse where its licence does not hold: the CSV row fails the end-of-input
    // premise, and the strict-number row fails it too; both refusals print their reasons above.
    expect(!synthesize(csv()).has_value(), "the synthesizer accepted the csv row although its premise fails");

    expect(!synthesize(json_strict()).has_value(), "the synthesizer accepted a grammar that fails the premise");

    // The synthesized carries and the tracking theorem.
    const auto gadget_carry{synthesize(gadget())};

    expect(gadget_carry.has_value(), "the parity gadget refused to synthesize");

    const auto two_carry{synthesize(two_string())};

    expect(two_carry.has_value(), "the two-string gadget refused to synthesize");

    if (gadget_carry && two_carry)
    {
        std::cout << "parity gadget carry: semigroup " << gadget_carry->semigroup << ", group "
                  << gadget_carry->group.size() << "; two-string carry: semigroup " << two_carry->semigroup
                  << ", group " << two_carry->group.size() << "\n";

        expect(gadget_carry->semigroup == 4 && gadget_carry->group.size() == 2,
               "the parity gadget's carry drifted from semigroup 4, group 2");

        auto only_quote{true};

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto identity{gadget_carry->sigma[static_cast<std::size_t>(byte)][0] == 0};

            only_quote = only_quote && (identity == (byte != '"'));
        }

        expect(only_quote, "the parity gadget's homomorphism is not quote parity");

        expect(two_carry->semigroup == 18 && two_carry->group.size() == 6,
               "the two-string carry drifted from semigroup 18, group 6");

        const auto& quote{two_carry->sigma[static_cast<std::size_t>('"')]};

        const auto& tick{two_carry->sigma[static_cast<std::size_t>('`')]};

        std::vector<int> quote_tick(3);

        std::vector<int> tick_quote(3);

        for (std::size_t at{0}; at < 3; ++at)
        {
            quote_tick[at] = tick[static_cast<std::size_t>(quote[at])];

            tick_quote[at] = quote[static_cast<std::size_t>(tick[at])];
        }

        expect(quote_tick != tick_quote, "the two-string carry composition commutes although it must not");

        expect(check_theorem(gadget(), *gadget_carry, "abcdefgh\"\"\"  ") == 80000,
               "the parity gadget's tracking check drifted or mismatched");

        expect(check_theorem(two_string(), *two_carry, "abcdef\"\"``  ") == 80000,
               "the two-string tracking check drifted or mismatched");
    }

    // Certificates behind the resolved flavor, and the plans they license.
    if (gadget_carry && two_carry)
    {
        const auto reentrant{init_reentrant(gadget())};

        const auto outside{window_walk(gadget(), *gadget_carry, reentrant, "\"a", 0)};

        const auto inside{window_walk(gadget(), *gadget_carry, reentrant, "\"a", 1)};

        expect(outside == std::optional<std::size_t>{0} && inside == std::optional<std::size_t>{1},
               "the flavor-dependent origins of the quote-letter window drifted");

        expect(!window_walk(gadget(), *gadget_carry, reentrant, "\"a", -1).has_value(),
               "the wall certified a window unconditionally");

        const auto one{run(gadget(), *gadget_carry, false)};

        std::cout << "parity gadget planning: cuts " << one.cuts << ", off-boundary " << one.off_boundary
                  << ", splice mismatches " << one.splice_mismatches << ", unconditional " << one.unconditional
                  << ", wrong-flavor bad cuts " << one.teeth_bad << "\n";

        expect(one.cuts == 210 && one.off_boundary == 0 && one.splice_mismatches == 0 && one.unconditional == 0 &&
                       one.teeth_bad == 210,
               "the parity gadget's planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

        const auto both{run(two_string(), *two_carry, true)};

        std::cout << "two-string planning: cuts " << both.cuts << ", off-boundary " << both.off_boundary
                  << ", splice mismatches " << both.splice_mismatches << ", unconditional " << both.unconditional
                  << ", wrong-flavor bad cuts " << both.teeth_bad << "\n";

        expect(both.cuts == 210 && both.off_boundary == 0 && both.splice_mismatches == 0 && both.unconditional == 0 &&
                       both.teeth_bad == 210,
               "the two-string planning campaign drifted from 210 clean cuts and 210 wrong-flavor refutations");

        // Strict forward progress, pinned as a malformed-plan determinism invariant: this input's last
        // string never closes, so the serial oracle refuses it, and no completely tokenizable input has
        // been found that separates the floors, so the guard appears redundant under the tokenizable
        // contract and load-bearing only here, where a relaxed floor adds a sixth cut at twelve. The
        // planner has no tokenizability precondition of its own, so its malformed behavior is pinned.
        const auto reentrant_gadget{init_reentrant(gadget())};

        const auto forward{plan(gadget(), *gadget_carry, reentrant_gadget, "\"a\"b\"c\"d\"e\"f\"", 7, false)};

        expect(forward.cuts == (std::vector<std::size_t>{3, 4, 7, 8, 11}),
               "the strict-forward plan drifted from 3, 4, 7, 8, 11");
    }

    // The price tower with its witness matrices.
    if (gadget_carry && two_carry)
    {
        const auto one{price("parity gadget", gadget(), *gadget_carry, "a\"")};

        expect(one.orbit == 2 && one.positional == 1 && one.compositional == 1 && one.summary == 2 &&
                       one.witnesses == 2,
               "the parity gadget's price tower drifted from 2 flavors, bits 1, 1, 2, witnesses 2");

        const auto both{price("two-string gadget", two_string(), *two_carry, "a\"`")};

        expect(both.orbit == 3 && both.positional == 2 && both.compositional == 3 && both.summary == 5 &&
                       both.witnesses == 6,
               "the two-string price tower drifted from 3 flavors, bits 2, 3, 5, witnesses 6");

        expect(both.positional < both.compositional && both.compositional < both.summary,
               "the two-string tower does not separate strictly");
    }

    // The refusal battery. Fixtured refusals: the unflavored initial image at both ends of the byte
    // order, the end-of-input premise at both byte ends of both products, the nullable scope for
    // synthesis and for the window walk directly, the closure budget, the assignments budget, the
    // subset budget, both labeling caps at both sides of their thresholds, the floor below two, and the
    // byte-dependent seed. The fixtures pin each guard's existence and direction; the exact budget
    // constants are policy, pinned only where an instance sits at the boundary. Defense-in-depth refusals
    // without a reachable negative fixture, recorded rather than pretended: the kernel-closure and
    // member-map checks (a violating shape falls out of the kernel before the check can fire) and the
    // labeling-conflict search (a byte's action is state-determined, so subset-dependent behavior needs
    // restart asymmetry no small table has produced). The window walk's nullable half left this list
    // when the two-state pair below showed the unguarded cloud certifying through the rename rule.
    {
        // A live unflavored initial-state image: one token over B* d B* d B* with the delimiter at the
        // top of the byte order, so a weakened guard runs past every unflavored image without tripping
        // the seed's own consistency check and synthesizes an unsound carry, failing this fixture, while
        // an ASCII delimiter would merely refuse for the wrong reason.
        Builder_dbg builder;

        const auto nond{any_of(Set::all() - Set{'\xff'})};

        builder.add_token(
                concat(kleene(nond), concat(text("\xff"), concat(kleene(nond), concat(text("\xff"), kleene(nond))))),
                Local::Str, 1);

        expect(!synthesize(extract(builder.dfa())).has_value(),
               "a live unflavored initial-state image was not refused");
    }

    {
        // The closure budget: five string types sit inside both dimensional caps, yet their transfer
        // closure is beyond any enumeration; the budget must refuse it promptly.
        Builder_dbg builder;

        const std::string delimiters{"\"`'#$"};

        Set bare_set{Set::all()};

        for (const auto delimiter : delimiters)
        {
            bare_set = bare_set - Set{delimiter};
        }

        for (std::size_t at{0}; at < delimiters.size(); ++at)
        {
            const auto delimiter{delimiters[at]};

            builder.add_token(
                    concat(text(std::string(1, delimiter)),
                           concat(kleene(any_of(Set::all() - Set{delimiter})), text(std::string(1, delimiter)))),
                    static_cast<Local>(at), 1);
        }

        builder.add_token(plus(any_of(bare_set)), Local::X, 2);

        expect(!synthesize(extract(builder.dfa())).has_value(), "the closure budget did not refuse promptly");
    }

    {
        // The subset-count cap, pinned on its own: seven kernel subsets of width two (six rotating bare
        // phases beside one string interior), which the labeling search could handle if admitted, so a
        // relaxed count cap synthesizes and fails this fixture.
        Table phases;

        phases.states = 9;

        phases.init = 0;

        phases.next.assign(9, {});

        for (auto& row : phases.next)
        {
            row.fill(dead);
        }

        constexpr std::size_t first_bare{1};

        constexpr std::size_t interior{7};

        constexpr std::size_t closed{8};

        for (int byte{0}; byte < 256; ++byte)
        {
            phases.next[0][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(interior) : static_cast<int>(first_bare);

            for (std::size_t at{0}; at < 6; ++at)
            {
                phases.next[first_bare + at][static_cast<std::size_t>(byte)] =
                        byte == '"' ? dead :
                        byte == 'r' ? static_cast<int>(first_bare + (at + 1) % 6) :
                                      static_cast<int>(first_bare + at);
            }

            phases.next[interior][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(closed) : static_cast<int>(interior);

            phases.next[closed][static_cast<std::size_t>(byte)] = dead;
        }

        phases.accept.assign(9, 0);

        for (std::size_t at{0}; at < 6; ++at)
        {
            phases.accept[first_bare + at] = 1;
        }

        phases.accept[closed] = 1;

        expect(!synthesize(phases).has_value(), "the subset-count cap did not refuse the seven phases");
    }

    {
        // The wall-width cap, pinned on its own: a single kernel subset of width seven, rotating under
        // one byte, which the identity labeling would accept if admitted, so a relaxed width cap
        // synthesizes and fails this fixture.
        Table rotation;

        rotation.states = 8;

        rotation.init = 0;

        rotation.next.assign(8, {});

        for (auto& row : rotation.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            rotation.next[0][static_cast<std::size_t>(byte)] = byte == 'g' ? 2 : 1;

            for (std::size_t at{0}; at < 7; ++at)
            {
                rotation.next[1 + at][static_cast<std::size_t>(byte)] =
                        byte == 'g' ? static_cast<int>(1 + (at + 1) % 7) : static_cast<int>(1 + at);
            }
        }

        rotation.accept.assign(8, 1);

        rotation.accept[0] = 0;

        expect(!synthesize(rotation).has_value(), "the wall-width cap did not refuse the seven rotation");
    }

    {
        // The floor refusal: redirecting every initial-state image to the bare state removes the
        // interior's entry, the parity pair collapses, and the state-granular floor falls below two, so
        // nothing is licensed and the synthesis must refuse.
        Table redirected;

        redirected.states = 4;

        redirected.init = 0;

        redirected.next.assign(4, {});

        for (auto& row : redirected.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            redirected.next[0][static_cast<std::size_t>(byte)] = 1;

            redirected.next[1][static_cast<std::size_t>(byte)] = byte == '"' ? dead : 1;

            redirected.next[2][static_cast<std::size_t>(byte)] = byte == '"' ? 3 : 2;

            redirected.next[3][static_cast<std::size_t>(byte)] = dead;
        }

        redirected.accept = {0, 1, 0, 1};

        expect(!synthesize(redirected).has_value(), "a floor below two was not refused");
    }

    {
        // End-of-input rollback: the only stale death of this table is the end of input, which no
        // byte-level check sees; the premise must refuse it all the same.
        Table rollback;

        rollback.states = 3;

        rollback.init = 0;

        rollback.next.assign(3, {});

        for (auto& row : rollback.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            rollback.next[0][static_cast<std::size_t>(byte)] = byte == 'a' ? 2 : 1;

            rollback.next[1][static_cast<std::size_t>(byte)] = byte == 'a' ? 1 : 2;

            rollback.next[2][static_cast<std::size_t>(byte)] = byte == 'a' ? 0 : dead;
        }

        rollback.accept = {0, 1, 1};

        expect(!synthesize(rollback).has_value(), "an end-of-input rollback table was not refused");
    }

    {
        // The nullable scope: the shipped certificate refuses nullable sets wholesale, and both the
        // synthesis and the window walk mirror it.
        Builder_dbg builder;

        builder.add_token(concat(text("\""), kleene(any_of(Set::all() - Set{'"'})), text("\"")), Local::Str, 1);

        builder.add_token(plus(any_of(Set::all() - Set{'"'})), Local::Chunk, 2);

        builder.add_token(kleene(text("e")), Local::Bare, 3);

        expect(!synthesize(extract(builder.dfa())).has_value(), "a nullable token set was not refused");
    }

    {
        // The resource cap: seven independent string types build an eight-subset, width-eight wall that
        // passes every other licence, and the subset-count cap must refuse it before any factorial
        // labeling work is attempted; this fixture also has to return promptly.
        Builder_dbg builder;

        const std::string delimiters{"\"`'#$%&"};

        Set bare_set{Set::all()};

        for (const auto delimiter : delimiters)
        {
            bare_set = bare_set - Set{delimiter};
        }

        for (std::size_t at{0}; at < delimiters.size(); ++at)
        {
            const auto delimiter{delimiters[at]};

            builder.add_token(
                    concat(text(std::string(1, delimiter)),
                           concat(kleene(any_of(Set::all() - Set{delimiter})), text(std::string(1, delimiter)))),
                    static_cast<Local>(at), 1);
        }

        builder.add_token(plus(any_of(bare_set)), Local::X, 2);

        expect(!synthesize(extract(builder.dfa())).has_value(),
               "the labeling cap did not refuse the seven-string wall");
    }

    {
        // The seed loop's last byte: the parity gadget with the initial state's image at byte 255
        // poisoned to the interior, while the closed state absorbs that byte so no restart ever consults
        // the poison and the wall stands untouched. The tip refuses on the byte-dependent seed, found
        // only at byte 255; a loop that stops one byte short synthesizes an unsound carry and fails.
        Table topmost;

        topmost.states = 4;

        topmost.init = 0;

        topmost.next.assign(4, {});

        for (auto& row : topmost.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            topmost.next[0][static_cast<std::size_t>(byte)] = byte == 0xff || byte == '"' ? 2 : 1;

            topmost.next[1][static_cast<std::size_t>(byte)] = byte == '"' ? dead : 1;

            topmost.next[2][static_cast<std::size_t>(byte)] = byte == '"' ? 3 : 2;

            topmost.next[3][static_cast<std::size_t>(byte)] = byte == 0xff ? 3 : dead;
        }

        topmost.accept = {0, 1, 0, 1};

        expect(!synthesize(topmost).has_value(), "a poisoned image at byte 255 was not refused");
    }

    {
        // The premise at both byte ends of both products, on tables compilation could produce: every
        // state is reachable and co-accessible, ordinary bytes hold position, and one boundary byte
        // swaps the accepting state with a non-accepting twin, so the only stale continuation crosses
        // exactly that byte. A product truncated at either end reports zero lag and synthesizes the
        // width-two swap carry, failing these rows; the earlier pocket fixture used a co-dead state the
        // extraction would have pruned, a lesson recorded in the notes.
        const auto swap_table{[](const int boundary) {
            Table t;

            t.states = 3;

            t.init = 0;

            t.next.assign(3, {});

            for (auto& row : t.next)
            {
                row.fill(dead);
            }

            for (int byte{0}; byte < 256; ++byte)
            {
                t.next[0][static_cast<std::size_t>(byte)] = byte == boundary ? 2 : 1;

                t.next[1][static_cast<std::size_t>(byte)] = byte == boundary ? 2 : 1;

                t.next[2][static_cast<std::size_t>(byte)] = byte == boundary ? 1 : 2;
            }

            t.accept = {0, 1, 0};

            return t;
        }};

        for (const auto boundary : {0, 255})
        {
            const auto table{swap_table(boundary)};

            expect(!zero_lag(table).holds, "the witness product missed the stale swap at a byte end");

            expect(!synthesize(table).has_value(), "the premise product missed the stale swap at a byte end");
        }
    }

    {
        // The byte-zero twin of the last-byte pin: this one-token grammar's only unflavored image sits
        // at byte zero, so a guard that skips the loop's first byte synthesizes an unsound carry and
        // fails this fixture, the mirror of the poisoned image at byte 255.
        Builder_dbg builder;

        const auto x{Set{'\x00'}};

        const auto d{Set{'\x01'}};

        builder.add_token(
                concat(kleene(any_of(x)),
                       choice(concat(any_of(Set::all() - x - d), kleene(any_of(Set::all() - d))),
                              concat(any_of(d), concat(kleene(any_of(Set::all() - d)),
                                                       concat(any_of(d), kleene(any_of(Set::all() - d))))))),
                Local::Str, 1);

        expect(!synthesize(extract(builder.dfa())).has_value(), "an unflavored image at byte zero was not refused");
    }

    {
        // The assignments budget's clean kill: three width-six blocks whose selector bytes shift
        // members source-independently, so a consistent labeling exists and a deleted or miscounted
        // budget synthesizes instead of refusing; the tip refuses on the budget with a tiny closure.
        Table shifts;

        shifts.states = 19;

        shifts.init = 0;

        shifts.next.assign(19, {});

        for (auto& row : shifts.next)
        {
            row.fill(dead);
        }

        shifts.accept.assign(19, 0);

        const auto slot{[](const int block, const int member) { return 1 + block * 6 + member; }};

        for (int block{0}; block < 3; ++block)
        {
            for (int member{0}; member < 6; ++member)
            {
                shifts.accept[static_cast<std::size_t>(slot(block, member))] = 1;
            }
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto target{byte < 18 ? byte / 6 : 0};

            const auto shift{byte < 18 ? byte % 6 : 0};

            shifts.next[0][static_cast<std::size_t>(byte)] = slot(target, shift);

            for (int block{0}; block < 3; ++block)
            {
                for (int member{0}; member < 6; ++member)
                {
                    shifts.next[static_cast<std::size_t>(slot(block, member))][static_cast<std::size_t>(byte)] =
                            slot(target, (member + shift) % 6);
                }
            }
        }

        expect(!synthesize(shifts).has_value(), "the assignments budget did not refuse the shifting blocks");
    }

    {
        // The assignments-budget witness, graduated from banked to pinned by the read that specified it:
        // one non-accepting initial state and six accepting blocks of width six, where byte (t, r, k)
        // sends block i member m to block t member (m + k i) mod 6, encoded as death plus restart when
        // the image member equals r. The closure holds 217 elements, far under its budget, while the
        // labeling search would need six-factorial to the fifth power assignments; the budget must
        // refuse before materializing any of it, and promptly.
        Table witness;

        witness.states = 37;

        witness.init = 0;

        witness.next.assign(37, {});

        for (auto& row : witness.next)
        {
            row.fill(dead);
        }

        witness.accept.assign(37, 0);

        const auto place{[](const int block, const int member) { return 1 + block * 6 + member; }};

        for (int block{0}; block < 6; ++block)
        {
            for (int member{0}; member < 6; ++member)
            {
                witness.accept[static_cast<std::size_t>(place(block, member))] = 1;
            }
        }

        for (int byte{0}; byte < 216; ++byte)
        {
            const auto target{byte / 36};

            const auto refused{(byte / 6) % 6};

            const auto twist{byte % 6};

            witness.next[0][static_cast<std::size_t>(byte)] = place(target, refused);

            for (int block{0}; block < 6; ++block)
            {
                for (int member{0}; member < 6; ++member)
                {
                    const auto image{(member + twist * block) % 6};

                    witness.next[static_cast<std::size_t>(place(block, member))][static_cast<std::size_t>(byte)] =
                            image == refused ? dead : place(target, image);
                }
            }
        }

        for (int byte{216}; byte < 256; ++byte)
        {
            for (int block{0}; block < 6; ++block)
            {
                for (int member{0}; member < 6; ++member)
                {
                    witness.next[static_cast<std::size_t>(place(block, member))][static_cast<std::size_t>(byte)] =
                            place(block, member);
                }
            }
        }

        expect(!synthesize(witness).has_value(), "the assignments budget did not refuse the six-block witness");
    }

    {
        // The window walk's own nullable clause, pinned directly: over the nullable pair (an accepting
        // initial state beside the single token b), the unguarded cloud would certify the window b at
        // origin zero through the rename rule, exactly the reasoning the nullable exclusion forbids.
        Table pair;

        pair.states = 2;

        pair.init = 0;

        pair.next.assign(2, {});

        for (auto& row : pair.next)
        {
            row.fill(dead);
        }

        pair.next[0][static_cast<std::size_t>('b')] = 1;

        pair.accept = {1, 1};

        Carry idle{};

        idle.state_flavor.assign(2, -1);

        expect(!window_walk(pair, idle, init_reentrant(pair), "b", -1).has_value(),
               "the walk certified a window over a nullable pair");
    }

    {
        // The carry-group ingestion at the last byte: with the delimiter at the top of the byte order,
        // the swap generator lives at byte 255 alone, so ingestion truncated by one byte reports a
        // trivial group and fails the pinned order.
        Builder_dbg builder;

        const auto nonff{any_of(Set::all() - Set{'\xff'})};

        builder.add_token(concat(text("\xff"), concat(kleene(nonff), text("\xff"))), Local::Str, 1);

        builder.add_token(plus(nonff), Local::Chunk, 2);

        const auto carry{synthesize(extract(builder.dfa()))};

        expect(carry.has_value() && carry->group.size() == 2, "the topmost-delimiter gadget lost its swap generator");
    }

    {
        // The subset budget: twenty independently killable states realize every subset of themselves
        // beside the initial state and the absorbing accept, so the walk's node count explodes long
        // before any dimensional check could see it; the budget must refuse promptly where its removal
        // would wander a million-node graph.
        Table powerset;

        powerset.states = 22;

        powerset.init = 0;

        powerset.next.assign(22, {});

        for (auto& row : powerset.next)
        {
            row.fill(dead);
        }

        powerset.accept.assign(22, 0);

        powerset.accept[21] = 1;

        for (int byte{0}; byte < 256; ++byte)
        {
            for (int state{0}; state < 21; ++state)
            {
                powerset.next[static_cast<std::size_t>(state)][static_cast<std::size_t>(byte)] = state;
            }

            powerset.next[21][static_cast<std::size_t>(byte)] = 21;
        }

        for (int at{0}; at < 20; ++at)
        {
            powerset.next[1 + at][static_cast<std::size_t>(at)] = dead;

            powerset.next[0][static_cast<std::size_t>(64 + at)] = 1 + at;
        }

        for (int state{0}; state < 21; ++state)
        {
            powerset.next[static_cast<std::size_t>(state)][200] = 21;
        }

        expect(!synthesize(powerset).has_value(), "the subset budget did not refuse the powerset family");

        expect(decide(powerset).bounded, "the decider's subset budget did not report the powerset as bounded");
    }

    {
        // Both caps sit exactly at their thresholds from the admitted side: five rotating bare phases
        // beside a string interior make six kernel subsets of width two, and a six-member rotation makes
        // one subset of width six; both must synthesize, so a cap tightened to refuse its own boundary
        // fails these rows.
        Table phases;

        phases.states = 8;

        phases.init = 0;

        phases.next.assign(8, {});

        for (auto& row : phases.next)
        {
            row.fill(dead);
        }

        constexpr std::size_t first_bare{1};

        constexpr std::size_t interior{6};

        constexpr std::size_t closed{7};

        for (int byte{0}; byte < 256; ++byte)
        {
            phases.next[0][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(interior) : static_cast<int>(first_bare);

            for (std::size_t at{0}; at < 5; ++at)
            {
                phases.next[first_bare + at][static_cast<std::size_t>(byte)] =
                        byte == '"' ? dead :
                        byte == 'r' ? static_cast<int>(first_bare + (at + 1) % 5) :
                                      static_cast<int>(first_bare + at);
            }

            phases.next[interior][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(closed) : static_cast<int>(interior);

            phases.next[closed][static_cast<std::size_t>(byte)] = dead;
        }

        phases.accept.assign(8, 0);

        for (std::size_t at{0}; at < 5; ++at)
        {
            phases.accept[first_bare + at] = 1;
        }

        phases.accept[closed] = 1;

        const auto six_nodes{synthesize(phases)};

        expect(six_nodes.has_value() && six_nodes->kernel_subsets == 6 && six_nodes->group.size() == 2,
               "the six-subset boundary instance did not synthesize its parity carry");

        Table sixwide;

        sixwide.states = 7;

        sixwide.init = 0;

        sixwide.next.assign(7, {});

        for (auto& row : sixwide.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            sixwide.next[0][static_cast<std::size_t>(byte)] = byte == 'g' ? 2 : 1;

            for (std::size_t at{0}; at < 6; ++at)
            {
                sixwide.next[1 + at][static_cast<std::size_t>(byte)] =
                        byte == 'g' ? static_cast<int>(1 + (at + 1) % 6) : static_cast<int>(1 + at);
            }
        }

        sixwide.accept.assign(7, 1);

        sixwide.accept[0] = 0;

        const auto six_wide{synthesize(sixwide)};

        expect(six_wide.has_value() && six_wide->width == 6 && six_wide->group.size() == 6,
               "the width-six boundary instance did not synthesize its rotation carry");
    }

    {
        // The seed consistency check in both inequality directions: this table's derivation meets the
        // larger flavor before the smaller one, the reverse of the poisoned-image fixture, so a check
        // weakened to one direction synthesizes here and fails.
        Table reversed;

        reversed.states = 4;

        reversed.init = 0;

        reversed.next.assign(4, {});

        for (auto& row : reversed.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            reversed.next[0][static_cast<std::size_t>(byte)] = byte == 0xff || byte == '"' ? 1 : 2;

            reversed.next[1][static_cast<std::size_t>(byte)] = byte == '"' ? 3 : 1;

            reversed.next[2][static_cast<std::size_t>(byte)] = byte == '"' ? dead : 2;

            reversed.next[3][static_cast<std::size_t>(byte)] = byte == 0xff ? 3 : dead;
        }

        reversed.accept = {0, 0, 1, 1};

        expect(!synthesize(reversed).has_value(), "a descending byte-dependent seed was not refused");
    }

    {
        // The caps from one past their thresholds: eight kernel subsets of width two, and one subset of
        // width eight, each refused at the tip; a cap warped to refuse only its exact boundary value
        // admits these and synthesizes, failing the rows.
        Table eight_phases;

        eight_phases.states = 10;

        eight_phases.init = 0;

        eight_phases.next.assign(10, {});

        for (auto& row : eight_phases.next)
        {
            row.fill(dead);
        }

        constexpr std::size_t first_bare{1};

        constexpr std::size_t interior{8};

        constexpr std::size_t closed{9};

        for (int byte{0}; byte < 256; ++byte)
        {
            eight_phases.next[0][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(interior) : static_cast<int>(first_bare);

            for (std::size_t at{0}; at < 7; ++at)
            {
                eight_phases.next[first_bare + at][static_cast<std::size_t>(byte)] =
                        byte == '"' ? dead :
                        byte == 'r' ? static_cast<int>(first_bare + (at + 1) % 7) :
                                      static_cast<int>(first_bare + at);
            }

            eight_phases.next[interior][static_cast<std::size_t>(byte)] =
                    byte == '"' ? static_cast<int>(closed) : static_cast<int>(interior);

            eight_phases.next[closed][static_cast<std::size_t>(byte)] = dead;
        }

        eight_phases.accept.assign(10, 0);

        for (std::size_t at{0}; at < 7; ++at)
        {
            eight_phases.accept[first_bare + at] = 1;
        }

        eight_phases.accept[closed] = 1;

        expect(!synthesize(eight_phases).has_value(), "eight subsets of width two were not refused");

        Table eightwide;

        eightwide.states = 9;

        eightwide.init = 0;

        eightwide.next.assign(9, {});

        for (auto& row : eightwide.next)
        {
            row.fill(dead);
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            eightwide.next[0][static_cast<std::size_t>(byte)] = byte == 'g' ? 2 : 1;

            for (std::size_t at{0}; at < 8; ++at)
            {
                eightwide.next[1 + at][static_cast<std::size_t>(byte)] =
                        byte == 'g' ? static_cast<int>(1 + (at + 1) % 8) : static_cast<int>(1 + at);
            }
        }

        eightwide.accept.assign(9, 1);

        eightwide.accept[0] = 0;

        expect(!synthesize(eightwide).has_value(), "one subset of width eight was not refused");
    }

    {
        // The factorial's own lower bound: five width-four blocks put the true assignment count at
        // twenty-four to the fourth, refused, while a factorial started one term late counts twelve to
        // the fourth and admits the search, which then synthesizes the consistent shift labeling and
        // fails this row.
        Table quads;

        quads.states = 21;

        quads.init = 0;

        quads.next.assign(21, {});

        for (auto& row : quads.next)
        {
            row.fill(dead);
        }

        quads.accept.assign(21, 0);

        const auto cell{[](const int block, const int member) { return 1 + block * 4 + member; }};

        for (int block{0}; block < 5; ++block)
        {
            for (int member{0}; member < 4; ++member)
            {
                quads.accept[static_cast<std::size_t>(cell(block, member))] = 1;
            }
        }

        for (int byte{0}; byte < 256; ++byte)
        {
            const auto target{byte < 20 ? byte / 4 : 0};

            const auto shift{byte < 20 ? byte % 4 : 0};

            quads.next[0][static_cast<std::size_t>(byte)] = cell(target, shift);

            for (int block{0}; block < 5; ++block)
            {
                for (int member{0}; member < 4; ++member)
                {
                    quads.next[static_cast<std::size_t>(cell(block, member))][static_cast<std::size_t>(byte)] =
                            cell(target, (member + shift) % 4);
                }
            }
        }

        expect(!synthesize(quads).has_value(), "the factorial undercount admitted the five quads");
    }

    {
        // The adversarial chain: one byte erodes the cloud by a single member per step, so the subset
        // graph is a five-hundred-node chain whose floors a naive relaxation would propagate one node
        // per pass at quadratic cost; the ascending-width propagation is single-pass by construction,
        // so this row's cost is the subset walk itself, and the refusal is the floor of one.
        constexpr std::size_t links{512};

        Table chain;

        chain.states = links;

        chain.init = 0;

        chain.next.assign(links, {});

        for (auto& row : chain.next)
        {
            row.fill(dead);
        }

        chain.accept.assign(links, 1);

        chain.accept[0] = 0;

        for (int byte{0}; byte < 256; ++byte)
        {
            for (std::size_t state{0}; state < links; ++state)
            {
                chain.next[state][static_cast<std::size_t>(byte)] = static_cast<int>(state);
            }
        }

        chain.next[0][0] = static_cast<int>(links - 1);

        chain.next[1][0] = 1;

        for (std::size_t state{2}; state < links; ++state)
        {
            chain.next[state][0] = static_cast<int>(state - 1);
        }

        expect(!synthesize(chain).has_value(), "the eroding chain was not refused on its floor");
    }

    {
        // The decider's own threshold, distinguished from the synthesizer's: eight killable states give
        // at least two hundred fifty-six subsets, past the decider's bound and far under the
        // synthesizer's, so a decider bound restored to the larger constant completes unbounded and
        // fails this row.
        Table small_powerset;

        small_powerset.states = 10;

        small_powerset.init = 0;

        small_powerset.next.assign(10, {});

        for (auto& row : small_powerset.next)
        {
            row.fill(dead);
        }

        small_powerset.accept.assign(10, 0);

        small_powerset.accept[9] = 1;

        for (int byte{0}; byte < 256; ++byte)
        {
            for (int state{0}; state < 9; ++state)
            {
                small_powerset.next[static_cast<std::size_t>(state)][static_cast<std::size_t>(byte)] = state;
            }

            small_powerset.next[9][static_cast<std::size_t>(byte)] = 9;
        }

        for (int at{0}; at < 8; ++at)
        {
            small_powerset.next[1 + at][static_cast<std::size_t>(at)] = dead;

            small_powerset.next[0][static_cast<std::size_t>(64 + at)] = 1 + at;
        }

        for (int state{0}; state < 9; ++state)
        {
            small_powerset.next[static_cast<std::size_t>(state)][200] = 9;
        }

        expect(decide(small_powerset).bounded, "the decider's own threshold did not bound the small powerset");
    }

    std::cout << (failures == 0 ? "all assertions hold\n" : "assertion failures\n");

    return failures == 0 ? 0 : 1;
}

#ifndef MUNCH_PAPER_FIGURES_MODULO_HPP
#define MUNCH_PAPER_FIGURES_MODULO_HPP

/*
 * The condition modulo discarded tokens as the report published it, and the exact decision the report proves, both
 * evaluated on the compiled tables through the simulator's view. Shared by the figure programs so that the sweep and
 * the applicability table judge the published condition by one copy of it.
 *
 * The published condition is a copy because the library no longer ships it: the rule it ships is the stronger one the
 * report describes as the current one, read through Lexer::is_split_point_ignoring(). The exact decision is the
 * report's construction as written: the complete-scan automaton over obligation sets, the failure automaton beside it,
 * the two chunk-completeness products, the kept-output product under the z^l K_t coding, and the finite
 * output-equality test with group delays. A negative answer carries an input and a cut, which decide_exactly() replays
 * through the real scanner before reporting it. Exhausting the budget is reported as such and never as an answer.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "munch/core/lexer.hpp"

namespace figures
{
/**
 * @brief The relaxed condition as the report published it, evaluated on the compiled tables.
 *
 * A state consuming the symbol live must be the non-re-entrant initial state, or accept a discarded token, reach no
 * kept token, and advance on the symbol to the very state the initial state advances to. Vacuous certificates are
 * withheld as the shipped predicates withhold them.
 */
inline bool published_condition(
        const munch::core::Lexer& lexer, const std::set<std::size_t>& ignored, const unsigned char symbol)
{
    const auto& simulator{lexer.simulator()};

    const auto states{simulator.state_count()};

    const auto discarded{[&](const std::size_t state) {
        const auto token{simulator.accepted(state)};

        return token.has_value() && ignored.contains(token->id());
    }};

    std::vector<bool> reaches_kept(states, false);

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (std::size_t state{0}; state < states; ++state)
        {
            if (reaches_kept[state])
            {
                continue;
            }

            auto reaches{simulator.is_accepting(state) && !discarded(state)};

            for (std::size_t byte{0}; byte < 256 && !reaches; ++byte)
            {
                const auto to{simulator.step(state, static_cast<unsigned char>(byte))};

                reaches = to.has_value() && reaches_kept[*to];
            }

            if (reaches)
            {
                reaches_kept[state] = true;

                changed = true;
            }
        }
    }

    const auto init{simulator.init_state()};

    const auto consumes{[&](const std::size_t state) {
        const auto to{simulator.step(state, symbol)};

        return to.has_value() && simulator.is_live(*to);
    }};

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!simulator.is_live(state) || !consumes(state) || (state == init && !simulator.init_reentrant()))
        {
            continue;
        }

        if (!discarded(state) || reaches_kept[state] || simulator.step(state, symbol) != simulator.step(init, symbol))
        {
            return false;
        }
    }

    return consumes(init);
}

/**
 * @brief What the exact decision answered for one symbol.
 */
struct Exact_verdict
{
    enum class Outcome : std::uint8_t
    {
        safe,
        unsafe,
        budget
    };

    Outcome outcome{Outcome::budget};

    // Which of the three tests failed, when unsafe.
    std::string reason;

    // The replayed witness, when unsafe: a completely tokenizable input and the offset of the cut.
    std::string text;
    std::size_t cut{};

    // The largest product the decision built, in vertices.
    std::size_t vertices{};
};

/**
 * @brief The three finite automata of the exact decision, stepped on one compiled token set.
 *
 * Bytes the tables do not tell apart step every machine identically, so one representative per class is stepped and
 * the witness is spelled in representatives; the candidate symbol represents its own class.
 */
class Scan_machines
{
public:
    static constexpr std::size_t max_states{512};

    struct Bits
    {
        std::array<std::uint64_t, max_states / 64> words{};

        auto operator<=>(const Bits&) const = default;

        [[nodiscard]] bool test(const std::size_t index) const { return (words[index / 64] >> (index % 64)) & 1U; }

        void set(const std::size_t index) { words[index / 64] |= std::uint64_t{1} << (index % 64); }
    };

    // A token in progress from `current`, or between tokens when `current` is fresh; the obligations are the tokens
    // already committed whose match could still be extended, each a state to be carried until it dies.
    struct Success
    {
        Bits obligations;
        int current{fresh};

        auto operator<=>(const Success&) const = default;
    };

    // The same, and once failing the state of the attempt that must never accept, dead once it has no transition.
    struct Failure
    {
        Bits obligations;
        int current{fresh};
        bool failing{false};

        auto operator<=>(const Failure&) const = default;
    };

    // The same, with the guess whether the token in progress is kept, made at its first byte and checked at its end.
    struct Output
    {
        Bits obligations;
        int current{fresh};
        int keep{no_guess};

        auto operator<=>(const Output&) const = default;
    };

    static constexpr int fresh{-1};
    static constexpr int dead{-2};
    static constexpr int no_guess{-1};

    // Output letters: z, then one letter per kept kind.
    static constexpr int z{1};

    using Word = std::vector<int>;

    Scan_machines(const munch::dfa::Simulator& simulator, const std::set<std::size_t>& ignored, const unsigned char b)
        : simulator_{simulator}, init_{static_cast<int>(simulator.init_state())}
    {
        const auto states{simulator.state_count()};

        accepting_.resize(states);
        live_.resize(states);
        discarded_.resize(states);
        kind_.resize(states);

        for (std::size_t state{0}; state < states; ++state)
        {
            accepting_[state] = simulator.is_accepting(state);
            live_[state] = simulator.is_live(state);

            const auto token{simulator.accepted(state)};

            kind_[state] = token.has_value() ? static_cast<int>(token->id()) : -1;
            discarded_[state] = token.has_value() && ignored.contains(token->id());
        }

        // One representative per class of bytes with identical columns, over the bytes some state consumes at all;
        // the candidate stands for its own class so that a witness spells the cut with it.
        std::map<std::vector<int>, unsigned char> classes;

        for (int byte{0}; byte < 256; ++byte)
        {
            std::vector<int> column;

            auto consumed{false};

            for (std::size_t state{0}; state < states; ++state)
            {
                const auto to{simulator.step(state, static_cast<unsigned char>(byte))};

                column.push_back(to.has_value() ? static_cast<int>(*to) : -1);

                consumed = consumed || to.has_value();
            }

            if (!consumed)
            {
                continue;
            }

            const auto symbol{static_cast<unsigned char>(byte)};

            auto [slot, inserted]{classes.try_emplace(column, symbol)};

            if (symbol == b)
            {
                slot->second = b;
            }
        }

        for (const auto& [column, symbol] : classes)
        {
            alphabet_.push_back(symbol);
        }
    }

    [[nodiscard]] bool fits() const { return simulator_.state_count() <= max_states; }

    [[nodiscard]] const std::vector<unsigned char>& alphabet() const { return alphabet_; }

    [[nodiscard]] std::vector<Success> success_step(const Success& from, const unsigned char byte) const
    {
        std::vector<Success> result;

        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return result;
        }

        const auto to{token_step(from.current, byte)};

        if (to < 0)
        {
            return result;
        }

        result.push_back({*obligations, to});

        if (accepting_[static_cast<std::size_t>(to)])
        {
            auto committed{*obligations};

            committed.set(static_cast<std::size_t>(to));

            result.push_back({committed, fresh});
        }

        return result;
    }

    [[nodiscard]] std::vector<Failure> failure_step(const Failure& from, const unsigned char byte) const
    {
        std::vector<Failure> result;

        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return result;
        }

        if (!from.failing)
        {
            for (const auto& next : success_step({from.obligations, from.current}, byte))
            {
                result.push_back({next.obligations, next.current, false});
            }

            if (from.current == fresh)
            {
                push_failing(result, *obligations, init_, byte);
            }

            return result;
        }

        if (from.current == dead)
        {
            result.push_back({*obligations, dead, true});

            return result;
        }

        push_failing(result, *obligations, from.current, byte);

        return result;
    }

    [[nodiscard]] std::vector<std::pair<Output, Word>> output_step(const Output& from, const unsigned char byte) const
    {
        std::vector<std::pair<Output, Word>> result;

        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return result;
        }

        const auto to{token_step(from.current, byte)};

        if (to < 0)
        {
            return result;
        }

        const auto state{static_cast<std::size_t>(to)};

        for (const auto keep : from.current == fresh ? std::vector<int>{0, 1} : std::vector<int>{from.keep})
        {
            const Word emitted{keep == 1 ? Word{z} : Word{}};

            result.emplace_back(Output{*obligations, to, keep}, emitted);

            if (!accepting_[state] || (discarded_[state] ? 0 : 1) != keep)
            {
                continue;
            }

            auto committed{*obligations};

            committed.set(state);

            auto output{emitted};

            if (keep == 1)
            {
                output.push_back(kind_[state] + 2);
            }

            result.emplace_back(Output{committed, fresh, no_guess}, output);
        }

        return result;
    }

private:
    // Every obligation moves on; one that accepts refutes the commitment it stands for, one with no transition or no
    // acceptance ahead is dropped.
    [[nodiscard]] std::optional<Bits> advance(const Bits& obligations, const unsigned char byte) const
    {
        Bits moved;

        for (std::size_t word{0}; word < obligations.words.size(); ++word)
        {
            for (auto bits{obligations.words[word]}; bits != 0; bits &= bits - 1)
            {
                const auto state{word * 64 + static_cast<std::size_t>(std::countr_zero(bits))};

                const auto to{simulator_.step(state, byte)};

                if (!to)
                {
                    continue;
                }

                if (accepting_[*to])
                {
                    return std::nullopt;
                }

                if (live_[*to])
                {
                    moved.set(*to);
                }
            }
        }

        return moved;
    }

    // The token in progress moves on, from the initial state when fresh; a state that can no longer accept ends it.
    [[nodiscard]] int token_step(const int current, const unsigned char byte) const
    {
        const auto from{current == fresh ? init_ : current};

        const auto to{simulator_.step(static_cast<std::size_t>(from), byte)};

        return to && live_[*to] ? static_cast<int>(*to) : -1;
    }

    // The failing attempt moves on and must not accept; with no transition, or none that could ever accept, it is
    // dead and stays so.
    void push_failing(
            std::vector<Failure>& result, const Bits& obligations, const int current, const unsigned char byte) const
    {
        const auto to{simulator_.step(static_cast<std::size_t>(current), byte)};

        if (!to || !live_[*to])
        {
            result.push_back({obligations, dead, true});

            return;
        }

        if (!accepting_[*to])
        {
            result.push_back({obligations, static_cast<int>(*to), true});
        }
    }

    const munch::dfa::Simulator& simulator_;
    int init_;
    std::vector<bool> accepting_, live_, discarded_;
    std::vector<int> kind_;
    std::vector<unsigned char> alphabet_;
};

namespace detail
{
constexpr int marker{-1};

// The phases of a marked input u # v: nothing read, inside a nonempty u, just past the marker so the next byte must be
// the candidate, and inside v past that byte.
constexpr int next_phase(const int phase)
{
    return phase < 2 ? 1 : 3;
}

struct Witness
{
    std::string text;
    std::size_t cut{};
};

// Spells the labels of a path as the input and the cut the marker stood at.
inline Witness spell(const std::vector<int>& labels)
{
    Witness witness;

    for (const auto label : labels)
    {
        if (label == marker)
        {
            witness.cut = witness.text.size();

            continue;
        }

        witness.text.push_back(static_cast<char>(label));
    }

    return witness;
}

// The whole scan on uv beside the failing scan of one chunk: left, on u up to the marker, or right, on v from it.
// An accepting path is a completely tokenizable input one of whose chunks the scanner does not consume.
struct Domain_vertex
{
    Scan_machines::Success whole;
    Scan_machines::Failure fragment;
    bool fragment_active{false};
    int phase{0};

    auto operator<=>(const Domain_vertex&) const = default;
};

inline std::optional<Witness> incomplete_chunk(
        const Scan_machines& machines, const unsigned char b, const bool left, const std::size_t budget,
        std::size_t& largest)
{
    std::vector<Domain_vertex> vertices{{{}, {}, left, 0}};
    std::vector<std::pair<std::size_t, int>> parents{{0, 0}};
    std::map<Domain_vertex, std::size_t> ids{{vertices[0], 0}};

    const auto push{[&](const Domain_vertex& vertex, const std::size_t from, const int label) {
        if (ids.try_emplace(vertex, vertices.size()).second)
        {
            vertices.push_back(vertex);
            parents.emplace_back(from, label);
        }
    }};

    for (std::size_t at{0}; at < vertices.size(); ++at)
    {
        largest = std::max(largest, vertices.size());

        if (vertices.size() > budget)
        {
            throw std::length_error{"budget"};
        }

        const auto vertex{vertices[at]};

        const auto fragment_final{vertex.fragment.failing};

        if (vertex.phase == 3 && vertex.whole.current == Scan_machines::fresh && (left || fragment_final))
        {
            std::vector<int> labels;

            for (auto id{at}; id != 0; id = parents[id].first)
            {
                labels.push_back(parents[id].second);
            }

            std::ranges::reverse(labels);

            return spell(labels);
        }

        if (vertex.phase == 1 && (!left || fragment_final))
        {
            push({vertex.whole, {}, !left, 2}, at, marker);
        }

        for (const auto byte : machines.alphabet())
        {
            if (vertex.phase == 2 && byte != b)
            {
                continue;
            }

            std::vector<Scan_machines::Failure> fragments{vertex.fragment};

            if (vertex.fragment_active)
            {
                fragments = machines.failure_step(vertex.fragment, byte);
            }

            for (const auto& whole : machines.success_step(vertex.whole, byte))
            {
                for (const auto& fragment : fragments)
                {
                    push({whole, fragment, vertex.fragment_active, next_phase(vertex.phase)}, at, byte);
                }
            }
        }
    }

    return std::nullopt;
}

// The kept-output product: the whole scan beside the scan restarted at the marker, each emitting z per byte of a kept
// token and the kind's letter at its end. An accepting path is a completely tokenizable input both of whose chunks
// are consumed, carrying the two kept streams as words.
struct Output_vertex
{
    Scan_machines::Output whole;
    Scan_machines::Output split;
    int phase{0};

    auto operator<=>(const Output_vertex&) const = default;
};

struct Output_edge
{
    std::size_t source{}, target{};
    int label{};
    Scan_machines::Word h, g;
};

struct Output_graph
{
    std::vector<Output_edge> edges;
    std::vector<std::vector<std::size_t>> out, in;
    std::vector<bool> accepting;
};

inline Output_graph output_product(
        const Scan_machines& machines, const unsigned char b, const std::size_t budget, std::size_t& largest)
{
    std::vector<Output_vertex> vertices{{}};
    std::map<Output_vertex, std::size_t> ids{{vertices[0], 0}};

    Output_graph graph;

    graph.out.emplace_back();
    graph.in.emplace_back();
    graph.accepting.push_back(false);

    const auto link{[&](const std::size_t from, const Output_vertex& vertex, const int label,
                        const Scan_machines::Word& h, const Scan_machines::Word& g) {
        auto [slot, inserted]{ids.try_emplace(vertex, vertices.size())};

        if (inserted)
        {
            vertices.push_back(vertex);
            graph.out.emplace_back();
            graph.in.emplace_back();
            graph.accepting.push_back(false);
        }

        graph.out[from].push_back(graph.edges.size());
        graph.in[slot->second].push_back(graph.edges.size());
        graph.edges.push_back({from, slot->second, label, h, g});
    }};

    for (std::size_t at{0}; at < vertices.size(); ++at)
    {
        largest = std::max(largest, vertices.size());

        if (vertices.size() > budget)
        {
            throw std::length_error{"budget"};
        }

        const auto vertex{vertices[at]};

        graph.accepting[at] = vertex.phase == 3 && vertex.whole.current == Scan_machines::fresh &&
                              vertex.split.current == Scan_machines::fresh;

        if (vertex.phase == 1 && vertex.split.current == Scan_machines::fresh)
        {
            link(at, {vertex.whole, {}, 2}, marker, {}, {});
        }

        for (const auto byte : machines.alphabet())
        {
            if (vertex.phase == 2 && byte != b)
            {
                continue;
            }

            for (const auto& [whole, h] : machines.output_step(vertex.whole, byte))
            {
                for (const auto& [split, g] : machines.output_step(vertex.split, byte))
                {
                    link(at, {whole, split, next_phase(vertex.phase)}, byte, h, g);
                }
            }
        }
    }

    return graph;
}

// The delay of two outputs A, B as the reduced group word A^{-1} B, and what one more edge does to it.
inline Scan_machines::Word delayed(const Scan_machines::Word& delay, const Output_edge& edge)
{
    Scan_machines::Word result;

    const auto push{[&result](const int letter) {
        if (!result.empty() && result.back() == -letter)
        {
            result.pop_back();
        }
        else
        {
            result.push_back(letter);
        }
    }};

    for (auto letter{edge.h.rbegin()}; letter != edge.h.rend(); ++letter)
    {
        push(-*letter);
    }

    for (const auto letter : delay)
    {
        push(letter);
    }

    for (const auto letter : edge.g)
    {
        push(letter);
    }

    return result;
}

// Whether every accepting path of the graph carries equal outputs, by the delays of shortest stems; otherwise an
// accepting path that does not, built from the stem and a shortest accepting suffix at the failing edge or vertex.
inline std::optional<std::vector<std::size_t>> unequal_path(const Output_graph& graph)
{
    const auto count{graph.out.size()};

    constexpr auto none{static_cast<std::size_t>(-1)};

    // Shortest accepting suffixes, found backwards from the final vertices; a vertex without one is not live.
    std::vector<std::size_t> onward(count, none);
    std::vector<bool> live(count, false);

    std::deque<std::size_t> queue;

    for (std::size_t vertex{0}; vertex < count; ++vertex)
    {
        if (graph.accepting[vertex])
        {
            live[vertex] = true;
            queue.push_back(vertex);
        }
    }

    while (!queue.empty())
    {
        const auto vertex{queue.front()};

        queue.pop_front();

        for (const auto edge : graph.in[vertex])
        {
            const auto source{graph.edges[edge].source};

            if (!live[source])
            {
                live[source] = true;
                onward[source] = edge;
                queue.push_back(source);
            }
        }
    }

    if (!live[0])
    {
        return std::nullopt; // no accepting path at all, so nothing to compare
    }

    // Shortest stems from the initial vertex over live vertices, each carrying its delay.
    std::vector<std::size_t> stem(count, none);
    std::vector<bool> reached(count, false);
    std::vector<Scan_machines::Word> delay(count);

    reached[0] = true;
    queue.push_back(0);

    while (!queue.empty())
    {
        const auto vertex{queue.front()};

        queue.pop_front();

        for (const auto edge : graph.out[vertex])
        {
            const auto target{graph.edges[edge].target};

            if (live[target] && !reached[target])
            {
                reached[target] = true;
                stem[target] = edge;
                delay[target] = delayed(delay[vertex], graph.edges[edge]);
                queue.push_back(target);
            }
        }
    }

    const auto path_to{[&](const std::size_t vertex) {
        std::vector<std::size_t> path;

        for (auto at{vertex}; stem[at] != none; at = graph.edges[stem[at]].source)
        {
            path.push_back(stem[at]);
        }

        std::ranges::reverse(path);

        return path;
    }};

    const auto path_from{[&](std::vector<std::size_t> path, std::size_t vertex) {
        for (; onward[vertex] != none; vertex = graph.edges[onward[vertex]].target)
        {
            path.push_back(onward[vertex]);
        }

        return path;
    }};

    const auto unequal{[&](const std::vector<std::size_t>& path) {
        Scan_machines::Word h, g;

        for (const auto edge : path)
        {
            h.insert(h.end(), graph.edges[edge].h.begin(), graph.edges[edge].h.end());
            g.insert(g.end(), graph.edges[edge].g.begin(), graph.edges[edge].g.end());
        }

        return h != g;
    }};

    for (std::size_t vertex{0}; vertex < count; ++vertex)
    {
        if (!reached[vertex])
        {
            continue;
        }

        if (graph.accepting[vertex] && !delay[vertex].empty())
        {
            return path_to(vertex);
        }

        for (const auto edge : graph.out[vertex])
        {
            const auto target{graph.edges[edge].target};

            if (!live[target] || delayed(delay[vertex], graph.edges[edge]) == delay[target])
            {
                continue;
            }

            // The two stems disagree on the delay at the target, so with one common suffix they cannot both carry
            // equal outputs.
            auto around{path_to(vertex)};

            around.push_back(edge);

            const auto first{path_from(path_to(target), target)};

            const auto second{path_from(around, target)};

            return unequal(first) ? first : second;
        }
    }

    return std::nullopt;
}
} // namespace detail

/**
 * @brief Replays a witness through the real scanner and names what it shows, or nothing when it shows nothing.
 */
inline std::optional<std::string> replayed(
        const munch::core::Lexer& lexer, const std::set<std::size_t>& ignored, const std::string& text,
        const std::size_t cut)
{
    using Stream = std::vector<std::pair<std::size_t, std::size_t>>;

    const auto scan{[&lexer, &ignored](const std::string& piece, Stream& kept) {
        return lexer.tokenize_all<std::size_t>(piece, [&](const std::size_t kind, const std::size_t length) {
            if (!ignored.contains(kind))
            {
                kept.emplace_back(kind, length);
            }
        });
    }};

    Stream whole, left, right;

    if (cut == 0 || cut >= text.size() || scan(text, whole) != text.size())
    {
        return std::nullopt;
    }

    if (scan(text.substr(0, cut), left) != cut)
    {
        return "left chunk incomplete";
    }

    if (scan(text.substr(cut), right) != text.size() - cut)
    {
        return "right chunk incomplete";
    }

    left.insert(left.end(), right.begin(), right.end());

    return left != whole ? std::optional<std::string>{"kept streams differ"} : std::nullopt;
}

/**
 * @brief Decides exactly whether every cut before the symbol, in every completely tokenizable input, leaves two
 *        completely tokenizable chunks whose kept streams concatenate to the whole input's.
 *
 * Unsafe answers are replayed through the real scanner and reported with the input and cut that show them; an
 * unsafe answer whose witness does not replay is reported as a budget outcome with a reason, never as an answer.
 */
inline Exact_verdict decide_exactly(
        const munch::core::Lexer& lexer, const std::set<std::size_t>& ignored, const unsigned char symbol,
        const std::size_t budget = 1U << 22U)
{
    Exact_verdict verdict;

    const Scan_machines machines{lexer.simulator(), ignored, symbol};

    if (!machines.fits())
    {
        verdict.reason = "more states than the machines carry";

        return verdict;
    }

    const auto report{[&](const detail::Witness& witness, const std::string& expected) {
        verdict.text = witness.text;
        verdict.cut = witness.cut;

        const auto shown{replayed(lexer, ignored, witness.text, witness.cut)};

        if (shown == expected)
        {
            verdict.outcome = Exact_verdict::Outcome::unsafe;
            verdict.reason = expected;
        }
        else
        {
            verdict.reason = "a witness for " + expected + " that does not replay";
        }

        return verdict;
    }};

    try
    {
        for (const auto left : {true, false})
        {
            const auto witness{detail::incomplete_chunk(machines, symbol, left, budget, verdict.vertices)};

            if (witness)
            {
                return report(*witness, left ? "left chunk incomplete" : "right chunk incomplete");
            }
        }

        const auto graph{detail::output_product(machines, symbol, budget, verdict.vertices)};

        const auto path{detail::unequal_path(graph)};

        if (!path)
        {
            verdict.outcome = Exact_verdict::Outcome::safe;

            return verdict;
        }

        std::vector<int> labels;

        for (const auto edge : *path)
        {
            labels.push_back(graph.edges[edge].label);
        }

        return report(detail::spell(labels), "kept streams differ");
    }
    catch (const std::length_error&)
    {
        verdict.reason = "budget exhausted";

        return verdict;
    }
}

} // namespace figures

#endif // MUNCH_PAPER_FIGURES_MODULO_HPP

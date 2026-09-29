#include "munch/dfa/verifier_decisions.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "munch/dfa/verifier.hpp"

namespace munch::dfa
{
namespace
{
/**
 * @brief A verifier state, as the decisions name one.
 */
using State_t = Verifier::State_t;

/**
 * @brief The two boundary bits a byte can carry, no boundary first.
 */
constexpr std::array marks{false, true};

/**
 * @brief How a search reached a key: the number of the key the symbol was read from, zero for the start, and the
 *        symbol.
 */
struct Parent
{
    /**
     * @brief The number of the key the symbol was read from, zero for the start.
     */
    std::size_t from{};

    /**
     * @brief The symbol read.
     */
    Marked symbol{};
};

/**
 * @brief A key of the certification product: the verifier state, how much of the window the text's end matches, the
 *        boundary marks of the last bytes read, and whether an occurrence has been covered from elsewhere than the
 *        origin.
 *
 * The marks are the boundary_after bits of the last |window| bytes read, oldest first, with the implicit boundary
 * before position zero taken as the mark of a byte before the input, so the start's marks are all clear but the last.
 * Before byte j is read, marks[t] is the mark after byte j - |window| + t, which is the boundary before position
 * j - |window| + 1 + t; when byte j completes an occurrence at k = j - |window| + 1, marks[t] is thus whether a
 * boundary sits before the occurrence's byte t, and the token containing byte j begins at k + t for the last t with
 * marks[t] set, or before k when none is. No step enters the start, so no later key equals the start's key, and the
 * start's marks stand for the implicit boundary at position zero and for nothing else.
 */
struct Certification_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Certification_key&) const = default;

    /**
     * @brief The verifier state.
     */
    State_t state{};

    /**
     * @brief How much of the window the text's end matches.
     */
    std::size_t matched{};

    /**
     * @brief The boundary marks of the last |window| bytes read, oldest first.
     */
    std::vector<bool> marks{};

    /**
     * @brief Whether an occurrence has been covered from elsewhere than the origin.
     */
    bool missed{};
};

/**
 * @brief The markless steps of a verifier, per state in ascending order of the symbol, and whether each state has a
 *        marked step.
 */
struct Markless_graph
{
    /**
     * @brief Per state, its markless steps as the symbol read and the state entered, ascending.
     */
    std::vector<std::vector<std::pair<Marked, State_t>>> successors{};

    /**
     * @brief Per state, the markless steps into it as the state left and the symbol read, ascending.
     */
    std::vector<std::vector<std::pair<State_t, Marked>>> predecessors{};

    /**
     * @brief Per state, whether a marked step leaves it.
     */
    std::vector<bool> marked_step{};
};

/**
 * @brief A key of the boundary half's product: the two verifier states, and whether the two markings have differed.
 */
struct Pair_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Pair_key&) const = default;

    /**
     * @brief The first verifier's state.
     */
    State_t a{};

    /**
     * @brief The second verifier's state.
     */
    State_t b{};

    /**
     * @brief Whether the two markings have differed.
     */
    bool differed{};
};

/**
 * @brief A key of the domain half's subset construction: the states each verifier can be in after the bytes read,
 *        sorted and without repeats.
 */
struct Subset_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Subset_key&) const = default;

    /**
     * @brief The states the first verifier can be in.
     */
    std::vector<State_t> a{};

    /**
     * @brief The states the second verifier can be in.
     */
    std::vector<State_t> b{};
};

/**
 * @brief A key of the marking search over one input: the number of bytes read and the verifier state.
 */
struct Marking_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Marking_key&) const = default;

    /**
     * @brief The number of bytes read.
     */
    std::size_t read{};

    /**
     * @brief The verifier state.
     */
    State_t state{};
};

/**
 * @brief Searches breadth first from a key, returning the symbols read to the first key admitted that is a goal.
 *
 * Keys are admitted in the order they are first reached and expanded in that order, so the path is a shortest one.
 * @tparam Key The search key type.
 * @tparam Goal The goal predicate's type.
 * @tparam Expand The expansion's callable type.
 * @param start The key the search starts from.
 * @param goal Whether a key ends the search.
 * @param expand Calls its second argument with each symbol read from a key and the key it leads to.
 * @return The symbols from the start to the first goal, or std::nullopt when no admitted key is one.
 */
template <typename Key, typename Goal, typename Expand>
[[nodiscard]] std::optional<std::vector<Marked>> shortest_path(Key start, const Goal& goal, const Expand& expand)
{
    std::map<Key, Parent> seen{};

    const auto [first, started]{seen.emplace(std::move(start), Parent{})};

    std::vector<typename std::map<Key, Parent>::const_iterator> admitted{first};

    /**
     * @brief Reads the symbols back from an admitted key to the start.
     * @param last The number of the key.
     * @return The symbols from the start to it.
     */
    const auto path_to{[&admitted](const std::size_t last) {
        std::vector<Marked> path{};

        for (auto at{last}; at != 0;)
        {
            const auto& [key, parent]{*admitted[at]};

            path.push_back(parent.symbol);

            at = parent.from;
        }

        std::ranges::reverse(path);

        return path;
    }};

    for (std::size_t next{0}; next < admitted.size(); ++next)
    {
        const auto& [key, parent]{*admitted[next]};

        if (goal(key))
        {
            return path_to(next);
        }

        /**
         * @brief Admits a key reached from the one expanded, unless it was reached before.
         * @param symbol The symbol read.
         * @param child The key reached.
         */
        const auto admit{[&](const Marked symbol, Key child) {
            const auto [entry, inserted]{seen.try_emplace(std::move(child), Parent{.from = next, .symbol = symbol})};

            if (inserted)
            {
                admitted.push_back(entry);
            }
        }};

        expand(key, admit);
    }

    return std::nullopt;
}

/**
 * @brief Reads every marked symbol over an alphabet from a verifier state.
 * @tparam Emit The callback type.
 * @param verifier The verifier.
 * @param alphabet The bytes read.
 * @param state The state read from.
 * @param emit Called with each symbol that steps and the state it leads to.
 */
template <typename Emit>
void expand_steps(
        const Verifier& verifier, const std::vector<unsigned char>& alphabet, const State_t state, const Emit& emit)
{
    for (const auto byte : alphabet)
    {
        for (const auto boundary_after : marks)
        {
            const Marked symbol{.byte = byte, .boundary_after = boundary_after};

            if (const auto next{verifier.step(state, symbol)})
            {
                emit(symbol, *next);
            }
        }
    }
}

/**
 * @brief Returns the marked string of a sequence of symbols.
 * @param symbols The symbols.
 * @return The bytes and their boundary bits.
 */
[[nodiscard]] Marked_string marked_string_of(const std::vector<Marked>& symbols)
{
    Marked_string marked{};

    for (const auto [byte, boundary_after] : symbols)
    {
        marked.bytes.push_back(static_cast<char>(byte));

        marked.boundaries.push_back(boundary_after);
    }

    return marked;
}

/**
 * @brief Sorts a list and drops the repeats.
 * @tparam T The element type.
 * @param values The list, sorted and without repeats on return.
 */
template <typename T>
void dedup(std::vector<T>& values)
{
    std::ranges::sort(values);

    const auto repeats{std::ranges::unique(values)};

    values.erase(repeats.begin(), repeats.end());
}

/**
 * @brief Returns the bytes on a verifier's transitions, ascending and without repeats.
 * @param verifier The verifier.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet_of(const Verifier& verifier)
{
    std::vector<unsigned char> alphabet{};

    for (const auto& [from, symbol] : verifier.transitions() | std::views::keys)
    {
        alphabet.push_back(symbol.byte);
    }

    dedup(alphabet);

    return alphabet;
}

/**
 * @brief Returns whether a verifier's domain is empty: its start neither accepts nor steps, as a trim verifier's start
 *        does exactly when it cannot complete.
 * @param verifier The verifier.
 * @return True when the verifier accepts no marked string.
 */
[[nodiscard]] bool domain_is_empty(const Verifier& verifier)
{
    return !verifier.accepts(verifier.start()) && verifier.transitions().empty();
}

/**
 * @brief Returns the border lengths of the window's prefixes: entry m is the longest proper prefix of the first m bytes
 *        that is also their suffix.
 * @param window The window.
 * @return The border lengths, one per prefix length from zero to the window's length.
 */
[[nodiscard]] std::vector<std::size_t> borders_of(const std::string_view window)
{
    std::vector<std::size_t> borders(window.size() + 1, 0);

    for (std::size_t length{2}; length <= window.size(); ++length)
    {
        auto border{borders[length - 1]};

        while (border > 0 && window[border] != window[length - 1])
        {
            border = borders[border];
        }

        borders[length] = window[border] == window[length - 1] ? border + 1 : 0;
    }

    return borders;
}

/**
 * @brief Returns how much of the window the text's end matches after one more byte.
 * @param window The window.
 * @param borders The border lengths of the window's prefixes.
 * @param matched How much the text's end matched before the byte.
 * @param byte The byte.
 * @return The length of the longest prefix of the window that ends the text.
 */
[[nodiscard]] std::size_t matched_after(
        const std::string_view window, const std::vector<std::size_t>& borders, std::size_t matched,
        const unsigned char byte)
{
    if (matched == window.size())
    {
        matched = borders[matched];
    }

    while (matched > 0 && static_cast<unsigned char>(window[matched]) != byte)
    {
        matched = borders[matched];
    }

    return static_cast<unsigned char>(window[matched]) == byte ? matched + 1 : 0;
}

/**
 * @brief Returns the index of a byte's covering boundary: the start of the token containing it.
 * @param segmentation The marked string.
 * @param index The byte.
 * @return The index of the token's first byte.
 */
[[nodiscard]] std::size_t covering_boundary(const Marked_string& segmentation, std::size_t index)
{
    while (index > 0 && !segmentation.boundaries[index - 1])
    {
        --index;
    }

    return index;
}

/**
 * @brief Returns the first occurrence of a window in a marked string whose covering boundary is not the origin.
 * @param segmentation The marked string, which holds one.
 * @param window The window.
 * @param origin The origin.
 * @return The index of the occurrence's first byte.
 */
[[nodiscard]] std::size_t missed_occurrence(
        const Marked_string& segmentation, const std::string_view window, const std::size_t origin)
{
    const std::string_view bytes{segmentation.bytes};

    std::size_t at{bytes.find(window)};

    for (;;)
    {
        const auto last{at + window.size() - 1};

        if (covering_boundary(segmentation, last) != at + origin)
        {
            return at;
        }

        at = bytes.find(window, at + 1);
    }
}

/**
 * @brief Collects the markless steps of a verifier and the states with a marked step.
 * @param verifier The verifier.
 * @return The markless graph.
 */
[[nodiscard]] Markless_graph markless_graph_of(const Verifier& verifier)
{
    const auto count{verifier.state_count()};

    Markless_graph graph{
            .successors = std::vector<std::vector<std::pair<Marked, State_t>>>(count),
            .predecessors = std::vector<std::vector<std::pair<State_t, Marked>>>(count),
            .marked_step = std::vector<bool>(count, false),
    };

    for (const auto& [key, to] : verifier.transitions())
    {
        const auto& [from, symbol]{key};

        if (symbol.boundary_after)
        {
            graph.marked_step[from] = true;

            continue;
        }

        graph.successors[from].emplace_back(symbol, to);

        graph.predecessors[to].emplace_back(from, symbol);
    }

    for (auto& edges : graph.successors)
    {
        std::ranges::sort(edges);
    }

    for (auto& edges : graph.predecessors)
    {
        std::ranges::sort(edges);
    }

    return graph;
}

/**
 * @brief Returns the states of the markless graph in topological order, as far as one exists: a state is listed once
 *        every markless predecessor is, so the states left out are those on or after a markless cycle.
 * @param graph The markless graph.
 * @return The ordered states.
 */
[[nodiscard]] std::vector<State_t> topological_order(const Markless_graph& graph)
{
    std::vector<std::size_t> pending(graph.predecessors.size(), 0);

    std::vector<State_t> order{};

    for (State_t state{0}; state < graph.predecessors.size(); ++state)
    {
        pending[state] = graph.predecessors[state].size();

        if (pending[state] == 0)
        {
            order.push_back(state);
        }
    }

    for (std::size_t next{0}; next < order.size(); ++next)
    {
        for (const auto to : graph.successors[order[next]] | std::views::values)
        {
            --pending[to];

            if (pending[to] == 0)
            {
                order.push_back(to);
            }
        }
    }

    return order;
}

/**
 * @brief Returns a markless cycle, found by walking markless predecessors among the states the topological order leaves
 *        out, each of which has such a predecessor, until a state repeats.
 * @param graph The markless graph.
 * @param ordered Whether each state is in the topological order; some state is not.
 * @return A state on the cycle, and the symbols of the cycle from it back to it.
 */
[[nodiscard]] std::pair<State_t, std::vector<Marked>> markless_cycle(
        const Markless_graph& graph, const std::vector<bool>& ordered)
{
    /**
     * @brief Returns whether a markless step leaves a state the topological order leaves out.
     * @param edge The step, as the state left and the symbol read.
     * @return True when its state is unordered.
     */
    const auto unordered{[&ordered](const std::pair<State_t, Marked>& edge) {
        const auto& [from, symbol]{edge};

        return !ordered[from];
    }};

    const auto first_unordered{std::ranges::find(ordered, false)};

    auto state{static_cast<State_t>(std::ranges::distance(ordered.cbegin(), first_unordered))};

    std::unordered_map<State_t, std::size_t> walked{};

    // symbols[t] is the symbol of the markless step into the walk's state t from its state t + 1.
    std::vector<Marked> symbols{};

    for (;;)
    {
        const auto [entry, fresh]{walked.try_emplace(state, symbols.size())};

        if (!fresh)
        {
            break;
        }

        const auto step{std::ranges::find_if(graph.predecessors[state], unordered)};

        const auto& [from, symbol]{*step};

        symbols.push_back(symbol);

        state = from;
    }

    const auto cycle_start{static_cast<std::ptrdiff_t>(walked.at(state))};

    std::vector<Marked> loop{symbols.cbegin() + cycle_start, symbols.cend()};

    std::ranges::reverse(loop);

    return {state, loop};
}

/**
 * @brief Returns the lasso through a markless cycle: the shortest stem from the start to a state on it, the cycle, and
 *        the shortest suffix from that state to an accepting one.
 * @param verifier The verifier, trim.
 * @param graph Its markless graph.
 * @param ordered Whether each state is in the topological order of the markless graph; some state is not.
 * @return The lasso.
 */
[[nodiscard]] Lasso lasso_of(const Verifier& verifier, const Markless_graph& graph, const std::vector<bool>& ordered)
{
    const auto [root, loop]{markless_cycle(graph, ordered)};

    const auto alphabet{alphabet_of(verifier)};

    /**
     * @brief Reads every marked symbol over the alphabet from a state.
     * @tparam Emit The callback type.
     * @param state The state read from.
     * @param emit Called with each symbol that steps and the state it leads to.
     */
    const auto expand{[&verifier, &alphabet]<typename Emit>(const State_t state, const Emit& emit) {
        expand_steps(verifier, alphabet, state, emit);
    }};

    /**
     * @brief Returns whether a state is the cycle's root.
     * @param state The state.
     * @return True when it is.
     */
    const auto at_root{[root](const State_t state) { return state == root; }};

    /**
     * @brief Returns whether a state accepts.
     * @param state The state.
     * @return True when it does.
     */
    const auto accepting{[&verifier](const State_t state) { return verifier.accepts(state); }};

    const auto stem{shortest_path(verifier.start(), at_root, expand)};

    const auto suffix{shortest_path(root, accepting, expand)};

    auto stem_string{marked_string_of(*stem)};

    auto loop_string{marked_string_of(loop)};

    auto suffix_string{marked_string_of(*suffix)};

    return {.stem = std::move(stem_string), .loop = std::move(loop_string), .suffix = std::move(suffix_string)};
}

/**
 * @brief Returns the longest segment over an acyclic markless graph: the longest markless path, plus one when a marked
 *        step leaves its end, as is when its end accepts.
 * @param verifier The verifier, trim.
 * @param graph Its markless graph.
 * @param order The states in topological order, all of them.
 * @return The supremum of the segment lengths.
 */
[[nodiscard]] std::size_t longest_segment(
        const Verifier& verifier, const Markless_graph& graph, const std::vector<State_t>& order)
{
    std::vector<std::size_t> longest(verifier.state_count(), 0);

    std::size_t gap{0};

    for (const auto state : order)
    {
        for (const auto to : graph.successors[state] | std::views::values)
        {
            longest[to] = std::max(longest[to], longest[state] + 1);
        }

        if (verifier.accepts(state))
        {
            gap = std::max(gap, longest[state]);
        }

        if (graph.marked_step[state])
        {
            gap = std::max(gap, longest[state] + 1);
        }
    }

    return gap;
}

/**
 * @brief Reads a byte into every state of a subset under both marks.
 * @param verifier The verifier.
 * @param states The states, sorted and without repeats.
 * @param byte The byte.
 * @return The states reached, sorted and without repeats.
 */
[[nodiscard]] std::vector<State_t> subset_after(
        const Verifier& verifier, const std::vector<State_t>& states, const unsigned char byte)
{
    std::vector<State_t> next{};

    for (const auto state : states)
    {
        for (const auto boundary_after : marks)
        {
            if (const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})})
            {
                next.push_back(*to);
            }
        }
    }

    dedup(next);

    return next;
}

/**
 * @brief Returns whether some state of a subset accepts.
 * @param verifier The verifier.
 * @param states The states.
 * @return True when one accepts.
 */
[[nodiscard]] bool accepts_any(const Verifier& verifier, const std::vector<State_t>& states)
{
    /**
     * @brief Returns whether a state accepts.
     * @param state The state.
     * @return True when it does.
     */
    const auto accepting{[&verifier](const State_t state) { return verifier.accepts(state); }};

    return std::ranges::any_of(states, accepting);
}

/**
 * @brief Returns whether a verifier accepts an input under some marking.
 * @param verifier The verifier.
 * @param bytes The input.
 * @return True when the input is in the verifier's domain.
 */
[[nodiscard]] bool in_domain(const Verifier& verifier, const std::string_view bytes)
{
    std::vector<State_t> states{verifier.start()};

    for (const auto byte : bytes)
    {
        const auto read{static_cast<unsigned char>(byte)};

        states = subset_after(verifier, states, read);
    }

    return accepts_any(verifier, states);
}

/**
 * @brief Searches the positions and states for a marking under which a verifier accepts an input.
 * @param verifier The verifier.
 * @param bytes The input, in the verifier's domain.
 * @return The marked input.
 */
[[nodiscard]] Marked_string accepted_marking(const Verifier& verifier, const std::string_view bytes)
{
    /**
     * @brief Returns whether a key has read the whole input into an accepting state.
     * @param key The key.
     * @return True when it has.
     */
    const auto goal{[&](const Marking_key& key) { return key.read == bytes.size() && verifier.accepts(key.state); }};

    /**
     * @brief Reads the next byte of the input from a key under both marks.
     * @tparam Emit The callback type.
     * @param key The key read from.
     * @param emit Called with each symbol that steps and the key it leads to.
     */
    const auto expand{[&]<typename Emit>(const Marking_key& key, const Emit& emit) {
        const auto [read, state]{key};

        if (read == bytes.size())
        {
            return;
        }

        const auto byte{static_cast<unsigned char>(bytes[read])};

        for (const auto boundary_after : marks)
        {
            const Marked symbol{.byte = byte, .boundary_after = boundary_after};

            if (const auto to{verifier.step(state, symbol)})
            {
                emit(symbol, Marking_key{.read = read + 1, .state = *to});
            }
        }
    }};

    const auto path{shortest_path(Marking_key{.read = 0, .state = verifier.start()}, goal, expand)};

    return marked_string_of(*path);
}

/**
 * @brief Returns the boundary half: the shortest input both verifiers accept under markings that differ, marked as the
 *        first accepts it.
 * @param a The first verifier.
 * @param b The second verifier.
 * @param alphabet The bytes both alphabets hold.
 * @return The witness, or std::nullopt when the two agree on every input both accept.
 */
[[nodiscard]] std::optional<Marked_string> boundary_divergence(
        const Verifier& a, const Verifier& b, const std::vector<unsigned char>& alphabet)
{
    /**
     * @brief Returns whether both verifiers accept after markings that differed.
     * @param key The key.
     * @return True when they do.
     */
    const auto goal{[&](const Pair_key& key) { return key.differed && a.accepts(key.a) && b.accepts(key.b); }};

    /**
     * @brief Reads every byte of the alphabet from a key, under a mark for each verifier.
     * @tparam Emit The callback type.
     * @param key The key read from.
     * @param emit Called with the first verifier's symbol and the key it leads to.
     */
    const auto expand{[&]<typename Emit>(const Pair_key& key, const Emit& emit) {
        /**
         * @brief Pairs one step of the first verifier with every step of the second on the same byte.
         * @param symbol The first verifier's symbol.
         * @param to_a The state the first verifier enters.
         */
        const auto pair_with_b{[&](const Marked symbol, const State_t to_a) {
            for (const auto mark_b : marks)
            {
                const auto to_b{b.step(key.b, {.byte = symbol.byte, .boundary_after = mark_b})};

                if (!to_b)
                {
                    continue;
                }

                const auto differed{key.differed || mark_b != symbol.boundary_after};

                emit(symbol, Pair_key{.a = to_a, .b = *to_b, .differed = differed});
            }
        }};

        expand_steps(a, alphabet, key.a, pair_with_b);
    }};

    const auto path{shortest_path(Pair_key{.a = a.start(), .b = b.start(), .differed = false}, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    return marked_string_of(*path);
}

/**
 * @brief Returns the domain half: the shortest input exactly one verifier accepts under some marking, marked as that
 *        one accepts it.
 * @param a The first verifier.
 * @param b The second verifier.
 * @param alphabet The bytes either alphabet holds.
 * @return The witness, or std::nullopt when the two domains are equal.
 */
[[nodiscard]] std::optional<Marked_string> domain_divergence(
        const Verifier& a, const Verifier& b, const std::vector<unsigned char>& alphabet)
{
    /**
     * @brief Returns whether exactly one verifier accepts after the bytes read.
     * @param key The key.
     * @return True when one does and the other does not.
     */
    const auto goal{[&](const Subset_key& key) { return accepts_any(a, key.a) != accepts_any(b, key.b); }};

    /**
     * @brief Reads every byte of the alphabet from a key into both subsets.
     * @tparam Emit The callback type.
     * @param key The key read from.
     * @param emit Called with each unmarked symbol and the key it leads to, unless both subsets die.
     */
    const auto expand{[&]<typename Emit>(const Subset_key& key, const Emit& emit) {
        for (const auto byte : alphabet)
        {
            auto next_a{subset_after(a, key.a, byte)};

            auto next_b{subset_after(b, key.b, byte)};

            Subset_key next{.a = std::move(next_a), .b = std::move(next_b)};

            if (!next.a.empty() || !next.b.empty())
            {
                emit(Marked{.byte = byte, .boundary_after = false}, std::move(next));
            }
        }
    }};

    const auto path{shortest_path(Subset_key{.a = {a.start()}, .b = {b.start()}}, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    const auto [bytes, boundaries]{marked_string_of(*path)};

    const auto& accepting{in_domain(a, bytes) ? a : b};

    return accepted_marking(accepting, bytes);
}

} // namespace

std::optional<Miscovering> miscovering(
        const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"miscovering: the window is empty or its origin lies outside it"};
    }

    const auto alphabet{alphabet_of(verifier)};

    const auto borders{borders_of(window)};

    std::vector<bool> start_marks(window.size(), false);

    start_marks.back() = true;

    /**
     * @brief Returns whether the covering boundary of an occurrence is its origin: the last boundary before one of its
     *        bytes is the one before byte origin.
     * @param marks The boundaries before each byte of the occurrence, as the certification key holds them.
     * @return True when the token containing the occurrence's final byte begins at the origin.
     */
    const auto covered_at_origin{[origin](const std::vector<bool>& marks) {
        return marks[origin] && std::ranges::none_of(marks | std::views::drop(origin + 1), std::identity{});
    }};

    /**
     * @brief Returns whether a key has missed an occurrence and accepts.
     * @param key The key.
     * @return True when it has and does.
     */
    const auto goal{[&verifier](const Certification_key& key) { return key.missed && verifier.accepts(key.state); }};

    /**
     * @brief Reads every marked symbol over the alphabet from a key, advancing the window's match and the marks.
     * @tparam Emit The callback type.
     * @param key The key read from.
     * @param emit Called with each symbol that steps and the key it leads to.
     */
    const auto expand{[&]<typename Emit>(const Certification_key& key, const Emit& emit) {
        /**
         * @brief Advances the key by one step of the verifier.
         * @param symbol The symbol read.
         * @param to The state entered.
         */
        const auto advance{[&](const Marked symbol, const State_t to) {
            const auto matched{matched_after(window, borders, key.matched, symbol.byte)};

            const auto missed{key.missed || (matched == window.size() && !covered_at_origin(key.marks))};

            Certification_key next{.state = to, .matched = matched, .marks = key.marks, .missed = missed};

            next.marks.erase(next.marks.begin());

            next.marks.push_back(symbol.boundary_after);

            emit(symbol, std::move(next));
        }};

        expand_steps(verifier, alphabet, key.state, advance);
    }};

    const Certification_key start{.state = verifier.start(), .matched = 0, .marks = start_marks, .missed = false};

    const auto path{shortest_path(start, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    auto segmentation{marked_string_of(*path)};

    const auto occurrence{missed_occurrence(segmentation, window, origin)};

    return Miscovering{.segmentation = std::move(segmentation), .occurrence = occurrence};
}

Boundary_gap boundary_gap(const Verifier& verifier)
{
    if (domain_is_empty(verifier))
    {
        throw std::invalid_argument{"boundary_gap: the domain is empty, so no segment has a length"};
    }

    const auto graph{markless_graph_of(verifier)};

    const auto order{topological_order(graph)};

    if (order.size() == verifier.state_count())
    {
        return longest_segment(verifier, graph, order);
    }

    std::vector<bool> ordered(verifier.state_count(), false);

    for (const auto state : order)
    {
        ordered[state] = true;
    }

    return lasso_of(verifier, graph, ordered);
}

std::optional<Mask> realizable(const Verifier& verifier)
{
    if (domain_is_empty(verifier))
    {
        return std::nullopt;
    }

    Mask mask(verifier.state_count());

    for (const auto& [from, symbol] : verifier.transitions() | std::views::keys)
    {
        mask[from].push_back(symbol);
    }

    for (auto& steps : mask)
    {
        std::ranges::sort(steps);
    }

    return mask;
}

std::optional<Divergence> divergence(const Verifier& a, const Verifier& b)
{
    const auto alphabet_a{alphabet_of(a)};

    const auto alphabet_b{alphabet_of(b)};

    std::vector<unsigned char> common{};

    std::vector<unsigned char> either{};

    std::ranges::set_intersection(alphabet_a, alphabet_b, std::back_inserter(common));

    std::ranges::set_union(alphabet_a, alphabet_b, std::back_inserter(either));

    if (auto witness{boundary_divergence(a, b, common)})
    {
        return Divergence{.half = Half::boundary, .witness = std::move(*witness)};
    }

    if (auto witness{domain_divergence(a, b, either)})
    {
        return Divergence{.half = Half::domain, .witness = std::move(*witness)};
    }

    return std::nullopt;
}

} // namespace munch::dfa

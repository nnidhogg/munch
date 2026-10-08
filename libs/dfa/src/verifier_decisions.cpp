#include "munch/dfa/verifier_decisions.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
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
 * @brief A key of the dependence product: whether the cut has been placed, the verifier's state, the continuing copy's
 *        after the cut, the restarted copy's state after the cut or none once a step refused it, how much of the window
 *        the text's end matches, the window's first origin bytes before the cut and its remaining bytes after it, and
 *        whether the prefix before the cut was not accepted.
 *
 * No step enters the start, so the start's key is reached before any byte and never again, and the cut at position zero
 * is placed from it alone.
 */
struct Cut_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Cut_key&) const = default;

    /**
     * @brief Whether the cut has been placed.
     */
    bool cut{};

    /**
     * @brief The verifier's state, the continuing copy's after the cut.
     */
    State_t state{};

    /**
     * @brief The restarted copy's state after the cut, or none before it and once a step refused the copy.
     */
    std::optional<State_t> restarted{};

    /**
     * @brief How much of the window the text's end matches: of its first origin bytes before the cut, of its remaining
     *        bytes after it.
     */
    std::size_t matched{};

    /**
     * @brief Whether the prefix before the cut was not accepted.
     */
    bool prefix_fails{};
};

/**
 * @brief A key of the chunk product: the continuing copy's state, the restarted copy's state or none once a step of
 *        the current chunk refused it, the bytes before the held ones an occurrence anchoring the next commit may begin
 *        in, the marked bytes read by the continuing copy and not yet by the restarted one, whether any byte has
 *        reached the restarted copy, and whether a chunk was refused.
 *
 * The held bytes are at most h, the longest window, and the earlier bytes at most h - 2: an occurrence anchoring the
 * position after the oldest held byte ends within h bytes of it and begins at most h - 1 bytes before it.
 */
struct Chunk_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Chunk_key&) const = default;

    /**
     * @brief The continuing copy's state.
     */
    State_t state{};

    /**
     * @brief The restarted copy's state, or none once a step of the current chunk refused it.
     */
    std::optional<State_t> restarted{};

    /**
     * @brief The bytes before the held ones an occurrence anchoring the next commit may begin in.
     */
    std::string earlier{};

    /**
     * @brief The marked bytes the continuing copy has read and the restarted copy has not.
     */
    std::vector<Marked> held{};

    /**
     * @brief Whether any byte has reached the restarted copy.
     */
    bool begun{};

    /**
     * @brief Whether a chunk was refused.
     */
    bool refused{};
};

/**
 * @brief A path a search found: the symbols read from the start to the goal, and the keys reached, the start first and
 *        the goal last, one more than the symbols.
 * @tparam Key The search key type.
 */
template <typename Key>
struct Path
{
    /**
     * @brief The symbols read, in order.
     */
    std::vector<Marked> symbols{};

    /**
     * @brief The keys reached, the start first and the goal last.
     */
    std::vector<Key> keys{};
};

/**
 * @brief Searches breadth first from a key, returning the path to the first key admitted that is a goal.
 *
 * Keys are admitted in the order they are first reached and expanded in that order, so the path is a shortest one.
 * @tparam Key The search key type.
 * @tparam Goal The goal predicate's type.
 * @tparam Expand The expansion's callable type.
 * @param start The key the search starts from.
 * @param goal Whether a key ends the search.
 * @param expand Calls its second argument with each symbol read from a key and the key it leads to.
 * @return The symbols and the keys from the start to the first goal, or std::nullopt when no admitted key is one.
 */
template <typename Key, typename Goal, typename Expand>
[[nodiscard]] std::optional<Path<Key>> shortest_path(Key start, const Goal& goal, const Expand& expand)
{
    std::map<Key, Parent> seen{};

    const auto [first, started]{seen.emplace(std::move(start), Parent{})};

    std::vector<typename std::map<Key, Parent>::const_iterator> admitted{first};

    const auto path_to{[&admitted](const std::size_t last) {
        Path<Key> path{};

        for (auto at{last}; at != 0;)
        {
            const auto& [key, parent]{*admitted[at]};

            path.symbols.push_back(parent.symbol);

            path.keys.push_back(key);

            at = parent.from;
        }

        path.keys.push_back(admitted[0]->first);

        std::ranges::reverse(path.symbols);

        std::ranges::reverse(path.keys);

        return path;
    }};

    for (std::size_t next{0}; next < admitted.size(); ++next)
    {
        const auto& [key, parent]{*admitted[next]};

        if (goal(key))
        {
            return path_to(next);
        }

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

    const auto expand{[&verifier, &alphabet]<typename Emit>(const State_t state, const Emit& emit) {
        expand_steps(verifier, alphabet, state, emit);
    }};

    const auto at_root{[root](const State_t state) { return state == root; }};

    const auto accepting{[&verifier](const State_t state) { return verifier.accepts(state); }};

    const auto stem{shortest_path(verifier.start(), at_root, expand)};

    const auto suffix{shortest_path(root, accepting, expand)};

    auto stem_string{marked_string_of(stem->symbols)};

    auto loop_string{marked_string_of(loop)};

    auto suffix_string{marked_string_of(suffix->symbols)};

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
    const auto goal{[&](const Marking_key& key) { return key.read == bytes.size() && verifier.accepts(key.state); }};

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

    return marked_string_of(path->symbols);
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
    const auto goal{[&](const Pair_key& key) { return key.differed && a.accepts(key.a) && b.accepts(key.b); }};

    const auto pair_with_b{
            [&b]<typename Emit>(const Pair_key& key, const Marked symbol, const State_t to_a, const Emit& emit) {
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

    const auto expand{[&]<typename Emit>(const Pair_key& key, const Emit& emit) {
        const auto pair_step{[&](const Marked symbol, const State_t to_a) { pair_with_b(key, symbol, to_a, emit); }};

        expand_steps(a, alphabet, key.a, pair_step);
    }};

    const auto path{shortest_path(Pair_key{.a = a.start(), .b = b.start(), .differed = false}, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    return marked_string_of(path->symbols);
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
    const auto goal{[&](const Subset_key& key) { return accepts_any(a, key.a) != accepts_any(b, key.b); }};

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

    const auto [bytes, boundaries]{marked_string_of(path->symbols)};

    const auto& accepting{in_domain(a, bytes) ? a : b};

    return accepted_marking(accepting, bytes);
}

/**
 * @brief Returns the cut of a dependence path: the number of symbols read when the restarted copy begins, which is at
 *        the path's start when the cut at position zero was placed, since no step enters the start.
 * @param keys The keys of the path, the start first.
 * @param start The verifier's start state.
 * @return The cut.
 */
[[nodiscard]] std::size_t cut_of(const std::vector<Cut_key>& keys, const State_t start)
{
    const auto at_cut{[start](const Cut_key& key) { return key.cut && key.restarted == start; }};

    const auto found{std::ranges::find_if(keys, at_cut)};

    if (found == keys.cend())
    {
        return 0;
    }

    const auto index{std::ranges::distance(keys.cbegin(), found)};

    return static_cast<std::size_t>(index);
}

/**
 * @brief Returns whether a position of a text is an anchor of an inventory: some window of it occurs origin bytes
 *        before the position.
 * @param inventory The certified pairs.
 * @param bytes The text.
 * @param position The position, at most the text's length.
 * @return True when a window anchors the position.
 */
[[nodiscard]] bool is_anchor(
        const std::span<const Certified_pair> inventory, const std::string_view bytes, const std::size_t position)
{
    for (const auto& [window, origin] : inventory)
    {
        if (position < origin)
        {
            continue;
        }

        const auto from_start{bytes.substr(position - origin)};

        if (from_start.starts_with(window))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Returns the anchors of an inventory in a text, ascending and without repeats.
 * @param bytes The text.
 * @param inventory The certified pairs.
 * @return The anchors.
 */
[[nodiscard]] std::vector<std::size_t> anchors_of(
        const std::string_view bytes, const std::span<const Certified_pair> inventory)
{
    std::vector<std::size_t> anchors{};

    for (std::size_t position{0}; position <= bytes.size(); ++position)
    {
        if (is_anchor(inventory, bytes, position))
        {
            anchors.push_back(position);
        }
    }

    return anchors;
}

/**
 * @brief Returns whether a chunk of a marked string is not accepted under the string's marks restricted to it, the
 *        chunk's own final byte carrying no boundary.
 * @param verifier The verifier.
 * @param segmentation The marked string.
 * @param chunk The chunk, within the string.
 * @return True when the verifier refuses a step of the chunk or does not accept where it ends.
 */
[[nodiscard]] bool chunk_fails(const Verifier& verifier, const Marked_string& segmentation, const Chunk& chunk)
{
    std::optional<State_t> state{verifier.start()};

    for (auto index{chunk.begin}; index < chunk.end && state; ++index)
    {
        const auto byte{static_cast<unsigned char>(segmentation.bytes[index])};

        const auto boundary_after{index + 1 < chunk.end && segmentation.boundaries[index]};

        state = verifier.step(*state, {.byte = byte, .boundary_after = boundary_after});
    }

    return !state || !verifier.accepts(*state);
}

/**
 * @brief Returns the chunks between consecutive anchors of a marked string, the first from position zero and the last
 *        to the end, that fail.
 * @param verifier The verifier.
 * @param segmentation The marked string.
 * @param anchors The anchors, ascending.
 * @return The failing chunks, ascending.
 */
[[nodiscard]] std::vector<Chunk> failing_chunks(
        const Verifier& verifier, const Marked_string& segmentation, const std::vector<std::size_t>& anchors)
{
    std::vector<Chunk> chunks{};

    std::size_t begin{0};

    for (const auto anchor : anchors)
    {
        chunks.push_back({.begin = begin, .end = anchor});

        begin = anchor;
    }

    chunks.push_back({.begin = begin, .end = segmentation.bytes.size()});

    const auto fails{[&](const Chunk& chunk) { return chunk_fails(verifier, segmentation, chunk); }};

    std::vector<Chunk> failing{};

    std::ranges::copy_if(chunks, std::back_inserter(failing), fails);

    return failing;
}

} // namespace

std::optional<Miscovering> miscovering(
        const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"miscovering: the window is empty or its origin lies outside it"};
    }

    const auto borders{borders_of(window)};

    std::vector<bool> start_marks(window.size(), false);

    start_marks.back() = true;

    // The token holding the occurrence's last byte begins at the origin: a mark before byte origin, none after it.
    const auto covered_at_origin{[origin](const std::vector<bool>& marks) {
        return marks[origin] && std::ranges::none_of(marks | std::views::drop(origin + 1), std::identity{});
    }};

    const auto goal{[&verifier](const Certification_key& key) { return key.missed && verifier.accepts(key.state); }};

    const auto alphabet{alphabet_of(verifier)};

    const auto expand{[&]<typename Emit>(const Certification_key& key, const Emit& emit) {
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

    auto segmentation{marked_string_of(path->symbols)};

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

std::optional<Dependence> dependence(const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"dependence: the window is empty or its origin lies outside it"};
    }

    if (const auto refutation{miscovering(verifier, window, origin)})
    {
        throw std::invalid_argument{std::format(
                "dependence: the verifier does not certify ({}, {}), the text {} covers an occurrence from elsewhere, "
                "so no cut is certified",
                window, origin, refutation->segmentation.bytes)};
    }

    const auto head{window.substr(0, origin)};

    const auto tail{window.substr(origin)};

    const auto borders{borders_of(head)};

    const auto alphabet{alphabet_of(verifier)};

    // Before the cut the matcher runs over the window's first origin bytes, which are none when the origin is zero.
    const auto head_matched{[&head, &borders](const std::size_t matched, const unsigned char byte) {
        return head.empty() ? 0 : matched_after(head, borders, matched, byte);
    }};

    const auto suffix_fails{
            [&verifier](const Cut_key& key) { return !key.restarted || !verifier.accepts(*key.restarted); }};

    const auto goal{[&](const Cut_key& key) {
        const auto confirmed{key.cut && key.matched == tail.size() && verifier.accepts(key.state)};

        const auto fails{key.prefix_fails || suffix_fails(key)};

        return confirmed && fails;
    }};

    const auto expand_after{[&]<typename Emit>(const Cut_key& key, const Emit& emit) {
        const auto follow{[&](const Marked symbol, const State_t continuing) {
            // The window's remaining bytes follow the cut at once.
            if (key.matched < tail.size() && symbol.byte != static_cast<unsigned char>(tail[key.matched]))
            {
                return;
            }

            const auto matched{std::min(key.matched + 1, tail.size())};

            const auto restarted{
                    key.restarted.and_then([&](const State_t state) { return verifier.step(state, symbol); })};

            emit(symbol, Cut_key{.cut = true,
                                 .state = continuing,
                                 .restarted = restarted,
                                 .matched = matched,
                                 .prefix_fails = key.prefix_fails});
        }};

        expand_steps(verifier, alphabet, key.state, follow);
    }};

    const auto expand_before{[&]<typename Emit>(const Cut_key& key, const Emit& emit) {
        const auto advance{[&](const Marked symbol, const State_t to) {
            const auto matched{head_matched(key.matched, symbol.byte)};

            emit(symbol, Cut_key{.cut = false,
                                 .state = to,
                                 .restarted = std::nullopt,
                                 .matched = matched,
                                 .prefix_fails = false});

            if (!symbol.boundary_after || matched != origin)
            {
                return;
            }

            // The cut: the marked byte ends the prefix, whose own final symbol carries no boundary, so the prefix is
            // accepted exactly when the same byte without the mark reaches an accepting state.
            const auto unmarked{verifier.step(key.state, {.byte = symbol.byte, .boundary_after = false})};

            const auto prefix_fails{!unmarked || !verifier.accepts(*unmarked)};

            emit(symbol, Cut_key{.cut = true,
                                 .state = to,
                                 .restarted = verifier.start(),
                                 .matched = 0,
                                 .prefix_fails = prefix_fails});
        }};

        expand_steps(verifier, alphabet, key.state, advance);
    }};

    const auto expand{[&]<typename Emit>(const Cut_key& key, const Emit& emit) {
        if (key.cut)
        {
            expand_after(key, emit);

            return;
        }

        expand_before(key, emit);

        if (origin == 0 && key.state == verifier.start())
        {
            // The cut at position zero, before any byte: an empty prefix, accepted exactly when the start accepts.
            const Cut_key at_zero{
                    .cut = true,
                    .state = verifier.start(),
                    .restarted = verifier.start(),
                    .matched = 0,
                    .prefix_fails = !verifier.accepts(verifier.start())};

            expand_after(at_zero, emit);
        }
    }};

    const Cut_key start{
            .cut = false,
            .state = verifier.start(),
            .restarted = std::nullopt,
            .matched = 0,
            .prefix_fails = false};

    const auto path{shortest_path(start, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    const auto& violating{path->keys.back()};

    const auto cut{cut_of(path->keys, verifier.start())};

    auto segmentation{marked_string_of(path->symbols)};

    return Dependence{
            .segmentation = std::move(segmentation),
            .occurrence = cut - origin,
            .cut = cut,
            .prefix_fails = violating.prefix_fails,
            .suffix_fails = suffix_fails(violating)};
}

std::optional<Chunk_dependence> chunk_dependence(
        const Verifier& verifier, const std::span<const Certified_pair> inventory)
{
    if (inventory.empty())
    {
        throw std::invalid_argument{"chunk_dependence: the inventory is empty, so no anchor is placed"};
    }

    for (const auto& [window, origin] : inventory)
    {
        if (window.empty() || origin >= window.size())
        {
            throw std::invalid_argument{std::format(
                    "chunk_dependence: the window {} is empty or its origin {} lies outside it", window, origin)};
        }

        if (const auto refutation{miscovering(verifier, window, origin)})
        {
            throw std::invalid_argument{std::format(
                    "chunk_dependence: the verifier does not certify ({}, {}), the text {} covers an occurrence from "
                    "elsewhere, so its anchors are not boundaries",
                    window, origin, refutation->segmentation.bytes)};
        }
    }

    const auto window_length{[](const Certified_pair& pair) { return pair.window.size(); }};

    const auto lengths{inventory | std::views::transform(window_length)};

    const auto longest{std::ranges::max(lengths)};

    // The earlier bytes kept, h - 2 for the longest window h and none below h = 3.
    const auto kept{longest - std::min<std::size_t>(longest, 2)};

    const auto alphabet{alphabet_of(verifier)};

    const auto restarted_after{[&verifier](const std::optional<State_t> state, const Marked symbol) {
        return state.and_then([&](const State_t from) { return verifier.step(from, symbol); });
    }};

    // Reads the first count held bytes into the restarted copy. Where the position after a byte is an anchor the copy
    // reads the byte without its mark, the chunk's own final symbol, is accepted or refused, and restarts.
    const auto commit{[&](Chunk_key& key, const std::size_t count) {
        std::string bytes{key.earlier};

        for (const auto byte : key.held | std::views::transform(&Marked::byte))
        {
            bytes.push_back(static_cast<char>(byte));
        }

        if (!key.begun && is_anchor(inventory, bytes, 0))
        {
            key.refused = key.refused || !verifier.accepts(verifier.start());
        }

        for (std::size_t index{0}; index < count; ++index)
        {
            const auto symbol{key.held[index]};

            const auto position{key.earlier.size() + index + 1};

            if (!is_anchor(inventory, bytes, position))
            {
                key.restarted = restarted_after(key.restarted, symbol);

                continue;
            }

            const auto final_state{restarted_after(key.restarted, {.byte = symbol.byte, .boundary_after = false})};

            key.refused = key.refused || !final_state || !verifier.accepts(*final_state);

            key.restarted = verifier.start();
        }
    }};

    // Every state is a candidate end: the held bytes reach the restarted copy with no more to come, and the last chunk
    // is its acceptance.
    const auto goal{[&](const Chunk_key& key) {
        if (!verifier.accepts(key.state))
        {
            return false;
        }

        auto ended{key};

        commit(ended, ended.held.size());

        return ended.refused || !ended.restarted || !verifier.accepts(*ended.restarted);
    }};

    const auto expand{[&]<typename Emit>(const Chunk_key& key, const Emit& emit) {
        const auto advance{[&](const Marked symbol, const State_t to) {
            Chunk_key next{
                    .state = to,
                    .restarted = key.restarted,
                    .earlier = key.earlier,
                    .held = key.held,
                    .begun = key.begun,
                    .refused = key.refused};

            next.held.push_back(symbol);

            if (next.held.size() <= longest)
            {
                emit(symbol, std::move(next));

                return;
            }

            // The oldest held byte reaches the restarted copy, every occurrence anchoring the position after it
            // complete, and joins the earlier bytes, of which the last kept stay.
            commit(next, 1);

            next.earlier.push_back(static_cast<char>(next.held.front().byte));

            if (next.earlier.size() > kept)
            {
                next.earlier.erase(0, 1);
            }

            next.held.erase(next.held.begin());

            next.begun = true;

            emit(symbol, std::move(next));
        }};

        expand_steps(verifier, alphabet, key.state, advance);
    }};

    const Chunk_key start{
            .state = verifier.start(),
            .restarted = verifier.start(),
            .earlier = {},
            .held = {},
            .begun = false,
            .refused = false};

    const auto path{shortest_path(start, goal, expand)};

    if (!path)
    {
        return std::nullopt;
    }

    auto segmentation{marked_string_of(path->symbols)};

    auto anchors{anchors_of(segmentation.bytes, inventory)};

    auto failing{failing_chunks(verifier, segmentation, anchors)};

    return Chunk_dependence{
            .segmentation = std::move(segmentation),
            .anchors = std::move(anchors),
            .failing = std::move(failing)};
}

} // namespace munch::dfa

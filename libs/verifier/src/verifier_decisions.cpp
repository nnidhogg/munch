#include "munch/verifier/verifier_decisions.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "munch/verifier/verifier.hpp"

namespace munch::verifier
{
namespace
{
/**
 * @brief A verifier state, as the decisions name one.
 */
using State_t = Verifier::State_t;

/**
 * @brief A set of states of a verifier's minimal automaton, one bit per class.
 */
using Class_set_t = std::vector<bool>;

/**
 * @brief A relation between the states of a verifier's minimal automaton, one bit per pair of classes in row-major
 *        order.
 */
using Relation_t = std::vector<bool>;

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
 * @brief A key of the right-context product: whether the cut has been placed, the verifier's state, the continuing
 *        copy's after the cut, the states a worker's copy can be in under any marks of the bytes read since the cut,
 *        how much of the window the text's end matches, of its first origin bytes before the cut and of its remaining
 *        bytes after it, and whether the worker's input ending at the last byte read fails.
 *
 * After the cut the matched count is the number of bytes read since the cut, up to the remaining bytes' length, so the
 * keys the cut places are the only keys after it with none matched.
 */
struct Context_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Context_key&) const = default;

    /**
     * @brief Whether the cut has been placed.
     */
    bool cut{};

    /**
     * @brief The verifier's state, the continuing copy's after the cut.
     */
    State_t state{};

    /**
     * @brief The states the worker's copy can be in after the cut, sorted and without repeats, none before it.
     */
    std::vector<State_t> worker{};

    /**
     * @brief How much of the window the text's end matches: of its first origin bytes before the cut, of its remaining
     *        bytes after it.
     */
    std::size_t matched{};

    /**
     * @brief Whether the worker's input ending at the last byte read is not accepted, false before the cut.
     */
    bool fails{};
};

/**
 * @brief The right-context product explored from its start: its keys numbered in the order they were first reached, the
 *        start zero, and per key the steps from it and the steps into it.
 */
struct Context_graph
{
    /**
     * @brief The keys, by number.
     */
    std::vector<Context_key> keys{};

    /**
     * @brief Per key, its steps as the symbol read and the key entered, in the order they were taken.
     */
    std::vector<std::vector<std::pair<Marked, std::size_t>>> successors{};

    /**
     * @brief Per key, the steps into it as the key left and the symbol read.
     */
    std::vector<std::vector<std::pair<std::size_t, Marked>>> predecessors{};
};

/**
 * @brief What the keys of the right-context product reach, one bit per key: whether it confirms the occurrence with the
 *        continuing copy accepting, whether a failing context ends at it, and whether it lies after the cut with such a
 *        key reachable from it.
 */
struct Context_reach
{
    /**
     * @brief Whether the key lies after the cut with the window's remaining bytes all read and the continuing copy
     *        accepting.
     */
    std::vector<bool> confirmed{};

    /**
     * @brief Whether a failing context ends at the key: it lies after the cut, carries the failure, and a step leads
     *        from it to a key from which a confirming key is reachable.
     */
    std::vector<bool> failing{};

    /**
     * @brief Whether the key lies after the cut and a key where a failing context ends is reachable from it, the key
     *        itself included.
     */
    std::vector<bool> relevant{};
};

/**
 * @brief Which contexts fail, per number of steps past the cut up to the first layer of the right-context product's
 *        relevant keys that repeats an earlier one, and the context whose layer it repeats.
 *
 * Each layer is a function of the one before, so from the repeated context on the failures repeat with the number of
 * contexts held less it.
 */
struct Context_failures
{
    /**
     * @brief Per context below the first repeated layer, whether it fails at some occurrence.
     */
    std::vector<bool> fails{};

    /**
     * @brief The context whose layer the first repeated layer repeats.
     */
    std::size_t repeated{};
};

/**
 * @brief A key of the truncation search: a key of the right-context product and the steps taken since the cut, up to
 *        the context the search is for, and one more once a step has left a key where that context fails.
 */
struct Truncation_key
{
    /**
     * @brief Ordered member by member, so keys index the search's map.
     */
    auto operator<=>(const Truncation_key&) const = default;

    /**
     * @brief The number of the product's key.
     */
    std::size_t key{};

    /**
     * @brief The steps taken since the cut, zero before it, at most one more than the context.
     */
    std::size_t read{};
};

/**
 * @brief The byte projection of a verifier's minimal automaton: the class of each verifier state, the classes being the
 *        states no marked continuation tells apart, the least state of each class, and per byte the steps from each
 *        class.
 *
 * The verifier is trim, so every class can complete but the start's of an empty domain, which takes no step.
 */
struct Projection
{
    /**
     * @brief The bytes on the verifier's transitions, ascending.
     */
    std::vector<unsigned char> alphabet{};

    /**
     * @brief Per verifier state, its class, the classes numbered in the order of their least states.
     */
    std::vector<std::size_t> class_of{};

    /**
     * @brief Per class, its least verifier state.
     */
    std::vector<State_t> representatives{};

    /**
     * @brief Per byte of the alphabet and class, the steps the byte takes from it as the boundary bit and the class
     *        entered, no boundary first.
     */
    std::vector<std::vector<std::vector<std::pair<bool, std::size_t>>>> steps{};
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
 * @brief Returns the first occurrence of a window in a marked string at which the token covering a byte of the window
 *        does not begin at the origin.
 * @param segmentation The marked string, which holds one.
 * @param window The window.
 * @param byte The byte, an index of the window.
 * @param origin The origin.
 * @return The index of the occurrence's first byte.
 */
[[nodiscard]] std::size_t missed_occurrence(
        const Marked_string& segmentation, const std::string_view window, const std::size_t byte,
        const std::size_t origin)
{
    const std::string_view bytes{segmentation.bytes};

    std::size_t at{bytes.find(window)};

    for (;;)
    {
        if (covering_boundary(segmentation, at + byte) != at + origin)
        {
            return at;
        }

        at = bytes.find(window, at + 1);
    }
}

/**
 * @brief Returns whether the marks of a certification key, read as an occurrence completes, place the token covering a
 *        byte of the window at the origin: a mark before byte origin and none after it up to the byte.
 * @param marks The marks, one per byte of the window, whether a boundary sits before it.
 * @param byte The byte, an index of the window.
 * @param origin The origin, at most the byte.
 * @return True when the covering token begins at the origin.
 */
[[nodiscard]] bool covers(const std::vector<bool>& marks, const std::size_t byte, const std::size_t origin)
{
    const auto between{marks | std::views::take(byte + 1) | std::views::drop(origin + 1)};

    return marks[origin] && std::ranges::none_of(between, std::identity{});
}

/**
 * @brief Searches the certification product of a window breadth first for the shortest accepted marked string with an
 *        occurrence whose marks a predicate refuses, the flag raised at that occurrence staying raised.
 * @tparam Covered The predicate's type.
 * @param verifier The verifier.
 * @param window The window, nonempty.
 * @param covered Whether the marks of a completed occurrence place its covering token where it should begin.
 * @return The marked string, or std::nullopt when every occurrence in every accepted marked string is covered.
 */
template <typename Covered>
[[nodiscard]] std::optional<Marked_string> first_miss(
        const Verifier& verifier, const std::string_view window, const Covered& covered)
{
    const auto borders{borders_of(window)};

    std::vector<bool> start_marks(window.size(), false);

    start_marks.back() = true;

    const auto goal{[&verifier](const Certification_key& key) { return key.missed && verifier.accepts(key.state); }};

    const auto alphabet{alphabet_of(verifier)};

    const auto expand{[&]<typename Emit>(const Certification_key& key, const Emit& emit) {
        const auto advance{[&](const Marked symbol, const State_t to) {
            const auto matched{matched_after(window, borders, key.matched, symbol.byte)};

            const auto missed{key.missed || (matched == window.size() && !covered(key.marks))};

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

    return marked_string_of(path->symbols);
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
 * @brief Searches the positions and states for a marking under which a verifier reads an input from a state to a state
 *        that ends the search.
 * @tparam Ends The end predicate's type.
 * @param verifier The verifier.
 * @param from The state the input is read from.
 * @param bytes The input, which some marking reads from the state to an end.
 * @param ends Whether a state the whole input reaches ends the search.
 * @return The marked input.
 */
template <typename Ends>
[[nodiscard]] Marked_string marking_between(
        const Verifier& verifier, const State_t from, const std::string_view bytes, const Ends& ends)
{
    const auto goal{[&](const Marking_key& key) { return key.read == bytes.size() && ends(key.state); }};

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

    const auto path{shortest_path(Marking_key{.read = 0, .state = from}, goal, expand)};

    return marked_string_of(path->symbols);
}

/**
 * @brief Searches the positions and states for a marking under which a verifier accepts an input.
 * @param verifier The verifier.
 * @param bytes The input, in the verifier's domain.
 * @return The marked input.
 */
[[nodiscard]] Marked_string accepted_marking(const Verifier& verifier, const std::string_view bytes)
{
    const auto accepting{[&verifier](const State_t state) { return verifier.accepts(state); }};

    return marking_between(verifier, verifier.start(), bytes, accepting);
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

/**
 * @brief Explores the right-context product of a pair from its start, numbering every key reachable.
 *
 * Before the cut a step reads a marked symbol into the verifier and the matcher of the window's first origin bytes, and
 * a marked byte that completes them places the cut as well, the worker's copy beginning at the state the marked byte
 * reaches and the worker's input ending at the cut failing when the byte without its mark reaches no acceptance. After
 * the cut a step reads a marked symbol into the continuing copy while the window's remaining bytes follow at once, the
 * worker's copy reads its byte under both marks, and the input ending at it fails when no state the copy could be in
 * before the byte reaches acceptance on the byte without its mark.
 * @param verifier The verifier.
 * @param window The window, nonempty.
 * @param origin The origin, a byte index of the window.
 * @return The product's keys and steps.
 */
[[nodiscard]] Context_graph context_graph_of(
        const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    const auto head{window.substr(0, origin)};

    const auto tail{window.substr(origin)};

    const auto borders{borders_of(head)};

    const auto alphabet{alphabet_of(verifier)};

    // Before the cut the matcher runs over the window's first origin bytes, which are none when the origin is zero.
    const auto head_matched{[&head, &borders](const std::size_t matched, const unsigned char byte) {
        return head.empty() ? 0 : matched_after(head, borders, matched, byte);
    }};

    const auto ends_failing{[&verifier](const std::vector<State_t>& states, const unsigned char byte) {
        const auto accepted_unmarked{[&](const State_t state) {
            const auto to{verifier.step(state, {.byte = byte, .boundary_after = false})};

            return to && verifier.accepts(*to);
        }};

        return std::ranges::none_of(states, accepted_unmarked);
    }};

    Context_graph graph{};

    std::map<Context_key, std::size_t> numbers{};

    const auto number_of{[&graph, &numbers](Context_key key) {
        const auto [entry, inserted]{numbers.try_emplace(key, graph.keys.size())};

        if (inserted)
        {
            graph.keys.push_back(std::move(key));

            graph.successors.emplace_back();

            graph.predecessors.emplace_back();
        }

        return entry->second;
    }};

    std::ignore =
            number_of(Context_key{.cut = false, .state = verifier.start(), .worker = {}, .matched = 0, .fails = false});

    for (std::size_t next{0}; next < graph.keys.size(); ++next)
    {
        const auto key{graph.keys[next]};

        const auto link{[&](const Marked symbol, Context_key child) {
            const auto to{number_of(std::move(child))};

            graph.successors[next].emplace_back(symbol, to);

            graph.predecessors[to].emplace_back(next, symbol);
        }};

        const auto before{[&](const Marked symbol, const State_t to) {
            const auto matched{head_matched(key.matched, symbol.byte)};

            link(symbol, Context_key{.cut = false, .state = to, .worker = {}, .matched = matched, .fails = false});

            if (!symbol.boundary_after || matched != origin)
            {
                return;
            }

            const auto fails{ends_failing({key.state}, symbol.byte)};

            link(symbol, Context_key{.cut = true, .state = to, .worker = {to}, .matched = 0, .fails = fails});
        }};

        const auto after{[&](const Marked symbol, const State_t to) {
            // The window's remaining bytes follow the cut at once.
            if (key.matched < tail.size() && symbol.byte != static_cast<unsigned char>(tail[key.matched]))
            {
                return;
            }

            const auto matched{std::min(key.matched + 1, tail.size())};

            auto worker{subset_after(verifier, key.worker, symbol.byte)};

            const auto fails{ends_failing(key.worker, symbol.byte)};

            link(symbol, Context_key{
                                 .cut = true,
                                 .state = to,
                                 .worker = std::move(worker),
                                 .matched = matched,
                                 .fails = fails});
        }};

        if (key.cut)
        {
            expand_steps(verifier, alphabet, key.state, after);

            continue;
        }

        expand_steps(verifier, alphabet, key.state, before);
    }

    return graph;
}

/**
 * @brief Returns the keys of the right-context product from which a target is reachable, the targets included.
 * @param graph The product.
 * @param targets Per key, whether it is a target.
 * @return Per key, whether a target is reachable from it.
 */
[[nodiscard]] std::vector<bool> reaching(const Context_graph& graph, std::vector<bool> targets)
{
    std::vector<std::size_t> frontier{};

    for (std::size_t key{0}; key < targets.size(); ++key)
    {
        if (targets[key])
        {
            frontier.push_back(key);
        }
    }

    for (std::size_t next{0}; next < frontier.size(); ++next)
    {
        for (const auto from : graph.predecessors[frontier[next]] | std::views::keys)
        {
            if (targets[from])
            {
                continue;
            }

            targets[from] = true;

            frontier.push_back(from);
        }
    }

    return targets;
}

/**
 * @brief Returns what the keys of the right-context product reach: the confirming keys, the keys where a failing
 *        context ends and the keys after the cut that lead to one.
 * @param verifier The verifier.
 * @param graph The product.
 * @param remaining The number of the window's bytes from the origin.
 * @return The three bits per key.
 */
[[nodiscard]] Context_reach context_reach_of(
        const Verifier& verifier, const Context_graph& graph, const std::size_t remaining)
{
    const auto count{graph.keys.size()};

    std::vector<bool> confirmed(count, false);

    for (std::size_t key{0}; key < count; ++key)
    {
        const auto& [cut, state, worker, matched, fails]{graph.keys[key]};

        confirmed[key] = cut && matched == remaining && verifier.accepts(state);
    }

    const auto completes{reaching(graph, confirmed)};

    const auto leads_to_completion{[&completes](const std::pair<Marked, std::size_t>& step) {
        const auto& [symbol, to]{step};

        return completes[to];
    }};

    std::vector<bool> failing(count, false);

    for (std::size_t key{0}; key < count; ++key)
    {
        const auto& [cut, state, worker, matched, fails]{graph.keys[key]};

        failing[key] = cut && fails && std::ranges::any_of(graph.successors[key], leads_to_completion);
    }

    auto relevant{reaching(graph, failing)};

    for (std::size_t key{0}; key < count; ++key)
    {
        relevant[key] = relevant[key] && graph.keys[key].cut;
    }

    return {.confirmed = std::move(confirmed), .failing = std::move(failing), .relevant = std::move(relevant)};
}

/**
 * @brief Returns which contexts fail, layer by layer over the relevant keys of the right-context product: layer zero
 *        the relevant keys the cut places and each next layer the relevant keys a step leads to from the one before,
 *        a context failing when its layer holds a key where a failing context ends, up to the first layer that repeats
 *        an earlier one.
 * @param graph The product.
 * @param reach What the product's keys reach.
 * @return Per context up to the first repeated layer, whether it fails, and the context whose layer repeats.
 */
[[nodiscard]] Context_failures context_failures_of(const Context_graph& graph, const Context_reach& reach)
{
    const auto count{graph.keys.size()};

    std::vector<bool> layer(count, false);

    for (std::size_t key{0}; key < count; ++key)
    {
        layer[key] = reach.relevant[key] && graph.keys[key].matched == 0;
    }

    Context_failures failures{};

    std::map<std::vector<bool>, std::size_t> numbers{};

    for (;;)
    {
        const auto [entry, fresh]{numbers.try_emplace(layer, failures.fails.size())};

        if (!fresh)
        {
            failures.repeated = entry->second;

            return failures;
        }

        std::vector<bool> next(count, false);

        auto fails{false};

        for (std::size_t key{0}; key < count; ++key)
        {
            if (!layer[key])
            {
                continue;
            }

            fails = fails || reach.failing[key];

            for (const auto to : graph.successors[key] | std::views::values)
            {
                next[to] = next[to] || reach.relevant[to];
            }
        }

        failures.fails.push_back(fails);

        layer = std::move(next);
    }
}

/**
 * @brief Returns whether a context fails, past the layers explored by the cycle with which they repeat.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @param context The context.
 * @return True when the context fails at some occurrence.
 */
[[nodiscard]] bool fails_at(const Context_failures& failures, const std::size_t context)
{
    const auto& [fails, repeated]{failures};

    if (context < fails.size())
    {
        return fails[context];
    }

    const auto cycle{fails.size() - repeated};

    return fails[repeated + (context - repeated) % cycle];
}

/**
 * @brief Returns the least period with which the failing contexts repeat from some point on, the least divisor of the
 *        layers' cycle with which they repeat over one cycle from the repeated layer.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @return The period, at least one.
 */
[[nodiscard]] std::size_t least_period(const Context_failures& failures)
{
    const auto& [fails, repeated]{failures};

    const auto cycle{fails.size() - repeated};

    const auto repeats_with{[&failures, &fails, repeated, cycle](const std::size_t period) {
        const auto same_after{[&failures, period](const std::size_t context) {
            return fails_at(failures, context) == fails_at(failures, context + period);
        }};

        const auto contexts{std::views::iota(repeated, fails.size())};

        return cycle % period == 0 && std::ranges::all_of(contexts, same_after);
    }};

    const auto periods{std::views::iota(std::size_t{1}, cycle + 1)};

    return *std::ranges::find_if(periods, repeats_with);
}

/**
 * @brief Returns the threshold, the least context from which the failing contexts repeat with the period.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @param period The least period.
 * @return The threshold, at most the repeated layer's context.
 */
[[nodiscard]] std::size_t threshold_of(const Context_failures& failures, const std::size_t period)
{
    auto threshold{failures.repeated};

    while (threshold > 0 && fails_at(failures, threshold - 1) == fails_at(failures, threshold - 1 + period))
    {
        --threshold;
    }

    return threshold;
}

/**
 * @brief Returns the shortest accepted marked string with an occurrence whose cut fails at a context, found breadth
 *        first over the right-context product's keys paired with the steps taken since the cut.
 *
 * At the context only a key where it fails steps on, and the search ends at a confirming key past it.
 * @param graph The product.
 * @param reach What the product's keys reach.
 * @param context The context, one that fails.
 * @return The symbols and the keys of the path.
 */
[[nodiscard]] Path<Truncation_key> shortest_truncation(
        const Context_graph& graph, const Context_reach& reach, const std::size_t context)
{
    const auto expand{[&graph, &reach, context]<typename Emit>(const Truncation_key& from, const Emit& emit) {
        const auto& [key, read]{from};

        const auto after_cut{graph.keys[key].cut};

        if (after_cut && read == context && !reach.failing[key])
        {
            return;
        }

        const auto read_after{after_cut ? std::min(read + 1, context + 1) : 0};

        for (const auto& [symbol, to] : graph.successors[key])
        {
            emit(symbol, Truncation_key{.key = to, .read = read_after});
        }
    }};

    const auto completed{
            [&reach, context](const Truncation_key& at) { return at.read == context + 1 && reach.confirmed[at.key]; }};

    return *shortest_path(Truncation_key{.key = 0, .read = 0}, completed, expand);
}

/**
 * @brief Returns the number of symbols a path over the right-context product reads before the cut.
 * @param graph The product.
 * @param keys The path's keys, the start first, at least one key after the cut.
 * @return The cut.
 */
[[nodiscard]] std::size_t cut_on(const Context_graph& graph, const std::vector<Truncation_key>& keys)
{
    const auto after_cut{[&graph](const std::size_t key) { return graph.keys[key].cut; }};

    const auto found{std::ranges::find_if(keys, after_cut, &Truncation_key::key)};

    const auto index{std::ranges::distance(keys.cbegin(), found)};

    return static_cast<std::size_t>(index);
}

/**
 * @brief Returns the shortest truncation at every context below the threshold that fails, ascending by context.
 * @param graph The product.
 * @param reach What the product's keys reach.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @param threshold The threshold.
 * @param origin The origin.
 * @return The truncations.
 */
[[nodiscard]] std::vector<Truncation> truncations_below(
        const Context_graph& graph, const Context_reach& reach, const Context_failures& failures,
        const std::size_t threshold, const std::size_t origin)
{
    std::vector<Truncation> truncations{};

    for (std::size_t context{0}; context < threshold; ++context)
    {
        if (!fails_at(failures, context))
        {
            continue;
        }

        const auto [symbols, keys]{shortest_truncation(graph, reach, context)};

        const auto cut{cut_on(graph, keys)};

        truncations.push_back(Truncation{
                .segmentation = marked_string_of(symbols),
                .occurrence = cut - origin,
                .cut = cut,
                .context = context});
    }

    return truncations;
}

/**
 * @brief Returns the truncation lasso of a failing residue: the shortest truncation at a context of the residue that
 *        reads more keys past the cut than are relevant, cut at its path's first repeated key into a stem up to the
 *        repeat, the cycle it closes read until its length is a multiple of the period, and the rest as the suffix.
 * @param graph The product.
 * @param reach What the product's keys reach.
 * @param context The context, one that fails, at least the number of relevant keys.
 * @param period The least period.
 * @param origin The origin.
 * @return The lasso.
 */
[[nodiscard]] Truncation_lasso truncation_lasso_of(
        const Context_graph& graph, const Context_reach& reach, const std::size_t context, const std::size_t period,
        const std::size_t origin)
{
    const auto [symbols, keys]{shortest_truncation(graph, reach, context)};

    const auto cut{cut_on(graph, keys)};

    // The path past the cut visits more keys than are relevant, all of them relevant, so one repeats.
    std::unordered_map<std::size_t, std::size_t> visited{};

    auto repeat{cut};

    for (;; ++repeat)
    {
        const auto [entry, fresh]{visited.try_emplace(keys[repeat].key, repeat)};

        if (!fresh)
        {
            break;
        }
    }

    const auto begin{symbols.cbegin()};

    const auto cycle_start{static_cast<std::ptrdiff_t>(visited.at(keys[repeat].key))};

    const auto cycle_end{static_cast<std::ptrdiff_t>(repeat)};

    const std::vector<Marked> cycle{begin + cycle_start, begin + cycle_end};

    std::vector<Marked> loop{};

    for (auto copies{period / std::gcd(cycle.size(), period)}; copies > 0; --copies)
    {
        loop.insert(loop.end(), cycle.cbegin(), cycle.cend());
    }

    const std::vector<Marked> stem{begin, begin + cycle_end};

    const std::vector<Marked> suffix{begin + cycle_end, symbols.cend()};

    Lasso lasso{.stem = marked_string_of(stem), .loop = marked_string_of(loop), .suffix = marked_string_of(suffix)};

    return {.lasso = std::move(lasso), .occurrence = cut - origin, .cut = cut, .context = context};
}

/**
 * @brief Returns a truncation lasso for every residue modulo the period at which the contexts from the threshold fail,
 *        at the least context of the residue that is at least the threshold and the number of relevant keys.
 * @param graph The product.
 * @param reach What the product's keys reach.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @param threshold The threshold.
 * @param period The least period.
 * @param origin The origin.
 * @return The lassos, ascending by context, none when the failing contexts are finite.
 */
[[nodiscard]] std::vector<Truncation_lasso> residue_lassos(
        const Context_graph& graph, const Context_reach& reach, const Context_failures& failures,
        const std::size_t threshold, const std::size_t period, const std::size_t origin)
{
    const auto relevant_keys{static_cast<std::size_t>(std::ranges::count(reach.relevant, true))};

    const auto first{std::max(threshold, relevant_keys)};

    std::vector<Truncation_lasso> lassos{};

    for (auto context{first}; context < first + period; ++context)
    {
        if (!fails_at(failures, context))
        {
            continue;
        }

        lassos.push_back(truncation_lasso_of(graph, reach, context, period, origin));
    }

    return lassos;
}

/**
 * @brief Returns the least context that fails at no occurrence, looked for below an end the failing contexts repeat
 *        within, the threshold plus the period.
 * @param failures Which contexts fail, up to the first repeated layer.
 * @param end The threshold plus the period.
 * @return The least context, or std::nullopt when every context fails.
 */
[[nodiscard]] std::optional<std::size_t> least_sufficing(const Context_failures& failures, const std::size_t end)
{
    const auto suffices{[&failures](const std::size_t context) { return !fails_at(failures, context); }};

    const auto contexts{std::views::iota(std::size_t{0}, end)};

    const auto found{std::ranges::find_if(contexts, suffices)};

    if (found == contexts.end())
    {
        return std::nullopt;
    }

    return *found;
}

/**
 * @brief Returns the classes of a verifier's states under Moore refinement: the states are told apart by acceptance and
 *        then by the class each marked symbol leads to or its refusal, until no class splits.
 * @param verifier The verifier, trim.
 * @param alphabet The bytes on its transitions.
 * @return Per state, its class, the classes numbered in the order of their least states.
 */
[[nodiscard]] std::vector<std::size_t> classes_of(const Verifier& verifier, const std::vector<unsigned char>& alphabet)
{
    const auto count{verifier.state_count()};

    const auto numbered{[count]<typename Signature>(const Signature& signature_of) {
        std::map<std::vector<std::size_t>, std::size_t> numbers{};

        std::vector<std::size_t> classes(count, 0);

        for (State_t state{0}; state < count; ++state)
        {
            auto signature{signature_of(state)};

            const auto [entry, inserted]{numbers.try_emplace(std::move(signature), numbers.size())};

            const auto& [interned, number]{*entry};

            classes[state] = number;
        }

        return std::pair{classes, numbers.size()};
    }};

    const auto acceptance{[&verifier](const State_t state) {
        return std::vector{verifier.accepts(state) ? std::size_t{1} : std::size_t{0}};
    }};

    auto [classes, class_count]{numbered(acceptance)};

    // A refused step is told apart from a step into any class.
    const auto refined_signature{[&](const State_t state) {
        std::vector signature{classes[state]};

        for (const auto byte : alphabet)
        {
            for (const auto boundary_after : marks)
            {
                const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})};

                const auto entered{to ? classes[*to] + 1 : 0};

                signature.push_back(entered);
            }
        }

        return signature;
    }};

    for (;;)
    {
        auto [refined, refined_count]{numbered(refined_signature)};

        if (refined_count == class_count)
        {
            return refined;
        }

        classes = std::move(refined);
        class_count = refined_count;
    }
}

/**
 * @brief Returns the byte projection of a verifier's minimal automaton.
 * @param verifier The verifier, trim.
 * @return The projection.
 */
[[nodiscard]] Projection projection_of(const Verifier& verifier)
{
    auto alphabet{alphabet_of(verifier)};

    auto class_of{classes_of(verifier, alphabet)};

    std::vector<State_t> representatives{};

    for (State_t state{0}; state < verifier.state_count(); ++state)
    {
        if (class_of[state] == representatives.size())
        {
            representatives.push_back(state);
        }
    }

    const auto steps_from{[&](const unsigned char byte, const State_t state) {
        std::vector<std::pair<bool, std::size_t>> steps{};

        for (const auto boundary_after : marks)
        {
            if (const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})})
            {
                steps.emplace_back(boundary_after, class_of[*to]);
            }
        }

        return steps;
    }};

    std::vector<std::vector<std::vector<std::pair<bool, std::size_t>>>> steps{};

    for (const auto byte : alphabet)
    {
        auto& row{steps.emplace_back()};

        for (const auto representative : representatives)
        {
            row.push_back(steps_from(byte, representative));
        }
    }

    return {.alphabet = std::move(alphabet),
            .class_of = std::move(class_of),
            .representatives = std::move(representatives),
            .steps = std::move(steps)};
}

/**
 * @brief Returns the states some byte enters under some mark from a state of a projection.
 * @param projection The projection.
 * @return The entered states.
 */
[[nodiscard]] Class_set_t entered_classes(const Projection& projection)
{
    Class_set_t entered(projection.representatives.size(), false);

    for (const auto to : projection.steps | std::views::join | std::views::join | std::views::values)
    {
        entered[to] = true;
    }

    return entered;
}

/**
 * @brief Returns the index of a byte in a projection's alphabet.
 * @param projection The projection.
 * @param byte The byte.
 * @return The index, or std::nullopt when no transition carries the byte.
 */
[[nodiscard]] std::optional<std::size_t> byte_index(const Projection& projection, const unsigned char byte)
{
    const auto found{std::ranges::lower_bound(projection.alphabet, byte)};

    if (found == projection.alphabet.cend() || *found != byte)
    {
        return std::nullopt;
    }

    const auto index{std::ranges::distance(projection.alphabet.cbegin(), found)};

    return static_cast<std::size_t>(index);
}

/**
 * @brief Returns the states a byte string leads to under some marks from a set of states of a projection.
 * @param projection The projection.
 * @param classes The states read from.
 * @param bytes The byte string.
 * @return The states reached.
 */
[[nodiscard]] Class_set_t image_of(const Projection& projection, Class_set_t classes, const std::string_view bytes)
{
    for (const auto byte : bytes)
    {
        const auto index{byte_index(projection, static_cast<unsigned char>(byte))};

        Class_set_t next(classes.size(), false);

        if (!index)
        {
            return next;
        }

        for (std::size_t from{0}; from < classes.size(); ++from)
        {
            if (!classes[from])
            {
                continue;
            }

            for (const auto to : projection.steps[*index][from] | std::views::values)
            {
                next[to] = true;
            }
        }

        classes = std::move(next);
    }

    return classes;
}

/**
 * @brief Returns the states of a projection from which a byte string can be read under some marks.
 * @param projection The projection.
 * @param bytes The byte string.
 * @return The states it can be read from.
 */
[[nodiscard]] Class_set_t coimage_of(const Projection& projection, const std::string_view bytes)
{
    Class_set_t classes(projection.representatives.size(), true);

    for (const auto byte : bytes | std::views::reverse)
    {
        const auto index{byte_index(projection, static_cast<unsigned char>(byte))};

        Class_set_t previous(classes.size(), false);

        if (!index)
        {
            return previous;
        }

        const auto into_classes{[&classes](const std::size_t to) { return classes[to]; }};

        for (std::size_t from{0}; from < classes.size(); ++from)
        {
            previous[from] = std::ranges::any_of(projection.steps[*index][from] | std::views::values, into_classes);
        }

        classes = std::move(previous);
    }

    return classes;
}

/**
 * @brief Returns the composition of two relations, the pairs joined by a step of the first and then one of the second.
 * @param first The first relation.
 * @param second The second relation.
 * @param count The number of states.
 * @return The composition.
 */
[[nodiscard]] Relation_t composed(const Relation_t& first, const Relation_t& second, const std::size_t count)
{
    Relation_t composition(first.size(), false);

    for (std::size_t pair{0}; pair < first.size(); ++pair)
    {
        if (!first[pair])
        {
            continue;
        }

        const auto from{pair / count};

        const auto through{pair % count};

        for (std::size_t to{0}; to < count; ++to)
        {
            if (second[through * count + to])
            {
                composition[from * count + to] = true;
            }
        }
    }

    return composition;
}

/**
 * @brief Returns whether a relation joins no pair of states.
 * @param relation The relation.
 * @return True when it is empty.
 */
[[nodiscard]] bool is_zero(const Relation_t& relation)
{
    return std::ranges::none_of(relation, std::identity{});
}

/**
 * @brief Returns the identity relation on the states of a projection, the relation of the empty string.
 * @param count The number of states.
 * @return The identity.
 */
[[nodiscard]] Relation_t identity_of(const std::size_t count)
{
    Relation_t identity(count * count, false);

    for (std::size_t state{0}; state < count; ++state)
    {
        identity[state * count + state] = true;
    }

    return identity;
}

/**
 * @brief Returns the relation of a byte string followed by one more byte: the pairs of states it joins under some
 *        marks.
 * @param projection The projection.
 * @param relation The relation of the byte string.
 * @param index The byte, as its index in the projection's alphabet.
 * @return The relation of the longer string.
 */
[[nodiscard]] Relation_t relation_after(
        const Projection& projection, const Relation_t& relation, const std::size_t index)
{
    const auto count{projection.representatives.size()};

    Relation_t extended(relation.size(), false);

    for (std::size_t pair{0}; pair < relation.size(); ++pair)
    {
        if (!relation[pair])
        {
            continue;
        }

        const auto from{pair / count};

        const auto through{pair % count};

        for (const auto to : projection.steps[index][through] | std::views::values)
        {
            extended[from * count + to] = true;
        }
    }

    return extended;
}

/**
 * @brief Returns the relations of the byte strings over a projection's alphabet, the empty string's identity first and
 *        then the nonzero relations of the nonempty strings, each with how the search reached it.
 *
 * Found breadth first from the empty string, each string extended by the bytes in ascending order and a relation kept
 * where it is first reached, so the relations follow their shortest strings, the least in byte order among the
 * shortest, in that order, and the parents spell those strings.
 * @param projection The projection.
 * @return The relations and their parents, in the order reached.
 */
[[nodiscard]] std::vector<std::pair<Relation_t, Parent>> relations_of(const Projection& projection)
{
    std::vector<std::pair<Relation_t, Parent>> relations{{identity_of(projection.representatives.size()), Parent{}}};

    std::set<Relation_t> seen{};

    for (std::size_t next{0}; next < relations.size(); ++next)
    {
        // A copy, since the relations grow below.
        const auto [relation, parent]{relations[next]};

        for (std::size_t index{0}; index < projection.alphabet.size(); ++index)
        {
            auto extended{relation_after(projection, relation, index)};

            if (is_zero(extended))
            {
                continue;
            }

            const auto [entry, inserted]{seen.insert(extended)};

            if (!inserted)
            {
                continue;
            }

            const Marked symbol{.byte = projection.alphabet[index], .boundary_after = false};

            relations.emplace_back(std::move(extended), Parent{.from = next, .symbol = symbol});
        }
    }

    return relations;
}

/**
 * @brief Returns the number of states a relation joins to themselves.
 * @param relation The relation.
 * @param count The number of states.
 * @return The number of fixed points.
 */
[[nodiscard]] std::size_t fixed_points(const Relation_t& relation, const std::size_t count)
{
    std::size_t fixed{0};

    for (std::size_t state{0}; state < count; ++state)
    {
        if (relation[state * count + state])
        {
            ++fixed;
        }
    }

    return fixed;
}

} // namespace

std::optional<Miscovering> miscovering(
        const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"miscovering: the window is empty or its origin lies outside it"};
    }

    const auto last{window.size() - 1};

    // The token holding the occurrence's last byte begins at the origin: a mark before byte origin, none after it.
    const auto covered_at_origin{
            [last, origin](const std::vector<bool>& marks) { return covers(marks, last, origin); }};

    auto segmentation{first_miss(verifier, window, covered_at_origin)};

    if (!segmentation)
    {
        return std::nullopt;
    }

    const auto occurrence{missed_occurrence(*segmentation, window, last, origin)};

    return Miscovering{.segmentation = std::move(*segmentation), .occurrence = occurrence};
}

Pinned_cut pinned_cut(const Verifier& verifier, const std::string_view window, const std::size_t byte)
{
    if (window.empty() || byte >= window.size())
    {
        throw std::invalid_argument{"pinned_cut: the window is empty or its byte lies outside it"};
    }

    // The first search refuses every occurrence, so it finds the shortest accepted marked string holding one.
    const auto anywhere{[](const std::vector<bool>&) { return false; }};

    auto first{first_miss(verifier, window, anywhere)};

    if (!first)
    {
        return Absent_window{};
    }

    const auto at{std::string_view{first->bytes}.find(window)};

    const auto start{covering_boundary(*first, at + byte)};

    if (start < at)
    {
        return Miscovering{.segmentation = std::move(*first), .occurrence = at};
    }

    const auto origin{start - at};

    const auto covered_at_origin{
            [byte, origin](const std::vector<bool>& marks) { return covers(marks, byte, origin); }};

    auto refutation{first_miss(verifier, window, covered_at_origin)};

    if (!refutation)
    {
        return origin;
    }

    const auto occurrence{missed_occurrence(*refutation, window, byte, origin)};

    return Miscovering{.segmentation = std::move(*refutation), .occurrence = occurrence};
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
        return Divergence{.witness = std::move(*witness), .half = Separation_half::boundary};
    }

    if (auto witness{domain_divergence(a, b, either)})
    {
        return Divergence{.witness = std::move(*witness), .half = Separation_half::domain};
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

Right_context right_context(const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"right_context: the window is empty or its origin lies outside it"};
    }

    if (const auto refutation{miscovering(verifier, window, origin)})
    {
        throw std::invalid_argument{std::format(
                "right_context: the verifier does not certify ({}, {}), the text {} covers an occurrence from "
                "elsewhere, so no cut is certified",
                window, origin, refutation->segmentation.bytes)};
    }

    const auto graph{context_graph_of(verifier, window, origin)};

    const auto reach{context_reach_of(verifier, graph, window.size() - origin)};

    const auto failures{context_failures_of(graph, reach)};

    const auto period{least_period(failures)};

    const auto threshold{threshold_of(failures, period)};

    auto failing{truncations_below(graph, reach, failures, threshold, origin)};

    auto residues{residue_lassos(graph, reach, failures, threshold, period, origin)};

    const auto least{least_sufficing(failures, threshold + period)};

    if (residues.empty())
    {
        return Finite_context{.context = threshold, .least = *least, .failing = std::move(failing)};
    }

    return Unbounded_context{
            .threshold = threshold,
            .period = period,
            .least = least,
            .failing = std::move(failing),
            .residues = std::move(residues)};
}

Fanout fanout(const Verifier& verifier, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"fanout: the window is empty or its origin lies outside it"};
    }

    const auto projection{projection_of(verifier)};

    const auto head{window.substr(0, origin)};

    const auto tail{window.substr(origin)};

    const Class_set_t every_class(projection.representatives.size(), true);

    // At origin zero the cut follows the byte before the occurrence, so its configuration is one a byte enters.
    const auto before{origin == 0 ? entered_classes(projection) : image_of(projection, every_class, head)};

    const auto after{coimage_of(projection, tail)};

    std::vector<State_t> configurations{};

    for (std::size_t index{0}; index < projection.representatives.size(); ++index)
    {
        if (before[index] && after[index])
        {
            configurations.push_back(projection.representatives[index]);
        }
    }

    const auto rank{configurations.size()};

    return Fanout{.rank = rank, .configurations = std::move(configurations)};
}

Smallest_fanout smallest_fanout(const Verifier& verifier)
{
    const auto projection{projection_of(verifier)};

    const auto count{projection.representatives.size()};

    const auto relations{relations_of(projection)};

    const auto is_idempotent{[&relations, count](const std::size_t number) {
        const auto& [relation, parent]{relations[number]};

        return composed(relation, relation, count) == relation;
    }};

    const auto degree_of{[&relations, count](const std::size_t number) {
        const auto& [relation, parent]{relations[number]};

        return fixed_points(relation, count);
    }};

    // Past the empty string's identity, the first idempotent of the least degree has the shortest string, the least in
    // byte order among the shortest.
    auto idempotents{std::views::iota(std::size_t{1}, relations.size()) | std::views::filter(is_idempotent)};

    const auto least{std::ranges::min_element(idempotents, {}, degree_of)};

    if (least == idempotents.end())
    {
        throw std::invalid_argument{"smallest_fanout: the domain is finite, so no window recurs"};
    }

    const auto& [idempotent, reached]{relations[*least]};

    const auto degree{fixed_points(idempotent, count)};

    std::vector<Marked> symbols{};

    for (auto at{*least}; at != 0;)
    {
        const auto& [relation, parent]{relations[at]};

        symbols.push_back(parent.symbol);

        at = parent.from;
    }

    std::ranges::reverse(symbols);

    const auto [word, unmarked]{marked_string_of(symbols)};

    const auto is_fixed{[&idempotent, count](const std::size_t state) { return idempotent[state * count + state]; }};

    const auto states{std::views::iota(std::size_t{0}, count)};

    const auto fixed{*std::ranges::find_if(states, is_fixed)};

    const auto in_fixed{[&projection, fixed](const State_t state) { return projection.class_of[state] == fixed; }};

    const auto accepting{[&verifier](const State_t state) { return verifier.accepts(state); }};

    const auto expand_states{[&verifier, &projection]<typename Emit>(const State_t state, const Emit& emit) {
        expand_steps(verifier, projection.alphabet, state, emit);
    }};

    const auto stem{shortest_path(verifier.start(), in_fixed, expand_states)};

    const auto entry{stem->keys.back()};

    const auto suffix{shortest_path(entry, accepting, expand_states)};

    auto stem_string{marked_string_of(stem->symbols)};

    auto loop_string{marking_between(verifier, entry, word, in_fixed)};

    auto suffix_string{marked_string_of(suffix->symbols)};

    auto window{word + word};

    Lasso lasso{.stem = std::move(stem_string), .loop = std::move(loop_string), .suffix = std::move(suffix_string)};

    return Smallest_fanout{
            .degree = degree,
            .window = std::move(window),
            .origin = word.size(),
            .lasso = std::move(lasso)};
}

} // namespace munch::verifier

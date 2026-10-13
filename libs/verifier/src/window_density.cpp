#include "munch/verifier/window_density.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "munch/verifier/rule_ceilings.hpp"
#include "munch/verifier/token_automaton.hpp"

namespace munch::verifier
{
namespace
{
/**
 * @brief The rank of every merge by its two parts' spellings.
 */
using Ranks_t = std::map<std::pair<std::string, std::string>, std::size_t>;

/**
 * @brief The density bound of windows that are dense, one more than the longest string avoiding them.
 */
struct Avoiding_bound
{
    /**
     * @brief The density bound.
     */
    std::size_t bound{};
};

/**
 * @brief The label of a cycle of avoiding strings, every power of which avoids the windows.
 */
struct Avoiding_loop
{
    /**
     * @brief The label, nonempty.
     */
    std::string loop{};
};

/**
 * @brief Marks an enumeration that ran out of its budget.
 */
struct Spent_budget
{
};

/**
 * @brief What the graph of avoiding strings decides: the density bound, a loop, or a budget spent first.
 */
using Avoidance_t = std::variant<Avoiding_bound, Avoiding_loop, Spent_budget>;

/**
 * @brief Returns a spelling as a refusal quotes it: a printable ASCII byte as itself, the quote and the backslash
 *        escaped, and every other byte as `\xHH`, so that a NUL in a spelling cannot cut the message short.
 * @param spelling The spelling.
 * @return The spelling, quoted.
 */
[[nodiscard]] std::string quoted(const std::string_view spelling)
{
    constexpr unsigned char first_printable{0x20};
    constexpr unsigned char last_printable{0x7E};

    std::string out{'"'};

    for (const auto byte : spelling)
    {
        const auto value{static_cast<unsigned char>(byte)};

        const auto printable{value >= first_printable && value <= last_printable};

        if (byte == '"' || byte == '\\')
        {
            out += std::format(R"(\{})", byte);
        }
        else if (printable)
        {
            out += byte;
        }
        else
        {
            out += std::format(R"(\x{:02X})", value);
        }
    }

    out += '"';

    return out;
}

/**
 * @brief Checks a merge table and returns the bytes its merges name, each part looked up among the bytes and the
 *        earlier products.
 * @param merges The merge table in rank order.
 * @return The bytes the merges' parts spell, ascending.
 * @throws std::invalid_argument If a merge names a part the vocabulary so far does not carry, or a product it already
 *         carries.
 */
[[nodiscard]] std::vector<unsigned char> named_bytes(const std::span<const Merge> merges)
{
    constexpr std::size_t byte_count{256};

    std::unordered_set<std::string> vocabulary{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        vocabulary.emplace(1, static_cast<char>(byte));
    }

    std::vector<bool> named(byte_count, false);

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        const auto left_known{vocabulary.contains(left)};
        const auto right_known{vocabulary.contains(right)};

        if (!left_known || !right_known)
        {
            const auto& missing{left_known ? right : left};

            throw std::invalid_argument{std::format(
                    "window_density: the merge ({}, {}) at rank {} names {}, which neither the alphabet nor an earlier "
                    "merge carries",
                    quoted(left), quoted(right), rank, quoted(missing))};
        }

        const auto spelling{left + right};

        const auto [product, inserted]{vocabulary.insert(spelling)};

        if (!inserted)
        {
            throw std::invalid_argument{std::format(
                    "window_density: the merge ({}, {}) at rank {} recreates the token {}, which the alphabet or an "
                    "earlier merge already carries",
                    quoted(left), quoted(right), rank, quoted(spelling))};
        }

        for (const auto byte : spelling)
        {
            named[static_cast<unsigned char>(byte)] = true;
        }
    }

    std::vector<unsigned char> bytes{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        if (named[byte])
        {
            bytes.push_back(static_cast<unsigned char>(byte));
        }
    }

    return bytes;
}

/**
 * @brief Returns the rank of every merge by its two parts, each pair of parts being one merge's, since its product is
 *        spelled once.
 * @param merges The admissible merge table in rank order.
 * @return The ranks, by the pair of parts.
 */
[[nodiscard]] Ranks_t ranks_of(const std::span<const Merge> merges)
{
    Ranks_t ranks{};

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        ranks.emplace(std::pair{left, right}, rank);
    }

    return ranks;
}

/**
 * @brief Encodes a text: at every step the lowest-ranked merge present anywhere is applied at its leftmost occurrence,
 *        until none applies.
 * @param ranks The rank of every merge by its parts.
 * @param text The text.
 * @return The tokens' spellings.
 */
[[nodiscard]] std::vector<std::string> encoded(const Ranks_t& ranks, const std::string_view text)
{
    std::vector<std::string> tokens{};

    for (const auto byte : text)
    {
        tokens.emplace_back(1, byte);
    }

    for (;;)
    {
        std::optional<std::pair<std::size_t, std::size_t>> best{};

        for (std::size_t index{0}; index + 1 < tokens.size(); ++index)
        {
            const auto found{ranks.find({tokens[index], tokens[index + 1]})};

            if (found == ranks.cend())
            {
                continue;
            }

            const auto& [parts, rank]{*found};

            if (!best || rank < best->second)
            {
                best = std::pair{index, rank};
            }
        }

        if (!best)
        {
            return tokens;
        }

        const auto [index, rank]{*best};

        const auto right{tokens.begin() + static_cast<std::ptrdiff_t>(index) + 1};

        tokens[index] += *right;
        tokens.erase(right);
    }
}

/**
 * @brief The graph of the strings that avoid every certified window of width at most R + 1, built over the bytes a
 *        table names, with each window decided once.
 */
class Avoiding_graph
{
public:
    /**
     * @brief Takes the token automaton, the merges' ranks, the bytes and the origin radius.
     * @param automaton The table's token automaton.
     * @param ranks The rank of every merge by its parts.
     * @param bytes The bytes the table names, ascending.
     * @param radius The finite origin radius R.
     */
    Avoiding_graph(
            const Token_automaton& automaton, const Ranks_t& ranks, std::vector<unsigned char> bytes,
            std::size_t radius);

    /**
     * @brief Decides whether the avoiding strings are finite: builds them layer by layer up to R bytes, then searches
     *        the graph of the R-byte ones depth first.
     * @param budget The most avoiding strings enumerated.
     * @return The density bound when they are finite, the label of the first cycle closed otherwise, or the budget
     *         spent first.
     */
    [[nodiscard]] Avoidance_t decide(std::size_t budget);

private:
    /**
     * @brief A step of the edge between two R-byte strings: the target's index and the byte read.
     */
    struct Edge
    {
        /**
         * @brief The target's index among the states.
         */
        std::size_t target{};

        /**
         * @brief The byte read.
         */
        unsigned char byte{};
    };

    /**
     * @brief A state on the depth-first path: its index, its next edge, the longest path found from it so far and the
     *        byte that entered it.
     */
    struct Frame
    {
        /**
         * @brief The state's index.
         */
        std::size_t state{};

        /**
         * @brief The index of the next edge to follow.
         */
        std::size_t next{};

        /**
         * @brief The longest path from the state among its edges followed so far.
         */
        std::size_t best{};

        /**
         * @brief The byte of the edge that entered the state, zero for the path's first.
         */
        unsigned char entered{};
    };

    /**
     * @brief A state's colour in the depth-first search.
     */
    enum class Colour : std::uint8_t
    {
        /**
         * @brief Not reached yet.
         */
        unseen,

        /**
         * @brief On the current path.
         */
        open,

        /**
         * @brief Finished, its longest path known.
         */
        closed
    };

    /**
     * @brief Returns whether a string avoiding the windows stays avoiding with its last byte: no suffix of width at
     *        most R + 1 is certified.
     * @param text The string.
     * @return True when no such suffix is certified.
     */
    [[nodiscard]] bool avoids(std::string_view text);

    /**
     * @brief Returns whether a window is certified, at the start its last byte has in its own encoding, deciding it by
     *        miscovering() the first time it is asked.
     * @param window The window, nonempty.
     * @return True when the certificate holds.
     */
    [[nodiscard]] bool is_certified(std::string_view window);

    /**
     * @brief Returns the edges of every R-byte avoiding string, each byte in ascending order.
     * @param states The R-byte avoiding strings, ascending.
     * @return The edges, by state.
     */
    [[nodiscard]] std::vector<std::vector<Edge>> edges_of(const std::vector<std::string>& states);

    /**
     * @brief Searches the graph depth first from each state in ascending order and returns the longest path, or the
     *        first cycle closed.
     * @param edges The edges, by state.
     * @return The longest path's length in edges, or the cycle's label.
     */
    [[nodiscard]] std::variant<std::size_t, Avoiding_loop> longest_path(
            const std::vector<std::vector<Edge>>& edges) const;

    /**
     * @brief The table's token automaton.
     */
    const Token_automaton& automaton_;

    /**
     * @brief The rank of every merge by its parts.
     */
    const Ranks_t& ranks_;

    /**
     * @brief The bytes the table names, ascending.
     */
    std::vector<unsigned char> bytes_{};

    /**
     * @brief The origin radius R.
     */
    std::size_t radius_{};

    /**
     * @brief Each window decided, with whether it is certified.
     */
    std::unordered_map<std::string, bool> certified_{};
};

Avoiding_graph::Avoiding_graph(
        const Token_automaton& automaton, const Ranks_t& ranks, std::vector<unsigned char> bytes,
        const std::size_t radius)
    : automaton_{automaton}, ranks_{ranks}, bytes_{std::move(bytes)}, radius_{radius}
{}

Avoidance_t Avoiding_graph::decide(const std::size_t budget)
{
    std::vector<std::string> layer{std::string{}};

    std::size_t enumerated{0};

    for (std::size_t length{1}; length <= radius_; ++length)
    {
        std::vector<std::string> longer{};

        for (const auto& text : layer)
        {
            for (const auto byte : bytes_)
            {
                auto extended{text + static_cast<char>(byte)};

                if (!avoids(extended))
                {
                    continue;
                }

                ++enumerated;

                if (enumerated > budget)
                {
                    return Spent_budget{};
                }

                longer.push_back(std::move(extended));
            }
        }

        layer = std::move(longer);
    }

    const auto edges{edges_of(layer)};

    const auto path{longest_path(edges)};

    if (std::holds_alternative<Avoiding_loop>(path))
    {
        return std::get<Avoiding_loop>(path);
    }

    const auto longest{radius_ + std::get<std::size_t>(path)};

    return Avoiding_bound{.bound = longest + 1};
}

bool Avoiding_graph::avoids(const std::string_view text)
{
    const auto widest{std::min(radius_ + 1, text.size())};

    for (std::size_t width{1}; width <= widest; ++width)
    {
        const auto suffix{text.substr(text.size() - width)};

        if (is_certified(suffix))
        {
            return false;
        }
    }

    return true;
}

bool Avoiding_graph::is_certified(const std::string_view window)
{
    const std::string key{window};

    if (const auto found{certified_.find(key)}; found != certified_.cend())
    {
        return found->second;
    }

    const auto tokens{encoded(ranks_, window)};

    const auto last_token{tokens.back().size()};

    const auto origin{window.size() - last_token};

    const auto refuted{miscovering(automaton_, window, origin).has_value()};

    certified_.emplace(key, !refuted);

    return !refuted;
}

std::vector<std::vector<Avoiding_graph::Edge>> Avoiding_graph::edges_of(const std::vector<std::string>& states)
{
    std::unordered_map<std::string, std::size_t> index{};

    for (std::size_t state{0}; state < states.size(); ++state)
    {
        index.emplace(states[state], state);
    }

    std::vector<std::vector<Edge>> edges(states.size());

    for (std::size_t state{0}; state < states.size(); ++state)
    {
        for (const auto byte : bytes_)
        {
            const auto extended{states[state] + static_cast<char>(byte)};

            if (!avoids(extended))
            {
                continue;
            }

            const auto shifted{extended.substr(1)};

            const auto target{index.at(shifted)};

            edges[state].push_back({.target = target, .byte = byte});
        }
    }

    return edges;
}

std::variant<std::size_t, Avoiding_loop> Avoiding_graph::longest_path(const std::vector<std::vector<Edge>>& edges) const
{
    std::vector colour(edges.size(), Colour::unseen);

    std::vector depth(edges.size(), std::size_t{0});

    std::size_t longest{0};

    for (std::size_t start{0}; start < edges.size(); ++start)
    {
        if (colour[start] != Colour::unseen)
        {
            continue;
        }

        std::vector<Frame> path{{.state = start, .next = 0, .best = 0, .entered = 0}};

        colour[start] = Colour::open;

        while (!path.empty())
        {
            auto& [state, next, best, entered]{path.back()};

            if (next == edges[state].size())
            {
                colour[state] = Colour::closed;
                depth[state] = best;
                longest = std::max(longest, best);

                const auto finished{best};

                path.pop_back();

                if (!path.empty())
                {
                    path.back().best = std::max(path.back().best, finished + 1);
                }

                continue;
            }

            const auto [target, byte]{edges[state][next]};

            ++next;

            if (colour[target] == Colour::closed)
            {
                best = std::max(best, depth[target] + 1);

                continue;
            }

            if (colour[target] == Colour::unseen)
            {
                colour[target] = Colour::open;

                path.push_back({.state = target, .next = 0, .best = 0, .entered = byte});

                continue;
            }

            // The target is on the path: the label runs from the frame after it to here, then the byte read.
            const auto on_path{[target](const Frame& frame) { return frame.state == target; }};

            const auto from{std::ranges::find_if(path, on_path)};

            std::string loop{};

            for (auto frame{from + 1}; frame != path.end(); ++frame)
            {
                loop.push_back(static_cast<char>(frame->entered));
            }

            loop.push_back(static_cast<char>(byte));

            return Avoiding_loop{.loop = std::move(loop)};
        }
    }

    return longest;
}

/**
 * @brief Returns the first pumping double of a table: a canonical token t whose pair (t, t) encodes as itself and a
 *        byte b whose encoding of ttb does not begin with t, the tokens in number order and the bytes ascending.
 * @param merges The admissible merge table in rank order.
 * @param ranks The rank of every merge by its parts.
 * @param bytes The bytes the table names, ascending.
 * @return The first pumping double, std::nullopt when no token has one.
 */
[[nodiscard]] std::optional<Pumping_double> first_pumping_double(
        const std::span<const Merge> merges, const Ranks_t& ranks, const std::vector<unsigned char>& bytes)
{
    std::vector<std::string> tokens{};

    for (const auto byte : bytes)
    {
        tokens.emplace_back(1, static_cast<char>(byte));
    }

    for (const auto& [left, right] : merges)
    {
        tokens.push_back(left + right);
    }

    for (const auto& token : tokens)
    {
        const auto alone{encoded(ranks, token)};

        const auto doubled{encoded(ranks, token + token)};

        const auto canonical{alone.size() == 1};

        const auto adjacent{doubled.size() == 2 && doubled.front() == token};

        if (!canonical || !adjacent)
        {
            continue;
        }

        for (const auto byte : bytes)
        {
            const auto text{token + token + static_cast<char>(byte)};

            const auto continued{encoded(ranks, text)};

            if (continued.front() != token)
            {
                const std::string symbol{static_cast<char>(byte)};

                return Pumping_double{.token = token, .symbol = symbol};
            }
        }
    }

    return std::nullopt;
}

} // namespace

Window_density window_density(const std::span<const Merge> merges, const std::size_t budget)
{
    const auto bytes{named_bytes(merges)};

    const auto ripple{ripple_radius(merges)};

    if (std::holds_alternative<Effective_self_pair>(ripple))
    {
        return std::get<Effective_self_pair>(ripple);
    }

    const auto radius{std::get<Finite_ripple>(ripple).radius};

    const auto start_radius{rule_ceilings(merges).start_radius};

    const Pinned_density pinned{.radius = radius, .start_radius = start_radius, .width = radius + start_radius + 1};

    // Every window of width R + 1 is certified when Q_start = 0, the report's Corollary 'Zero start radius'.
    if (start_radius == 0)
    {
        return Dense_windows{.pinned = pinned, .bound = radius + 1};
    }

    const auto automaton{token_automaton(merges)};

    const auto ranks{ranks_of(merges)};

    Avoiding_graph graph{automaton, ranks, bytes, radius};

    const auto avoidance{graph.decide(budget)};

    if (std::holds_alternative<Avoiding_bound>(avoidance))
    {
        const auto [bound]{std::get<Avoiding_bound>(avoidance)};

        return Dense_windows{.pinned = pinned, .bound = bound};
    }

    if (std::holds_alternative<Spent_budget>(avoidance))
    {
        return Density_budget{.pinned = pinned};
    }

    auto loop{std::get<Avoiding_loop>(avoidance).loop};

    auto pumping_double{first_pumping_double(merges, ranks, bytes)};

    return Sparse_windows{.pinned = pinned, .loop = std::move(loop), .pumping_double = std::move(pumping_double)};
}

} // namespace munch::verifier

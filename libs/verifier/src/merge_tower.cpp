#include "munch/verifier/merge_tower.hpp"

#include <algorithm>
#include <compare>
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
#include <utility>
#include <vector>

namespace munch::verifier
{
namespace
{
/**
 * @brief A token of the table's vocabulary: a byte by its value, or a product by its merge's rank past the bytes.
 */
using Token_t = std::size_t;

/**
 * @brief The number of bytes, which the vocabulary carries before the products.
 */
constexpr Token_t byte_count{256};

/**
 * @brief What the top stage has emitted: nothing, a token with a boundary claimed after it, or a token claimed to be
 *        the last.
 */
enum class Tail : std::uint8_t
{
    /**
     * @brief Nothing has been emitted.
     */
    empty,

    /**
     * @brief A token with a boundary claimed after it.
     */
    open,

    /**
     * @brief A token claimed to be the last.
     */
    closed
};

/**
 * @brief One merge stage of a configuration: the symbol it holds, if any, and the boundary claimed after it.
 */
struct Stage
{
    /**
     * @brief Ordered member by member, so configurations key the map of interned ones.
     */
    auto operator<=>(const Stage&) const = default;

    /**
     * @brief The held symbol, always the merge's left part.
     */
    std::optional<Token_t> held{};

    /**
     * @brief Whether a boundary is claimed after the held symbol.
     */
    bool claim{};
};

/**
 * @brief A configuration of the tower: what the top stage has emitted, and each stage's held symbol with its claim.
 */
struct Configuration
{
    /**
     * @brief Ordered member by member, so configurations key the map of interned ones.
     */
    auto operator<=>(const Configuration&) const = default;

    /**
     * @brief What the top stage has emitted.
     */
    Tail tail{};

    /**
     * @brief The stages, one per merge in rank order.
     */
    std::vector<Stage> stages{};
};

/**
 * @brief The merge table resolved to tokens: each merge's parts by rank, the product of rank r being the token
 *        byte_count + r.
 */
struct Table
{
    /**
     * @brief The left part of each merge, by rank.
     */
    std::vector<Token_t> lefts{};

    /**
     * @brief The right part of each merge, by rank.
     */
    std::vector<Token_t> rights{};
};

/**
 * @brief Returns the token of a merge's product.
 * @param rank The merge's rank.
 * @return The product's token.
 */
[[nodiscard]] constexpr Token_t product_of(const std::size_t rank) noexcept
{
    return byte_count + rank;
}

/**
 * @brief Returns a token's spelling as a refusal quotes it: a printable ASCII byte as itself, the quote and the
 *        backslash escaped, and every other byte as `\xHH`, so that a NUL in a spelling cannot cut the message short.
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
 * @brief Resolves a merge table to tokens, each part looked up among the bytes and the earlier products.
 * @param merges The merge table in rank order.
 * @return The resolved table.
 * @throws std::invalid_argument If a merge names a part the vocabulary so far does not carry, or a product it already
 *         carries.
 */
[[nodiscard]] Table table_of(const std::span<const Merge> merges)
{
    std::unordered_map<std::string, Token_t> tokens{};

    for (Token_t byte{0}; byte < byte_count; ++byte)
    {
        tokens.emplace(std::string{static_cast<char>(byte)}, byte);
    }

    Table table{};

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        const auto left_found{tokens.find(left)};
        const auto right_found{tokens.find(right)};

        if (left_found == tokens.cend() || right_found == tokens.cend())
        {
            const auto& missing{left_found == tokens.cend() ? left : right};

            throw std::invalid_argument{std::format(
                    "merge_tower: the merge ({}, {}) at rank {} names {}, which neither the alphabet nor an earlier "
                    "merge carries",
                    quoted(left), quoted(right), rank, quoted(missing))};
        }

        const auto& [left_spelling, left_token]{*left_found};
        const auto& [right_spelling, right_token]{*right_found};

        table.lefts.push_back(left_token);
        table.rights.push_back(right_token);

        const auto spelling{left + right};

        const auto [product, inserted]{tokens.try_emplace(spelling, product_of(rank))};

        if (!inserted)
        {
            throw std::invalid_argument{std::format(
                    "merge_tower: the merge ({}, {}) at rank {} recreates the token {}, which the alphabet or an "
                    "earlier merge already carries",
                    quoted(left), quoted(right), rank, quoted(spelling))};
        }
    }

    return table;
}

/**
 * @brief The tower under exploration: the merge table resolved to tokens, the configuration each state reads as, the
 *        transitions and accepting states found so far, and the interned configurations with their states.
 */
class Exploration
{
public:
    /**
     * @brief Resolves the merge table to tokens, nothing explored yet.
     * @param merges The merge table in rank order.
     * @throws std::invalid_argument If a merge names a part the vocabulary so far does not carry, or a product it
     *         already carries.
     */
    explicit Exploration(std::span<const Merge> merges);

    /**
     * @brief Explores every state reachable from the configuration of empty stages and returns the verifier they make.
     * @return The verifier, its start the configuration of empty stages.
     */
    [[nodiscard]] Verifier run() &&;

private:
    /**
     * @brief Returns the state of a configuration, interning it as a new state when it is first found.
     * @param configuration The configuration.
     * @return The configuration's state.
     */
    [[nodiscard]] Verifier::State_t intern(Configuration configuration);

    /**
     * @brief Returns whether a configuration accepts: passing every held symbol upward, from the lowest stage, is not
     *        refused, and the top stage's last token carries no claimed boundary.
     * @param configuration The configuration, flushed in a copy.
     * @return True when the input may end in the configuration.
     */
    [[nodiscard]] bool is_accepting(Configuration configuration) const;

    /**
     * @brief Takes the symbol a stage holds, with its claim, and delivers it to the stage above.
     * @param configuration The configuration, changed in place.
     * @param level The holding stage.
     * @return True when the delivery is not refused.
     */
    [[nodiscard]] bool pass_upward(Configuration& configuration, std::size_t level) const;

    /**
     * @brief Delivers a token with the boundary claimed after it to a stage, cascading upward.
     *
     * A stage holding its merge's left part joins an arriving right part into the product, delivered to the stage above
     * with the arriving claim, and refuses the delivery when a boundary is claimed after the held part; a held symbol
     * the arrival does not complete is passed upward first. A stage holds an arrival equal to its left part, and any
     * other arrival passes to the stage above. Above the top stage the token is emitted, refused after a token claimed
     * to be the last.
     * @param configuration The configuration, changed in place.
     * @param level The stage delivered to; the stage count names the top stage's emission.
     * @param token The token delivered.
     * @param claim Whether a boundary is claimed after the token.
     * @return True when the delivery is not refused.
     */
    [[nodiscard]] bool deliver(Configuration& configuration, std::size_t level, Token_t token, bool claim) const;

    /**
     * @brief Reads every marked symbol over the bytes from one state, interning the configurations reached.
     * @param state The state read from.
     */
    void expand(Verifier::State_t state);

    /**
     * @brief Reads one marked symbol into a configuration: the byte with its boundary bit is delivered to the lowest
     *        stage and the result normalized.
     * @param from The configuration read from.
     * @param symbol The marked symbol.
     * @return The configuration after the symbol, or std::nullopt when the step is refused.
     */
    [[nodiscard]] std::optional<Configuration> successor(const Configuration& from, Marked symbol) const;

    /**
     * @brief Passes upward every held symbol whose merge no possible next arrival completes, from the lowest such stage
     *        and again until no stage changes.
     * @param configuration The configuration, changed in place.
     * @return True when no delivery is refused.
     */
    [[nodiscard]] bool normalize(Configuration& configuration) const;

    /**
     * @brief Returns the tokens that can next arrive at a stage from the nearest holding stage below it: that stage's
     *        held symbol alone or merged, extended through the empty stages between, each of which may hold a token
     *        equal to its left part and later pass it alone or merged.
     * @param low The nearest holding stage below.
     * @param level The stage the arrivals are for.
     * @return The possible arrivals, without repeats.
     */
    [[nodiscard]] std::vector<Token_t> arrivals(std::size_t low, std::size_t level) const;

    /**
     * @brief The resolved merge table.
     */
    Table table_{};

    /**
     * @brief The configuration each state reads as, by state.
     */
    std::vector<Configuration> sources_{};

    /**
     * @brief The transitions found so far.
     */
    Verifier::Transitions_t transitions_{};

    /**
     * @brief The accepting states found so far.
     */
    Verifier::Accept_states_t accept_states_{};

    /**
     * @brief The interned configurations with their states.
     */
    std::map<Configuration, Verifier::State_t> interned_{};
};

Exploration::Exploration(const std::span<const Merge> merges) : table_{table_of(merges)}
{}

Verifier Exploration::run() &&
{
    const auto start{intern({.tail = Tail::empty, .stages = std::vector<Stage>(table_.lefts.size())})};

    for (auto state{start}; state < sources_.size(); ++state)
    {
        expand(state);
    }

    return Verifier{start, std::move(transitions_), std::move(accept_states_)};
}

Verifier::State_t Exploration::intern(Configuration configuration)
{
    const auto [entry, inserted]{interned_.try_emplace(configuration, sources_.size())};

    const auto& [key, state]{*entry};

    if (!inserted)
    {
        return state;
    }

    if (is_accepting(configuration))
    {
        accept_states_.insert(state);
    }

    sources_.push_back(std::move(configuration));

    return state;
}

bool Exploration::is_accepting(Configuration configuration) const
{
    for (std::size_t level{0}; level < configuration.stages.size(); ++level)
    {
        if (!configuration.stages[level].held)
        {
            continue;
        }

        const auto passed{pass_upward(configuration, level)};

        if (!passed)
        {
            return false;
        }
    }

    return configuration.tail != Tail::open;
}

bool Exploration::pass_upward(Configuration& configuration, const std::size_t level) const
{
    const auto [held, held_claim]{std::exchange(configuration.stages[level], Stage{})};

    return deliver(configuration, level + 1, *held, held_claim);
}

bool Exploration::deliver(Configuration& configuration, std::size_t level, Token_t token, const bool claim) const
{
    while (level < table_.lefts.size())
    {
        auto& [held, held_claim]{configuration.stages[level]};

        if (held && token == table_.rights[level])
        {
            if (held_claim)
            {
                return false;
            }

            held.reset();
            token = product_of(level);
            ++level;

            continue;
        }

        if (held)
        {
            const auto passed{pass_upward(configuration, level)};

            if (!passed)
            {
                return false;
            }
        }

        if (token == table_.lefts[level])
        {
            held = token;
            held_claim = claim;

            return true;
        }

        ++level;
    }

    if (configuration.tail == Tail::closed)
    {
        return false;
    }

    configuration.tail = claim ? Tail::open : Tail::closed;

    return true;
}

void Exploration::expand(const Verifier::State_t state)
{
    // The state's configuration, read from a copy while interning grows the sources.
    const auto from{sources_[state]};

    for (Token_t byte{0}; byte < byte_count; ++byte)
    {
        for (const auto boundary_after : {false, true})
        {
            const Marked symbol{.byte = static_cast<unsigned char>(byte), .boundary_after = boundary_after};

            auto next{successor(from, symbol)};

            if (!next)
            {
                continue;
            }

            const auto to{intern(std::move(*next))};

            transitions_.emplace(Verifier::Key_t{state, symbol}, to);
        }
    }
}

std::optional<Configuration> Exploration::successor(const Configuration& from, const Marked symbol) const
{
    if (from.tail == Tail::closed)
    {
        return std::nullopt;
    }

    auto next{from};

    const auto delivered{deliver(next, 0, symbol.byte, symbol.boundary_after)};

    if (!delivered)
    {
        return std::nullopt;
    }

    const auto normalized{normalize(next)};

    if (!normalized)
    {
        return std::nullopt;
    }

    return next;
}

bool Exploration::normalize(Configuration& configuration) const
{
    for (auto changed{true}; changed;)
    {
        changed = false;

        std::optional<std::size_t> below{};

        for (std::size_t level{0}; level < configuration.stages.size() && !changed; ++level)
        {
            if (!configuration.stages[level].held)
            {
                continue;
            }

            if (!below)
            {
                below = level;

                continue;
            }

            const auto possible{arrivals(*below, level)};

            if (std::ranges::contains(possible, table_.rights[level]))
            {
                below = level;

                continue;
            }

            const auto passed{pass_upward(configuration, level)};

            if (!passed)
            {
                return false;
            }

            changed = true;
        }
    }

    return true;
}

std::vector<Token_t> Exploration::arrivals(const std::size_t low, const std::size_t level) const
{
    std::vector<Token_t> possible{table_.lefts[low], product_of(low)};

    for (auto between{low + 1}; between < level; ++between)
    {
        if (std::ranges::contains(possible, table_.lefts[between]))
        {
            possible.push_back(product_of(between));
        }
    }

    return possible;
}

} // namespace

Verifier merge_tower(const std::span<const Merge> merges)
{
    Exploration exploration{merges};

    return std::move(exploration).run();
}

} // namespace munch::verifier

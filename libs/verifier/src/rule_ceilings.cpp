#include "munch/verifier/rule_ceilings.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
 * @brief The merges taken in rank order: whether each is useful, and the rank of the first useful merge with equal
 *        parts when there is one.
 */
struct Stages
{
    /**
     * @brief Whether each merge is useful, by rank.
     */
    std::vector<bool> useful{};

    /**
     * @brief The rank of the first useful merge with equal parts, the effective self-pair.
     */
    std::optional<std::size_t> self_pair{};
};

/**
 * @brief One step of the growth of an encoding's first token: the stage after which the first token is the token, the
 *        number of merges below the one that made it, zero for the first byte.
 */
struct Growth
{
    /**
     * @brief The stage, one past the rank of the merge that made the token, zero for the first byte.
     */
    std::size_t stage{};

    /**
     * @brief The first token from that stage on.
     */
    Token_t token{};
};

/**
 * @brief The growth of an encoding's first token, by stage ascending, the first byte first.
 */
using History_t = std::vector<Growth>;

/**
 * @brief A state of the chain search: a string, a byte appended to it whose encoding differs from the string's in its
 *        first token, and the rank of the last merge of its chain.
 */
struct State
{
    /**
     * @brief The string.
     */
    std::string text{};

    /**
     * @brief The appended byte.
     */
    unsigned char symbol{};

    /**
     * @brief The rank of the chain's last merge.
     */
    std::size_t rank{};
};

/**
 * @brief The largest value a search attains and a state attaining it first.
 */
struct Attained
{
    /**
     * @brief The value, zero when no state attains a positive one.
     */
    std::size_t value{};

    /**
     * @brief The appended input attaining it.
     */
    std::optional<Appended_input> witness{};
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
        const std::string spelling{static_cast<char>(byte)};

        tokens.emplace(spelling, byte);
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
                    "rule_ceilings: the merge ({}, {}) at rank {} names {}, which neither the alphabet nor an earlier "
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
                    "rule_ceilings: the merge ({}, {}) at rank {} recreates the token {}, which the alphabet or an "
                    "earlier merge already carries",
                    quoted(left), quoted(right), rank, quoted(spelling))};
        }
    }

    return table;
}

/**
 * @brief Returns the spelling of every token: each byte as itself and each product as its two parts.
 * @param merges The merge table in rank order.
 * @return The spellings, by token.
 */
[[nodiscard]] std::vector<std::string> spellings_of(const std::span<const Merge> merges)
{
    std::vector<std::string> spellings{};

    for (Token_t byte{0}; byte < byte_count; ++byte)
    {
        const std::string spelling{static_cast<char>(byte)};

        spellings.push_back(spelling);
    }

    for (const auto& [left, right] : merges)
    {
        spellings.push_back(left + right);
    }

    return spellings;
}

/**
 * @brief Returns the rank of every merge by its two parts, each pair of parts being one merge's, since its product is
 *        spelled once.
 * @param table The resolved merge table.
 * @return The ranks, by the pair of parts.
 */
[[nodiscard]] std::map<std::pair<Token_t, Token_t>, std::size_t> ranks_of(const Table& table)
{
    std::map<std::pair<Token_t, Token_t>, std::size_t> ranks{};

    for (std::size_t rank{0}; rank < table.lefts.size(); ++rank)
    {
        ranks.emplace(std::pair{table.lefts[rank], table.rights[rank]}, rank);
    }

    return ranks;
}

/**
 * @brief Returns the bytes of a text as tokens, one per byte.
 * @param text The text.
 * @return The byte tokens.
 */
[[nodiscard]] std::vector<Token_t> bytes_of(const std::string_view text)
{
    std::vector<Token_t> tokens{};

    for (const auto byte : text)
    {
        tokens.push_back(static_cast<unsigned char>(byte));
    }

    return tokens;
}

/**
 * @brief The encoder over a resolved table: the ranks of the merges by their parts and the spelling of every token.
 */
class Encoder
{
public:
    /**
     * @brief Takes the resolved table and the spellings.
     * @param table The resolved merge table.
     * @param spellings The spelling of every token, by token.
     */
    Encoder(const Table& table, std::vector<std::string> spellings);

    /**
     * @brief Returns a token's spelling.
     * @param token The token.
     * @return The spelling.
     */
    [[nodiscard]] const std::string& spelling(Token_t token) const;

    /**
     * @brief Encodes a text with the merges below a rank: at every step the lowest-ranked of them present anywhere,
     *        applied at its leftmost occurrence, until none applies.
     * @param text The text.
     * @param cutoff The rank the merges applied lie below.
     * @return The tokens.
     */
    [[nodiscard]] std::vector<Token_t> encoded(std::string_view text, std::size_t cutoff) const;

    /**
     * @brief Encodes a text with every merge and returns the growth of its first token.
     * @param text The text, nonempty.
     * @return The first token's growth, by stage.
     */
    [[nodiscard]] History_t history(std::string_view text) const;

private:
    /**
     * @brief Returns where the next merge of an encoding applies: the leftmost occurrence of the lowest-ranked merge
     *        below a rank present in the tokens, as its index and rank.
     * @param tokens The tokens.
     * @param cutoff The rank the merges applied lie below.
     * @return The index of the occurrence's left part and the merge's rank, std::nullopt when no merge applies.
     */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> next_merge(
            const std::vector<Token_t>& tokens, std::size_t cutoff) const;

    /**
     * @brief The rank of every merge by its two parts.
     */
    std::map<std::pair<Token_t, Token_t>, std::size_t> ranks_{};

    /**
     * @brief The spelling of every token, by token.
     */
    std::vector<std::string> spellings_{};
};

Encoder::Encoder(const Table& table, std::vector<std::string> spellings)
    : ranks_{ranks_of(table)}, spellings_{std::move(spellings)}
{}

const std::string& Encoder::spelling(const Token_t token) const
{
    return spellings_[token];
}

std::vector<Token_t> Encoder::encoded(const std::string_view text, const std::size_t cutoff) const
{
    auto tokens{bytes_of(text)};

    for (auto next{next_merge(tokens, cutoff)}; next; next = next_merge(tokens, cutoff))
    {
        const auto [index, rank]{*next};

        tokens[index] = product_of(rank);
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    }

    return tokens;
}

std::optional<std::pair<std::size_t, std::size_t>> Encoder::next_merge(
        const std::vector<Token_t>& tokens, const std::size_t cutoff) const
{
    std::optional<std::pair<std::size_t, std::size_t>> best{};

    for (std::size_t index{0}; index + 1 < tokens.size(); ++index)
    {
        const auto found{ranks_.find({tokens[index], tokens[index + 1]})};

        if (found == ranks_.cend())
        {
            continue;
        }

        const auto& [parts, rank]{*found};

        const auto lower{!best || rank < best->second};

        if (rank < cutoff && lower)
        {
            best = std::pair{index, rank};
        }
    }

    return best;
}

History_t Encoder::history(const std::string_view text) const
{
    auto tokens{bytes_of(text)};

    History_t growth{{.stage = 0, .token = tokens.front()}};

    constexpr auto every_merge{std::numeric_limits<std::size_t>::max()};

    for (auto next{next_merge(tokens, every_merge)}; next; next = next_merge(tokens, every_merge))
    {
        const auto [index, rank]{*next};

        tokens[index] = product_of(rank);
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(index) + 1);

        if (index == 0)
        {
            growth.push_back({.stage = rank + 1, .token = tokens.front()});
        }
    }

    return growth;
}

/**
 * @brief Takes the merges in rank order, each useful when both parts are canonical and the merges before it encode the
 *        two parts' spellings as the two of them, and finds the first useful merge with equal parts.
 * @param table The resolved merge table.
 * @param encoder The encoder over the table.
 * @return Whether each merge is useful, and the first effective self-pair's rank when there is one.
 */
[[nodiscard]] Stages stages_of(const Table& table, const Encoder& encoder)
{
    const auto merge_count{table.lefts.size()};

    std::vector<bool> canonical(byte_count + merge_count, false);

    std::fill_n(canonical.begin(), byte_count, true);

    Stages stages{.useful = std::vector<bool>(merge_count, false), .self_pair = std::nullopt};

    for (std::size_t rank{0}; rank < merge_count; ++rank)
    {
        const auto left{table.lefts[rank]};
        const auto right{table.rights[rank]};

        if (!canonical[left] || !canonical[right])
        {
            continue;
        }

        const auto spelling{encoder.spelling(left) + encoder.spelling(right)};

        const auto encoding{encoder.encoded(spelling, rank)};

        if (encoding != std::vector{left, right})
        {
            continue;
        }

        if (left == right && !stages.self_pair)
        {
            stages.self_pair = rank;
        }

        stages.useful[rank] = true;
        canonical[product_of(rank)] = true;
    }

    return stages;
}

/**
 * @brief Returns the first token of an encoding with the merges below a rank, read off its first token's growth: the
 *        token of the latest step at a stage of at most the rank.
 * @param history The first token's growth.
 * @param rank The rank the merges applied lie below.
 * @return The first token.
 */
[[nodiscard]] Token_t first_at(const History_t& history, const std::size_t rank)
{
    auto token{history.front().token};

    for (const auto& [stage, grown] : history)
    {
        if (stage > rank)
        {
            break;
        }

        token = grown;
    }

    return token;
}

/**
 * @brief The chain search over a table under the ordering condition: the encoder, the useful merges by right part, and
 *        the histories and adjacencies decided so far.
 */
class Chain_search
{
public:
    /**
     * @brief Takes the resolved table, its encoder and which of its merges are useful, nothing searched yet.
     * @param table The resolved merge table.
     * @param encoder The encoder over the table.
     * @param useful Whether each merge is useful, by rank.
     */
    Chain_search(Table table, const Encoder& encoder, const std::vector<bool>& useful);

    /**
     * @brief Visits every state once, from the states of one merge, and returns the right span radius and the right
     *        start radius with a state attaining each first.
     * @return The span radius and the start radius.
     */
    [[nodiscard]] std::pair<Attained, Attained> run() &&;

private:
    /**
     * @brief Returns the growth of the first token of a text's encoding, deciding it once per text.
     * @param text The text, nonempty.
     * @return The growth.
     */
    [[nodiscard]] const History_t& history(const std::string& text);

    /**
     * @brief Returns the states a state extends to: for each step of either encoding's first-token growth and each
     *        useful merge of a higher rank whose right part is that step's token at the merge's stage, the state with
     *        the merge's left part prepended, when the other encoding's first token there differs and the merges below
     *        encode the left part followed by it as the two of them.
     * @param state The state.
     * @param text_history The growth of the first token of the state's text.
     * @param appended_history The growth of the first token of the state's text with its byte appended.
     * @return The states extended to.
     */
    [[nodiscard]] std::vector<State> extensions(
            const State& state, const History_t& text_history, const History_t& appended_history);

    /**
     * @brief Returns whether the merges below a rank encode one token's spelling followed by another's as the two
     *        tokens, deciding it once per rank and pair.
     * @param rank The rank the merges applied lie below.
     * @param first The first token.
     * @param second The second token.
     * @return True when the two tokens may be adjacent under the merges below the rank.
     */
    [[nodiscard]] bool is_adjacent_below(std::size_t rank, Token_t first, Token_t second);

    /**
     * @brief The resolved merge table.
     */
    Table table_{};

    /**
     * @brief The encoder over the table.
     */
    const Encoder& encoder_;

    /**
     * @brief The useful merges' ranks, ascending.
     */
    std::vector<std::size_t> useful_{};

    /**
     * @brief The useful merges' ranks by their right part, each ascending.
     */
    std::map<Token_t, std::vector<std::size_t>> by_right_{};

    /**
     * @brief The first-token growth of every text asked so far.
     */
    std::map<std::string, History_t> histories_{};

    /**
     * @brief Whether the merges below each rank asked so far keep each pair asked adjacent.
     */
    std::map<std::tuple<std::size_t, Token_t, Token_t>, bool> adjacencies_{};
};

Chain_search::Chain_search(Table table, const Encoder& encoder, const std::vector<bool>& useful)
    : table_{std::move(table)}, encoder_{encoder}
{
    for (std::size_t rank{0}; rank < useful.size(); ++rank)
    {
        if (!useful[rank])
        {
            continue;
        }

        useful_.push_back(rank);
        by_right_[table_.rights[rank]].push_back(rank);
    }
}

std::pair<Attained, Attained> Chain_search::run() &&
{
    std::vector<State> stack{};

    for (const auto rank : useful_)
    {
        const auto right{table_.rights[rank]};

        if (right >= byte_count)
        {
            continue;
        }

        const auto& left{encoder_.spelling(table_.lefts[rank])};

        stack.push_back({.text = left, .symbol = static_cast<unsigned char>(right), .rank = rank});
    }

    Attained span{};
    Attained start{};

    while (!stack.empty())
    {
        auto state{std::move(stack.back())};

        stack.pop_back();

        const auto& [text, symbol, rank]{state};

        const auto appended{text + static_cast<char>(symbol)};

        const auto& text_history{history(text)};
        const auto& appended_history{history(appended)};

        const auto text_first{encoder_.spelling(text_history.back().token).size()};
        const auto appended_first{encoder_.spelling(appended_history.back().token).size()};

        const auto start_ripple{text.size() - std::min(text_first, appended_first)};

        const auto attained{[&](const std::size_t value) {
            Appended_input witness{.input = text, .symbol = {static_cast<char>(symbol)}};

            return Attained{.value = value, .witness = std::move(witness)};
        }};

        if (text.size() > span.value)
        {
            span = attained(text.size());
        }

        if (start_ripple > start.value)
        {
            start = attained(start_ripple);
        }

        auto extended{extensions(state, text_history, appended_history)};

        std::ranges::move(extended, std::back_inserter(stack));
    }

    return {span, start};
}

const History_t& Chain_search::history(const std::string& text)
{
    const auto known{histories_.find(text)};

    if (known != histories_.cend())
    {
        const auto& [known_text, growth]{*known};

        return growth;
    }

    auto growth{encoder_.history(text)};

    const auto [entry, inserted]{histories_.emplace(text, std::move(growth))};

    const auto& [entry_text, entry_growth]{*entry};

    return entry_growth;
}

std::vector<State> Chain_search::extensions(
        const State& state, const History_t& text_history, const History_t& appended_history)
{
    const auto& [text, symbol, last_rank]{state};

    std::vector<State> extended{};

    const auto extend_from{[&](const History_t& growth, const History_t& other) {
        for (std::size_t step{0}; step < growth.size(); ++step)
        {
            const auto& [stage, token]{growth[step]};

            const auto has_next{step + 1 < growth.size()};

            const auto next_stage{has_next ? growth[step + 1].stage : std::numeric_limits<std::size_t>::max()};

            const auto consumers{by_right_.find(token)};

            if (consumers == by_right_.cend())
            {
                continue;
            }

            const auto& [right, ranks]{*consumers};

            for (const auto rank : ranks)
            {
                // The token is the first token of the encoding below the rank exactly when the rank lies in its stages.
                if (rank <= last_rank || rank < stage || rank >= next_stage)
                {
                    continue;
                }

                const auto other_token{first_at(other, rank)};

                if (other_token == token)
                {
                    continue;
                }

                const auto left{table_.lefts[rank]};

                if (!is_adjacent_below(rank, left, other_token))
                {
                    continue;
                }

                const auto prepended{encoder_.spelling(left) + text};

                extended.push_back({.text = prepended, .symbol = symbol, .rank = rank});
            }
        }
    }};

    extend_from(text_history, appended_history);
    extend_from(appended_history, text_history);

    return extended;
}

bool Chain_search::is_adjacent_below(const std::size_t rank, const Token_t first, const Token_t second)
{
    const auto key{std::tuple{rank, first, second}};

    const auto known{adjacencies_.find(key)};

    if (known != adjacencies_.cend())
    {
        const auto& [asked, adjacent]{*known};

        return adjacent;
    }

    const auto spelling{encoder_.spelling(first) + encoder_.spelling(second)};

    const auto encoding{encoder_.encoded(spelling, rank)};

    const auto adjacent{encoding == std::vector{first, second}};

    adjacencies_.emplace(key, adjacent);

    return adjacent;
}

} // namespace

Rule_ceilings rule_ceilings(const std::span<const Merge> merges)
{
    auto table{table_of(merges)};

    const Encoder encoder{table, spellings_of(merges)};

    const auto [useful, self_pair]{stages_of(table, encoder)};

    std::size_t left_lengths{0};
    std::size_t useful_right_lengths{0};
    std::size_t product_lengths{0};
    std::vector<bool> named(byte_count, false);

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        const auto product{left + right};

        left_lengths += left.size();
        product_lengths += product.size();

        if (useful[rank])
        {
            useful_right_lengths += right.size();
        }

        for (const auto byte : product)
        {
            named[static_cast<unsigned char>(byte)] = true;
        }
    }

    const auto named_bytes{static_cast<std::size_t>(std::ranges::count(named, true))};

    const auto pairs_bound{1 + named_bytes + product_lengths};

    const auto start_ceiling{left_lengths == 0 ? std::size_t{0} : left_lengths - 1};

    const auto radius_ceiling{self_pair ? std::nullopt : std::optional{useful_right_lengths}};

    const auto loose_ceiling{self_pair ? std::nullopt : std::optional{pairs_bound * pairs_bound}};

    Chain_search search{std::move(table), encoder, useful};

    auto [span, start]{std::move(search).run()};

    return {.start_ceiling = start_ceiling,
            .radius_ceiling = radius_ceiling,
            .loose_ceiling = loose_ceiling,
            .span_radius = span.value,
            .span_witness = std::move(span.witness),
            .start_radius = start.value,
            .start_witness = std::move(start.witness)};
}

} // namespace munch::verifier

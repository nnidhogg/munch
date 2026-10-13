#include "munch/verifier/ripple_radius.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
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
 * @brief A token of the table's vocabulary: a symbol of the alphabet by its place in it, so a byte by its value, or a
 *        product by its merge's rank past the symbols.
 */
using Token_t = std::size_t;

/**
 * @brief The number of bytes, the symbols of the byte alphabet.
 */
constexpr std::size_t byte_count{256};

/**
 * @brief The merge table resolved to tokens over an alphabet: the token of each symbol, and each merge's parts by rank,
 *        the product of rank r being the token r past the symbols.
 */
struct Table
{
    /**
     * @brief The token of each symbol of the alphabet, by its spelling.
     */
    std::unordered_map<std::string, Token_t> symbols{};

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
 * @brief The merges taken in rank order: the useful ones before the first effective self-pair, and that self-pair's
 *        rank when there is one.
 */
struct Stages
{
    /**
     * @brief The ranks of the useful merges, ascending, each with unequal parts.
     */
    std::vector<std::size_t> useful{};

    /**
     * @brief The rank of the first useful merge with equal parts, the effective self-pair.
     */
    std::optional<std::size_t> self_pair{};
};

/**
 * @brief The canonical tokens of a table with no effective self-pair, with what the encoding and the graph read.
 */
struct Vocabulary
{
    /**
     * @brief The spelling of every token, the symbols and every product, by token.
     */
    std::vector<std::string> spellings{};

    /**
     * @brief The ranks of the useful merges, ascending, the passes of the encoding.
     */
    std::vector<std::size_t> passes{};

    /**
     * @brief The symbols the table names, ascending.
     */
    std::vector<Token_t> named{};

    /**
     * @brief The canonical tokens beginning with each byte, by byte, ascending.
     */
    std::vector<std::vector<Token_t>> beginning{};

    /**
     * @brief The bytes of the symbols the table names, ascending.
     */
    std::vector<unsigned char> bytes{};
};

/**
 * @brief Where one copy of the spelling automaton is: the token it has guessed and how many of its bytes it has read,
 *        or the start, no token guessed.
 */
struct Position
{
    /**
     * @brief Ordered member by member, so pairs key the map of interned ones.
     */
    auto operator<=>(const Position&) const = default;

    /**
     * @brief The guessed token, none at the start.
     */
    std::optional<Token_t> token{};

    /**
     * @brief The bytes of the token read, from one to its length.
     */
    std::size_t read{};
};

/**
 * @brief A node of the overlap graph: the copy reading the input alone and the copy that read the prepended symbol
 *        first.
 */
struct Pair
{
    /**
     * @brief Ordered member by member, so pairs key the map of interned ones.
     */
    auto operator<=>(const Pair&) const = default;

    /**
     * @brief The copy reading the input alone, at the start before the first byte.
     */
    Position base{};

    /**
     * @brief The copy that read the prepended symbol as the first symbol of its first token.
     */
    Position prepended{};
};

/**
 * @brief One copy's reading of a byte: where the copy is after it, and the byte's offset in its guessed token with that
 *        token's length.
 */
struct Reading
{
    /**
     * @brief The position after the byte.
     */
    Position next{};

    /**
     * @brief The byte's offset in its token.
     */
    std::size_t offset{};

    /**
     * @brief The token's length.
     */
    std::size_t length{};
};

/**
 * @brief A step of the overlap graph: the node it reaches, the byte read, the byte's offset in the base copy's token,
 *        and whether the two copies' tokens covering the byte differ.
 */
struct Step
{
    /**
     * @brief The node reached.
     */
    std::size_t target{};

    /**
     * @brief The byte read.
     */
    unsigned char byte{};

    /**
     * @brief The byte's offset in the base copy's token.
     */
    std::size_t offset{};

    /**
     * @brief Whether the two copies' tokens covering the byte differ in offset or length.
     */
    bool marked{};
};

/**
 * @brief A step named by the node it leaves and its index among that node's steps.
 */
struct Arrival
{
    /**
     * @brief The node the step leaves.
     */
    std::size_t node{};

    /**
     * @brief The step's index among the node's steps.
     */
    std::size_t step{};
};

/**
 * @brief The longest paths inside the marked part of the graph: each node's longest distance from a source and the step
 *        that attains it.
 */
struct Longest
{
    /**
     * @brief The longest distance from a source, by node, zero outside the marked part.
     */
    std::vector<std::size_t> depths{};

    /**
     * @brief The step into each node on its longest path, none at a source or outside the marked part.
     */
    std::vector<std::optional<Arrival>> arrivals{};
};

/**
 * @brief The marked step attaining a value, and the value.
 */
struct Best
{
    /**
     * @brief The value attained, zero when no marked step attains one.
     */
    std::size_t value{};

    /**
     * @brief The marked step attaining it.
     */
    std::optional<Arrival> step{};
};

/**
 * @brief Returns the token of a merge's product, past the symbols of the alphabet.
 * @param table The resolved merge table.
 * @param rank The merge's rank.
 * @return The product's token.
 */
[[nodiscard]] Token_t product_of(const Table& table, const std::size_t rank) noexcept
{
    return table.symbols.size() + rank;
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
 * @brief Resolves a merge table to tokens over an alphabet, each part looked up among the symbols and the earlier
 *        products.
 * @param merges The merge table in rank order.
 * @param symbols The alphabet's symbols, each spelled by its bytes, numbered in order.
 * @return The resolved table.
 * @throws std::invalid_argument If the alphabet holds a symbol twice, or a merge names a part the vocabulary so far
 *         does not carry, or a product it already carries.
 */
[[nodiscard]] Table table_of(const std::span<const Merge> merges, const std::span<const std::string> symbols)
{
    Table table{};

    for (Token_t symbol{0}; symbol < symbols.size(); ++symbol)
    {
        const auto& spelling{symbols[symbol]};

        const auto [entry, inserted]{table.symbols.try_emplace(spelling, symbol)};

        if (!inserted)
        {
            throw std::invalid_argument{std::format("ripple_radius: the alphabet carries {} twice", quoted(spelling))};
        }
    }

    auto tokens{table.symbols};

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        const auto left_found{tokens.find(left)};
        const auto right_found{tokens.find(right)};

        if (left_found == tokens.cend() || right_found == tokens.cend())
        {
            const auto& missing{left_found == tokens.cend() ? left : right};

            throw std::invalid_argument{std::format(
                    "ripple_radius: the merge ({}, {}) at rank {} names {}, which neither the alphabet nor an earlier "
                    "merge carries",
                    quoted(left), quoted(right), rank, quoted(missing))};
        }

        const auto& [left_spelling, left_token]{*left_found};
        const auto& [right_spelling, right_token]{*right_found};

        table.lefts.push_back(left_token);
        table.rights.push_back(right_token);

        const auto spelling{left + right};

        const auto [product, inserted]{tokens.try_emplace(spelling, product_of(table, rank))};

        if (!inserted)
        {
            throw std::invalid_argument{std::format(
                    "ripple_radius: the merge ({}, {}) at rank {} recreates the token {}, which the alphabet or an "
                    "earlier merge already carries",
                    quoted(left), quoted(right), rank, quoted(spelling))};
        }
    }

    return table;
}

/**
 * @brief Returns the byte alphabet: every byte, each a symbol spelled by itself, in order of value.
 * @return The spellings of the 256 bytes.
 */
[[nodiscard]] std::vector<std::string> byte_alphabet()
{
    std::vector<std::string> symbols{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        const std::string spelling{static_cast<char>(byte)};

        symbols.push_back(spelling);
    }

    return symbols;
}

/**
 * @brief Returns the characters of an alphabet given as UTF-8 text, each spelled by its bytes: the text split before
 *        every byte that is not a continuation byte.
 * @param alphabet The alphabet, UTF-8.
 * @return The characters, in order.
 */
[[nodiscard]] std::vector<std::string> characters_of(const std::string_view alphabet)
{
    constexpr unsigned char continuation_mask{0xC0};
    constexpr unsigned char continuation_bits{0x80};

    std::vector<std::string> characters{};

    for (const auto byte : alphabet)
    {
        const auto value{static_cast<unsigned char>(byte)};

        const auto continues{(value & continuation_mask) == continuation_bits};

        if (continues && !characters.empty())
        {
            characters.back() += byte;

            continue;
        }

        characters.push_back(std::string{byte});
    }

    return characters;
}

/**
 * @brief Returns the spelling of every token: each symbol as itself and each product as its two parts.
 * @param merges The merge table in rank order.
 * @param symbols The alphabet's symbols, each spelled by its bytes, numbered in order.
 * @return The spellings, by token.
 */
[[nodiscard]] std::vector<std::string> spellings_of(
        const std::span<const Merge> merges, const std::span<const std::string> symbols)
{
    std::vector<std::string> spellings{symbols.begin(), symbols.end()};

    for (const auto& [left, right] : merges)
    {
        spellings.push_back(left + right);
    }

    return spellings;
}

/**
 * @brief Returns the symbols of a text as tokens, the text split wherever the bytes read since the last split spell a
 *        symbol.
 * @param table The resolved merge table.
 * @param text The text, the spelling of tokens.
 * @return The symbol tokens.
 */
[[nodiscard]] std::vector<Token_t> symbols_of(const Table& table, const std::string_view text)
{
    std::vector<Token_t> tokens{};

    std::string pending{};

    for (const auto byte : text)
    {
        pending += byte;

        const auto found{table.symbols.find(pending)};

        if (found == table.symbols.cend())
        {
            continue;
        }

        const auto& [spelling, symbol]{*found};

        tokens.push_back(symbol);
        pending.clear();
    }

    return tokens;
}

/**
 * @brief Encodes a token sequence by one left-to-right pass per merge, in rank order, each joining every occurrence of
 *        the merge's parts it reaches into the product.
 *
 * Under the ordering condition this is the encoder's segmentation, the merges that never apply left out.
 * @param table The resolved merge table.
 * @param passes The ranks of the merges to apply, ascending.
 * @param tokens The sequence, the symbols of a text or tokens of its encoding.
 * @return The encoded sequence.
 */
[[nodiscard]] std::vector<Token_t> encoded(
        const Table& table, const std::span<const std::size_t> passes, std::vector<Token_t> tokens)
{
    for (const auto rank : passes)
    {
        const auto left{table.lefts[rank]};
        const auto right{table.rights[rank]};

        std::vector<Token_t> joined{};

        for (std::size_t index{0}; index < tokens.size(); ++index)
        {
            const auto has_next{index + 1 < tokens.size()};

            const auto joins{has_next && tokens[index] == left && tokens[index + 1] == right};

            if (joins)
            {
                joined.push_back(product_of(table, rank));
                ++index;

                continue;
            }

            joined.push_back(tokens[index]);
        }

        tokens = std::move(joined);
    }

    return tokens;
}

/**
 * @brief Takes the merges in rank order, each useful when both parts are canonical and the useful merges before it
 *        encode the two parts as the two of them, until the first useful merge with equal parts.
 * @param table The resolved merge table.
 * @param spellings The spelling of every token, by token.
 * @return The useful merges before the first effective self-pair, and its rank when there is one.
 */
[[nodiscard]] Stages stages_of(const Table& table, const std::vector<std::string>& spellings)
{
    std::vector<bool> canonical(spellings.size(), false);

    std::fill_n(canonical.begin(), table.symbols.size(), true);

    Stages stages{};

    for (std::size_t rank{0}; rank < table.lefts.size(); ++rank)
    {
        const auto left{table.lefts[rank]};
        const auto right{table.rights[rank]};

        if (!canonical[left] || !canonical[right])
        {
            continue;
        }

        const auto spelling{spellings[left] + spellings[right]};

        const auto parts{symbols_of(table, spelling)};

        const auto encoding{encoded(table, stages.useful, parts)};

        if (encoding != std::vector{left, right})
        {
            continue;
        }

        if (left == right)
        {
            stages.self_pair = rank;

            return stages;
        }

        stages.useful.push_back(rank);
        canonical[product_of(table, rank)] = true;
    }

    return stages;
}

/**
 * @brief Returns the canonical tokens of a table with no effective self-pair: the symbols the table names and the
 *        useful products, grouped by their first byte.
 * @param table The resolved merge table.
 * @param merges The merge table in rank order.
 * @param spellings The spelling of every token, by token.
 * @param useful The ranks of the useful merges, ascending.
 * @return The vocabulary.
 */
[[nodiscard]] Vocabulary vocabulary_of(
        const Table& table, const std::span<const Merge> merges, std::vector<std::string> spellings,
        std::vector<std::size_t> useful)
{
    std::vector<bool> named(table.symbols.size(), false);

    for (const auto& [left, right] : merges)
    {
        const auto spelling{left + right};

        for (const auto symbol : symbols_of(table, spelling))
        {
            named[symbol] = true;
        }
    }

    Vocabulary vocabulary{
            .spellings = std::move(spellings),
            .passes = std::move(useful),
            .named = {},
            .beginning = std::vector<std::vector<Token_t>>(byte_count),
            .bytes = {}};

    std::vector<bool> spelled(byte_count, false);

    for (Token_t symbol{0}; symbol < named.size(); ++symbol)
    {
        if (!named[symbol])
        {
            continue;
        }

        const auto& spelling{vocabulary.spellings[symbol]};

        const auto first{static_cast<unsigned char>(spelling.front())};

        vocabulary.named.push_back(symbol);
        vocabulary.beginning[first].push_back(symbol);

        for (const auto byte : spelling)
        {
            spelled[static_cast<unsigned char>(byte)] = true;
        }
    }

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        if (spelled[byte])
        {
            vocabulary.bytes.push_back(static_cast<unsigned char>(byte));
        }
    }

    for (const auto rank : vocabulary.passes)
    {
        const auto product{product_of(table, rank)};

        const auto first{static_cast<unsigned char>(vocabulary.spellings[product].front())};

        vocabulary.beginning[first].push_back(product);
    }

    return vocabulary;
}

/**
 * @brief Returns the nodes from which a path leads to the given ones, the given ones included.
 * @param predecessors The predecessors, by node.
 * @param reached Whether each node is given, by node.
 * @return Whether each node leads to a given one, by node.
 */
[[nodiscard]] std::vector<bool> leading_to(
        const std::vector<std::vector<std::size_t>>& predecessors, std::vector<bool> reached)
{
    std::vector<std::size_t> frontier{};

    for (std::size_t node{0}; node < reached.size(); ++node)
    {
        if (reached[node])
        {
            frontier.push_back(node);
        }
    }

    while (!frontier.empty())
    {
        const auto node{frontier.back()};

        frontier.pop_back();

        for (const auto predecessor : predecessors[node])
        {
            if (reached[predecessor])
            {
                continue;
            }

            reached[predecessor] = true;
            frontier.push_back(predecessor);
        }
    }

    return reached;
}

/**
 * @brief The overlap graph under exploration: the resolved merge table and its vocabulary, the pair each node reads as,
 *        the interned pairs with their nodes, each node's steps, the nodes ending both copies, and the adjacencies
 *        decided so far.
 */
class Exploration
{
public:
    /**
     * @brief Takes the resolved merge table and its vocabulary, nothing explored yet.
     * @param table The resolved merge table.
     * @param vocabulary The canonical tokens of the table, which has no effective self-pair.
     */
    Exploration(Table table, Vocabulary vocabulary);

    /**
     * @brief Explores the overlap graph from every node pairing the start with a canonical token, and returns the
     *        radius and the mirrored constant read off it, each with a prepended input attaining it.
     * @return The finite ripple.
     */
    [[nodiscard]] Finite_ripple run() &&;

private:
    /**
     * @brief Returns the node of a pair, interning it as a new node when it is first found.
     * @param pair The pair.
     * @return The pair's node.
     */
    [[nodiscard]] std::size_t intern(const Pair& pair);

    /**
     * @brief Returns whether a copy is at a token end: at the start, or with every byte of its token read.
     * @param position The copy's position.
     * @return True when the copy may end or begin a token.
     */
    [[nodiscard]] bool is_at_end(const Position& position) const;

    /**
     * @brief Reads every byte of the symbols the table names from one node, both copies reading it, interning the
     *        pairs reached.
     * @param node The node read from.
     */
    void expand(std::size_t node);

    /**
     * @brief Returns one copy's readings of a byte: inside its token the token's next byte, and at a token end the
     *        first byte of every canonical token that may follow the token just ended.
     * @param position The copy's position.
     * @param byte The byte read.
     * @return The readings, none when the copy cannot read the byte.
     */
    [[nodiscard]] std::vector<Reading> readings(const Position& position, unsigned char byte);

    /**
     * @brief Returns whether the encoding of two tokens' spellings together is the two tokens, deciding it once per
     *        pair.
     * @param first The first token.
     * @param second The second token.
     * @return True when the second token may follow the first in an encoding.
     */
    [[nodiscard]] bool is_adjacent(Token_t first, Token_t second);

    /**
     * @brief Returns every node's predecessors, the nodes with a step into it.
     * @return The predecessors, by node.
     */
    [[nodiscard]] std::vector<std::vector<std::size_t>> predecessors_of() const;

    /**
     * @brief Returns the nodes with a marked step into a live node.
     * @param live Whether each node leads to a node ending both copies, by node.
     * @return Whether each node has such a step, by node.
     */
    [[nodiscard]] std::vector<bool> marking(const std::vector<bool>& live) const;

    /**
     * @brief Returns the longest paths from the sources inside the marked part, the nodes leading to a marked step into
     *        a live node, which is acyclic without an effective self-pair, taken in topological order.
     * @param marked_part Whether each node is in the marked part, by node.
     * @return Each node's longest distance and the step attaining it.
     */
    [[nodiscard]] Longest longest_paths(const std::vector<bool>& marked_part) const;

    /**
     * @brief Returns the marked steps into live nodes from the marked part that attain the radius, one past the longest
     *        distance to the step, and the mirrored constant, that less the step's offset in the base copy's token.
     * @param live Whether each node leads to a node ending both copies, by node.
     * @param marked_part Whether each node is in the marked part, by node.
     * @param longest The longest paths inside the marked part.
     * @return The step attaining the radius and the one attaining the mirrored constant.
     */
    [[nodiscard]] std::pair<Best, Best> best_steps(
            const std::vector<bool>& live, const std::vector<bool>& marked_part, const Longest& longest) const;

    /**
     * @brief Returns the prepended input a marked step attains its value on: the prepended symbol of the source the
     *        longest path to the step begins at, and the bytes of that path, the step and a shortest completion.
     * @param longest The longest paths inside the marked part.
     * @param best The step attaining the value.
     * @return The prepended input, none when no step attains a value.
     */
    [[nodiscard]] std::optional<Prepended_input> witness(const Longest& longest, const Best& best) const;

    /**
     * @brief Returns the bytes of a shortest path from a node to a node ending both copies.
     * @param from The node, live.
     * @return The bytes read on the path.
     */
    [[nodiscard]] std::string completion(std::size_t from) const;

    /**
     * @brief The resolved merge table.
     */
    Table table_{};

    /**
     * @brief The canonical tokens of the table.
     */
    Vocabulary vocabulary_{};

    /**
     * @brief The pair each node reads as, by node.
     */
    std::vector<Pair> pairs_{};

    /**
     * @brief Whether each node ends both copies at a token end, by node.
     */
    std::vector<bool> accepting_{};

    /**
     * @brief The interned pairs with their nodes.
     */
    std::map<Pair, std::size_t> interned_{};

    /**
     * @brief The steps found so far, by node.
     */
    std::vector<std::vector<Step>> steps_{};

    /**
     * @brief Whether the encoding of each pair of tokens asked so far is the two tokens.
     */
    std::map<std::pair<Token_t, Token_t>, bool> adjacencies_{};
};

Exploration::Exploration(Table table, Vocabulary vocabulary)
    : table_{std::move(table)}, vocabulary_{std::move(vocabulary)}
{}

Finite_ripple Exploration::run() &&
{
    for (const auto symbol : vocabulary_.named)
    {
        const auto& spelling{vocabulary_.spellings[symbol]};

        const auto first{static_cast<unsigned char>(spelling.front())};

        for (const auto token : vocabulary_.beginning[first])
        {
            if (!vocabulary_.spellings[token].starts_with(spelling))
            {
                continue;
            }

            std::ignore = intern({.base = {}, .prepended = {.token = token, .read = spelling.size()}});
        }
    }

    for (std::size_t node{0}; node < pairs_.size(); ++node)
    {
        expand(node);
    }

    const auto predecessors{predecessors_of()};

    const auto live{leading_to(predecessors, accepting_)};

    const auto marked{marking(live)};

    const auto marked_part{leading_to(predecessors, marked)};

    const auto longest{longest_paths(marked_part)};

    const auto [radius, mirrored]{best_steps(live, marked_part, longest)};

    auto radius_witness{witness(longest, radius)};

    auto mirrored_witness{witness(longest, mirrored)};

    return {.radius = radius.value,
            .radius_witness = std::move(radius_witness),
            .mirrored = mirrored.value,
            .mirrored_witness = std::move(mirrored_witness)};
}

std::size_t Exploration::intern(const Pair& pair)
{
    const auto [entry, inserted]{interned_.try_emplace(pair, pairs_.size())};

    const auto& [key, node]{*entry};

    if (!inserted)
    {
        return node;
    }

    const auto accepting{is_at_end(pair.base) && is_at_end(pair.prepended)};

    pairs_.push_back(pair);
    steps_.emplace_back();
    accepting_.push_back(accepting);

    return node;
}

bool Exploration::is_at_end(const Position& position) const
{
    return !position.token || position.read == vocabulary_.spellings[*position.token].size();
}

void Exploration::expand(const std::size_t node)
{
    // The node's pair, read from a copy while interning grows the pairs.
    const auto [base, prepended]{pairs_[node]};

    for (const auto byte : vocabulary_.bytes)
    {
        const auto base_readings{readings(base, byte)};
        const auto prepended_readings{readings(prepended, byte)};

        for (const auto& [base_next, base_offset, base_length] : base_readings)
        {
            for (const auto& [prepended_next, prepended_offset, prepended_length] : prepended_readings)
            {
                const auto target{intern({.base = base_next, .prepended = prepended_next})};

                const auto marked{base_offset != prepended_offset || base_length != prepended_length};

                steps_[node].push_back({.target = target, .byte = byte, .offset = base_offset, .marked = marked});
            }
        }
    }
}

std::vector<Reading> Exploration::readings(const Position& position, const unsigned char byte)
{
    const auto& [token, read]{position};

    if (!is_at_end(position))
    {
        const auto& spelling{vocabulary_.spellings[*token]};

        if (static_cast<unsigned char>(spelling[read]) != byte)
        {
            return {};
        }

        return {{.next = {.token = token, .read = read + 1}, .offset = read, .length = spelling.size()}};
    }

    std::vector<Reading> found{};

    for (const auto next : vocabulary_.beginning[byte])
    {
        if (token && !is_adjacent(*token, next))
        {
            continue;
        }

        const auto length{vocabulary_.spellings[next].size()};

        found.push_back({.next = {.token = next, .read = 1}, .offset = 0, .length = length});
    }

    return found;
}

bool Exploration::is_adjacent(const Token_t first, const Token_t second)
{
    const auto known{adjacencies_.find({first, second})};

    if (known != adjacencies_.cend())
    {
        const auto& [tokens, adjacent]{*known};

        return adjacent;
    }

    const auto spelling{vocabulary_.spellings[first] + vocabulary_.spellings[second]};

    const auto parts{symbols_of(table_, spelling)};

    const auto encoding{encoded(table_, vocabulary_.passes, parts)};

    const auto adjacent{encoding == std::vector{first, second}};

    adjacencies_.emplace(std::pair{first, second}, adjacent);

    return adjacent;
}

std::vector<std::vector<std::size_t>> Exploration::predecessors_of() const
{
    std::vector<std::vector<std::size_t>> predecessors(steps_.size());

    for (std::size_t node{0}; node < steps_.size(); ++node)
    {
        for (const auto& [target, byte, offset, marked] : steps_[node])
        {
            predecessors[target].push_back(node);
        }
    }

    return predecessors;
}

std::vector<bool> Exploration::marking(const std::vector<bool>& live) const
{
    std::vector<bool> marks(steps_.size(), false);

    const auto into_live{[&live](const Step& step) { return step.marked && live[step.target]; }};

    for (std::size_t node{0}; node < steps_.size(); ++node)
    {
        marks[node] = std::ranges::any_of(steps_[node], into_live);
    }

    return marks;
}

Longest Exploration::longest_paths(const std::vector<bool>& marked_part) const
{
    const auto node_count{steps_.size()};

    std::vector<std::size_t> incoming(node_count, 0);

    for (std::size_t node{0}; node < node_count; ++node)
    {
        for (const auto& [target, byte, offset, marked] : steps_[node])
        {
            if (!marked_part[node] || !marked_part[target])
            {
                continue;
            }

            ++incoming[target];
        }
    }

    // The sources, the nodes whose base copy is at the start, have no step into them.
    std::vector<std::size_t> ready{};

    for (std::size_t node{0}; node < node_count; ++node)
    {
        if (marked_part[node] && !pairs_[node].base.token)
        {
            ready.push_back(node);
        }
    }

    Longest longest{
            .depths = std::vector<std::size_t>(node_count, 0),
            .arrivals = std::vector<std::optional<Arrival>>(node_count)};

    for (std::size_t head{0}; head < ready.size(); ++head)
    {
        const auto node{ready[head]};

        const auto& steps{steps_[node]};

        for (std::size_t index{0}; index < steps.size(); ++index)
        {
            const auto target{steps[index].target};

            if (!marked_part[target])
            {
                continue;
            }

            if (longest.depths[node] + 1 > longest.depths[target])
            {
                longest.depths[target] = longest.depths[node] + 1;
                longest.arrivals[target] = Arrival{.node = node, .step = index};
            }

            --incoming[target];

            if (incoming[target] == 0)
            {
                ready.push_back(target);
            }
        }
    }

    return longest;
}

std::pair<Best, Best> Exploration::best_steps(
        const std::vector<bool>& live, const std::vector<bool>& marked_part, const Longest& longest) const
{
    Best radius{};
    Best mirrored{};

    for (std::size_t node{0}; node < steps_.size(); ++node)
    {
        if (!marked_part[node])
        {
            continue;
        }

        const auto& steps{steps_[node]};

        for (std::size_t index{0}; index < steps.size(); ++index)
        {
            const auto& [target, byte, offset, marked]{steps[index]};

            if (!marked || !live[target])
            {
                continue;
            }

            const auto reach{longest.depths[node] + 1};

            if (reach > radius.value)
            {
                radius = {.value = reach, .step = Arrival{.node = node, .step = index}};
            }

            if (reach - offset > mirrored.value)
            {
                mirrored = {.value = reach - offset, .step = Arrival{.node = node, .step = index}};
            }
        }
    }

    return {radius, mirrored};
}

std::optional<Prepended_input> Exploration::witness(const Longest& longest, const Best& best) const
{
    if (!best.step)
    {
        return std::nullopt;
    }

    const auto [node, index]{*best.step};

    const auto& [target, byte, offset, marked]{steps_[node][index]};

    std::string bytes{static_cast<char>(byte)};

    auto source{node};

    for (auto arrival{longest.arrivals[source]}; arrival; arrival = longest.arrivals[source])
    {
        const auto [previous, previous_step]{*arrival};

        bytes += static_cast<char>(steps_[previous][previous_step].byte);
        source = previous;
    }

    std::ranges::reverse(bytes);

    bytes += completion(target);

    const auto& [base, prepended]{pairs_[source]};

    const auto& first_token{vocabulary_.spellings[*prepended.token]};

    auto symbol{first_token.substr(0, prepended.read)};

    return Prepended_input{.symbol = std::move(symbol), .input = std::move(bytes)};
}

std::string Exploration::completion(const std::size_t from) const
{
    std::vector<std::optional<Arrival>> arrivals(steps_.size());
    std::vector<bool> seen(steps_.size(), false);
    std::vector<std::size_t> frontier{from};

    seen[from] = true;

    auto end{from};

    for (std::size_t head{0}; !accepting_[end]; ++head)
    {
        const auto node{frontier[head]};

        const auto& steps{steps_[node]};

        for (std::size_t index{0}; index < steps.size() && !accepting_[end]; ++index)
        {
            const auto target{steps[index].target};

            if (seen[target])
            {
                continue;
            }

            seen[target] = true;
            arrivals[target] = Arrival{.node = node, .step = index};
            frontier.push_back(target);
            end = target;
        }
    }

    std::string bytes{};

    for (auto node{end}; node != from;)
    {
        const auto [previous, index]{*arrivals[node]};

        bytes += static_cast<char>(steps_[previous][index].byte);
        node = previous;
    }

    std::ranges::reverse(bytes);

    return bytes;
}

/**
 * @brief Decides the origin radius of a merge table over an alphabet of symbols: the first effective self-pair when
 *        the merges taken in rank order meet one, and otherwise the radius and the mirrored constant the overlap graph
 *        gives.
 * @param merges The merge table in rank order.
 * @param symbols The alphabet's symbols, each spelled by its bytes, numbered in order.
 * @return The radius with the mirrored constant when finite, otherwise the first effective self-pair.
 * @throws std::invalid_argument If the alphabet holds a symbol twice, or a merge names a part the vocabulary so far
 *         does not carry, or a product it already carries.
 */
[[nodiscard]] Ripple_radius radius_over(const std::span<const Merge> merges, const std::span<const std::string> symbols)
{
    auto table{table_of(merges, symbols)};

    auto spellings{spellings_of(merges, symbols)};

    auto [useful, self_pair]{stages_of(table, spellings)};

    if (self_pair)
    {
        return Effective_self_pair{.rank = *self_pair};
    }

    auto vocabulary{vocabulary_of(table, merges, std::move(spellings), std::move(useful))};

    Exploration exploration{std::move(table), std::move(vocabulary)};

    return std::move(exploration).run();
}

} // namespace

Ripple_radius ripple_radius(const std::span<const Merge> merges)
{
    const auto symbols{byte_alphabet()};

    return radius_over(merges, symbols);
}

Ripple_radius ripple_radius(const std::span<const Merge> merges, const std::string_view alphabet)
{
    const auto symbols{characters_of(alphabet)};

    return radius_over(merges, symbols);
}

} // namespace munch::verifier

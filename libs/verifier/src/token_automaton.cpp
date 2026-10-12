#include "munch/verifier/token_automaton.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
namespace
{
/**
 * @brief A token of the table's vocabulary.
 */
using Token_t = Token_automaton::Token_t;

/**
 * @brief A state of the token automaton.
 */
using State_t = Token_automaton::State_t;

/**
 * @brief The number of bytes, the symbols of the byte alphabet.
 */
constexpr std::size_t byte_count{256};

/**
 * @brief A node of the search: the state the last token entered and the number of the window's bytes spelled so far.
 */
struct Node
{
    /**
     * @brief Ordered member by member, so nodes key the map of interned ones.
     */
    auto operator<=>(const Node&) const = default;

    /**
     * @brief The state the last token entered.
     */
    State_t state{};

    /**
     * @brief The number of the window's bytes spelled so far, at least one and short of the window's length.
     */
    std::size_t spelled{};
};

/**
 * @brief How the search first reached a node: the node before it and the token read from there, or no node for a seed,
 *        whose token is the first of the run.
 */
struct Link
{
    /**
     * @brief The node the token was read from, std::nullopt for a seed.
     */
    std::optional<Node> previous{};

    /**
     * @brief The token read.
     */
    Token_t token{};
};

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
 * @brief The search under way: the token automaton searched, the window certificate decided, how the search first
 *        reached each node, and the nodes reached and not yet expanded.
 */
class Search
{
public:
    /**
     * @brief Starts the search of a window certificate on a token automaton, nothing searched yet.
     * @param automaton The token automaton.
     * @param window The window, nonempty.
     * @param origin The certified origin, a byte index of the window.
     */
    Search(const Token_automaton& automaton, std::string_view window, std::size_t origin);

    /**
     * @brief Searches for a miscovering, first a token holding the whole window and then the runs of two or more tokens
     *        over one occurrence of it.
     * @return The first miscovering found, or std::nullopt when the certificate holds.
     */
    [[nodiscard]] std::optional<Miscovering> run() &&;

private:
    /**
     * @brief Returns the miscovering of one token holding the whole window, unless it holds it only at its start and
     *        the origin is zero, the first such token in number order.
     * @return The token alone, marked with no boundary, and the occurrence, or std::nullopt when no token is one.
     */
    [[nodiscard]] std::optional<Miscovering> held_miscovering() const;

    /**
     * @brief Searches the runs of two or more tokens over one occurrence of the window for a miscovering, from the
     *        seeds, the tokens whose spelling ends with a proper prefix of the window, through the tokens inside the
     *        window.
     * @return The first miscovering found, or std::nullopt when the search exhausts the nodes.
     */
    [[nodiscard]] std::optional<Miscovering> chained_miscovering();

    /**
     * @brief Returns, for each number k of the window's bytes spelled other than zero and the origin, the tokens a
     *        transition may read whose spelling begins with the rest of the window, in number order.
     * @return The covering tokens by k, empty at zero and at the origin.
     */
    [[nodiscard]] std::vector<std::vector<Token_t>> covering_tokens() const;

    /**
     * @brief Records how a node was first reached and queues it, unless the search has reached it before.
     * @param node The node reached.
     * @param link The node before it and the token read from there.
     */
    void reach(Node node, Link link);

    /**
     * @brief Returns the miscovering a run of tokens spells: the first token's tail from the occurrence on, the tokens
     *        inside the window, and the token that contains the window's final byte, each with a boundary after it but
     *        the last.
     * @param node The node the covering token was read from.
     * @param covering The covering token.
     * @return The run's segmentation and the occurrence of the window in it.
     */
    [[nodiscard]] Miscovering miscovering_of(Node node, Token_t covering) const;

    /**
     * @brief The certified origin.
     */
    std::size_t origin_{};

    /**
     * @brief The window, nonempty.
     */
    std::string_view window_{};

    /**
     * @brief How the search first reached each node.
     */
    std::map<Node, Link> links_{};

    /**
     * @brief The nodes reached and not yet expanded.
     */
    std::vector<Node> frontier_{};

    /**
     * @brief The token automaton searched.
     */
    const Token_automaton& automaton_;
};

Search::Search(const Token_automaton& automaton, const std::string_view window, const std::size_t origin)
    : origin_{origin}, window_{window}, automaton_{automaton}
{}

std::optional<Miscovering> Search::run() &&
{
    if (auto held{held_miscovering()})
    {
        return held;
    }

    return chained_miscovering();
}

std::optional<Miscovering> Search::held_miscovering() const
{
    for (const auto& [spelling, left, target] : automaton_.tokens())
    {
        const auto first{spelling.find(window_)};

        if (!target || first == std::string::npos)
        {
            continue;
        }

        // An occurrence at the token's start is covered from the origin when the origin is zero.
        const auto occurrence{first == 0 && origin_ == 0 ? spelling.find(window_, 1) : first};

        if (occurrence == std::string::npos)
        {
            continue;
        }

        return Miscovering{
                .segmentation = {.bytes = spelling, .boundaries = std::vector<bool>(spelling.size(), false)},
                .occurrence = occurrence};
    }

    return std::nullopt;
}

std::optional<Miscovering> Search::chained_miscovering()
{
    const auto covering{covering_tokens()};

    for (std::size_t spelled{1}; spelled < window_.size(); ++spelled)
    {
        const auto prefix{window_.substr(0, spelled)};

        for (Token_t token{0}; token < automaton_.tokens().size(); ++token)
        {
            const auto& [spelling, left, target]{automaton_.tokens()[token]};

            if (target && spelling.ends_with(prefix))
            {
                reach({.state = *target, .spelled = spelled}, {.token = token});
            }
        }
    }

    const auto spelling_length{[](const Token_automaton::Token& token) { return token.spelling.size(); }};

    const auto lengths{automaton_.tokens() | std::views::transform(spelling_length)};

    const auto longest{std::ranges::fold_left(lengths, std::size_t{0}, std::ranges::max)};

    while (!frontier_.empty())
    {
        const auto node{frontier_.back()};

        frontier_.pop_back();

        const auto [state, spelled]{node};

        const auto is_readable{[&](const Token_t token) { return automaton_.is_allowed(state, token); }};

        const auto found{std::ranges::find_if(covering[spelled], is_readable)};

        if (found != covering[spelled].end())
        {
            return miscovering_of(node, *found);
        }

        const auto rest{window_.substr(spelled)};

        // A piece longer than the longest token names none.
        const auto probed{std::min(rest.size(), longest + 1)};

        for (std::size_t length{1}; length < probed; ++length)
        {
            const auto piece{rest.substr(0, length)};

            const auto inner{automaton_.token_of(piece)};

            if (!inner || !is_readable(*inner))
            {
                continue;
            }

            const auto entered{*automaton_.tokens()[*inner].target};

            reach({.state = entered, .spelled = spelled + length}, {.previous = node, .token = *inner});
        }
    }

    return std::nullopt;
}

std::vector<std::vector<Token_t>> Search::covering_tokens() const
{
    std::vector<std::vector<Token_t>> covering(window_.size());

    for (std::size_t spelled{1}; spelled < window_.size(); ++spelled)
    {
        if (spelled == origin_)
        {
            continue;
        }

        const auto rest{window_.substr(spelled)};

        for (Token_t token{0}; token < automaton_.tokens().size(); ++token)
        {
            const auto& [spelling, left, target]{automaton_.tokens()[token]};

            if (target && spelling.starts_with(rest))
            {
                covering[spelled].push_back(token);
            }
        }
    }

    return covering;
}

void Search::reach(const Node node, const Link link)
{
    const auto [entry, inserted]{links_.try_emplace(node, link)};

    if (inserted)
    {
        frontier_.push_back(node);
    }
}

Miscovering Search::miscovering_of(const Node node, const Token_t covering) const
{
    std::vector<Token_t> run{covering};

    auto seed{node};

    for (std::optional at{node}; at;)
    {
        const auto& [previous, token]{links_.at(*at)};

        run.push_back(token);
        seed = *at;
        at = previous;
    }

    std::ranges::reverse(run);

    Marked_string segmentation{};

    auto& [bytes, boundaries]{segmentation};

    for (const auto token : run)
    {
        bytes += automaton_.tokens()[token].spelling;

        boundaries.resize(bytes.size(), false);
        boundaries.back() = true;
    }

    boundaries.back() = false;

    const auto& first_spelling{automaton_.tokens()[run.front()].spelling};

    const auto occurrence{first_spelling.size() - seed.spelled};

    return {.segmentation = std::move(segmentation), .occurrence = occurrence};
}

} // namespace

Token_automaton::Token_automaton(const std::span<const Merge> merges, const std::span<const std::string> symbols)
    : states_{State{}}
{
    for (Token_t symbol{0}; symbol < symbols.size(); ++symbol)
    {
        const auto& spelling{symbols[symbol]};

        const auto [entry, inserted]{index_.try_emplace(spelling, symbol)};

        if (!inserted)
        {
            throw std::invalid_argument{
                    std::format("token_automaton: the alphabet carries {} twice", quoted(spelling))};
        }

        tokens_.push_back({.spelling = spelling, .target = base_state});
    }

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        const auto left_found{index_.find(left)};
        const auto right_found{index_.find(right)};

        if (left_found == index_.cend() || right_found == index_.cend())
        {
            const auto& missing{left_found == index_.cend() ? left : right};

            throw std::invalid_argument{std::format(
                    "token_automaton: the merge ({}, {}) at rank {} names {}, which neither the alphabet nor an "
                    "earlier merge carries",
                    quoted(left), quoted(right), rank, quoted(missing))};
        }

        const auto& [left_spelling, left_token]{*left_found};
        const auto& [right_spelling, right_token]{*right_found};

        const auto spelling{left + right};

        const auto product{tokens_.size()};

        const auto [entry, inserted]{index_.try_emplace(spelling, product)};

        if (!inserted)
        {
            throw std::invalid_argument{std::format(
                    "token_automaton: the merge ({}, {}) at rank {} recreates the token {}, which the alphabet or an "
                    "earlier merge already carries",
                    quoted(left), quoted(right), rank, quoted(spelling))};
        }

        const auto copied{tokens_[left_token].target};

        const auto readable{copied && is_allowed(*copied, right_token)};

        if (!readable)
        {
            tokens_.push_back({.spelling = spelling, .left = left_token});

            continue;
        }

        std::vector removed{right_token};

        if (left_token == right_token)
        {
            removed.push_back(product);
        }

        tokens_.push_back({.spelling = spelling, .left = left_token, .target = tokens_[right_token].target});

        tokens_[left_token].target = states_.size();

        states_.push_back({.parent = *copied, .birth = product + 1, .removed = std::move(removed)});
    }
}

const std::vector<Token_automaton::Token>& Token_automaton::tokens() const noexcept
{
    return tokens_;
}

std::optional<Token_automaton::Token_t> Token_automaton::token_of(const std::string_view spelling) const
{
    const auto found{index_.find(std::string{spelling})};

    if (found == index_.cend())
    {
        return std::nullopt;
    }

    const auto& [key, token]{*found};

    return token;
}

Token_automaton::State_t Token_automaton::state_count() const noexcept
{
    return states_.size();
}

bool Token_automaton::is_allowed(State_t state, Token_t token) const
{
    if (!tokens_[token].target)
    {
        return false;
    }

    while (state != base_state)
    {
        const auto& [parent, birth, removed]{states_[state]};

        if (token >= birth)
        {
            token = *tokens_[token].left;

            continue;
        }

        if (std::ranges::contains(removed, token))
        {
            return false;
        }

        state = parent;
    }

    return true;
}

std::vector<Token_automaton::Token_t> Token_automaton::allowed_tokens(const State_t state) const
{
    std::vector<Token_t> allowed{};

    for (Token_t token{0}; token < tokens_.size(); ++token)
    {
        if (is_allowed(state, token))
        {
            allowed.push_back(token);
        }
    }

    return allowed;
}

Token_automaton token_automaton(const std::span<const Merge> merges)
{
    const auto symbols{byte_alphabet()};

    return Token_automaton{merges, symbols};
}

Token_automaton token_automaton(const std::span<const Merge> merges, const std::string_view alphabet)
{
    const auto symbols{characters_of(alphabet)};

    return Token_automaton{merges, symbols};
}

std::optional<Miscovering> miscovering(
        const Token_automaton& automaton, const std::string_view window, const std::size_t origin)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"miscovering: the window is empty or its origin lies outside it"};
    }

    Search search{automaton, window, origin};

    return std::move(search).run();
}

} // namespace munch::verifier

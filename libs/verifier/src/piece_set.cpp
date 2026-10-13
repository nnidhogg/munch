#include "munch/verifier/piece_set.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::verifier
{
namespace
{
/**
 * @brief The number of bytes, the width of a row of the pair table.
 */
constexpr std::size_t byte_count{256};

/**
 * @brief Returns a piece's spelling as a refusal quotes it: a printable ASCII byte as itself, the quote and the
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
 * @brief Returns the characters of a text, each spelled by its bytes: the text split before every byte that is not a
 *        UTF-8 continuation byte.
 * @param text The text.
 * @return The characters, in order.
 */
[[nodiscard]] std::vector<std::string> characters_of(const std::string_view text)
{
    constexpr unsigned char continuation_mask{0xC0};
    constexpr unsigned char continuation_bits{0x80};

    std::vector<std::string> characters{};

    for (const auto byte : text)
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
 * @brief Returns the refutation of a cut by a piece placed across it that agrees with the window: the window extended
 *        by the piece's bytes that hang past either end, marked with the piece as one token and every other character
 *        as one, and the window's start in it.
 * @param window The window.
 * @param offset The cut, the index of the window's first byte after it.
 * @param piece The piece.
 * @param placed The index of the piece's byte placed on the window's byte at the offset, between two of its bytes.
 * @return The refutation, with no boundary at the cut.
 */
[[nodiscard]] Straddling straddling(
        const std::string_view window, const std::size_t offset, const std::string_view piece, const std::size_t placed)
{
    const auto before{placed > offset ? placed - offset : 0};
    const auto past{std::min(piece.size(), window.size() - offset + placed)};

    const auto left{piece.substr(0, before)};
    const auto right{piece.substr(past)};

    std::string text{left};

    text += window;
    text += right;

    const auto piece_begin{before + offset - placed};

    auto tokens{characters_of(text.substr(0, piece_begin))};

    tokens.emplace_back(piece);

    const auto after{characters_of(text.substr(piece_begin + piece.size()))};

    tokens.insert(tokens.end(), after.begin(), after.end());

    Marked_string segmentation{};

    auto& [bytes, boundaries]{segmentation};

    for (const auto& token : tokens)
    {
        bytes += token;

        boundaries.resize(bytes.size(), false);
        boundaries.back() = true;
    }

    boundaries.back() = false;

    return {.segmentation = std::move(segmentation), .occurrence = before};
}

} // namespace

Piece_set::Piece_set(const std::span<const std::string> pieces)
    : pieces_{pieces.begin(), pieces.end()}, nodes_{Node{}}, pairs_(pair_count, false)
{
    for (std::size_t index{0}; index < pieces_.size(); ++index)
    {
        const auto& piece{pieces_[index]};

        if (piece.empty())
        {
            throw std::invalid_argument{std::format("piece_set: the piece at index {} is empty", index)};
        }

        add_piece(piece);

        longest_ = std::max(longest_, piece.size());
    }

    link_failures();
}

void Piece_set::add_piece(const std::string_view piece)
{
    Node_t node{root};

    for (const auto byte : piece)
    {
        const auto [edge, inserted]{nodes_[node].children.try_emplace(static_cast<unsigned char>(byte), nodes_.size())};

        const auto child{edge->second};

        if (inserted)
        {
            nodes_.emplace_back();
        }

        node = child;
    }

    if (nodes_[node].ending == piece.size())
    {
        throw std::invalid_argument{std::format("piece_set: the pieces carry {} twice", quoted(piece))};
    }

    nodes_[node].ending = piece.size();

    for (std::size_t index{1}; index < piece.size(); ++index)
    {
        pairs_[pair_index(piece[index - 1], piece[index])] = true;
    }
}

std::size_t Piece_set::pair_index(const char first, const char second) noexcept
{
    const auto row{static_cast<unsigned char>(first)};
    const auto column{static_cast<unsigned char>(second)};

    return row * byte_count + column;
}

void Piece_set::link_failures()
{
    std::deque<Node_t> frontier{};

    for (const auto& [byte, child] : nodes_[root].children)
    {
        frontier.push_back(child);
    }

    while (!frontier.empty())
    {
        const auto node{frontier.front()};

        frontier.pop_front();

        for (const auto& [byte, child] : nodes_[node].children)
        {
            const auto failure{next(nodes_[node].failure, byte)};

            nodes_[child].failure = failure;
            nodes_[child].ending = std::max(nodes_[child].ending, nodes_[failure].ending);

            frontier.push_back(child);
        }
    }
}

Piece_set::Node_t Piece_set::next(Node_t node, const unsigned char byte) const
{
    while (true)
    {
        const auto& children{nodes_[node].children};

        const auto found{children.find(byte)};

        if (found != children.cend())
        {
            return found->second;
        }

        if (node == root)
        {
            return root;
        }

        node = nodes_[node].failure;
    }
}

const std::vector<std::string>& Piece_set::pieces() const noexcept
{
    return pieces_;
}

std::size_t Piece_set::longest() const noexcept
{
    return longest_;
}

bool Piece_set::has_pair(const char first, const char second) const noexcept
{
    return pairs_[pair_index(first, second)];
}

std::vector<std::size_t> Piece_set::straddle_free(const std::string_view input) const
{
    const auto size{input.size()};

    // The furthest end of an occurrence of two bytes or more beginning at each byte, zero for none.
    std::vector<std::size_t> reach(size + 1, 0);

    Node_t node{root};

    for (std::size_t end{1}; end <= size; ++end)
    {
        node = next(node, static_cast<unsigned char>(input[end - 1]));

        const auto length{nodes_[node].ending};

        if (length < 2)
        {
            continue;
        }

        // The ends increase, so the last occurrence recorded at a start reaches furthest.
        reach[end - length] = end;
    }

    std::vector<std::size_t> cuts{};

    std::size_t furthest{0};

    for (std::size_t position{1}; position < size; ++position)
    {
        furthest = std::max(furthest, reach[position - 1]);

        if (furthest <= position)
        {
            cuts.push_back(position);
        }
    }

    return cuts;
}

Piece_set piece_set(const std::span<const std::string> pieces)
{
    return Piece_set{pieces};
}

std::optional<Straddling> piece_cut(const Piece_set& set, const std::string_view window, const std::size_t offset)
{
    if (window.empty() || offset == 0 || offset >= window.size())
    {
        throw std::invalid_argument{
                "piece_cut: the window is empty or its offset does not lie between two of its bytes"};
    }

    if (!set.has_pair(window[offset - 1], window[offset]))
    {
        return std::nullopt;
    }

    // The piece placed with its byte j on the window's byte at the offset, compared where the two overlap.
    const auto agrees{[&window, offset](const std::string_view piece, const std::size_t j) {
        const auto first{j > offset ? j - offset : 0};
        const auto last{std::min(piece.size(), window.size() - offset + j)};

        const auto overlap{piece.substr(first, last - first)};

        const auto begin{offset + first - j};

        return window.substr(begin, last - first) == overlap;
    }};

    for (const auto& piece : set.pieces())
    {
        for (std::size_t j{1}; j < piece.size(); ++j)
        {
            if (agrees(piece, j))
            {
                return straddling(window, offset, piece, j);
            }
        }
    }

    return std::nullopt;
}

} // namespace munch::verifier

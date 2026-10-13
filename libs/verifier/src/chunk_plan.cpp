#include "munch/verifier/chunk_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "munch/verifier/verifier_decisions.hpp"

namespace munch::verifier
{
namespace
{
/**
 * @brief A token of the automaton's table, as Token_automaton numbers it.
 */
using Token_t = Token_automaton::Token_t;

/**
 * @brief The number of bytes, the symbols of a byte table's automaton, each numbered by its value.
 */
constexpr std::size_t byte_count{256};

/**
 * @brief A cut the search found: its position, the bytes a worker reads past it, and its evidence.
 */
struct Found_cut
{
    /**
     * @brief The cut, the index of the first byte after it.
     */
    std::size_t position{};

    /**
     * @brief The bytes a worker whose chunk ends at the cut reads past it.
     */
    std::size_t context{};

    /**
     * @brief The evidence licensing the cut.
     */
    Cut_evidence evidence{};
};

/**
 * @brief The encoder of a token automaton's byte-pair table: the lowest-ranked merge present anywhere applied at its
 *        leftmost occurrence until none applies, each merge read back from the automaton's products.
 */
class Table_encoder
{
public:
    /**
     * @brief Reads the table back from the automaton's tokens: each product's left part, its right part the token
     *        spelling the rest of the product, and its rank past the symbols.
     * @param automaton The token automaton of a byte table.
     * @throws std::invalid_argument If the automaton's symbols are not the 256 bytes, each numbered by its value, as
     *         token_automaton(merges, alphabet) numbers the characters of its alphabet.
     */
    explicit Table_encoder(const Token_automaton& automaton);

    /**
     * @brief Returns where the token covering a byte begins in a text's own encoding.
     * @param text The text.
     * @param byte The byte, an index of the text.
     * @return The index of the covering token's first byte, at most the byte.
     */
    [[nodiscard]] std::size_t covering_start(std::string_view text, std::size_t byte) const;

private:
    /**
     * @brief Encodes a text: at every step the lowest-ranked merge present anywhere, applied at its leftmost
     *        occurrence, until none applies.
     * @param text The text.
     * @return The tokens.
     */
    [[nodiscard]] std::vector<Token_t> encoded(std::string_view text) const;

    /**
     * @brief Returns where the next merge of an encoding applies: the leftmost occurrence of the lowest-ranked merge
     *        present in the tokens, as its index and rank.
     * @param tokens The tokens.
     * @return The index of the occurrence's left part and the merge's rank, std::nullopt when no merge applies.
     */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> next_merge(
            const std::vector<Token_t>& tokens) const;

    /**
     * @brief The token automaton, whose tokens spell the encoding.
     */
    const Token_automaton& automaton_;

    /**
     * @brief The number of symbols, which the tokens carry before the products.
     */
    std::size_t symbol_count_{0};

    /**
     * @brief The rank of every merge by its two parts.
     */
    std::map<std::pair<Token_t, Token_t>, std::size_t> ranks_{};
};

Table_encoder::Table_encoder(const Token_automaton& automaton) : automaton_{automaton}
{
    const auto& tokens{automaton_.tokens()};

    const auto is_symbol{[](const Token_automaton::Token& token) { return !token.left; }};

    const auto first_product{std::ranges::find_if_not(tokens, is_symbol)};

    symbol_count_ = static_cast<std::size_t>(first_product - tokens.begin());

    const auto spells_its_byte{[&tokens](const std::size_t symbol) {
        const std::string byte{static_cast<char>(symbol)};

        return tokens[symbol].spelling == byte;
    }};

    const auto symbols{std::views::iota(std::size_t{0}, symbol_count_)};

    const auto reads_bytes{symbol_count_ == byte_count && std::ranges::all_of(symbols, spells_its_byte)};

    if (!reads_bytes)
    {
        throw std::invalid_argument{std::format(
                "chunk_plan: the token automaton reads {} symbols, the characters of an alphabet, where the plan takes "
                "the automaton of a byte table, whose symbols are the {} bytes",
                symbol_count_, byte_count)};
    }

    for (auto token{symbol_count_}; token < tokens.size(); ++token)
    {
        const auto& [spelling, left, target]{tokens[token]};

        const auto left_length{tokens[*left].spelling.size()};

        const auto right_spelling{std::string_view{spelling}.substr(left_length)};

        const auto right{automaton_.token_of(right_spelling)};

        ranks_.emplace(std::pair{*left, *right}, token - symbol_count_);
    }
}

std::size_t Table_encoder::covering_start(const std::string_view text, const std::size_t byte) const
{
    const auto& tokens{automaton_.tokens()};

    std::size_t start{0};

    for (const auto token : encoded(text))
    {
        const auto end{start + tokens[token].spelling.size()};

        if (byte < end)
        {
            break;
        }

        start = end;
    }

    return start;
}

std::vector<Token_t> Table_encoder::encoded(const std::string_view text) const
{
    std::vector<Token_t> tokens{};

    for (const auto byte : text)
    {
        tokens.push_back(static_cast<unsigned char>(byte));
    }

    for (auto next{next_merge(tokens)}; next; next = next_merge(tokens))
    {
        const auto [index, rank]{*next};

        tokens[index] = symbol_count_ + rank;
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(index) + 1);
    }

    return tokens;
}

std::optional<std::pair<std::size_t, std::size_t>> Table_encoder::next_merge(const std::vector<Token_t>& tokens) const
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

        if (!best || rank < best->second)
        {
            best = std::pair{index, rank};
        }
    }

    return best;
}

/**
 * @brief The route over a verifier: a window certified at its last byte with every certified cut independent, which
 *        a worker beginning at the chunk's first byte must still read.
 */
class Verifier_route
{
public:
    /**
     * @brief Whether a window must begin at or after the chunk's first byte, so that the worker reads it.
     */
    static constexpr bool spaced{true};

    /**
     * @brief Takes the verifier.
     * @param verifier The verifier, functional.
     */
    explicit Verifier_route(const Verifier& verifier);

    /**
     * @brief Decides a window: the origin pinned_cut() certifies at its last byte, when dependence() finds every
     *        certified cut of the pair independent.
     * @param window The window, nonempty.
     * @return The certified origin, or std::nullopt when the window certifies no independent cut.
     */
    [[nodiscard]] std::optional<std::size_t> certified_origin(std::string_view window) const;

    /**
     * @brief Returns the uniform window at a byte, which a verifier has none of, carrying no table to encode it.
     * @param input The input.
     * @param byte The byte walked.
     * @return std::nullopt.
     */
    [[nodiscard]] std::optional<Uniform_window> uniform_window(std::string_view input, std::size_t byte) const;

private:
    /**
     * @brief The verifier.
     */
    const Verifier& verifier_;
};

Verifier_route::Verifier_route(const Verifier& verifier) : verifier_{verifier}
{}

std::optional<std::size_t> Verifier_route::certified_origin(const std::string_view window) const
{
    const auto pinned{pinned_cut(verifier_, window, window.size() - 1)};

    if (!std::holds_alternative<std::size_t>(pinned))
    {
        return std::nullopt;
    }

    const auto origin{std::get<std::size_t>(pinned)};

    const auto dependent{dependence(verifier_, window, origin)};

    if (dependent)
    {
        return std::nullopt;
    }

    return origin;
}

std::optional<Uniform_window> Verifier_route::uniform_window(
        [[maybe_unused]] const std::string_view input, [[maybe_unused]] const std::size_t byte) const
{
    return std::nullopt;
}

/**
 * @brief The route over the token automaton of a byte-pair table: a window certified at the start its last byte has in
 *        its own encoding, anywhere in the input, and the uniform window when the radii are given.
 */
class Automaton_route
{
public:
    /**
     * @brief Whether a window must begin at or after the chunk's first byte; the bytes on either side of a boundary are
     *        encoded alone as the whole input encodes them, so it need not.
     */
    static constexpr bool spaced{false};

    /**
     * @brief Takes the automaton and the radii, reading the table back from the automaton.
     * @param automaton The token automaton of the byte-pair table.
     * @param radii The origin radius and the right start radius, or ceilings on them, std::nullopt for no uniform
     *        window.
     */
    Automaton_route(const Token_automaton& automaton, std::optional<Uniform_radii> radii);

    /**
     * @brief Decides a window at the start its last byte has in its own encoding, when miscovering() on the automaton
     *        finds no miscovering there.
     * @param window The window, nonempty.
     * @return The certified origin, or std::nullopt when the window is refused there.
     */
    [[nodiscard]] std::optional<std::size_t> certified_origin(std::string_view window) const;

    /**
     * @brief Returns the uniform window whose byte R is the byte walked, its pinned byte's start read off its own
     *        encoding, when the radii are given and the window lies in the input.
     * @param input The input.
     * @param byte The byte walked.
     * @return The uniform window, or std::nullopt when there are no radii or the window does not fit the input there.
     */
    [[nodiscard]] std::optional<Uniform_window> uniform_window(std::string_view input, std::size_t byte) const;

private:
    /**
     * @brief The token automaton.
     */
    const Token_automaton& automaton_;

    /**
     * @brief The encoder of the automaton's table.
     */
    Table_encoder encoder_;

    /**
     * @brief The radii, or ceilings on them, std::nullopt for no uniform window.
     */
    std::optional<Uniform_radii> radii_{};
};

Automaton_route::Automaton_route(const Token_automaton& automaton, const std::optional<Uniform_radii> radii)
    : automaton_{automaton}, encoder_{automaton}, radii_{radii}
{}

std::optional<std::size_t> Automaton_route::certified_origin(const std::string_view window) const
{
    const auto origin{encoder_.covering_start(window, window.size() - 1)};

    const auto refuted{miscovering(automaton_, window, origin)};

    if (refuted)
    {
        return std::nullopt;
    }

    return origin;
}

std::optional<Uniform_window> Automaton_route::uniform_window(
        const std::string_view input, const std::size_t byte) const
{
    if (!radii_)
    {
        return std::nullopt;
    }

    const auto [radius, start_radius]{*radii_};

    const auto width{radius + start_radius + 1};

    const auto fits{byte >= radius && byte - radius + width <= input.size()};

    if (!fits)
    {
        return std::nullopt;
    }

    const auto begin{byte - radius};

    const auto window{input.substr(begin, width)};

    const auto start{encoder_.covering_start(window, radius)};

    return Uniform_window{.begin = begin, .width = width, .byte = radius, .start = start};
}

/**
 * @brief The search of one plan over a route: the input, the request, and the window decisions made so far by their
 *        bytes, with the asks counted.
 * @tparam Route The route, Verifier_route or Automaton_route.
 */
template <typename Route>
class Search
{
public:
    /**
     * @brief Takes the route, the input and the request, nothing decided yet.
     * @param route The route.
     * @param input The input.
     * @param request The chunks aimed for, the budget and the supplied cuts.
     */
    Search(const Route& route, std::string_view input, const Plan_request& request);

    /**
     * @brief Plans the chunks: each interior cut the first found at or after the later of its equal-division target
     *        and one past the previous cut, and the targets from the first with none dropped.
     * @return The plan.
     */
    [[nodiscard]] Chunk_plan run() &&;

private:
    /**
     * @brief Walks the bytes from a floor and returns the first cut at or after it: at each byte a supplied cut, then
     *        the windows ending there, then the uniform window.
     * @param begin The first byte of the chunk the cut ends.
     * @param floor The byte the walk starts at, after the chunk's first byte.
     * @return The cut, or std::nullopt when none lies at or after the floor.
     */
    [[nodiscard]] std::optional<Found_cut> cut_from(std::size_t begin, std::size_t floor);

    /**
     * @brief Returns the supplied cut at a byte.
     * @param byte The byte.
     * @return The supplied cut there, or std::nullopt when the caller supplies none.
     */
    [[nodiscard]] std::optional<Found_cut> supplied_at(std::size_t byte) const;

    /**
     * @brief Tries the windows ending at a byte from the narrowest to the budget, and returns the first certified one's
     *        cut when it lies at or after the floor; a certified window whose cut lies before it is not widened, since
     *        every wider window ending at the byte certifies the same cut.
     * @param begin The first byte of the chunk the cut ends.
     * @param floor The least cut taken.
     * @param byte The windows' last byte.
     * @return The cut, or std::nullopt when no window within the budget gives one at or after the floor.
     */
    [[nodiscard]] std::optional<Found_cut> window_at(std::size_t begin, std::size_t floor, std::size_t byte);

    /**
     * @brief Returns a window's certified origin, deciding each window once by its bytes.
     * @param first The index of the window's first byte.
     * @param width The window's width.
     * @return The certified origin, or std::nullopt when the route refuses the window.
     */
    [[nodiscard]] std::optional<std::size_t> certified_origin(std::size_t first, std::size_t width);

    /**
     * @brief The route.
     */
    const Route& route_;

    /**
     * @brief The input.
     */
    std::string_view input_{};

    /**
     * @brief The chunks aimed for, the budget and the supplied cuts.
     */
    Plan_request request_{};

    /**
     * @brief The window decisions made so far, by the window's bytes.
     */
    std::map<std::string, std::optional<std::size_t>, std::less<>> decisions_{};

    /**
     * @brief The window decisions asked for so far, repeats included.
     */
    std::size_t asked_{0};
};

template <typename Route>
Search<Route>::Search(const Route& route, const std::string_view input, const Plan_request& request)
    : route_{route}, input_{input}, request_{request}
{}

template <typename Route>
Chunk_plan Search<Route>::run() &&
{
    const auto size{input_.size()};

    // A chunk needs at least one byte, so asking for more chunks than bytes adds targets that can find nothing.
    const auto usable{std::min(request_.chunks, size)};

    std::vector<Planned_chunk> chunks{};

    std::size_t begin{0};

    for (std::size_t index{1}; index < usable; ++index)
    {
        const auto target{index * (size / usable) + index * (size % usable) / usable};

        const auto floor{std::max(target, begin + 1)};

        const auto found{cut_from(begin, floor)};

        // Every later floor lies at or after this one, so no later target finds a cut either.
        if (!found)
        {
            break;
        }

        const auto& [position, context, evidence]{*found};

        const auto read_end{std::min(size, position + context)};

        chunks.push_back({.begin = begin, .owned_end = position, .read_end = read_end, .evidence = evidence});

        begin = position;
    }

    chunks.push_back({.begin = begin, .owned_end = size, .read_end = size, .evidence = Input_end{}});

    return {.chunks = std::move(chunks), .windows_asked = asked_, .windows_decided = decisions_.size()};
}

template <typename Route>
std::optional<Found_cut> Search<Route>::cut_from(const std::size_t begin, const std::size_t floor)
{
    for (auto byte{floor}; byte < input_.size(); ++byte)
    {
        if (auto supplied{supplied_at(byte)})
        {
            return supplied;
        }

        if (auto window{window_at(begin, floor, byte)})
        {
            return window;
        }

        const auto uniform{route_.uniform_window(input_, byte)};

        if (!uniform)
        {
            continue;
        }

        const auto position{uniform->begin + uniform->start};

        if (position >= floor)
        {
            return Found_cut{.position = position, .context = 0, .evidence = *uniform};
        }
    }

    return std::nullopt;
}

template <typename Route>
std::optional<Found_cut> Search<Route>::supplied_at(const std::size_t byte) const
{
    const auto found{std::ranges::lower_bound(request_.supplied, byte, {}, &Supplied_cut::position)};

    if (found == request_.supplied.end() || found->position != byte)
    {
        return std::nullopt;
    }

    return Found_cut{.position = byte, .context = found->context, .evidence = *found};
}

template <typename Route>
std::optional<Found_cut> Search<Route>::window_at(
        const std::size_t begin, const std::size_t floor, const std::size_t byte)
{
    const auto lowest{Route::spaced ? begin : std::size_t{0}};

    const auto widest{std::min(request_.budget, byte - lowest + 1)};

    for (std::size_t width{1}; width <= widest; ++width)
    {
        const auto first{byte + 1 - width};

        const auto origin{certified_origin(first, width)};

        if (!origin)
        {
            continue;
        }

        const auto position{first + *origin};

        if (position < floor)
        {
            return std::nullopt;
        }

        const Certified_window window{.begin = first, .width = width, .origin = *origin};

        return Found_cut{.position = position, .context = 0, .evidence = window};
    }

    return std::nullopt;
}

template <typename Route>
std::optional<std::size_t> Search<Route>::certified_origin(const std::size_t first, const std::size_t width)
{
    ++asked_;

    const auto window{input_.substr(first, width)};

    if (const auto known{decisions_.find(window)}; known != decisions_.end())
    {
        const auto& [known_window, origin]{*known};

        return origin;
    }

    const auto origin{route_.certified_origin(window)};

    decisions_.emplace(window, origin);

    return origin;
}

} // namespace

Chunk_plan chunk_plan(const Verifier& verifier, const std::string_view input, const Plan_request& request)
{
    const Verifier_route route{verifier};

    Search search{route, input, request};

    return std::move(search).run();
}

Chunk_plan chunk_plan(
        const Token_automaton& automaton, const std::string_view input, const Plan_request& request,
        const std::optional<Uniform_radii> radii)
{
    const Automaton_route route{automaton, radii};

    Search search{route, input, request};

    return std::move(search).run();
}

} // namespace munch::verifier

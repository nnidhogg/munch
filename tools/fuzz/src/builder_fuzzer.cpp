#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/core/lexer.hpp"
#include "munch/core/window_planner.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/fuzz/reader.hpp"

namespace
{
using munch::core::Window_planner;
using munch::tools::fuzz::Reader;
using munch::tools::fuzz::require;

/**
 * @brief A scan's tokens, each a kind and a length, in stream order.
 */
using Stream_t = std::vector<std::pair<unsigned, std::size_t>>;

/**
 * @brief The most DFA states a fuzzed grammar may build into.
 */
constexpr std::size_t state_limit{256};

/**
 * @brief Decodes one byte-coded regex tree of bounded depth.
 *
 * At depth zero only leaves decode. Counted repetition expands the NFA by its count, so nesting compounds
 * multiplicatively; the library documents that bound as the caller's to enforce, and this harness enforces it by
 * allowing at most two nested ranges. Everything else, including nullable patterns such as a bare kleene, is fair game.
 * @param reader The fuzz input.
 * @param depth The deepest nesting left.
 * @param repeats The counted repetitions that may still nest.
 * @return The regex.
 */
munch::regex::Regex make_regex(Reader& reader, const std::size_t depth, const std::size_t repeats = 2)
{
    using namespace munch::regex;

    const auto kinds_here{[depth, repeats] {
        if (depth == 0)
        {
            return 2U;
        }

        return repeats == 0 ? 7U : 8U;
    }};

    // The left operand decodes before the right, which fixes the grammar an input yields.
    const auto operands{[&reader, depth, repeats] {
        auto left{make_regex(reader, depth - 1, repeats)};

        auto right{make_regex(reader, depth - 1, repeats)};

        return std::pair{std::move(left), std::move(right)};
    }};

    const auto kinds{kinds_here()};

    const auto kind{reader.byte() % kinds};

    switch (kind)
    {
    case 0:
        return text(static_cast<char>(reader.byte()));
    case 1:
        return any_of(Set{static_cast<char>(reader.byte()), static_cast<char>(reader.byte())});
    case 2:
    {
        auto [left, right]{operands()};

        return concat(std::move(left), std::move(right));
    }
    case 3:
    {
        auto [left, right]{operands()};

        return choice(std::move(left), std::move(right));
    }
    case 4:
    {
        auto operand{make_regex(reader, depth - 1, repeats)};

        return kleene(std::move(operand));
    }
    case 5:
    {
        auto operand{make_regex(reader, depth - 1, repeats)};

        return plus(std::move(operand));
    }
    case 6:
    {
        auto operand{make_regex(reader, depth - 1, repeats)};

        return optional(std::move(operand));
    }
    default:
    {
        const auto min{reader.byte() % 3U};

        // The operand is decoded before the bound's byte, which fixes the grammar an input yields.
        auto operand{make_regex(reader, depth - 1, repeats - 1)};

        const auto max{min + 1U + reader.byte() % 2U};

        return range(std::move(operand), min, max);
    }
    }
}

/**
 * @brief Decodes the token set into the builder: a count, then each token's expression before its priority byte, which
 *        fixes the grammar an input yields.
 * @param reader The fuzz input.
 * @param builder The builder receiving the tokens.
 * @return The number of tokens added, their IDs running from zero.
 */
unsigned add_tokens(Reader& reader, munch::core::Builder& builder)
{
    const auto count{1U + reader.byte() % 4U};

    for (unsigned token{0}; token < count; ++token)
    {
        const auto regex{make_regex(reader, 5)};

        // Two priority levels make equal-priority ties and shadowed (dead) tokens common rather than rare.
        const auto priority{reader.byte() % 2U};

        builder.add_token(regex, token, priority);
    }

    return count;
}

/**
 * @brief Scans the whole input serially and checks every token's ID and length and the total consumed.
 * @param lexer The lexer under test.
 * @param count The number of tokens in the set.
 * @param input The text to scan.
 * @return The serial stream and the bytes it consumed.
 */
std::pair<Stream_t, std::size_t> check_serial_scan(
        const munch::core::Lexer& lexer, const unsigned count, const std::string& input)
{
    Stream_t serial{};

    std::size_t total{0};

    const auto record{[&serial, &total, count](const unsigned token, const std::size_t length) {
        require(token < count);

        require(length > 0);

        serial.emplace_back(token, length);

        total += length;
    }};

    const auto consumed{lexer.tokenize_all<unsigned>(input, record)};

    require(consumed <= input.size() && total == consumed);

    return {std::move(serial), consumed};
}

/**
 * @brief Checks that the first token of the whole-input scan is exactly one longest-match attempt at offset zero.
 * @param lexer The lexer under test.
 * @param input The scanned text.
 * @param serial The serial stream of the whole input.
 */
void check_first_token(const munch::core::Lexer& lexer, const std::string& input, const Stream_t& serial)
{
    const auto [first_token, first_length]{lexer.tokenize<unsigned>(input)};

    require(first_length <= input.size());

    if (serial.empty())
    {
        return;
    }

    const auto& [serial_token, serial_length]{serial.front()};

    require(first_token == serial_token && first_length == serial_length);
}

/**
 * @brief Checks the shape of a plan's cuts: it spans the input, its interior cuts ascend strictly inside it, and each
 *        interior cut is certified.
 * @tparam Certified The type of the certification predicate.
 * @param cuts The plan's cuts, both ends included.
 * @param input_size The planned input's size.
 * @param certified The predicate returning whether an interior cut is certified.
 */
template <typename Certified>
void check_cuts(const std::vector<std::size_t>& cuts, const std::size_t input_size, const Certified& certified)
{
    require(cuts.front() == 0 && cuts.back() == input_size);

    for (std::size_t index{1}; index + 1 < cuts.size(); ++index)
    {
        require(cuts[index] > cuts[index - 1] && cuts[index] < input_size);

        require(certified(cuts[index]));
    }
}

/**
 * @brief Returns whether a cut is a certified byte or the reported origin of a certified window at most three bytes
 *        back, of two to four bytes.
 * @param lexer The lexer under test.
 * @param input The planned text.
 * @param cut The cut.
 * @return Whether it is certified.
 */
bool is_certified_cut(const munch::core::Lexer& lexer, const std::string_view input, const std::size_t cut)
{
    if (lexer.is_split_point(input[cut]))
    {
        return true;
    }

    const auto window_reports_cut{[&lexer, input, cut](const std::size_t back) {
        const auto start{cut - back};

        const auto fits{[&input, start](const std::size_t length) { return start + length <= input.size(); }};

        const auto reports_cut{[&lexer, input, start, back](const std::size_t length) {
            const auto window{input.substr(start, length)};

            const auto origin{lexer.is_split_window(window)};

            return origin == back;
        }};

        const auto shortest{std::max(Window_planner::shortest_window, back + 1)};

        const auto lengths{
                std::views::iota(shortest, Window_planner::longest_window + 1) | std::views::take_while(fits)};

        return std::ranges::any_of(lengths, reports_cut);
    }};

    const auto farthest_back{std::min(cut, Window_planner::longest_window - 1)};

    const auto backs{std::views::iota(std::size_t{0}, farthest_back + 1)};

    return std::ranges::any_of(backs, window_reports_cut);
}

/**
 * @brief Checks that the stream of each windowed chunk, scanned alone, consumes the chunk fully and that their
 *        concatenation equals the serial stream.
 *
 * On completely tokenizable input the window theorem promises exact agreement for non-nullable token sets, and a
 * nullable set's plan here is its byte plan, which promises the same, so fuzz-generated complete inputs hold the window
 * plan to it. On malformed input the windows contract deliberately promises nothing, so the caller checks complete
 * inputs only.
 * @param lexer The lexer under test.
 * @param input The planned text, completely tokenizable.
 * @param windowed The window plan's cuts.
 * @param serial The serial stream of the whole input.
 */
void check_rejoined(
        const munch::core::Lexer& lexer, const std::string_view input, const std::vector<std::size_t>& windowed,
        const Stream_t& serial)
{
    Stream_t rejoined{};

    const auto append{
            [&rejoined](const unsigned token, const std::size_t length) { rejoined.emplace_back(token, length); }};

    auto complete{true};

    for (std::size_t index{1}; index < windowed.size(); ++index)
    {
        const auto chunk{input.substr(windowed[index - 1], windowed[index] - windowed[index - 1])};

        const auto part{lexer.tokenize_all<unsigned>(chunk, append)};

        complete = complete && part == chunk.size();
    }

    require(complete && rejoined == serial);
}

/**
 * @brief Scans the byte plan's chunks in parallel and checks the concatenated streams against the serial one.
 *
 * The serial stream is a prefix of the concatenated chunk streams, not always their equal: a serial scan stops at the
 * first unmatched offset while chunks past it still scan. On full consumption the two must agree exactly, which is the
 * certified split-point guarantee this harness exists to attack.
 * @param lexer The lexer under test.
 * @param input The scanned text.
 * @param chunks The chunk count asked of the planner.
 * @param boundaries The byte plan's cuts.
 * @param serial The serial stream of the whole input.
 * @param consumed The bytes the serial scan consumed.
 */
void check_parallel(
        const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks,
        const std::vector<std::size_t>& boundaries, const Stream_t& serial, const std::size_t consumed)
{
    std::vector<Stream_t> streams(boundaries.size() - 1);

    const auto append{[&streams](const std::size_t chunk, const unsigned token, const std::size_t length) {
        streams[chunk].emplace_back(token, length);
    }};

    const auto consumed_per_chunk{lexer.tokenize_all_parallel<unsigned>(input, chunks, append)};

    Stream_t parallel{};

    for (const auto& stream : streams)
    {
        parallel.insert(parallel.end(), stream.begin(), stream.end());
    }

    require(parallel.size() >= serial.size());

    require(std::ranges::equal(serial, parallel | std::views::take(serial.size())));

    if (consumed != input.size())
    {
        return;
    }

    require(parallel.size() == serial.size());

    std::size_t sum{0};

    for (std::size_t chunk{0}; chunk < consumed_per_chunk.size(); ++chunk)
    {
        sum += consumed_per_chunk[chunk];

        require(consumed_per_chunk[chunk] == boundaries[chunk + 1] - boundaries[chunk]);
    }

    require(sum == input.size());
}

/**
 * @brief Builds the lexer and runs every check on it, in order: the serial scan, the first token, the byte plan, the
 *        window plan, the rejoined window chunks and the parallel scan.
 * @param builder The builder holding the decoded token set.
 * @param count The number of tokens in the set.
 * @param chunks The chunk count asked of the planners.
 * @param input The text to scan.
 */
void check_lexer(
        munch::core::Builder& builder, const unsigned count, const std::size_t chunks, const std::string& input)
{
    const auto lexer{builder.build()};

    std::ignore = builder.diagnose();

    const auto [serial, consumed]{check_serial_scan(lexer, count, input)};

    check_first_token(lexer, input, serial);

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    const auto is_split_cut{[&lexer, &input](const std::size_t cut) { return lexer.is_split_point(input[cut]); }};

    check_cuts(boundaries, input.size(), is_split_cut);

    // The explicit window planner: each interior cut must be a certified byte or the reported origin of a certified
    // window found at most three bytes back. Its stream guarantee is conditional on completely tokenizable input by
    // documentation, so the stream comparison is made on complete inputs only.
    const auto windowed{lexer.chunk_boundaries_with_windows(input, chunks)};

    const auto is_window_cut{[&lexer, &input](const std::size_t cut) { return is_certified_cut(lexer, input, cut); }};

    check_cuts(windowed, input.size(), is_window_cut);

    if (consumed == input.size())
    {
        check_rejoined(lexer, input, windowed, serial);
    }

    check_parallel(lexer, input, chunks, boundaries, serial, consumed);
}

} // namespace

/**
 * @brief Checks one fuzz input, the fuzz entry point run once per input.
 * @param data The fuzz input.
 * @param size The input's size.
 * @return Zero; a violated invariant traps instead.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Reader reader{std::span{data, size}};

    munch::core::Builder builder{};

    // Fuzzed grammars are the untrusted-input case the limit exists for; it also keeps every build fast.
    builder.set_state_limit(state_limit);

    const auto count{add_tokens(reader, builder)};

    const auto chunks{reader.byte() % 5U};

    const auto input{reader.remainder()};

    try
    {
        check_lexer(builder, count, chunks, input);
    }
    catch (const munch::core::State_limit_error&)
    {
        // The state limit fired; rejecting the grammar is the documented behavior for untrusted token sets. Any other
        // exception escapes and counts as a finding.
    }

    return 0;
}

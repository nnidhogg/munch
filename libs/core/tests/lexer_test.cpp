#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/determinize.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/core/window_planner.hpp"
#include "munch/dfa/boundary_search.hpp"
#include "munch/dfa/simulator.hpp"
#include "munch/dfa/tools/graphviz.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/regex/unicode.hpp"
#include "munch/regex/utf8.hpp"

using namespace munch;
using namespace munch::core;
using namespace munch::regex;

namespace
{
/**
 * @brief A Builder exposing its protected NFA and DFA, for tests reading the automata themselves.
 */
class Builder_dbg : public Builder
{
public:
    using Builder::dfa;
    using Builder::nfa;
};

/**
 * @brief A window, one of its gaps, and whether an occurrence was cut there rather than crossed.
 */
using Gap_key_t = std::tuple<std::string_view, std::size_t, bool>;

/**
 * @brief A token stream collected from a scan, each token with its length, and the bytes the scan consumed.
 * @tparam T The token type.
 */
template <typename T>
struct Scanned
{
    /**
     * @brief The tokens in input order, each with its length.
     */
    std::vector<std::pair<T, std::size_t>> tokens{};

    /**
     * @brief The number of input bytes the scan consumed.
     */
    std::size_t consumed{};
};

/**
 * @brief A conforming random-access byte iterator that additionally converts to char.
 *
 * Both its elements and the iterator object itself convert to char, so planning over it shows that the planners read
 * each window's bytes from the input's elements and never from the iterator objects. The operators the planners never
 * call exist for the random-access iterator concept alone.
 */
struct Char_convertible_iterator
{
    /**
     * @brief The element type.
     */
    using value_type = char;

    /**
     * @brief The distance type.
     */
    using difference_type = std::ptrdiff_t;

    /**
     * @brief Converts the iterator object itself to the byte it points at.
     * @return The byte.
     */
    operator char() const { return *position; }

    /**
     * @brief Reads the byte the iterator points at.
     * @return The byte.
     */
    char operator*() const { return *position; }

    /**
     * @brief Reads a byte at an offset from the iterator.
     * @param at The offset.
     * @return The byte.
     */
    char operator[](const difference_type at) const { return position[at]; }

    /**
     * @brief Advances by one.
     * @return This iterator.
     */
    Char_convertible_iterator& operator++()
    {
        ++position;

        return *this;
    }

    /**
     * @brief Advances by one.
     * @return The iterator before the step.
     */
    Char_convertible_iterator operator++(int) { return {.position = position++}; }

    /**
     * @brief Steps back by one.
     * @return This iterator.
     */
    Char_convertible_iterator& operator--()
    {
        --position;

        return *this;
    }

    /**
     * @brief Steps back by one.
     * @return The iterator before the step.
     */
    Char_convertible_iterator operator--(int) { return {.position = position--}; }

    /**
     * @brief Advances by a distance.
     * @param by The distance.
     * @return This iterator.
     */
    Char_convertible_iterator& operator+=(const difference_type by)
    {
        position += by;

        return *this;
    }

    /**
     * @brief Steps back by a distance.
     * @param by The distance.
     * @return This iterator.
     */
    Char_convertible_iterator& operator-=(const difference_type by)
    {
        position -= by;

        return *this;
    }

    /**
     * @brief Returns an iterator a distance further on.
     * @param it The iterator.
     * @param by The distance.
     * @return The advanced iterator.
     */
    [[maybe_unused]] friend Char_convertible_iterator operator+(
            const Char_convertible_iterator it, const difference_type by)
    {
        return {.position = it.position + by};
    }

    /**
     * @brief Returns an iterator a distance further on.
     * @param by The distance.
     * @param it The iterator.
     * @return The advanced iterator.
     */
    [[maybe_unused]] friend Char_convertible_iterator operator+(
            const difference_type by, const Char_convertible_iterator it)
    {
        return {.position = it.position + by};
    }

    /**
     * @brief Returns an iterator a distance back.
     * @param it The iterator.
     * @param by The distance.
     * @return The iterator stepped back.
     */
    [[maybe_unused]] friend Char_convertible_iterator operator-(
            const Char_convertible_iterator it, const difference_type by)
    {
        return {.position = it.position - by};
    }

    /**
     * @brief Returns the distance between two iterators.
     * @param lhs The later iterator.
     * @param rhs The earlier iterator.
     * @return The distance.
     */
    friend difference_type operator-(const Char_convertible_iterator lhs, const Char_convertible_iterator rhs)
    {
        return lhs.position - rhs.position;
    }

    /**
     * @brief Orders iterators by position.
     */
    [[maybe_unused]] friend auto operator<=>(const Char_convertible_iterator&, const Char_convertible_iterator&) =
            default;

    /**
     * @brief The position in the underlying string.
     */
    std::string::const_iterator position{};
};

static_assert(std::random_access_iterator<Char_convertible_iterator>);

/**
 * @brief Whether the tests export their NFA and DFA graphs in Graphviz DOT format, for debugging.
 */
constexpr bool debug_dot{false};

/**
 * @brief The number of distinct byte values.
 */
constexpr int byte_count{256};

/**
 * @brief The largest Unicode scalar value.
 */
constexpr char32_t max_code_point{0x10FFFF};

/**
 * @brief The first surrogate code point, which UTF-8 cannot carry.
 */
constexpr char32_t first_surrogate{0xD800};

/**
 * @brief The last surrogate code point.
 */
constexpr char32_t last_surrogate{0xDFFF};

/**
 * @brief The match of a scan that matched nothing.
 * @tparam T The token type.
 */
template <typename T>
const Lexer::Match<T> no_match{.token = std::nullopt, .length = 0};

/**
 * @brief Ignores every token of a serial scan.
 * @tparam T The token type.
 */
constexpr auto ignore_token{[]<typename T>(const T, const std::size_t) {}};

/**
 * @brief Ignores every token of a parallel scan.
 * @tparam T The token type.
 */
constexpr auto ignore_chunk_token{[]<typename T>(const std::size_t, const T, const std::size_t) {}};

/**
 * @brief Runs the window planner's walk without the mandatory-core filter: every position from the floor, lengths
 *        ascending, first certificate wins, refusal only at the end of the input.
 *
 * The filtered planner claims equality with this walk, so the equality tests hold the claim byte for byte. The walk
 * carries none of the planner's guards, so callers must first assert what the guards decide: a grammar with no split
 * points, no nullable token, an input of at least two bytes, and a chunk count of at least one, since the walk divides
 * by it where the planner returns the serial plan.
 * @param lexer The token set.
 * @param text The input to plan.
 * @param chunks The number of chunks aimed for, at least one.
 * @return Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
 */
std::vector<std::size_t> reference_window_walk(
        const Lexer& lexer, const std::string_view text, const std::size_t chunks)
{
    std::vector<std::size_t> boundaries{0};

    const auto size{text.size()};

    const auto usable{std::min(chunks, size)};

    const auto step{size / usable};

    const auto step_remainder{size % usable};

    std::map<std::string, std::optional<std::size_t>, std::less<>> memo{};

    std::size_t window_target{0};

    std::size_t window_carry{0};

    const auto decision{[&](const std::string& window) {
        if (const auto found{memo.find(window)}; found != memo.end())
        {
            const auto& [known_window, known_verdict]{*found};

            return known_verdict;
        }

        const auto verdict{lexer.is_split_window(window)};

        memo.emplace(window, verdict);

        return verdict;
    }};

    const auto cut_at{[&](const std::size_t occurrence) -> std::optional<std::size_t> {
        const auto limit{std::min(Window_planner::longest_window, size - occurrence)};

        for (auto length{Window_planner::shortest_window}; length <= limit; ++length)
        {
            const std::string window{text.substr(occurrence, length)};

            if (const auto origin{decision(window)})
            {
                return occurrence + *origin;
            }
        }

        return std::nullopt;
    }};

    for (std::size_t index{1}; index < usable; ++index)
    {
        window_target += step;

        window_carry += step_remainder;

        if (window_carry >= usable)
        {
            ++window_target;

            window_carry -= usable;
        }

        for (auto occurrence{std::max(window_target, boundaries.back() + 1)};
             occurrence + Window_planner::shortest_window <= size; ++occurrence)
        {
            const auto cut{cut_at(occurrence)};

            if (!cut)
            {
                continue;
            }

            boundaries.push_back(*cut);

            break;
        }
    }

    boundaries.push_back(size);

    return boundaries;
}

/**
 * @brief Expects the window planner to plan exactly as the reference walk at every chunk count of a spread, and every
 *        walk to make real cuts or to be one given refusal.
 *
 * The comparison must be over real cuts, or the equality would hold vacuously between empty plans; where the walk
 * refuses, the refusal must be the walk's own conclusion, reached through the same candidates, not a guess.
 * @param lexer The token set, under the reference walk's preconditions.
 * @param input The input to plan.
 * @param refusal The plan every walk must be, or std::nullopt when every walk must cut the input.
 */
void expect_planner_equals_walk(
        const Lexer& lexer, const std::string_view input, const std::optional<std::vector<std::size_t>>& refusal)
{
    for (const auto chunks : {2U, 3U, 5U, 8U, 13U})
    {
        const auto walk{reference_window_walk(lexer, input, chunks)};

        const auto planned{lexer.chunk_boundaries_with_windows(input, chunks)};

        EXPECT_EQ(planned, walk) << chunks;

        if (refusal)
        {
            EXPECT_EQ(walk, *refusal) << chunks;
        }
        else
        {
            EXPECT_GT(walk.size(), 2U) << chunks;
        }
    }
}

/**
 * @brief Returns where the tokens of a complete scan of an input start.
 * @param lexer The token set.
 * @param input The input.
 * @return The offset of every token in input order, or std::nullopt when the scan does not consume every byte.
 */
std::optional<std::vector<std::size_t>> token_starts(const Lexer& lexer, const std::string_view input)
{
    std::vector<std::size_t> starts{};

    std::size_t next{0};

    const auto record{[&starts, &next](const std::size_t, const std::size_t length) {
        starts.push_back(next);

        next += length;
    }};

    const auto consumed{lexer.tokenize_all<std::size_t>(input, record)};

    if (consumed != input.size())
    {
        return std::nullopt;
    }

    return starts;
}

/**
 * @brief Returns whether a witness is what window_counterexample() claims: a completely tokenizable input holding an
 *        occurrence of the window whose final byte is covered by a token beginning elsewhere than the origin, checked
 *        as the research oracle checks its own, by scanning the witness and reading the covering token's start off the
 *        scan.
 * @param lexer The token set.
 * @param witness The input claimed.
 * @param window The window of the certificate.
 * @param origin The origin of the certificate.
 * @return True when the witness tokenizes completely and some occurrence of the window in it fails the certificate.
 */
bool fails(const Lexer& lexer, const std::string_view witness, const std::string_view window, const std::size_t origin)
{
    const auto starts{token_starts(lexer, witness)};

    if (!starts)
    {
        return false;
    }

    // The token covering a byte begins at the last start at or before it; the first start is zero, so one exists.
    for (auto at{witness.find(window)}; at != std::string_view::npos; at = witness.find(window, at + 1))
    {
        const auto last_byte{at + window.size() - 1};

        const auto past_covering{std::ranges::upper_bound(*starts, last_byte)};

        const auto covering{std::prev(past_covering)};

        if (*covering != at + origin)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Returns the marking a token set gives an input, as the research oracle reads it off its reference scan: one
 *        bit per byte, set where a token boundary follows the byte, the final bit clear; nothing when the input leaves
 *        the domain.
 * @param lexer The token set.
 * @param input The input.
 * @return The marking, or std::nullopt when the scan does not consume every byte.
 */
std::optional<std::string> marking(const Lexer& lexer, const std::string_view input)
{
    std::string marks(input.size(), '0');

    std::size_t next{0};

    const auto mark{[&marks, &next, &input](const std::size_t, const std::size_t length) {
        next += length;

        if (next < input.size())
        {
            marks[next - 1] = '1';
        }
    }};

    const auto consumed{lexer.tokenize_all<std::size_t>(input, mark)};

    if (consumed != input.size())
    {
        return std::nullopt;
    }

    return marks;
}

/**
 * @brief Returns whether a separation is what segmentation_difference() claims of it: a domain witness one token set
 *        tokenizes completely and the other does not, a boundary witness both do and mark apart.
 * @param lexer The token set the comparison started from.
 * @param other The token set compared against.
 * @param witness The input claimed.
 * @param half The half claimed.
 * @return True when the two scans of the witness bear the claim out.
 */
bool separates(const Lexer& lexer, const Lexer& other, const std::string_view witness, const dfa::Separation_half half)
{
    const auto mine{marking(lexer, witness)};

    const auto theirs{marking(other, witness)};

    if (half == dfa::Separation_half::domain)
    {
        return mine.has_value() != theirs.has_value();
    }

    return mine.has_value() && theirs.has_value() && *mine != *theirs;
}

/**
 * @brief Returns whether a witness is what boundary_counterexample() or crossing_counterexample() claims: a completely
 *        tokenizable input holding an occurrence of the window crossed by a token at the gap, or cut there, checked by
 *        scanning the witness and reading the boundaries off the scan, the token starts and the input's end.
 * @param lexer The token set.
 * @param witness The input claimed.
 * @param window The window asked about.
 * @param gap The gap of the window asked about.
 * @param cut Whether the occurrence claimed has a boundary at the gap, as against crossing_counterexample(), rather
 *        than a token crossing it, as against boundary_counterexample().
 * @return True when the witness tokenizes completely and some occurrence of the window in it is cut at the gap exactly
 *         when claimed.
 */
bool shows(
        const Lexer& lexer, const std::string_view witness, const std::string_view window, const std::size_t gap,
        const bool cut)
{
    auto boundaries{token_starts(lexer, witness)};

    if (!boundaries)
    {
        return false;
    }

    boundaries->push_back(witness.size());

    for (auto at{witness.find(window)}; at != std::string_view::npos; at = witness.find(window, at + 1))
    {
        if (std::ranges::binary_search(*boundaries, at + gap) == cut)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Returns the token starts maximal munch gives an input over a set of literal tokens, computed from the literals
 *        themselves rather than by any compiled machine: from every start the longest literal the input continues with
 *        is taken.
 * @param tokens The literals, non-empty and distinct.
 * @param input The input.
 * @return One flag per byte, set where a token begins, or std::nullopt when the scan stops before the end.
 */
std::optional<std::vector<bool>> munch_starts(const std::vector<std::string>& tokens, const std::string_view input)
{
    std::vector<bool> starts(input.size(), false);

    for (std::size_t at{0}; at < input.size();)
    {
        std::size_t longest{0};

        for (const auto& token : tokens)
        {
            if (token.size() > longest && input.substr(at).starts_with(token))
            {
                longest = token.size();
            }
        }

        if (longest == 0)
        {
            return std::nullopt;
        }

        starts[at] = true;

        at += longest;
    }

    return starts;
}

/**
 * @brief Records, by maximal munch over literal tokens, the shortest input holding an occurrence of each window cut or
 *        crossed at each of its gaps, the input's end a cut.
 * @param tokens The literals, non-empty and distinct.
 * @param inputs The inputs scanned, shorter ones first.
 * @param windows The windows, which the keys view.
 * @return The shortest input length per window, gap and whether the gap was cut; no entry where none was seen.
 */
std::map<Gap_key_t, std::size_t> shortest_occurrences(
        const std::vector<std::string>& tokens, const std::vector<std::string>& inputs,
        const std::vector<std::string>& windows)
{
    std::map<Gap_key_t, std::size_t> shortest{};

    const auto note{
            [&shortest](const std::string& input, const std::vector<bool>& starts, const std::string_view window) {
                for (auto at{input.find(window)}; at != std::string::npos; at = input.find(window, at + 1))
                {
                    for (std::size_t gap{0}; gap <= window.size(); ++gap)
                    {
                        const auto cut{at + gap == input.size() || starts[at + gap]};

                        shortest.try_emplace({window, gap, cut}, input.size());
                    }
                }
            }};

    for (const auto& input : inputs)
    {
        const auto starts{munch_starts(tokens, input)};

        if (!starts)
        {
            continue;
        }

        for (const auto& window : windows)
        {
            note(input, *starts, window);
        }
    }

    return shortest;
}

/**
 * @brief Lists every string over an alphabet from length one up to a bound, shorter strings first and each length in
 *        the alphabet's order.
 * @param alphabet The bytes the strings are made of.
 * @param longest The longest length enumerated.
 * @return The strings.
 */
std::vector<std::string> words(const std::string_view alphabet, const std::size_t longest)
{
    std::vector<std::string> out{};

    std::vector<std::string> layer{""};

    for (std::size_t length{1}; length <= longest; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : layer)
        {
            for (const auto byte : alphabet)
            {
                next.push_back(prefix + byte);
            }
        }

        out.insert(out.end(), next.begin(), next.end());

        layer = std::move(next);
    }

    return out;
}

/**
 * @brief Lists every set of one to a given number of distinct literals drawn from a pool, in the pool's order.
 * @param pool The literals drawn from.
 * @param largest The most literals a set holds.
 * @return The sets.
 */
std::vector<std::vector<std::string>> literal_sets(const std::vector<std::string>& pool, const std::size_t largest)
{
    std::vector<std::vector<std::string>> out{};

    std::vector<std::vector<std::size_t>> layer{{}};

    const auto literal_at{[&pool](const std::size_t picked) { return pool[picked]; }};

    for (std::size_t size{1}; size <= largest; ++size)
    {
        std::vector<std::vector<std::size_t>> next{};

        for (const auto& chosen : layer)
        {
            for (auto index{chosen.empty() ? 0 : chosen.back() + 1}; index < pool.size(); ++index)
            {
                auto extended{chosen};

                extended.push_back(index);

                const auto literals{extended | std::views::transform(literal_at)};

                out.emplace_back(literals.begin(), literals.end());

                next.push_back(std::move(extended));
            }
        }

        layer = std::move(next);
    }

    return out;
}

/**
 * @brief Builds a lexer over a list of token patterns at one priority, the kinds the patterns' indices.
 * @param tokens The patterns.
 * @return The lexer.
 */
Lexer regex_lexer(const std::vector<Regex>& tokens)
{
    Builder builder{};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        builder.add_token(tokens[index], index, 1);
    }

    return builder.build();
}

/**
 * @brief Builds a lexer over a set of literal tokens, one text token per literal at one priority, the kinds the
 *        literals' indices.
 * @param tokens The literals, non-empty and distinct.
 * @return The lexer.
 */
Lexer literal_lexer(const std::vector<std::string>& tokens)
{
    const auto as_text{[](const std::string& literal) { return text(literal); }};

    std::vector<Regex> patterns{};

    std::ranges::transform(tokens, std::back_inserter(patterns), as_text);

    return regex_lexer(patterns);
}

/**
 * @brief Whether Lexer::tokenize() over a container accepts the container type.
 *
 * The dependent probes for the byte-domain assertions: a requires-expression over concrete types is checked as ordinary
 * code, so viability is asked through these instead. One probe per public overload, because a combined expression
 * proves only that at least one call rejects a type, and a single overload could then quietly lose its constraint.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Single_scan_over =
        requires(const Lexer& lexer, const Container& container) { lexer.template tokenize<int>(container); };

/**
 * @brief Whether Lexer::tokenize_all() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Full_scan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.template tokenize_all<int>(container, ignore_token);
};

/**
 * @brief Whether Lexer::chunk_boundaries() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Byte_plan_over =
        requires(const Lexer& lexer, const Container& container) { lexer.chunk_boundaries(container, std::size_t{2}); };

/**
 * @brief Whether Lexer::chunk_boundaries_with_windows() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Window_plan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.chunk_boundaries_with_windows(container, std::size_t{2});
};

/**
 * @brief Whether Lexer::tokenize_all_parallel() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Parallel_scan_over = requires(const Lexer& lexer, const Container& container) {
    lexer.template tokenize_all_parallel<int>(container, std::size_t{2}, ignore_chunk_token);
};

/**
 * @brief Whether Lexer::tokenize() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Single_scan_through =
        requires(const Lexer& lexer, const Iterator& iterator) { lexer.template tokenize<int>(iterator, iterator); };

/**
 * @brief Whether Lexer::tokenize_all() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Full_scan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all<int>(iterator, iterator, ignore_token);
};

/**
 * @brief Whether Lexer::chunk_boundaries() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Byte_plan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.chunk_boundaries(iterator, iterator, std::size_t{2});
};

/**
 * @brief Whether Lexer::chunk_boundaries_with_windows() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Window_plan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.chunk_boundaries_with_windows(iterator, iterator, std::size_t{2});
};

/**
 * @brief Whether Lexer::tokenize_all_parallel() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Parallel_scan_through = requires(const Lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all_parallel<int>(iterator, iterator, std::size_t{2}, ignore_chunk_token);
};

/**
 * @brief Whether dfa::Simulator::run() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Dfa_run_through =
        requires(const dfa::Simulator& simulator, const Iterator& iterator) { simulator.run(iterator, iterator); };

/**
 * @brief Whether dfa::Simulator::run_all() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Dfa_run_all_through = requires(const dfa::Simulator& simulator, const Iterator& iterator) {
    simulator.run_all(iterator, iterator, [](const dfa::Token&, std::size_t, std::uint64_t) {});
};

/**
 * @brief Whether dfa::Simulator::run() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Dfa_scan_over =
        requires(const dfa::Simulator& simulator, const Container& container) { simulator.run(container); };

/**
 * @brief Whether nfa::Simulator::run() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Nfa_scan_through = requires(const nfa::Nfa& automaton, const Iterator& iterator) {
    nfa::Simulator::run(automaton, iterator, iterator);
};

/**
 * @brief Whether nfa::Simulator::run() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Nfa_scan_over =
        requires(const nfa::Nfa& automaton, const Container& container) { nfa::Simulator::run(automaton, container); };

/**
 * @brief Scans an input serially in one tokenize_all() call, collecting the stream.
 * @tparam T The token type.
 * @param lexer The lexer.
 * @param input The input.
 * @return The stream and the bytes consumed.
 */
template <typename T>
Scanned<T> serial_stream(const Lexer& lexer, const std::string_view input)
{
    Scanned<T> scanned{};

    const auto collect{
            [&scanned](const T token, const std::size_t length) { scanned.tokens.emplace_back(token, length); }};

    scanned.consumed = lexer.tokenize_all<T>(input, collect);

    return scanned;
}

/**
 * @brief Scans an input one tokenize() call per token, stopping where no token or a zero-width one matches.
 * @tparam T The token type.
 * @param lexer The lexer.
 * @param input The input.
 * @return The stream and the bytes consumed up to where the scan stopped.
 */
template <typename T>
Scanned<T> per_token_stream(const Lexer& lexer, const std::string_view input)
{
    Scanned<T> scanned{};

    while (scanned.consumed < input.size())
    {
        const auto rest{input.substr(scanned.consumed)};

        const auto [token, length]{lexer.tokenize<T>(rest)};

        if (!token || length == 0)
        {
            break;
        }

        scanned.tokens.emplace_back(*token, length);

        scanned.consumed += length;
    }

    return scanned;
}

/**
 * @brief Scans every chunk of an input on its own and concatenates the chunk-local streams.
 * @tparam T The token type.
 * @param lexer The lexer.
 * @param input The input.
 * @param boundaries Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
 * @return The concatenated stream and the bytes the chunks consumed in total, the input's size exactly when every chunk
 *         consumed to its end.
 */
template <typename T>
Scanned<T> rejoined_stream(const Lexer& lexer, const std::string_view input, const std::vector<std::size_t>& boundaries)
{
    Scanned<T> scanned{};

    const auto collect{
            [&scanned](const T token, const std::size_t length) { scanned.tokens.emplace_back(token, length); }};

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        const auto chunk_begin{boundaries[index - 1]};

        const auto chunk_length{boundaries[index] - chunk_begin};

        const auto chunk{input.substr(chunk_begin, chunk_length)};

        scanned.consumed += lexer.tokenize_all<T>(chunk, collect);
    }

    return scanned;
}

/**
 * @brief Expects a completely tokenizable input to scan serially to its end, its chunks under a plan to scan to their
 *        ends, and the rejoined chunk-local streams to equal the serial stream token for token.
 * @tparam T The token type.
 * @param lexer The lexer.
 * @param input The input.
 * @param boundaries Offsets from 0 to the input size inclusive; adjacent pairs delimit the chunks.
 */
template <typename T>
void expect_rejoined_equals_serial(
        const Lexer& lexer, const std::string_view input, const std::vector<std::size_t>& boundaries)
{
    const auto [serial, consumed]{serial_stream<T>(lexer, input)};

    ASSERT_EQ(consumed, input.size());

    const auto [rejoined, rejoined_consumed]{rejoined_stream<T>(lexer, input, boundaries)};

    ASSERT_EQ(rejoined_consumed, input.size());

    EXPECT_EQ(rejoined, serial);
}

/**
 * @brief Returns whether no byte value is a certified split point of a lexer.
 * @param lexer The lexer.
 * @return True when is_split_point() refuses every byte value.
 */
bool certifies_no_byte(const Lexer& lexer)
{
    const auto certifies{[&lexer](const int symbol) { return lexer.is_split_point(static_cast<char>(symbol)); }};

    return std::ranges::none_of(std::views::iota(0, byte_count), certifies);
}

} // namespace

/**
 * @brief The fixture: the token patterns of a C-like language, and Graphviz export for debugging.
 */
class Lexer_test : public testing::Test
{
protected:
    /**
     * @brief Returns an identifier pattern: a letter or underscore, then letters, digits and underscores.
     * @return The pattern.
     */
    static auto identifier_regex()
    {
        const auto identifier{concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_')))};

        return identifier;
    }

    /**
     * @brief Returns a decimal integer literal pattern.
     * @return The pattern.
     */
    static auto integer_literal_regex()
    {
        const auto integer_literal{plus(any_of(Set::digits()))};

        return integer_literal;
    }

    /**
     * @brief Returns a double-quoted string literal pattern over printable bytes.
     * @return The pattern.
     */
    static auto string_literal_regex()
    {
        const auto string_literal{concat(text(R"(")"), kleene(any_of(Set::printable())), text(R"(")"))};

        return string_literal;
    }

    /**
     * @brief Returns a fixed-point literal pattern: digits, a dot, digits.
     * @return The pattern.
     */
    static auto fixed_point_literal_regex()
    {
        const auto fixed_point_literal{concat(plus(any_of(Set::digits())), text("."), plus(any_of(Set::digits())))};

        return fixed_point_literal;
    }

    /**
     * @brief Returns a signed floating-point literal pattern with an optional exponent.
     * @return The pattern.
     */
    static auto floating_point_literal_regex()
    {
        const auto any_digit{any_of(Set::digits())};

        const auto sign_part{choice(text("+"), text("-"))};

        const auto exponent_part{concat(choice(text("e"), text("E")), optional(sign_part), plus(any_digit))};

        const auto leading_digits{concat(plus(any_digit), text("."), kleene(any_digit), optional(exponent_part))};

        const auto leading_decimal{concat(text("."), plus(any_digit), optional(exponent_part))};

        const auto forced_exponent{concat(plus(any_digit), exponent_part)};

        const auto fraction_part{choice(leading_digits, leading_decimal, forced_exponent)};

        const auto floating_point_literal{concat(optional(sign_part), fraction_part)};

        return floating_point_literal;
    }

    /**
     * @brief Returns a wide string literal pattern, L and a double-quoted string.
     * @return The pattern.
     */
    static auto wide_string_literal_regex()
    {
        const auto wide_string_literal{
                concat(text(R"(L")"), kleene(any_of(Set::printable() + Set::escape())), text(R"(")"))};

        return wide_string_literal;
    }

    /**
     * @brief Returns a single-quoted character literal pattern.
     * @return The pattern.
     */
    static auto character_literal_regex()
    {
        const auto character_literal{concat(text("'"), any_of(Set::printable() + Set::escape()), text("'"))};

        return character_literal;
    }

    /**
     * @brief Returns a wide character literal pattern, L and a single-quoted character.
     * @return The pattern.
     */
    static auto wide_character_literal_regex()
    {
        const auto wide_character_literal{concat(text("L'"), any_of(Set::printable() + Set::escape()), text("'"))};

        return wide_character_literal;
    }

    /**
     * @brief Returns a single-line comment pattern, two slashes up to the end of the line.
     * @return The pattern.
     */
    static auto single_line_comment_regex()
    {
        const auto single_line_comment{
                concat(text("//"), kleene(any_of(Set::printable() + Set::escape() - Set::newline())))};

        return single_line_comment;
    }

    /**
     * @brief Returns a line comment pattern over every byte, two slashes up to the newline.
     * @return The pattern.
     */
    static auto line_comment_regex()
    {
        const auto line_comment{concat(text("//"), kleene(any_of(Set::all() - Set{'\n'})))};

        return line_comment;
    }

    /**
     * @brief Returns a multi-line comment pattern, slash-star to star-slash.
     * @return The pattern.
     */
    static auto multi_line_comment_regex()
    {
        const auto multi_line_comment{concat(text("/*"), kleene(any_of(Set::printable() + Set::escape())), text("*/"))};

        return multi_line_comment;
    }

    /**
     * @brief Returns a block comment pattern over every byte: slash-star, then any run holding no star-slash, then the
     *        closing star-slash.
     * @return The pattern.
     */
    static auto block_comment_regex()
    {
        const auto not_star{any_of(Set::all() - Set{'*'})};

        const auto stars_then_other{concat(plus(any_of(Set{'*'})), any_of(Set::all() - Set{'*'} - Set{'/'}))};

        const auto block_comment{
                concat(text("/*"), kleene(choice(not_star, stars_then_other)), plus(any_of(Set{'*'})), text("/"))};

        return block_comment;
    }

    /**
     * @brief Writes an NFA as a Graphviz DOT file in the debug directory.
     * @param nfa The NFA.
     * @param name The file's name without its extension.
     */
    void write_dot(const nfa::Nfa& nfa, const std::string& name) const
    {
        const std::filesystem::path dot_path{debug_path_ / (name + ".dot")};

        nfa::tools::Graphviz::to_file(nfa, dot_path);
    }

    /**
     * @brief Writes a DFA as a Graphviz DOT file in the debug directory.
     * @param dfa The DFA.
     * @param name The file's name without its extension.
     */
    void write_dot(const dfa::Dfa& dfa, const std::string& name) const
    {
        const std::filesystem::path dot_path{debug_path_ / (name + ".dot")};

        dfa::tools::Graphviz::to_file(dfa, dot_path);
    }

private:
    /**
     * @brief The directory the DOT files are written to.
     */
    std::filesystem::path debug_path_{std::string{SOURCE_DIR} + "/debug/"};
};

TEST_F(Lexer_test, An_empty_token_set_matches_nothing)
{
    const Builder builder{};

    const auto lexer{builder.build()};

    constexpr std::vector<char> input{};

    EXPECT_EQ(lexer.tokenize<int>(input), no_match<int>);
}

TEST_F(Lexer_test, Keywords_match_at_their_full_length)
{
    enum class Token_kind : std::uint8_t
    {
        boolean,
        char_keyword,
        string,
        int8,
        uint8,
        int16,
        uint16,
        int32,
        uint32,
        int64,
        uint64
    };

    Builder_dbg builder{};

    builder.add_token(text("boolean"), Token_kind::boolean, 1);
    builder.add_token(text("char"), Token_kind::char_keyword, 1);
    builder.add_token(text("string"), Token_kind::string, 1);
    builder.add_token(text("int8"), Token_kind::int8, 1);
    builder.add_token(text("uint8"), Token_kind::uint8, 1);
    builder.add_token(text("int16"), Token_kind::int16, 1);
    builder.add_token(text("uint16"), Token_kind::uint16, 1);
    builder.add_token(text("int32"), Token_kind::int32, 1);
    builder.add_token(text("uint32"), Token_kind::uint32, 1);
    builder.add_token(text("int64"), Token_kind::int64, 1);
    builder.add_token(text("uint64"), Token_kind::uint64, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "keywords_nfa");

        write_dot(builder.dfa(), "keywords_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), (Match{.token = Token_kind::boolean, .length = 7}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), (Match{.token = Token_kind::char_keyword, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), (Match{.token = Token_kind::string, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), (Match{.token = Token_kind::int8, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), (Match{.token = Token_kind::uint8, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), (Match{.token = Token_kind::int16, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), (Match{.token = Token_kind::uint16, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), (Match{.token = Token_kind::int32, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), (Match{.token = Token_kind::uint32, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), (Match{.token = Token_kind::int64, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), (Match{.token = Token_kind::uint64, .length = 6}));
}

TEST_F(Lexer_test, Identifiers_match_letters_digits_and_underscores)
{
    enum class Token_kind : std::uint8_t
    {
        identifier
    };

    Builder_dbg builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "identifier_nfa");

        write_dot(builder.dfa(), "identifier_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name"), (Match{.token = Token_kind::identifier, .length = 13}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("_someVar"), (Match{.token = Token_kind::identifier, .length = 8}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("MyVariable123"), (Match{.token = Token_kind::identifier, .length = 13}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("__Another_var__99"), (Match{.token = Token_kind::identifier, .length = 17}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("camelCase"), (Match{.token = Token_kind::identifier, .length = 9}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("___"), (Match{.token = Token_kind::identifier, .length = 3}));
}

TEST_F(Lexer_test, Integer_literals_match_every_digit)
{
    enum class Token_kind : std::uint8_t
    {
        integer_literal
    };

    Builder_dbg builder{};

    builder.add_token(integer_literal_regex(), Token_kind::integer_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "integer_literal_nfa");

        write_dot(builder.dfa(), "integer_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("123"), (Match{.token = Token_kind::integer_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("007"), (Match{.token = Token_kind::integer_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1234567890"), (Match{.token = Token_kind::integer_literal, .length = 10}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("5"), (Match{.token = Token_kind::integer_literal, .length = 1}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("0"), (Match{.token = Token_kind::integer_literal, .length = 1}));
}

TEST_F(Lexer_test, String_literals_match_through_the_closing_quote)
{
    enum class Token_kind : std::uint8_t
    {
        string_literal
    };

    Builder_dbg builder{};

    builder.add_token(string_literal_regex(), Token_kind::string_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "string_literal_nfa");

        write_dot(builder.dfa(), "string_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>(R"("Hello")"), (Match{.token = Token_kind::string_literal, .length = 7}));
    EXPECT_EQ(lexer.tokenize<Token_kind>(R"("")"), (Match{.token = Token_kind::string_literal, .length = 2}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"("Hello world")"), (Match{.token = Token_kind::string_literal, .length = 13}));
    EXPECT_EQ(lexer.tokenize<Token_kind>(R"("\"Quote\"")"), (Match{.token = Token_kind::string_literal, .length = 11}));
}

TEST_F(Lexer_test, Fixed_point_literals_need_digits_on_both_sides_of_the_dot)
{
    enum class Token_kind : std::uint8_t
    {
        fixed_point_literal
    };

    Builder_dbg builder{};

    builder.add_token(fixed_point_literal_regex(), Token_kind::fixed_point_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "fixed_point_literal_nfa");

        write_dot(builder.dfa(), "fixed_point_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("1.2"), (Match{.token = Token_kind::fixed_point_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14"), (Match{.token = Token_kind::fixed_point_literal, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("123.456"), (Match{.token = Token_kind::fixed_point_literal, .length = 7}));

    EXPECT_EQ(lexer.tokenize<Token_kind>("."), no_match<Token_kind>);
    EXPECT_EQ(lexer.tokenize<Token_kind>(".1"), no_match<Token_kind>);
    EXPECT_EQ(lexer.tokenize<Token_kind>("58."), no_match<Token_kind>);
}

TEST_F(Lexer_test, Floating_point_literals_match_signs_fractions_and_exponents)
{
    enum class Token_kind : std::uint8_t
    {
        floating_point_literal
    };

    Builder_dbg builder{};

    builder.add_token(floating_point_literal_regex(), Token_kind::floating_point_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "floating_point_literal_nfa");

        write_dot(builder.dfa(), "floating_point_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("3.14159"), (Match{.token = Token_kind::floating_point_literal, .length = 7}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("2e10"), (Match{.token = Token_kind::floating_point_literal, .length = 4}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("-1.23E-4"), (Match{.token = Token_kind::floating_point_literal, .length = 8}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("+0.5"), (Match{.token = Token_kind::floating_point_literal, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("1e-10"), (Match{.token = Token_kind::floating_point_literal, .length = 5}));
}

TEST_F(Lexer_test, Wide_string_literals_match_after_the_l_prefix)
{
    enum class Token_kind : std::uint8_t
    {
        wide_string_literal
    };

    Builder_dbg builder{};

    builder.add_token(wide_string_literal_regex(), Token_kind::wide_string_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "wide_string_literal_nfa");

        write_dot(builder.dfa(), "wide_string_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"(L"Hello")"), (Match{.token = Token_kind::wide_string_literal, .length = 8}));
    EXPECT_EQ(lexer.tokenize<Token_kind>(R"(L"")"), (Match{.token = Token_kind::wide_string_literal, .length = 3}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"(L"Wide world")"),
            (Match{.token = Token_kind::wide_string_literal, .length = 13}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"(L"\"Escaped\"")"),
            (Match{.token = Token_kind::wide_string_literal, .length = 14}));
}

TEST_F(Lexer_test, Character_literals_match_one_quoted_character)
{
    enum class Token_kind : std::uint8_t
    {
        character_literal
    };

    Builder_dbg builder{};

    builder.add_token(character_literal_regex(), Token_kind::character_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "character_literal_nfa");

        write_dot(builder.dfa(), "character_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), (Match{.token = Token_kind::character_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("' '"), (Match{.token = Token_kind::character_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'\n'"), (Match{.token = Token_kind::character_literal, .length = 3}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'''"), (Match{.token = Token_kind::character_literal, .length = 3}));
}

TEST_F(Lexer_test, Wide_character_literals_match_after_the_l_prefix)
{
    enum class Token_kind : std::uint8_t
    {
        wide_character_literal
    };

    Builder_dbg builder{};

    builder.add_token(wide_character_literal_regex(), Token_kind::wide_character_literal, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "wide_character_literal_nfa");

        write_dot(builder.dfa(), "wide_character_literal_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), (Match{.token = Token_kind::wide_character_literal, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L' '"), (Match{.token = Token_kind::wide_character_literal, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'\n'"), (Match{.token = Token_kind::wide_character_literal, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'''"), (Match{.token = Token_kind::wide_character_literal, .length = 4}));
}

TEST_F(Lexer_test, Single_line_comments_match_from_the_two_slashes)
{
    enum class Token_kind : std::uint8_t
    {
        single_line_comment
    };

    Builder_dbg builder{};

    builder.add_token(single_line_comment_regex(), Token_kind::single_line_comment, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "single_line_comment_nfa");

        write_dot(builder.dfa(), "single_line_comment_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(
            lexer.tokenize<Token_kind>("// This is a comment"),
            (Match{.token = Token_kind::single_line_comment, .length = 20}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("//"), (Match{.token = Token_kind::single_line_comment, .length = 2}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("// @#$%^&*()"),
            (Match{.token = Token_kind::single_line_comment, .length = 12}));
}

TEST_F(Lexer_test, Multi_line_comments_match_through_the_closing_star_slash)
{
    enum class Token_kind : std::uint8_t
    {
        multi_line_comment
    };

    Builder_dbg builder{};

    builder.add_token(multi_line_comment_regex(), Token_kind::multi_line_comment, 1);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "multi_line_comment_nfa");

        write_dot(builder.dfa(), "multi_line_comment_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* comment */"),
            (Match{.token = Token_kind::multi_line_comment, .length = 13}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* multi\n   line\n   comment */"),
            (Match{.token = Token_kind::multi_line_comment, .length = 30}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* start /* nested */ end */"),
            (Match{.token = Token_kind::multi_line_comment, .length = 28}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("/**/"), (Match{.token = Token_kind::multi_line_comment, .length = 4}));
}

TEST_F(Lexer_test, A_combined_c_like_token_set_matches_every_token_kind)
{
    enum class Token_kind : std::uint8_t
    {
        // Keywords
        boolean,
        char_keyword,
        string,
        int8,
        uint8,
        int16,
        uint16,
        int32,
        uint32,
        int64,
        uint64,

        // Identifier
        identifier,

        // Literals
        integer_literal,
        string_literal,
        wide_string_literal,
        character_literal,
        wide_character_literal,
        fixed_point_literal,
        floating_point_literal,

        // Comments
        single_line_comment,
        multi_line_comment
    };

    Builder_dbg builder{};

    builder.add_token(text("boolean"), Token_kind::boolean, 1);
    builder.add_token(text("char"), Token_kind::char_keyword, 1);
    builder.add_token(text("string"), Token_kind::string, 1);
    builder.add_token(text("int8"), Token_kind::int8, 1);
    builder.add_token(text("uint8"), Token_kind::uint8, 1);
    builder.add_token(text("int16"), Token_kind::int16, 1);
    builder.add_token(text("uint16"), Token_kind::uint16, 1);
    builder.add_token(text("int32"), Token_kind::int32, 1);
    builder.add_token(text("uint32"), Token_kind::uint32, 1);
    builder.add_token(text("int64"), Token_kind::int64, 1);
    builder.add_token(text("uint64"), Token_kind::uint64, 1);

    builder.add_token(identifier_regex(), Token_kind::identifier, 4);

    builder.add_token(integer_literal_regex(), Token_kind::integer_literal, 2);
    builder.add_token(string_literal_regex(), Token_kind::string_literal, 2);
    builder.add_token(character_literal_regex(), Token_kind::character_literal, 2);
    builder.add_token(wide_string_literal_regex(), Token_kind::wide_string_literal, 2);
    builder.add_token(wide_character_literal_regex(), Token_kind::wide_character_literal, 2);

    builder.add_token(fixed_point_literal_regex(), Token_kind::fixed_point_literal, 2);
    builder.add_token(floating_point_literal_regex(), Token_kind::floating_point_literal, 3);

    builder.add_token(single_line_comment_regex(), Token_kind::single_line_comment, 0);
    builder.add_token(multi_line_comment_regex(), Token_kind::multi_line_comment, 0);

    if constexpr (debug_dot)
    {
        write_dot(builder.nfa(), "combined_nfa");

        write_dot(builder.dfa(), "combined_dfa");
    }

    const auto lexer{builder.build()};

    using Match = Lexer::Match<Token_kind>;

    EXPECT_EQ(lexer.tokenize<Token_kind>("boolean"), (Match{.token = Token_kind::boolean, .length = 7}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("char"), (Match{.token = Token_kind::char_keyword, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("string"), (Match{.token = Token_kind::string, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int8"), (Match{.token = Token_kind::int8, .length = 4}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint8"), (Match{.token = Token_kind::uint8, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int16"), (Match{.token = Token_kind::int16, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint16"), (Match{.token = Token_kind::uint16, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int32"), (Match{.token = Token_kind::int32, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint32"), (Match{.token = Token_kind::uint32, .length = 6}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("int64"), (Match{.token = Token_kind::int64, .length = 5}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("uint64"), (Match{.token = Token_kind::uint64, .length = 6}));

    EXPECT_EQ(lexer.tokenize<Token_kind>("variable_name_1"), (Match{.token = Token_kind::identifier, .length = 15}));

    EXPECT_EQ(lexer.tokenize<Token_kind>("1234"), (Match{.token = Token_kind::integer_literal, .length = 4}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"("hello world")"), (Match{.token = Token_kind::string_literal, .length = 13}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("'a'"), (Match{.token = Token_kind::character_literal, .length = 3}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>(R"(L"wide string")"),
            (Match{.token = Token_kind::wide_string_literal, .length = 14}));
    EXPECT_EQ(lexer.tokenize<Token_kind>("L'a'"), (Match{.token = Token_kind::wide_character_literal, .length = 4}));

    EXPECT_EQ(lexer.tokenize<Token_kind>("123.45"), (Match{.token = Token_kind::fixed_point_literal, .length = 6}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("3.14159e+2"),
            (Match{.token = Token_kind::floating_point_literal, .length = 10}));

    EXPECT_EQ(
            lexer.tokenize<Token_kind>("// a comment"),
            (Match{.token = Token_kind::single_line_comment, .length = 12}));
    EXPECT_EQ(
            lexer.tokenize<Token_kind>("/* a comment */"),
            (Match{.token = Token_kind::multi_line_comment, .length = 15}));
}

TEST_F(Lexer_test, Tokenize_all_matches_sequential_tokenization)
{
    enum class Token_kind : std::uint8_t
    {
        keyword,
        identifier,
        integer_literal,
        whitespace
    };

    Builder builder{};

    builder.add_token(text("boolean"), Token_kind::keyword, 1);
    builder.add_token(identifier_regex(), Token_kind::identifier, 4);
    builder.add_token(integer_literal_regex(), Token_kind::integer_literal, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 2);

    const auto lexer{builder.build()};

    const std::string input{"boolean x 1234 boolean_ish  42 y7"};

    const auto [batch, consumed]{serial_stream<Token_kind>(lexer, input)};

    EXPECT_EQ(consumed, input.size());

    const auto [sequential, sequential_consumed]{per_token_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(sequential_consumed, input.size());

    EXPECT_EQ(batch, sequential);
}

TEST_F(Lexer_test, Tokenize_all_stops_at_unmatched_input)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"abc def @rest"};

    std::size_t tokens{0};

    const auto count{[&tokens](const Token_kind, const std::size_t) { ++tokens; }};

    const auto consumed{lexer.tokenize_all<Token_kind>(input, count)};

    EXPECT_EQ(consumed, 8U);
    EXPECT_EQ(tokens, 4U);

    const auto empty_consumed{lexer.tokenize_all<Token_kind>(std::string{}, ignore_token)};

    EXPECT_EQ(empty_consumed, 0U);
}

TEST_F(Lexer_test, Tokenize_all_stops_when_the_sink_returns_false)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"one two three"};

    std::size_t tokens{0};

    const auto count_two{[&tokens](const Token_kind, const std::size_t) { return ++tokens < 2; }};

    const auto consumed{lexer.tokenize_all<Token_kind>(input, count_two)};

    EXPECT_EQ(tokens, 2U);
    EXPECT_EQ(consumed, 4U);
}

TEST_F(Lexer_test, Split_points_depend_on_the_token_set)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        newline
    };

    // A single-character newline token: '\n' is consumed only at a token start, so it certifies as a split point.
    Builder single{};

    single.add_token(identifier_regex(), Token_kind::identifier, 1);
    single.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    single.add_token(text("\n"), Token_kind::newline, 1);

    const auto single_lexer{single.build()};

    EXPECT_TRUE(single_lexer.is_split_point('\n'));
    EXPECT_FALSE(single_lexer.is_split_point('a'));
    EXPECT_FALSE(single_lexer.is_split_point(' '));

    // A newline-run token: "\n\n" is one token, so splitting at the second '\n' would change the tokenization, and the
    // analysis correctly refuses what a split-at-newline heuristic would wrongly allow.
    Builder run{};

    run.add_token(identifier_regex(), Token_kind::identifier, 1);
    run.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    run.add_token(plus(any_of(Set::newline())), Token_kind::newline, 1);

    const auto run_lexer{run.build()};

    EXPECT_FALSE(run_lexer.is_split_point('\n'));
}

TEST_F(Lexer_test, Split_windows_generalize_the_byte_certificate)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword,
        number,
        operator_symbol
    };

    // The refutation grammar from the certified-windows study: the byte certificate reaches only "a", which cannot cut
    // inside the window, while the three-byte window "abx" pins the covering token of its final byte to origin 1, the
    // occurrence tokenizing as a|bx.
    Builder builder{};

    builder.add_token(text("a"), Token_kind::identifier, 2);
    builder.add_token(text("abc"), Token_kind::keyword, 1);
    builder.add_token(text("bx"), Token_kind::number, 2);
    builder.add_token(text("x"), Token_kind::operator_symbol, 2);

    const auto lexer{builder.build()};

    const auto abx{lexer.is_split_window("abx")};

    ASSERT_TRUE(abx.has_value());
    EXPECT_EQ(*abx, 1U);

    // The specialization theorem, executable: at length one the window decision coincides with the byte predicate on
    // every byte value, certificates and refusals alike.
    for (int symbol{0}; symbol < byte_count; ++symbol)
    {
        const auto byte{static_cast<char>(symbol)};

        const std::string window{byte};

        EXPECT_EQ(lexer.is_split_window(window).has_value(), lexer.is_split_point(byte));
    }
}

TEST_F(Lexer_test, Split_windows_stay_conservative_and_scoped)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword,
        operator_symbol
    };

    // Strictness: over {a, ab, b} the input "ab" always tokenizes as the single token ab, making it a true certificate
    // at origin 0, and the conservative model still refuses it: the accepting a seeds a competing origin for b and
    // unanimity is lost. Refusal is model-relative, never proof that no certificate exists.
    Builder strict{};

    strict.add_token(text("a"), Token_kind::identifier, 1);
    strict.add_token(text("ab"), Token_kind::keyword, 1);
    strict.add_token(text("b"), Token_kind::operator_symbol, 1);

    EXPECT_FALSE(strict.build().is_split_window("ab").has_value());

    // Vacuity: over {0, 00, 01} the model certifies "1001" at origin 2, and no completely tokenizable input contains
    // that window. The call answers the model; the certificate is conditional on occurrence, and a caller holds an
    // occurrence only by finding the window in input at hand.
    Builder vacuous{};

    vacuous.add_token(text("0"), Token_kind::identifier, 1);
    vacuous.add_token(text("00"), Token_kind::keyword, 1);
    vacuous.add_token(text("01"), Token_kind::operator_symbol, 1);

    const auto model{vacuous.build().is_split_window("1001")};

    ASSERT_TRUE(model.has_value());
    EXPECT_EQ(*model, 2U);

    // A grammar whose search space exhausts: a+ accepts after every byte, every offset stays in play, and no window of
    // any length certifies.
    Builder unbounded{};

    unbounded.add_token(plus(text("a")), Token_kind::identifier, 1);

    const auto plus_lexer{unbounded.build()};

    EXPECT_FALSE(plus_lexer.is_split_window("a").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aa").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aaaaa").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("aaaaaa").has_value());

    // A nullable set is decided as its positive-width equivalent, so a* answers exactly as a+ does: "a" sits mid-run
    // and is refused. The empty window certifies nothing either.
    Builder nullable{};

    nullable.add_token(kleene(text("a")), Token_kind::identifier, 1);

    EXPECT_FALSE(nullable.build().is_split_window("a").has_value());
    EXPECT_FALSE(plus_lexer.is_split_window("").has_value());
}

TEST_F(Lexer_test, Window_occurrence_splits_vacuous_certificates_from_occurring_ones)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword,
        number,
        operator_symbol
    };

    const auto contains_and_tokenizes{
            [](const Lexer& lexer, const std::string& witness, const std::string_view window) {
                const auto consumed{lexer.tokenize_all<Token_kind>(witness, ignore_token)};

                return consumed == witness.size() && witness.contains(window);
            }};

    // The vacuous certificate of the conservative model: over {0, 00, 01} the window 1001 is certified at origin 2, and
    // no completely tokenizable input contains it, since the 1 ends a token, 00 is then the longest match, and no token
    // begins with the 1 after it. The decision proves as much, from an exhausted search, while 001 occurs in 0001, the
    // shortest input holding it: 001 itself tokenizes 00 and stops, and 0001 is 00 then 01.
    Builder vacuous{};

    vacuous.add_token(text("0"), Token_kind::identifier, 1);
    vacuous.add_token(text("00"), Token_kind::keyword, 1);
    vacuous.add_token(text("01"), Token_kind::operator_symbol, 1);

    const auto vacuous_lexer{vacuous.build()};

    ASSERT_EQ(vacuous_lexer.is_split_window("1001"), std::optional<std::size_t>{2});

    const auto [none, settled]{vacuous_lexer.window_occurrence("1001")};

    EXPECT_TRUE(settled);
    EXPECT_TRUE(none.empty());

    const auto [witness, exhaustive]{vacuous_lexer.window_occurrence("001")};

    EXPECT_TRUE(exhaustive);
    EXPECT_EQ(witness, "0001");
    EXPECT_TRUE(contains_and_tokenizes(vacuous_lexer, witness, "001"));

    // The refutation grammar's certificate is an occurring one: abx is certified at origin 1 and is itself a completely
    // tokenizable input, a|bx, so it is its own shortest witness.
    Builder refutation{};

    refutation.add_token(text("a"), Token_kind::identifier, 2);
    refutation.add_token(text("abc"), Token_kind::keyword, 1);
    refutation.add_token(text("bx"), Token_kind::number, 2);
    refutation.add_token(text("x"), Token_kind::operator_symbol, 2);

    const auto refutation_lexer{refutation.build()};

    ASSERT_EQ(refutation_lexer.is_split_window("abx"), std::optional<std::size_t>{1});

    const auto [occurring, decided]{refutation_lexer.window_occurrence("abx")};

    EXPECT_TRUE(decided);
    EXPECT_EQ(occurring, "abx");
    EXPECT_TRUE(contains_and_tokenizes(refutation_lexer, occurring, "abx"));

    // A window is_split_window() refuses is decided too: ab is refused over this set and occurs in abc, and a window
    // over bytes no token uses occurs nowhere.
    const auto [in_abc, ab_settled]{refutation_lexer.window_occurrence("ab")};

    const auto [nowhere, q_settled]{refutation_lexer.window_occurrence("q")};

    EXPECT_FALSE(refutation_lexer.is_split_window("ab").has_value());
    EXPECT_EQ(in_abc, "abc");
    EXPECT_TRUE(ab_settled);
    EXPECT_TRUE(nowhere.empty());
    EXPECT_TRUE(q_settled);

    // The empty window is contained in every input; its witness is a shortest token, never the empty input, so that an
    // empty witness keeps meaning none.
    const auto [shortest_token, empty_settled]{refutation_lexer.window_occurrence("")};

    EXPECT_EQ(shortest_token, "a");
    EXPECT_TRUE(empty_settled);
}

TEST_F(Lexer_test, Window_occurrence_agrees_with_the_research_oracle_on_every_small_window)
{
    enum class Token_kind : std::uint8_t
    {
        first,
        second,
        third
    };

    // One of the five regex token sets occurring_windows.py validates against bounded enumeration, with the windows
    // over {a, b} up to length three the oracle finds occurring.
    struct Universe
    {
        std::string_view name{};

        // Registered at one priority, in order.
        std::vector<Regex> tokens{};

        // Each with the length of its shortest witness.
        std::vector<std::pair<std::string_view, std::size_t>> occurring{};
    };

    // The oracle's verdicts, as the program printed them: the occurring windows of each universe with the length of the
    // witness, every other window of the fourteen occurring in no completely tokenizable input.
    const std::vector<Universe> universes{
            {.name = "{a+b, a}",
             .tokens = {concat(plus(text("a")), text("b")), text("a")},
             .occurring =
                     {{"a", 1},
                      {"b", 2},
                      {"aa", 2},
                      {"ab", 2},
                      {"ba", 3},
                      {"aaa", 3},
                      {"aab", 3},
                      {"aba", 3},
                      {"baa", 4},
                      {"bab", 4}}},
            {.name = "{ab, a, b}",
             .tokens = {concat(text("a"), text("b")), text("a"), text("b")},
             .occurring =
                     {{"a", 1},
                      {"b", 1},
                      {"aa", 2},
                      {"ab", 2},
                      {"ba", 2},
                      {"bb", 2},
                      {"aaa", 3},
                      {"aab", 3},
                      {"aba", 3},
                      {"abb", 3},
                      {"baa", 3},
                      {"bab", 3},
                      {"bba", 3},
                      {"bbb", 3}}},
            {.name = "{a+, b}",
             .tokens = {plus(text("a")), text("b")},
             .occurring =
                     {{"a", 1},
                      {"b", 1},
                      {"aa", 2},
                      {"ab", 2},
                      {"ba", 2},
                      {"bb", 2},
                      {"aaa", 3},
                      {"aab", 3},
                      {"aba", 3},
                      {"abb", 3},
                      {"baa", 3},
                      {"bab", 3},
                      {"bba", 3},
                      {"bbb", 3}}},
            {.name = "{a|ab, b}",
             .tokens = {choice(text("a"), concat(text("a"), text("b"))), text("b")},
             .occurring =
                     {{"a", 1},
                      {"b", 1},
                      {"aa", 2},
                      {"ab", 2},
                      {"ba", 2},
                      {"bb", 2},
                      {"aaa", 3},
                      {"aab", 3},
                      {"aba", 3},
                      {"abb", 3},
                      {"baa", 3},
                      {"bab", 3},
                      {"bba", 3},
                      {"bbb", 3}}},
            {.name = "{aa}", .tokens = {concat(text("a"), text("a"))}, .occurring = {{"a", 2}, {"aa", 2}, {"aaa", 4}}}};

    const auto windows{words("ab", 3)};

    std::size_t decided{0};

    for (const auto& [name, tokens, occurring] : universes)
    {
        const auto lexer{regex_lexer(tokens)};

        for (const auto& window : windows)
        {
            const auto [witness, exhaustive]{lexer.window_occurrence(window)};

            const auto occurring_windows{occurring | std::views::keys};

            const auto expected{std::ranges::find(occurring_windows, window).base()};

            ASSERT_TRUE(exhaustive) << name << ' ' << window;
            EXPECT_EQ(!witness.empty(), expected != occurring.end()) << name << ' ' << window;

            ++decided;

            if (expected == occurring.end())
            {
                continue;
            }

            // Both searches find a shortest witness, so the lengths agree even where the witnesses need not; the
            // witness is checked as the oracle checks its own, by tokenizing it and finding the window in it.
            const auto& [occurring_window, shortest]{*expected};

            const auto consumed{lexer.tokenize_all<Token_kind>(witness, ignore_token)};

            EXPECT_EQ(witness.size(), shortest) << name << ' ' << window;
            EXPECT_TRUE(witness.contains(window)) << name << ' ' << window;
            EXPECT_EQ(consumed, witness.size()) << name << ' ' << window;
        }
    }

    // The oracle's own count: 70 windows across five regex token sets.
    EXPECT_EQ(decided, 70U);
}

TEST_F(Lexer_test, The_occurrence_cap_is_a_ceiling_on_the_states_the_search_holds)
{
    enum class Token_kind : std::uint8_t
    {
        zero,
        pair,
        one
    };

    // Over {0, 00, 01} the window 001 occurs in 0001. The cap is the most states the search may hold, so every cap
    // below the smallest one that settles the question answers nothing rather than something, and every cap from it on
    // answers the same witness; zero holds nothing, not even the state the search starts in.
    Builder builder{};

    builder.add_token(text("0"), Token_kind::zero, 1);
    builder.add_token(text("00"), Token_kind::pair, 1);
    builder.add_token(text("01"), Token_kind::one, 1);

    const auto lexer{builder.build()};

    const auto [held_nothing, zero_settled]{lexer.window_occurrence("001", 0)};

    EXPECT_FALSE(zero_settled);
    EXPECT_TRUE(held_nothing.empty());

    std::size_t holds{1};

    for (;; ++holds)
    {
        const auto [witness, exhaustive]{lexer.window_occurrence("001", holds)};

        if (exhaustive)
        {
            break;
        }

        ASSERT_TRUE(witness.empty()) << "cap " << holds;
    }

    // Every byte of the witness admits at least one state, and the search settled with the witness at the cap.
    const auto [first_witness, first_settled]{lexer.window_occurrence("001", holds)};

    EXPECT_GE(holds, 4U);
    EXPECT_EQ(first_witness, "0001");

    for (std::size_t cap{holds}; cap <= holds + 8; ++cap)
    {
        const auto [witness, exhaustive]{lexer.window_occurrence("001", cap)};

        EXPECT_TRUE(exhaustive) << "cap " << cap;
        EXPECT_EQ(witness, "0001") << "cap " << cap;
    }

    // A window occurring nowhere is proved so only by an exhausted search, and a cap that stops the search before it
    // exhausts says nothing about it.
    const auto [stopped_witness, stopped_settled]{lexer.window_occurrence("1001", 1)};

    const auto [absent_witness, absent_settled]{lexer.window_occurrence("1001")};

    EXPECT_FALSE(stopped_settled);
    EXPECT_TRUE(absent_settled);
}

TEST_F(Lexer_test, Window_counterexample_settles_what_the_model_refuses_and_proves_what_it_certifies)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword,
        number,
        operator_symbol
    };

    // The strictness witness of the split-windows report: over {a, ab, b} the window ab is certified at origin 0 in
    // every completely tokenizable input, since a begins a token wherever it occurs and maximal munch then takes ab,
    // and the conservative model refuses it. The exact decision proves the certificate from an exhausted search, and
    // the window occurs, in ab itself, so the certificate the model missed is about an input. At origin 1 that very
    // input is the counterexample.
    Builder strict{};

    strict.add_token(text("a"), Token_kind::identifier, 1);
    strict.add_token(text("ab"), Token_kind::keyword, 1);
    strict.add_token(text("b"), Token_kind::operator_symbol, 1);

    const auto strict_lexer{strict.build()};

    ASSERT_FALSE(strict_lexer.is_split_window("ab").has_value());

    const auto [proved, settled]{strict_lexer.window_counterexample("ab", 0)};

    EXPECT_TRUE(settled);
    EXPECT_TRUE(proved.empty());

    const auto [occurring, occurrence_settled]{strict_lexer.window_occurrence("ab")};

    EXPECT_EQ(occurring, "ab");

    const auto [shifted, decided]{strict_lexer.window_counterexample("ab", 1)};

    EXPECT_TRUE(decided);
    EXPECT_EQ(shifted, "ab");
    EXPECT_TRUE(fails(strict_lexer, shifted, "ab", 1));

    // The vacuous certificate: over {0, 00, 01} the model certifies 1001 at origin 2 and no completely tokenizable
    // input contains the window, so no input fails it at any origin, and the search proves as much at every one. The
    // window 001 occurs, in 0001 as 00 then 01, which covers the 1 from offset 1: exact there, and failed at origin 0
    // by that shortest input.
    Builder vacuous{};

    vacuous.add_token(text("0"), Token_kind::identifier, 1);
    vacuous.add_token(text("00"), Token_kind::keyword, 1);
    vacuous.add_token(text("01"), Token_kind::operator_symbol, 1);

    const auto vacuous_lexer{vacuous.build()};

    const auto [absent, absent_settled]{vacuous_lexer.window_occurrence("1001")};

    ASSERT_EQ(vacuous_lexer.is_split_window("1001"), std::optional<std::size_t>{2});
    ASSERT_TRUE(absent.empty());

    for (std::size_t origin{0}; origin < 4; ++origin)
    {
        const auto [none, exhausted]{vacuous_lexer.window_counterexample("1001", origin)};

        EXPECT_TRUE(exhausted) << origin;
        EXPECT_TRUE(none.empty()) << origin;
    }

    const auto [exact, exact_settled]{vacuous_lexer.window_counterexample("001", 1)};

    const auto [failing, failing_settled]{vacuous_lexer.window_counterexample("001", 0)};

    EXPECT_TRUE(exact.empty());
    EXPECT_TRUE(exact_settled);
    EXPECT_EQ(failing, "0001");
    EXPECT_TRUE(fails(vacuous_lexer, "0001", "001", 0));

    // The refutation grammar: abx is certified at origin 1 and has no counterexample there, and the refused ab is
    // refused rightly at both origins, abx covering the b from offset 1 and abc from offset 0, each the shortest
    // counterexample.
    Builder refutation{};

    refutation.add_token(text("a"), Token_kind::identifier, 2);
    refutation.add_token(text("abc"), Token_kind::keyword, 1);
    refutation.add_token(text("bx"), Token_kind::number, 2);
    refutation.add_token(text("x"), Token_kind::operator_symbol, 2);

    const auto refutation_lexer{refutation.build()};

    const auto [certified, certified_settled]{refutation_lexer.window_counterexample("abx", 1)};

    ASSERT_EQ(refutation_lexer.is_split_window("abx"), std::optional<std::size_t>{1});
    EXPECT_TRUE(certified.empty());
    EXPECT_TRUE(certified_settled);

    ASSERT_FALSE(refutation_lexer.is_split_window("ab").has_value());

    const auto [elsewhere, found]{refutation_lexer.window_counterexample("ab", 0)};

    EXPECT_TRUE(found);
    EXPECT_EQ(elsewhere, "abx");
    EXPECT_TRUE(fails(refutation_lexer, elsewhere, "ab", 0));

    const auto [covering_from_zero, from_zero_settled]{refutation_lexer.window_counterexample("ab", 1)};

    EXPECT_EQ(covering_from_zero, "abc");
    EXPECT_TRUE(fails(refutation_lexer, "abc", "ab", 1));

    // Neither the empty window nor an origin outside the window is a certificate.
    EXPECT_THROW(std::ignore = refutation_lexer.window_counterexample("", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = refutation_lexer.window_counterexample("ab", 2), std::invalid_argument);
}

TEST_F(Lexer_test, Window_counterexample_agrees_with_the_research_oracles_on_every_small_certificate)
{
    // One of the four regex token sets certified_inventory_regex.py decides against the bounded enumeration of
    // offline_certification.py, with every (window, origin) pair over {a, b} to length three the oracles refute; both
    // certify every other pair.
    struct Universe
    {
        std::string_view name{};

        // Registered at one priority, in order.
        std::vector<Regex> tokens{};

        // Window, origin and the length of the shortest counterexample.
        std::vector<std::tuple<std::string_view, std::size_t, std::size_t>> refuted{};
    };

    // The oracles' verdicts, as the program printed them.
    const std::vector<Universe> universes{
            {.name = "{a+b, a}",
             .tokens = {concat(plus(text("a")), text("b")), text("a")},
             .refuted = {{"a", 0, 3},   {"b", 0, 2},   {"aa", 0, 2},  {"aa", 1, 3},  {"ab", 0, 3},
                         {"ab", 1, 2},  {"ba", 0, 3},  {"aaa", 0, 3}, {"aaa", 1, 3}, {"aaa", 2, 4},
                         {"aab", 0, 4}, {"aab", 1, 3}, {"aab", 2, 3}, {"aba", 0, 3}, {"aba", 1, 3},
                         {"baa", 0, 4}, {"baa", 1, 4}, {"baa", 2, 5}, {"bab", 0, 4}, {"bab", 2, 4}}},
            {.name = "{ab, a, b}",
             .tokens = {concat(text("a"), text("b")), text("a"), text("b")},
             .refuted = {{"b", 0, 2},   {"aa", 0, 2},  {"ab", 1, 2},  {"ba", 0, 2},  {"bb", 0, 2},  {"aaa", 0, 3},
                         {"aaa", 1, 3}, {"aab", 0, 3}, {"aab", 2, 3}, {"aba", 0, 3}, {"aba", 1, 3}, {"abb", 0, 3},
                         {"abb", 1, 3}, {"baa", 0, 3}, {"baa", 1, 3}, {"bab", 0, 3}, {"bab", 2, 3}, {"bba", 0, 3},
                         {"bba", 1, 3}, {"bbb", 0, 3}, {"bbb", 1, 3}}},
            {.name = "{a+, b}",
             .tokens = {plus(text("a")), text("b")},
             .refuted = {{"a", 0, 2},   {"aa", 0, 3},  {"aa", 1, 2},  {"ab", 0, 2},  {"ba", 0, 2},  {"bb", 0, 2},
                         {"aaa", 0, 4}, {"aaa", 1, 3}, {"aaa", 2, 3}, {"aab", 0, 3}, {"aab", 1, 3}, {"aba", 0, 3},
                         {"aba", 1, 3}, {"abb", 0, 3}, {"abb", 1, 3}, {"baa", 0, 3}, {"baa", 2, 3}, {"bab", 0, 3},
                         {"bab", 1, 3}, {"bba", 0, 3}, {"bba", 1, 3}, {"bbb", 0, 3}, {"bbb", 1, 3}}},
            {.name = "{a|ab, b}",
             .tokens = {choice(text("a"), concat(text("a"), text("b"))), text("b")},
             .refuted = {{"b", 0, 2},   {"aa", 0, 2},  {"ab", 1, 2},  {"ba", 0, 2},  {"bb", 0, 2},  {"aaa", 0, 3},
                         {"aaa", 1, 3}, {"aab", 0, 3}, {"aab", 2, 3}, {"aba", 0, 3}, {"aba", 1, 3}, {"abb", 0, 3},
                         {"abb", 1, 3}, {"baa", 0, 3}, {"baa", 1, 3}, {"bab", 0, 3}, {"bab", 2, 3}, {"bba", 0, 3},
                         {"bba", 1, 3}, {"bbb", 0, 3}, {"bbb", 1, 3}}}};

    const auto windows{words("ab", 3)};

    // Every (window, origin) pair, windows in order and origins ascending within each.
    std::vector<std::pair<std::string_view, std::size_t>> certificates{};

    for (const auto& window : windows)
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            certificates.emplace_back(window, origin);
        }
    }

    std::size_t decided{0};

    for (const auto& [name, tokens, refuted] : universes)
    {
        const auto lexer{regex_lexer(tokens)};

        for (const auto& [window, origin] : certificates)
        {
            const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

            const auto names_certificate{[&](const std::tuple<std::string_view, std::size_t, std::size_t>& row) {
                const auto& [refuted_window, refuted_origin, shortest]{row};

                return refuted_window == window && refuted_origin == origin;
            }};

            const auto expected{std::ranges::find_if(refuted, names_certificate)};

            ASSERT_TRUE(exhaustive) << name << ' ' << window << ' ' << origin;
            EXPECT_EQ(!witness.empty(), expected != refuted.end()) << name << ' ' << window << ' ' << origin;

            ++decided;

            if (expected == refuted.end())
            {
                continue;
            }

            // Both searches find a shortest counterexample, so the lengths agree even where the witnesses need not; the
            // witness is checked as the oracle checks its own, by scanning it and reading off the covering token's
            // start at an occurrence.
            const auto& [refuted_window, refuted_origin, shortest]{*expected};

            EXPECT_EQ(witness.size(), shortest) << name << ' ' << window << ' ' << origin;
            EXPECT_TRUE(fails(lexer, witness, window, origin)) << name << ' ' << window << ' ' << origin;
        }
    }

    // The oracles' own count: 136 decisions across four regex token sets.
    EXPECT_EQ(decided, 136U);
}

TEST_F(Lexer_test, The_counterexample_cap_is_a_ceiling_on_the_states_the_search_holds)
{
    enum class Token_kind : std::uint8_t
    {
        zero,
        pair,
        one
    };

    // Over {0, 00, 01} the certificate (001, 0) is failed by 0001. The cap is the most states the search may hold, so
    // every cap below the smallest one that settles the question answers nothing rather than something, and every cap
    // from it on answers the same witness; zero holds nothing, not even the state the search starts in.
    Builder builder{};

    builder.add_token(text("0"), Token_kind::zero, 1);
    builder.add_token(text("00"), Token_kind::pair, 1);
    builder.add_token(text("01"), Token_kind::one, 1);

    const auto lexer{builder.build()};

    const auto [held_nothing, zero_settled]{lexer.window_counterexample("001", 0, 0)};

    EXPECT_FALSE(zero_settled);
    EXPECT_TRUE(held_nothing.empty());

    std::size_t holds{1};

    for (;; ++holds)
    {
        const auto [witness, exhaustive]{lexer.window_counterexample("001", 0, holds)};

        if (exhaustive)
        {
            break;
        }

        ASSERT_TRUE(witness.empty()) << "cap " << holds;
    }

    // Every byte of the witness admits at least one state, and the search settled with the witness at the cap.
    const auto [first_witness, first_settled]{lexer.window_counterexample("001", 0, holds)};

    EXPECT_GE(holds, 4U);
    EXPECT_EQ(first_witness, "0001");

    for (std::size_t cap{holds}; cap <= holds + 8; ++cap)
    {
        const auto [witness, exhaustive]{lexer.window_counterexample("001", 0, cap)};

        EXPECT_TRUE(exhaustive) << "cap " << cap;
        EXPECT_EQ(witness, "0001") << "cap " << cap;
    }

    // A certificate is proved exact only by an exhausted search, and a cap that stops the search before it exhausts
    // says nothing about it.
    const auto [stopped_witness, stopped_settled]{lexer.window_counterexample("1001", 2, 1)};

    const auto [exact_witness, exact_settled]{lexer.window_counterexample("1001", 2)};

    EXPECT_FALSE(stopped_settled);
    EXPECT_TRUE(exact_settled);
}

TEST_F(Lexer_test, Boundary_profile_finds_boundaries_where_no_covering_origin_is_fixed)
{
    // Over {0, 1, x, 001x, 011x} the window 0011 occurs in 0011 itself, cut 0|0|1|1, and in 0011x, cut 0|011x: gaps 0
    // and 1 are boundaries at every occurrence, while the token covering the final byte begins at offset 3 on the one
    // and at offset 1 on the other, so no certificate holds of the window at any origin. Gaps 2, 3 and 4 are cut in
    // 0011 and crossed in 0011x.
    const auto lexer{literal_lexer({"0", "1", "x", "001x", "011x"})};

    const auto [occurring, occurrence_settled]{lexer.window_occurrence("0011")};

    EXPECT_EQ(occurring, "0011");

    for (std::size_t origin{0}; origin < 4; ++origin)
    {
        const auto [witness, exhaustive]{lexer.window_counterexample("0011", origin)};

        EXPECT_TRUE(exhaustive) << origin;
        EXPECT_TRUE(fails(lexer, witness, "0011", origin)) << origin;
    }

    const auto [covered_from_three, from_three_settled]{lexer.window_counterexample("0011", 1)};

    const auto [covered_from_one, from_one_settled]{lexer.window_counterexample("0011", 3)};

    EXPECT_EQ(covered_from_three, "0011");
    EXPECT_EQ(covered_from_one, "0011x");

    const auto profile{lexer.boundary_profile("0011")};

    ASSERT_EQ(profile.size(), 5U);

    const std::vector expected{dfa::Gap::must, dfa::Gap::must, dfa::Gap::may, dfa::Gap::may, dfa::Gap::may};

    for (std::size_t gap{0}; gap < profile.size(); ++gap)
    {
        const auto& [verdict, crossed, cut]{profile[gap]};

        const auto& [crossed_witness, crossed_exhaustive]{crossed};

        const auto& [cut_witness, cut_exhaustive]{cut};

        EXPECT_EQ(verdict, expected[gap]) << gap;
        EXPECT_TRUE(crossed_exhaustive && cut_exhaustive) << gap;
        EXPECT_EQ(cut_witness, "0011") << gap;
        EXPECT_TRUE(shows(lexer, cut_witness, "0011", gap, true)) << gap;
        EXPECT_EQ(crossed_witness, verdict == dfa::Gap::must ? "" : "0011x") << gap;
        EXPECT_TRUE(crossed_witness.empty() || shows(lexer, crossed_witness, "0011", gap, false)) << gap;

        // Each verdict holds the two single-gap decisions.
        const auto [boundary_witness, boundary_settled]{lexer.boundary_counterexample("0011", gap)};

        const auto [crossing_witness, crossing_settled]{lexer.crossing_counterexample("0011", gap)};

        EXPECT_EQ(crossed_witness, boundary_witness) << gap;
        EXPECT_EQ(cut_witness, crossing_witness) << gap;
    }

    // The cap is the same ceiling as for the other searches: zero holds nothing, one holds only the state the search
    // starts in, and neither settles anything.
    const auto [boundary_at_zero, boundary_zero_settled]{lexer.boundary_counterexample("0011", 0, 0)};

    const auto [boundary_at_one, boundary_one_settled]{lexer.boundary_counterexample("0011", 2, 1)};

    const auto [crossing_at_one, crossing_one_settled]{lexer.crossing_counterexample("0011", 4, 1)};

    EXPECT_FALSE(boundary_zero_settled);
    EXPECT_FALSE(boundary_one_settled);
    EXPECT_TRUE(boundary_at_one.empty());
    EXPECT_FALSE(crossing_one_settled);

    const auto capped_profile{lexer.boundary_profile("0011", 1)};

    const auto undetermined{[](const dfa::Gap_verdict& gap) {
        const auto& [verdict, crossed, cut]{gap};

        return verdict == dfa::Gap::undetermined;
    }};

    EXPECT_TRUE(std::ranges::all_of(capped_profile, undetermined));

    // The empty window has no gap and a gap past the window's end names none of its gaps.
    EXPECT_THROW(std::ignore = lexer.boundary_counterexample("", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = lexer.boundary_counterexample("0011", 5), std::invalid_argument);
    EXPECT_THROW(std::ignore = lexer.crossing_counterexample("0011", 5), std::invalid_argument);
    EXPECT_THROW(std::ignore = lexer.boundary_profile(""), std::invalid_argument);
}

TEST_F(Lexer_test, Boundary_profile_agrees_with_a_brute_force_maximal_munch_oracle)
{
    // Every set of one to three literals over {a, b} of length one to three, and every window over {a, b} of length one
    // to four at every gap, the one after its final byte included. The oracle scans every input to length ten by
    // maximal munch over the literals themselves and records, per gap, the shortest input holding an occurrence crossed
    // there and the shortest holding one cut there, the input's end a cut. The profile must be exhaustive and read the
    // records: must where only a cut was seen, never where only a crossing was, may where both were and absent where
    // the window never occurred, which must be exactly where window_occurrence() places it nowhere, with every witness
    // as short as the oracle's. Every witness falls inside the oracle's bound, which bounds the check: a verdict whose
    // shortest refutation were longer than ten would pass here.
    constexpr std::size_t bound{10};

    const auto windows{words("ab", 4)};

    const auto inputs{words("ab", bound)};

    std::size_t decided{0};

    std::map<dfa::Gap, std::size_t> tally{};

    for (const auto& tokens : literal_sets(words("ab", 3), 3))
    {
        const auto lexer{literal_lexer(tokens)};

        const auto append_token{[](std::string out, const std::string& token) { return std::move(out) + token + ' '; }};

        const auto name{std::ranges::fold_left(tokens, std::string{}, append_token)};

        const auto shortest{shortest_occurrences(tokens, inputs, windows)};

        const auto length{[&](const std::string_view window, const std::size_t gap, const bool cut) {
            const auto found{shortest.find({window, gap, cut})};

            if (found == shortest.end())
            {
                return std::size_t{0};
            }

            const auto& [key, shortest_length]{*found};

            return shortest_length;
        }};

        for (const auto& window : windows)
        {
            const auto profile{lexer.boundary_profile(window)};

            const auto [occurring, occurrence_settled]{lexer.window_occurrence(window)};

            ASSERT_EQ(profile.size(), window.size() + 1) << name << window;
            ASSERT_TRUE(occurrence_settled) << name << window;

            for (std::size_t gap{0}; gap < profile.size(); ++gap)
            {
                const auto& [verdict, crossed, cut]{profile[gap]};

                const auto& [crossed_witness, crossed_exhaustive]{crossed};

                const auto& [cut_witness, cut_exhaustive]{cut};

                const auto crossing{length(window, gap, false)};

                const auto cutting{length(window, gap, true)};

                const auto expected{
                        crossing == 0 && cutting == 0 ? dfa::Gap::absent :
                        crossing == 0                 ? dfa::Gap::must :
                        cutting == 0                  ? dfa::Gap::never :
                                                        dfa::Gap::may};

                ++decided;

                ++tally[verdict];

                ASSERT_TRUE(crossed_exhaustive && cut_exhaustive) << name << window << ' ' << gap;
                EXPECT_EQ(verdict, expected) << name << window << ' ' << gap;
                EXPECT_EQ(verdict == dfa::Gap::absent, occurring.empty()) << name << window << ' ' << gap;
                EXPECT_EQ(crossed_witness.size(), crossing) << name << window << ' ' << gap;
                EXPECT_EQ(cut_witness.size(), cutting) << name << window << ' ' << gap;
                EXPECT_TRUE(crossed_witness.empty() || shows(lexer, crossed_witness, window, gap, false))
                        << name << window << ' ' << gap;
                EXPECT_TRUE(cut_witness.empty() || shows(lexer, cut_witness, window, gap, true))
                        << name << window << ' ' << gap;
            }
        }
    }

    // 469 token sets, 128 gaps each, and every verdict but undetermined reached.
    EXPECT_EQ(decided, 469U * 128U);

    for (const auto verdict : {dfa::Gap::must, dfa::Gap::never, dfa::Gap::may, dfa::Gap::absent})
    {
        EXPECT_GT(tally[verdict], 0U) << std::to_underlying(verdict);
    }
}

TEST_F(Lexer_test, A_window_certificate_holds_exactly_where_its_origin_is_must_and_every_later_gap_never)
{
    // The token covering the final byte begins at o exactly when gap o is a boundary and gaps o + 1 to |W| - 1 are not,
    // occurrence by occurrence, so the certificate (W, o) holds exactly when the profile is must at o and never at
    // every later gap inside the window, a window occurring nowhere holding every certificate vacuously with every gap
    // absent. Checked over the four regex token sets of the research oracles and every set of one to three literals
    // over {a, b} of length one to three, at every window over {a, b} of length one to four and every origin; a window
    // is_split_window() certifies holds the profile's certificate at the origin it reports, and some origins are must
    // without a certificate.
    std::vector<Lexer> lexers{};

    for (const auto& tokens : std::vector<std::vector<Regex>>{
                 {concat(plus(text("a")), text("b")), text("a")},
                 {concat(text("a"), text("b")), text("a"), text("b")},
                 {plus(text("a")), text("b")},
                 {choice(text("a"), concat(text("a"), text("b"))), text("b")}})
    {
        lexers.push_back(regex_lexer(tokens));
    }

    for (const auto& tokens : literal_sets(words("ab", 3), 3))
    {
        lexers.push_back(literal_lexer(tokens));
    }

    std::size_t certified{0};

    std::size_t uncertified{0};

    const auto check_window{
            [&certified, &uncertified](const Lexer& lexer, const std::size_t index, const std::string& window) {
                const auto model{lexer.is_split_window(window)};

                const auto profile{lexer.boundary_profile(window)};

                const auto is{[&profile](const std::size_t gap, const dfa::Gap verdict) {
                    const auto& [found, crossed, cut]{profile[gap]};

                    return found == verdict || found == dfa::Gap::absent;
                }};

                const auto is_never{[&is](const std::size_t gap) { return is(gap, dfa::Gap::never); }};

                for (std::size_t origin{0}; origin < window.size(); ++origin)
                {
                    const auto [counterexample, settled]{lexer.window_counterexample(window, origin)};

                    const auto later{std::ranges::all_of(std::views::iota(origin + 1, window.size()), is_never)};

                    const auto holds{is(origin, dfa::Gap::must) && later};

                    ASSERT_TRUE(settled) << index << ' ' << window << ' ' << origin;
                    EXPECT_EQ(counterexample.empty(), holds) << index << ' ' << window << ' ' << origin;

                    EXPECT_TRUE(model != origin || holds) << index << ' ' << window << ' ' << origin;

                    if (holds)
                    {
                        ++certified;
                    }

                    if (is(origin, dfa::Gap::must) && !holds)
                    {
                        ++uncertified;
                    }
                }
            }};

    for (std::size_t index{0}; index < lexers.size(); ++index)
    {
        const auto& lexer{lexers[index]};

        for (const auto& window : words("ab", 4))
        {
            check_window(lexer, index, window);
        }
    }

    EXPECT_GT(certified, 0U);
    EXPECT_GT(uncertified, 0U);
}

TEST_F(Lexer_test, Absence_is_given_at_every_gap_or_at_none_from_the_first_cap_at_which_any_search_proves_it)
{
    enum class Token_kind : std::uint8_t
    {
        run
    };

    // Over {a+} the window ab occurs nowhere, and a search at one gap exhausts from a smaller cap than one at another,
    // so reading absence off each gap's own searches alone would mix it with undetermined. The profile gives it at
    // every gap from the first cap at which any proof of it exhausts, window_occurrence() or a gap's two searches, and
    // at no gap below that, every gap undetermined there; the caps are read off the searches, and here the occurrence
    // search's is the first.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);

    const auto lexer{builder.build()};

    const auto first_cap{[]<typename Exhausts>(const Exhausts& exhausts) {
        std::size_t cap{1};

        while (!exhausts(cap))
        {
            ++cap;
        }

        return cap;
    }};

    const auto occurrence_exhausts{[&lexer](const std::size_t cap) {
        const auto [witness, exhaustive]{lexer.window_occurrence("ab", cap)};

        return exhaustive;
    }};

    const auto occurrence{first_cap(occurrence_exhausts)};

    auto proof{occurrence};

    for (std::size_t gap{0}; gap <= 2; ++gap)
    {
        const auto gap_exhausts{[&lexer, gap](const std::size_t cap) {
            const auto [boundary_witness, boundary_exhaustive]{lexer.boundary_counterexample("ab", gap, cap)};

            if (!boundary_exhaustive)
            {
                return false;
            }

            const auto [crossing_witness, crossing_exhaustive]{lexer.crossing_counterexample("ab", gap, cap)};

            return crossing_exhaustive;
        }};

        const auto gap_proof{first_cap(gap_exhausts)};

        proof = std::min(proof, gap_proof);
    }

    const auto [absent, absent_settled]{lexer.window_occurrence("ab", occurrence)};

    EXPECT_EQ(proof, occurrence);
    EXPECT_TRUE(absent.empty());

    std::size_t proved{0};

    std::size_t stopped{0};

    for (const auto cap : std::array<std::size_t, 7>{3, 4, 5, 6, 7, 8, 1024})
    {
        const auto profile{lexer.boundary_profile("ab", cap)};

        const auto absent_gaps{std::ranges::count(profile, dfa::Gap::absent, &dfa::Gap_verdict::verdict)};

        ASSERT_EQ(profile.size(), 3U) << cap;
        EXPECT_TRUE(absent_gaps == 0 || std::cmp_equal(absent_gaps, profile.size())) << cap;
        EXPECT_EQ(std::cmp_equal(absent_gaps, profile.size()), cap >= proof) << cap;

        if (cap >= proof)
        {
            ++proved;

            continue;
        }

        ++stopped;

        for (std::size_t gap{0}; gap < profile.size(); ++gap)
        {
            const auto& [verdict, crossed, cut]{profile[gap]};

            const auto [boundary_witness, boundary_exhaustive]{lexer.boundary_counterexample("ab", gap, cap)};

            const auto [crossing_witness, crossing_exhaustive]{lexer.crossing_counterexample("ab", gap, cap)};

            EXPECT_EQ(verdict, dfa::Gap::undetermined) << cap << ' ' << gap;
            EXPECT_FALSE(boundary_exhaustive) << cap << ' ' << gap;
            EXPECT_FALSE(crossing_exhaustive) << cap << ' ' << gap;
        }
    }

    EXPECT_GT(proved, 0U);
    EXPECT_GT(stopped, 0U);
}

TEST_F(Lexer_test, A_gap_verdict_decided_under_a_small_cap_is_the_verdict_under_the_default_one)
{
    // A verdict is a proof, so a cap that leaves it decided leaves the verdict the default cap gives: must, never or
    // may with the same witnesses, showing the window occurring, and absent at every gap where the default cap gives
    // it, never at some gaps alone. Checked over every set of one to three literals over {a, b} of length one to three,
    // every window over {a, b} of length one to three and six caps, one that stops every search.
    std::size_t decided{0};

    std::size_t stopped{0};

    const auto check{[&](const std::vector<dfa::Gap_verdict>& profile, const std::vector<dfa::Gap_verdict>& reference,
                         const bool occurs, const std::string& label) {
        const auto absent_gaps{std::ranges::count(profile, dfa::Gap::absent, &dfa::Gap_verdict::verdict)};

        ASSERT_EQ(profile.size(), reference.size()) << label;
        EXPECT_TRUE(absent_gaps == 0 || std::cmp_equal(absent_gaps, profile.size())) << label;

        for (std::size_t gap{0}; gap < profile.size(); ++gap)
        {
            const auto& [verdict, crossed, cut]{profile[gap]};

            if (verdict == dfa::Gap::undetermined)
            {
                ++stopped;

                continue;
            }

            ++decided;

            const auto& [reference_verdict, reference_crossed, reference_cut]{reference[gap]};

            const auto& [crossed_witness, crossed_exhaustive]{crossed};

            const auto& [cut_witness, cut_exhaustive]{cut};

            const auto& [reference_crossed_witness, reference_crossed_exhaustive]{reference_crossed};

            const auto& [reference_cut_witness, reference_cut_exhaustive]{reference_cut};

            EXPECT_EQ(verdict, reference_verdict) << label << ' ' << gap;
            EXPECT_EQ(crossed_witness, reference_crossed_witness) << label << ' ' << gap;
            EXPECT_EQ(cut_witness, reference_cut_witness) << label << ' ' << gap;
            EXPECT_EQ(verdict == dfa::Gap::absent, !occurs) << label << ' ' << gap;
            EXPECT_EQ(verdict == dfa::Gap::absent, crossed_witness.empty() && cut_witness.empty())
                    << label << ' ' << gap;
        }
    }};

    for (const auto& tokens : literal_sets(words("ab", 3), 3))
    {
        const auto lexer{literal_lexer(tokens)};

        const auto name{std::format("{} {}", tokens.front(), tokens.size())};

        for (const auto& window : words("ab", 3))
        {
            const auto reference{lexer.boundary_profile(window)};

            const auto [witness, exhaustive]{lexer.window_occurrence(window)};

            ASSERT_TRUE(exhaustive) << name << ' ' << window;

            for (const auto cap : std::array<std::size_t, 6>{2, 4, 8, 16, 32, 1024})
            {
                const auto capped{lexer.boundary_profile(window, cap)};

                const auto label{std::format("{} {} {}", name, window, cap)};

                check(capped, reference, !witness.empty(), label);
            }
        }
    }

    EXPECT_GT(decided, 0U);
    EXPECT_GT(stopped, 0U);
}

TEST_F(Lexer_test, Must_and_never_gaps_stay_so_when_the_window_is_extended_on_either_side)
{
    // Every occurrence of an extension of W holds an occurrence of W, so a verdict quantified over every occurrence
    // carries over at the shifted gap: a gap that is a boundary at every occurrence of W is one at every occurrence of
    // the extension, a gap crossed at every occurrence stays crossed, and a window occurring nowhere has no occurring
    // extension. An extension may occur nowhere itself, every gap of it absent, which both claims allow. May carries
    // nothing, an extension selecting the occurrences on one side of it. Checked over every set of one or two literals
    // over {a, b} of length one to three, every window over {a, b} of length one to three and each of its four one-byte
    // extensions, at every gap, shifted by one on the left and unshifted on the right.
    std::size_t carried{0};

    std::size_t sharpened{0};

    const auto compare{[&](const std::vector<dfa::Gap_verdict>& profile, const std::vector<dfa::Gap_verdict>& extension,
                           const std::size_t shift, const std::string& label) {
        for (std::size_t gap{0}; gap < profile.size(); ++gap)
        {
            const auto& [verdict, crossed, cut]{profile[gap]};

            const auto& [extended, extended_crossed, extended_cut]{extension[gap + shift]};

            if (verdict == dfa::Gap::may)
            {
                if (extended == dfa::Gap::must || extended == dfa::Gap::never)
                {
                    ++sharpened;
                }

                continue;
            }

            EXPECT_TRUE(extended == verdict || extended == dfa::Gap::absent) << label << ' ' << gap;

            ++carried;
        }
    }};

    for (const auto& tokens : literal_sets(words("ab", 3), 2))
    {
        const auto lexer{literal_lexer(tokens)};

        for (const auto& window : words("ab", 3))
        {
            const auto profile{lexer.boundary_profile(window)};

            for (const auto byte : std::string_view{"ab"})
            {
                const auto label{std::format("{} {} {}", tokens.front(), byte, window)};

                const auto extended_left{lexer.boundary_profile(byte + window)};

                const auto extended_right{lexer.boundary_profile(window + byte)};

                compare(profile, extended_left, 1, label);
                compare(profile, extended_right, 0, label);
            }
        }
    }

    EXPECT_GT(carried, 0U);
    EXPECT_GT(sharpened, 0U);
}

TEST_F(Lexer_test, Window_fallback_plans_parallel_cuts_where_no_byte_certifies)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        operator_symbol,
        string
    };

    // The study's motivating shape: whitespace runs swallow the newline, string bodies swallow every printable byte,
    // and no single byte certifies; the two-byte window "\n!" still pins a token start at the '!'.
    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);
    builder.add_token(text("!"), Token_kind::operator_symbol, 1);
    builder.add_token(string_literal_regex(), Token_kind::string, 2);

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    const auto window{lexer.is_split_window("\n!")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    std::string input{};

    for (int block{0}; block < 64; ++block)
    {
        input += "alpha beta \"quoted text!\" gamma\n!delta\n!";
    }

    // The default planner keeps the unconditional byte contract and degenerates here; window recovery is the explicit
    // sibling with its documented completely-tokenizable condition.
    ASSERT_EQ(lexer.chunk_boundaries(input, 8), (std::vector<std::size_t>{0, input.size()}));

    const auto boundaries{lexer.chunk_boundaries_with_windows(input.begin(), input.end(), 8)};

    ASSERT_GT(boundaries.size(), 2U);

    for (std::size_t index{1}; index < boundaries.size(); ++index)
    {
        EXPECT_GT(boundaries[index], boundaries[index - 1]);
    }

    // The semantic assertion: the concatenated chunk-local streams equal the serial stream, token for token.
    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Window_fallback_degrades_honestly_and_the_equality_check_has_teeth)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    // A grammar the window search exhausts plans the single whole-input chunk rather than cutting unsafely.
    Builder unbounded{};

    unbounded.add_token(plus(text("a")), Token_kind::identifier, 1);

    const std::string runs(64, 'a');

    const auto exhausted{unbounded.build().chunk_boundaries_with_windows(runs.begin(), runs.end(), 4)};

    EXPECT_EQ(exhausted, (std::vector<std::size_t>{0, runs.size()}));

    // A nullable set is planned as its positive-width equivalent, {a+, space}: the space certifies as a byte and no
    // window over a run of a certifies, so on this input it takes the same degradation.
    Builder nullable{};

    nullable.add_token(kleene(text("a")), Token_kind::identifier, 1);
    nullable.add_token(text(" "), Token_kind::whitespace, 1);

    const auto refused{nullable.build().chunk_boundaries_with_windows(runs.begin(), runs.end(), 4)};

    EXPECT_EQ(refused, (std::vector<std::size_t>{0, runs.size()}));

    // The teeth of the equality check above: a deliberately wrong cut inside a token must not reproduce the serial
    // stream, so the assertion that plans do reproduce it is falsifiable, not decorative.
    Builder identifier_words{};

    identifier_words.add_token(identifier_regex(), Token_kind::identifier, 1);
    identifier_words.add_token(text(" "), Token_kind::whitespace, 1);

    const auto lexer{identifier_words.build()};

    const std::string input{"alpha beta"};

    const auto [serial, consumed]{serial_stream<Token_kind>(lexer, input)};

    const std::vector<std::size_t> wrong_cut{0, 2, input.size()};

    const auto [wrongly_chunked, wrongly_consumed]{rejoined_stream<Token_kind>(lexer, input, wrong_cut)};

    EXPECT_NE(wrongly_chunked, serial);
}

TEST_F(Lexer_test, Malformed_input_keeps_the_default_prefix_guarantee_and_windows_promise_nothing)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    // The minimal vector: over {a, aa, ab, bc} the serial scan of "abc" takes ab and fails at c, consuming two of
    // three. The default plan stays byte-only and reproduces exactly that. The window plan legally cuts at the
    // certified "bc" occurrence, both fragments consume fully, and the concatenation is not even a prefix of the serial
    // stream: full per-chunk consumption proves nothing about the whole input, which is precisely the conditional
    // contract chunk_boundaries_with_windows() documents.
    Builder builder{};

    builder.add_token(choice(choice(text("a"), text("aa")), choice(text("ab"), text("bc"))), Token_kind::word, 1);

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    const auto window{lexer.is_split_window("bc")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 0U);

    const std::string input{"abc"};

    const auto [serial, consumed]{serial_stream<Token_kind>(lexer, input)};

    EXPECT_EQ(consumed, 2U);
    EXPECT_EQ(serial, (std::vector<std::pair<Token_kind, std::size_t>>{{Token_kind::word, 2}}));

    const auto byte_plan{lexer.chunk_boundaries(input, 3)};

    EXPECT_EQ(byte_plan, (std::vector<std::size_t>{0, input.size()}));

    std::vector<std::pair<Token_kind, std::size_t>> parallel{};

    const auto collect{[&parallel](const std::size_t, const Token_kind kind, const std::size_t length) {
        parallel.emplace_back(kind, length);
    }};

    const auto per_chunk{lexer.tokenize_all_parallel<Token_kind>(input, 3, collect)};

    EXPECT_EQ(per_chunk, (std::vector<std::size_t>{2}));
    EXPECT_EQ(parallel, serial);

    const auto windowed{lexer.chunk_boundaries_with_windows(input, 3)};

    EXPECT_EQ(windowed, (std::vector<std::size_t>{0, 1, 3}));

    const auto [rejoined, rejoined_consumed]{rejoined_stream<Token_kind>(lexer, input, windowed)};

    EXPECT_EQ(rejoined_consumed, input.size());

    ASSERT_EQ(
            rejoined, (std::vector<std::pair<Token_kind, std::size_t>>{{Token_kind::word, 1}, {Token_kind::word, 2}}));
    EXPECT_NE(rejoined, serial);
}

TEST_F(Lexer_test, Relaxed_certificate_admits_a_restart_into_an_equivalent_state)
{
    enum class Token_kind : std::uint8_t
    {
        word,
        run,
        kept
    };

    // Splitting at a b inside ab* leaves a shorter ab* on the left and a b+ on the right, both discarded. The restart
    // on b enters a state accepting b+, the interrupted scan stays in one accepting ab*: different states, which
    // minimization keeps apart, and the same future once the two discarded kinds are not told apart.
    Builder builder{};

    builder.add_token(concat(text("a"), kleene(text("b"))), Token_kind::word, 1);
    builder.add_token(plus(text("b")), Token_kind::run, 1);
    builder.add_token(text("c"), Token_kind::kept, 1);
    builder.set_ignored_tokens({Token_kind::word, Token_kind::run});

    const auto lexer{builder.build()};

    EXPECT_TRUE(lexer.is_split_point_ignoring('b'));
    EXPECT_FALSE(lexer.is_split_point('b'));
}

TEST_F(Lexer_test, Relaxed_certificate_tells_a_kept_kind_from_the_discarded_ones_whatever_its_id)
{
    // Discarded a and ac, kept c. Cutting ac before its c turns one discarded token into a discarded a and a kept c, so
    // c is no split point modulo the discard. The restart on c accepts the kept c and the interrupted scan the
    // discarded ac, both with nothing after, so only their kinds tell them apart, and the largest ID must not pass for
    // a discarded one.
    Builder builder{};

    builder.add_token(choice(text("a"), text("ac")), std::size_t{0}, 1);
    builder.add_token(text("c"), std::numeric_limits<std::size_t>::max(), 1);
    builder.set_ignored_tokens(std::vector<std::size_t>{0});

    EXPECT_FALSE(builder.build().is_split_point_ignoring('c'));
}

TEST_F(Lexer_test, Relaxed_certificate_still_refuses_a_safe_symbol_whose_restart_differs_at_once)
{
    enum class Token_kind : std::uint8_t
    {
        discarded,
        kept
    };

    // Every cut before a b is safe modulo the discarded kind: inside abc the left piece is a and the right piece scans
    // b then c, all discarded, as the serial abc is. The restart on b accepts at once where the interrupted ab does
    // not, so the two states have different futures and the certificate refuses. This is the known limit of a local
    // test; deciding such symbols exactly needs a search, not a table row.
    Builder builder{};

    for (const std::string_view word : {"a", "b", "c", "abc"})
    {
        builder.add_token(text(word), Token_kind::discarded, 1);
    }

    builder.add_token(text("k"), Token_kind::kept, 1);
    builder.add_token(text("kk"), Token_kind::kept, 1);
    builder.set_ignored_tokens({Token_kind::discarded});

    const auto lexer{builder.build()};

    EXPECT_FALSE(lexer.is_split_point_ignoring('b'));
}

TEST_F(Lexer_test, Default_planning_uses_the_exact_certificate_never_the_relaxed_one)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        operator_symbol
    };

    // With the run declared ignored, the relaxed map certifies bytes the exact map refuses; the default planner must
    // plan with the exact certificate only, or malformed input loses the serial-prefix guarantee the relaxed
    // certificate never carried.
    Builder builder{};

    builder.add_token(plus(text("a")), Token_kind::run, 1);
    builder.add_token(text("b"), Token_kind::operator_symbol, 1);
    builder.set_ignored_tokens({Token_kind::run});

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_point_ignoring('a'));
    ASSERT_FALSE(lexer.is_split_point('a'));

    const std::string input{"aaaab"};

    // The exact map certifies only 'b', so the only interior cut available to the default plan is at the 'b'.
    const auto boundaries{lexer.chunk_boundaries(input, 3)};

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_TRUE(lexer.is_split_point(input[boundaries[index]]));

        EXPECT_EQ(input[boundaries[index]], 'b');
    }
}

TEST_F(Lexer_test, Window_decisions_unroll_a_nullable_start_and_keep_the_live_target_filter)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        operator_symbol
    };

    // A nullable set is decided as its positive-width equivalent: optional(a) accepts emptily, but the scan never emits
    // the empty token, so the set scans exactly as {a, b} does, and there every b begins a token. The decision runs
    // over the compiled tables, whose start state is the fresh unrolled one that neither accepts nor is re-entered, so
    // the window proof's premise holds and "b" certifies at 0.
    Builder nullable{};

    nullable.add_token(optional(text("a")), Token_kind::identifier, 1);
    nullable.add_token(text("b"), Token_kind::operator_symbol, 1);

    EXPECT_EQ(nullable.build().is_split_window("b"), std::optional<std::size_t>{0});

    // The non-re-entrant rename guard, pinned by the language (ab)*a: reading "a" from the initial state must not be
    // treated as beginning a token there, because live paths re-enter the start; the completely tokenizable input "aba"
    // is one token covering its final "a" from offset 0, so ("a", 0) is no certificate and the window is refused.
    Builder reentrant{};

    reentrant.add_token(concat(kleene(text("ab")), text("a")), Token_kind::identifier, 1);

    EXPECT_FALSE(reentrant.build().is_split_window("a").has_value());

    // A token whose tail is the empty language leaves live-looking transitions into dead states: over {ab followed by
    // nothing acceptable, b} the window "ab" must empty the cloud and refuse, because no accepting future exists past
    // the a.
    Builder dead_tail{};

    dead_tail.add_token(concat(text("ab"), any_of(Set{})), Token_kind::identifier, 1);
    dead_tail.add_token(text("b"), Token_kind::operator_symbol, 1);

    const auto dead_lexer{dead_tail.build()};

    EXPECT_FALSE(dead_lexer.is_split_window("ab").has_value());

    // The acceptance-gated seed has its own live-target filter, and the window "a" reaches it alone: every direct
    // history steps dead, so the only hypothesis left is the fresh seed landing in the dead tail, which that filter
    // drops, and the cloud empties and refuses.
    EXPECT_FALSE(dead_lexer.is_split_window("a").has_value());
}

TEST_F(Lexer_test, Next_certified_start_answers_both_certificate_kinds_in_evidence_order)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        semicolon,
        optional_a,
        letter_b
    };

    // Windows only: over identifiers and whitespace no byte certifies, and the first certificate past the junk is the
    // four-byte "def " at offset 6 with origin 3, so the answer is 9 from either starting point before it; at or past
    // the end there is no answer.
    Builder word_set{};

    word_set.add_token(identifier_regex(), Token_kind::identifier, 2);
    word_set.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto word_lexer{word_set.build()};

    const std::string junk{"abc@@@def ghi"};

    EXPECT_EQ(word_lexer.next_certified_start(junk, 0), std::optional<std::size_t>{9});
    EXPECT_EQ(word_lexer.next_certified_start(junk, 4), std::optional<std::size_t>{9});
    EXPECT_FALSE(word_lexer.next_certified_start(junk, junk.size()).has_value());
    EXPECT_FALSE(word_lexer.next_certified_start(junk, junk.size() + 42).has_value());

    // Bytes answer at their own position, and the offset itself is a candidate: the search is inclusive.
    Builder pair{};

    pair.add_token(text("a"), Token_kind::identifier, 1);
    pair.add_token(text(";"), Token_kind::semicolon, 1);

    const auto pair_lexer{pair.build()};

    EXPECT_EQ(pair_lexer.next_certified_start("a?;b", 1), std::optional<std::size_t>{2});
    EXPECT_EQ(pair_lexer.next_certified_start(";", 0), std::optional<std::size_t>{0});

    // Both certificate kinds at once: the semicolon is a certified byte, yet a window met earlier in the walk answers
    // first, because the walk returns the first certificate in evidence order and keeps both kinds live, so the answer
    // is 3, ahead of the semicolon.
    Builder mixed{};

    mixed.add_token(identifier_regex(), Token_kind::identifier, 2);
    mixed.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    mixed.add_token(text(";"), Token_kind::semicolon, 3);

    const auto mixed_lexer{mixed.build()};

    ASSERT_TRUE(mixed_lexer.is_split_point(';'));
    EXPECT_EQ(mixed_lexer.next_certified_start("x@ abc;", 1), std::optional<std::size_t>{3});

    // A nullable set is decided through its positive-width equivalent, here {a, b}: 'b' certifies as a byte and "bb" as
    // a window at the second b, and the walk answers at the byte first, in evidence order.
    Builder nullable{};

    nullable.add_token(optional(text("a")), Token_kind::optional_a, 1);
    nullable.add_token(text("b"), Token_kind::letter_b, 1);

    const auto nullable_lexer{nullable.build()};

    ASSERT_TRUE(nullable_lexer.is_split_point('b'));
    ASSERT_EQ(nullable_lexer.is_split_window("bb"), std::optional<std::size_t>{1});
    EXPECT_EQ(nullable_lexer.next_certified_start("?bb", 1), std::optional<std::size_t>{1});

    // A single unbounded run certifies nothing at any length.
    Builder unbounded{};

    unbounded.add_token(plus(any_of(Set{'a'})), Token_kind::identifier, 1);

    EXPECT_FALSE(unbounded.build().next_certified_start("aaaa", 1).has_value());
}

TEST_F(Lexer_test, Malformed_input_keeps_the_serial_prefix_across_real_cuts)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        separator
    };

    // Certified cuts on both sides of a malformed byte: serial fails at the '?', earlier chunks reproduce its stream
    // exactly, and later chunks scan independently, so the serial stream is a strict prefix of the concatenation, which
    // is precisely tokenize_all_parallel()'s documented malformed-input promise.
    Builder builder{};

    builder.add_token(text("a"), Token_kind::identifier, 1);
    builder.add_token(text(";"), Token_kind::separator, 1);

    const auto lexer{builder.build()};

    const std::string input{"a;?;a"};

    const auto boundaries{lexer.chunk_boundaries(input, 3)};

    ASSERT_GT(boundaries.size(), 2U);

    const auto [serial, consumed]{serial_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(consumed, 2U);

    std::vector<std::vector<std::pair<Token_kind, std::size_t>>> streams(boundaries.size() - 1);

    const auto collect{[&streams](const std::size_t chunk, const Token_kind kind, const std::size_t length) {
        streams[chunk].emplace_back(kind, length);
    }};

    const auto per_chunk{lexer.tokenize_all_parallel<Token_kind>(input, 3, collect)};

    ASSERT_EQ(per_chunk.size(), boundaries.size() - 1);

    std::vector<std::pair<Token_kind, std::size_t>> joined{};

    for (const auto& stream : streams)
    {
        joined.insert(joined.end(), stream.begin(), stream.end());
    }

    ASSERT_GE(joined.size(), serial.size());

    const auto joined_prefix{joined | std::views::take(serial.size())};

    EXPECT_TRUE(std::ranges::equal(serial, joined_prefix));

    EXPECT_GT(joined.size(), serial.size());
}

TEST_F(Lexer_test, Window_planner_reaches_the_four_byte_study_case)
{
    enum class Token_kind : std::uint8_t
    {
        whitespace,
        comment,
        star,
        slash
    };

    // The study's principal four-byte certificate: block comments over an operator alphabet, where the pair */ reads as
    // a closer in comment context and as two operators otherwise, so no shorter window resolves the origin. The planner
    // promises windows of two to four bytes; this pins the upper bound at the length the paper's table actually needs.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'\t'})), Token_kind::whitespace, 1);
    builder.add_token(block_comment_regex(), Token_kind::comment, 1);
    builder.add_token(text("*"), Token_kind::star, 2);
    builder.add_token(text("/"), Token_kind::slash, 2);

    const auto lexer{builder.build()};

    const auto window{lexer.is_split_window("\t*/\t")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 3U);

    const std::string input{"\t*/\t\t*/\t"};

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 2)};

    ASSERT_EQ(boundaries.size(), 3U);
    EXPECT_EQ(boundaries[1], 7U);
}

TEST_F(Lexer_test, Lag_and_rescue_freeness_decide_the_rollback_shape)
{
    enum class Token_kind : std::uint8_t
    {
        a,
        abc,
        bc,
        ab_star_c,
        b,
        x,
        abb,
        c,
        semicolon
    };

    // {a, abc}: one stretch of one state after the accepted a, opened by b, which starts no token: lag one,
    // rescue-free. Not zero-lag, yet the restart abstraction is exact on it.
    Builder toy{};

    toy.add_token(text("a"), Token_kind::a, 2);
    toy.add_token(text("abc"), Token_kind::abc, 1);

    const auto toy_lexer{toy.build()};

    EXPECT_EQ(toy_lexer.lag(), std::optional<std::size_t>{1});
    EXPECT_TRUE(toy_lexer.rescue_free());

    // {a, abc, bc}: the stretch after a opens on b, which starts the token bc from the initial state, so a gate reading
    // the tables alone cannot tell this set from a rescuable one; but every completely tokenizable continuation of that
    // stretch begins with bc, whose c closes abc instead, so the scan never rolls back to a. The exact decision says
    // so.
    Builder guarded{};

    guarded.add_token(text("a"), Token_kind::a, 2);
    guarded.add_token(text("abc"), Token_kind::abc, 1);
    guarded.add_token(text("bc"), Token_kind::bc, 2);

    const auto guarded_lexer{guarded.build()};

    EXPECT_EQ(guarded_lexer.lag(), std::optional<std::size_t>{1});
    EXPECT_TRUE(guarded_lexer.rescue_free());

    const auto [guarded_witness, guarded_exhaustive]{guarded_lexer.rescue()};

    EXPECT_TRUE(guarded_exhaustive);
    EXPECT_TRUE(guarded_witness.empty());

    // {a, ab*c, b, x}: the b-loop after the accepted a is a post-accept nonaccepting cycle, the executed unboundedness
    // certificate; the decider must refuse a number rather than invent one.
    Builder classic{};

    classic.add_token(text("a"), Token_kind::a, 2);
    classic.add_token(concat(text("a"), concat(kleene(text("b")), text("c"))), Token_kind::ab_star_c, 1);
    classic.add_token(text("b"), Token_kind::b, 2);
    classic.add_token(text("x"), Token_kind::x, 2);

    const auto classic_lexer{classic.build()};

    EXPECT_FALSE(classic_lexer.lag().has_value());

    // {a, abb, b, c}: bounded lag but rescuable: on ab the scan of a reads the b, the input ends, and the rollback to a
    // leaves b as the next token, where a scheme restarting at every accept stands in the stretch with nothing to emit.
    // The witness is that shortest input, and a search capped below its own size says nothing rather than something.
    Builder rescuable{};

    rescuable.add_token(text("a"), Token_kind::a, 2);
    rescuable.add_token(text("abb"), Token_kind::abb, 1);
    rescuable.add_token(text("b"), Token_kind::b, 2);
    rescuable.add_token(text("c"), Token_kind::c, 2);

    const auto rescuable_lexer{rescuable.build()};

    EXPECT_EQ(rescuable_lexer.lag(), std::optional<std::size_t>{1});
    EXPECT_FALSE(rescuable_lexer.rescue_free());

    const auto [rescued_on, rescue_settled]{rescuable_lexer.rescue()};

    const auto [capped_witness, capped_settled]{rescuable_lexer.rescue(1)};

    EXPECT_EQ(rescued_on, "ab");
    EXPECT_TRUE(rescue_settled);
    EXPECT_FALSE(capped_settled);
    EXPECT_TRUE(capped_witness.empty());

    // Two single-byte tokens: no stretch exists, lag zero, rescue-free vacuously.
    Builder flat{};

    flat.add_token(text("a"), Token_kind::a, 1);
    flat.add_token(text(";"), Token_kind::semicolon, 2);

    const auto flat_lexer{flat.build()};

    EXPECT_EQ(flat_lexer.lag(), std::optional<std::size_t>{0});
    EXPECT_TRUE(flat_lexer.rescue_free());
}

TEST_F(Lexer_test, A_search_cap_is_a_ceiling_on_the_states_a_decision_holds)
{
    enum class Token_kind : std::uint8_t
    {
        a,
        abc,
        b,
        c,
        a0c,
        hex
    };

    // {a, abc, b, c} is rescued on "ab": the scan of a reads the b, the input ends, and the rollback to a leaves b as
    // the next token. The tokens a0c and 0x add a live successor on '0', a byte before 'b', to the position the search
    // reaches after the a, so the expansion that finds the witness admits a state before it answers.
    //
    // The cap is the most states the search may hold, so an answer is always one the cap paid for: the witness comes
    // back first at the cap that equals the thirteen states holding it takes, and every smaller cap answers nothing
    // rather than something, twelve included, which pins the cap at every admission the search makes.
    Builder builder{};

    builder.add_token(text("a"), Token_kind::a, 2);
    builder.add_token(text("abc"), Token_kind::abc, 1);
    builder.add_token(text("b"), Token_kind::b, 2);
    builder.add_token(text("c"), Token_kind::c, 2);
    builder.add_token(text("a0c"), Token_kind::a0c, 1);
    builder.add_token(text("0x"), Token_kind::hex, 1);

    const auto lexer{builder.build()};

    constexpr std::size_t holds{13};

    for (std::size_t cap{0}; cap < holds; ++cap)
    {
        const auto [witness, exhaustive]{lexer.rescue(cap)};

        EXPECT_FALSE(exhaustive) << "cap " << cap;
        EXPECT_TRUE(witness.empty()) << "cap " << cap;
    }

    for (std::size_t cap{holds}; cap <= holds + 8; ++cap)
    {
        const auto [witness, exhaustive]{lexer.rescue(cap)};

        EXPECT_TRUE(exhaustive) << "cap " << cap;
        EXPECT_EQ(witness, "ab") << "cap " << cap;
    }

    // Zero holds nothing, not even the state a search starts in, so neither decision settles anything under it.
    const auto [rescue_witness, rescue_settled]{lexer.rescue(0)};

    const auto [difference_witness, difference_settled]{lexer.boundary_difference(lexer, 0)};

    EXPECT_FALSE(rescue_settled);
    EXPECT_FALSE(difference_settled);
    EXPECT_TRUE(difference_witness.empty());
}

TEST_F(Lexer_test, Anchored_starts_answer_where_certificates_cannot)
{
    enum class Token_kind : std::uint8_t
    {
        ab,
        ba
    };

    // {ab, ba}: the end-of-input witness. Position zero of the tail "ab" is invariant under every completely
    // tokenizable repair, because the only crossing scenario dies in the tail, yet no certified window of any length
    // explains it: "baba" refutes every candidate, and the refuting continuations need bytes past the end. The anchored
    // query answers zero while the certificate walk stays silent, which is the whole point of anchoring; the quantifier
    // is complete repairs alone, since the failing repair "b" reaches one-byte evidence in "bab" with no boundary at
    // the answer's image.
    Builder builder{};

    builder.add_token(text("ab"), Token_kind::ab, 1);
    builder.add_token(text("ba"), Token_kind::ba, 2);

    const auto lexer{builder.build()};

    EXPECT_EQ(lexer.next_anchored_start("ab", 0), std::optional<std::size_t>{0});
    EXPECT_FALSE(lexer.next_certified_start("ab", 0).has_value());

    // Position one is not invariant, and nothing at or after it is: a refusal, not a weaker answer.
    EXPECT_FALSE(lexer.next_anchored_start("ab", 1).has_value());

    // The alternating tail carries every even position and nothing else.
    EXPECT_EQ(lexer.next_anchored_start("abab", 0), std::optional<std::size_t>{0});
    EXPECT_EQ(lexer.next_anchored_start("abab", 1), std::optional<std::size_t>{2});
    EXPECT_FALSE(lexer.next_anchored_start("abab", 3).has_value());

    // A tail beyond repair is a refusal, never a vacuous answer: every position of an unrepairable tail is invariant
    // over an empty set of repairs, and answering one would send a driver to a dead spot.
    EXPECT_FALSE(lexer.next_anchored_start("z", 0).has_value());
}

TEST_F(Lexer_test, Anchored_starts_reach_past_the_window_cap)
{
    enum class Token_kind : std::uint8_t
    {
        bb,
        aab,
        baa,
        aaa
    };

    // Over {bb, aab, baa, aaa} the shortest usable certificate is five bytes, one past the documented window cap, so
    // the certificate walk refuses everywhere on this tail; the anchored decider answers three positions, the five-byte
    // window's origin among them.
    Builder builder{};

    builder.add_token(text("bb"), Token_kind::bb, 1);
    builder.add_token(text("aab"), Token_kind::aab, 2);
    builder.add_token(text("baa"), Token_kind::baa, 3);
    builder.add_token(text("aaa"), Token_kind::aaa, 4);

    const auto lexer{builder.build()};

    const std::string tail{"baaabbbb"};

    EXPECT_FALSE(lexer.next_certified_start(tail, 0).has_value());

    EXPECT_EQ(lexer.next_anchored_start(tail, 0), std::optional<std::size_t>{1});
    EXPECT_EQ(lexer.next_anchored_start(tail, 2), std::optional<std::size_t>{4});
    EXPECT_EQ(lexer.next_anchored_start(tail, 5), std::optional<std::size_t>{6});
    EXPECT_FALSE(lexer.next_anchored_start(tail, 7).has_value());
}

TEST_F(Lexer_test, Minimal_repair_prices_the_tail_or_certifies_refusal)
{
    enum class Token_kind : std::uint8_t
    {
        ab,
        ba
    };

    Builder builder{};

    builder.add_token(text("ab"), Token_kind::ab, 1);
    builder.add_token(text("ba"), Token_kind::ba, 2);

    const auto lexer{builder.build()};

    // A tail that tokenizes needs nothing; the empty tail is already a boundary.
    EXPECT_EQ(lexer.minimal_repair("ab"), std::optional<std::string>{""});
    EXPECT_EQ(lexer.minimal_repair(""), std::optional<std::string>{""});

    // The dangling b is repaired by one byte, the shortest witness of the completing crossing entry, and the repaired
    // whole must actually tokenize.
    const auto repaired{lexer.minimal_repair("b")};

    ASSERT_TRUE(repaired.has_value());
    EXPECT_EQ(*repaired, "a");

    // No repair of any length saves a byte outside every token: the refusal is a certificate.
    EXPECT_FALSE(lexer.minimal_repair("z").has_value());
}

TEST_F(Lexer_test, Window_planner_stops_at_the_documented_longest_window)
{
    enum class Token_kind : std::uint8_t
    {
        double_b,
        aab,
        baa,
        triple_a
    };

    // Over {bb, aab, baa, aaa} no byte certifies and no window of two to four bytes does, while "baaab" certifies at 4:
    // the shortest usable certificate is five bytes, one past the planner's documented reach. The planner therefore
    // leaves the plan degenerate where a longer search would cut, and the equality below pins the documented upper
    // bound, which the four-byte study case cannot.
    Builder builder{};

    builder.add_token(text("bb"), Token_kind::double_b, 1);
    builder.add_token(text("aab"), Token_kind::aab, 1);
    builder.add_token(text("baa"), Token_kind::baa, 1);
    builder.add_token(text("aaa"), Token_kind::triple_a, 1);

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    // Every window the planner is documented to try refuses; the sweep covers the whole consumable alphabet.
    for (auto length{Window_planner::shortest_window}; length <= Window_planner::longest_window; ++length)
    {
        for (std::size_t pattern{0}; pattern < (std::size_t{1} << length); ++pattern)
        {
            std::string window{};

            for (std::size_t at{0}; at < length; ++at)
            {
                window += (pattern & (std::size_t{1} << at)) != 0 ? 'b' : 'a';
            }

            EXPECT_FALSE(lexer.is_split_window(window).has_value());
        }
    }

    const auto window{lexer.is_split_window("baaab")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 4U);

    std::string input{};

    for (int block{0}; block < 8; ++block)
    {
        input += "bbaaabb";
    }

    const auto [serial, consumed]{serial_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(input.size(), consumed);

    const auto plan{lexer.chunk_boundaries_with_windows(input, 2)};

    EXPECT_EQ(plan, (std::vector<std::size_t>{0, input.size()}));
}

TEST_F(Lexer_test, Complete_c_like_grammar_windows_end_to_end)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        integer,
        string,
        comment,
        whitespace,
        operator_symbol
    };

    // The study's conventional row as a complete grammar: identifiers, integers, strings whose bodies exclude the
    // newline, line comments that end before it, whitespace runs that include it, and single-byte operators. No byte
    // certifies, and the two-byte window of a newline followed by an operator resolves the origin at the operator,
    // which must begin a token.
    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::integer, 2);
    builder.add_token(string_literal_regex(), Token_kind::string, 2);
    builder.add_token(line_comment_regex(), Token_kind::comment, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {"!", "=", "+", ";", "(", ")", "{", "}"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    const auto window{lexer.is_split_window("\n!")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    // Source with operator-initial lines, the occurrence shape the campaign corpus discloses as favorable.
    std::string input{};

    for (int block{0}; block < 32; ++block)
    {
        input += "count = count + 42; // trailing note\n";
        input += "!(flag) name = \"a (string) with // inside\";\n";
        input += "!done = 1;\n";
    }

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Complete_json_grammar_windows_end_to_end)
{
    enum class Token_kind : std::uint8_t
    {
        string,
        number,
        punct,
        whitespace
    };

    // The study's JSON row as a complete grammar: strings, integers, structural punctuation, and whitespace runs over
    // space, tab, and newline. The tab-then-quote window certifies at the quote, and pretty-printed JSON contains that
    // occurrence at every indented key, so the favorable shape here is the format's own.
    Builder builder{};

    builder.add_token(
            concat(text(R"(")"), kleene(any_of(Set::printable() - Set{'"'})), text(R"(")")), Token_kind::string, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 2);

    for (const std::string_view punct : {"{", "}", "[", "]", ":", ","})
    {
        builder.add_token(text(punct), Token_kind::punct, 1);
    }

    builder.add_token(plus(any_of(Set{' '} + '\t' + '\n')), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    const auto window{lexer.is_split_window("\t\"")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    std::string input{"{\n"};

    for (int entry{0}; entry < 64; ++entry)
    {
        input += "\t\"key\": [1, 22, 333],\n";
        input += "\t\"name\": \"value with [brackets] and 42\",\n";
    }

    input += "\t\"last\": 0\n}";

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Complete_c_like_grammar_with_block_comments_windows_end_to_end)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        integer,
        string,
        line_comment,
        block_comment,
        whitespace,
        operator_symbol
    };

    // The conventional row extended with multiline block comments. A newline may sit inside a comment, so every
    // newline-anchored window dies, and only the four-byte family around the comment closer survives: a guard byte
    // ahead of */ against reading the star as an opener's star, and a newline on one side to defeat line comments and
    // strings. The origin lands directly after the closer, the study's four-byte mechanism inside a complete grammar.
    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::integer, 2);
    builder.add_token(string_literal_regex(), Token_kind::string, 2);
    builder.add_token(line_comment_regex(), Token_kind::line_comment, 1);

    builder.add_token(block_comment_regex(), Token_kind::block_comment, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {"!", "=", "+", ";", "(", ")", "{", "}", "*", "/"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    EXPECT_TRUE(certifies_no_byte(lexer));

    // The conventional row's two-byte window is gone, a block comment can contain both of its bytes.
    EXPECT_FALSE(lexer.is_split_window("\n!").has_value());

    // The study case's own window is gone too, a line comment can contain all four bytes.
    EXPECT_FALSE(lexer.is_split_window("\t*/\t").has_value());

    // A closer at the very front is refused, the leading star could be an opener's star with the slash and the newline
    // still inside the comment.
    EXPECT_FALSE(lexer.is_split_window("*/\n!").has_value());

    const auto window{lexer.is_split_window(" */\n")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 3U);

    // Source where block comments close at line ends, the occurrence shape the surviving windows require.
    std::string input{};

    for (int block{0}; block < 24; ++block)
    {
        input += "/* block header\n   spanning two lines */\n";
        input += "!(flag) total = total + 7; // trailing note\n";
        input += "text = \"keep /* this */ inline\";\n";
        input += "value = value * 2 / 4; /* closing note */\n";
    }

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 8)};

    ASSERT_GT(boundaries.size(), 2U);

    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Mandatory_core_reports_the_family_verdicts_and_a_long_core_refuses_planning)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        string,
        comment,
        whitespace,
        operator_symbol
    };

    const auto block_comment{block_comment_regex()};

    // The block-comment row: the closer is the only exit from an input-total comment interior, so every death word
    // carries "*/" before its killing byte and the accessor reports the closer. This is the licence the filtered
    // planner runs on.
    {
        Builder builder{};

        builder.add_token(identifier_regex(), Token_kind::identifier, 2);
        builder.add_token(block_comment, Token_kind::comment, 1);
        builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

        EXPECT_EQ(builder.build().mandatory_core(), "*/");
    }

    // The JSON row: the string interior dies on the bytes outside the printable set, so no live state consumes
    // everything and nothing proposes a core. Certified windows exist for this family; the accelerator simply has no
    // licence, and planning keeps the exhaustive walk.
    {
        Builder builder{};

        builder.add_token(
                concat(text(R"(")"), kleene(any_of(Set::printable() - Set{'"'})), text(R"(")")), Token_kind::string, 2);
        builder.add_token(plus(any_of(Set::digits())), Token_kind::identifier, 2);
        builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

        for (const std::string_view op : {"{", "}", "[", "]", ":", ","})
        {
            builder.add_token(text(op), Token_kind::operator_symbol, 3);
        }

        EXPECT_EQ(builder.build().mandatory_core(), "");
    }

    // Single-byte tokens leave no input-total state at all, so the accessor is empty for the toy row too.
    {
        Builder builder{};

        builder.add_token(text("a"), Token_kind::identifier, 2);
        builder.add_token(text("b"), Token_kind::operator_symbol, 2);

        EXPECT_EQ(builder.build().mandatory_core(), "");
    }

    // The nullable row proves the same core as the first: an optional token accepts emptily, but the set is compiled as
    // its positive-width equivalent, {a, comment}, whose comment interior forces "*/" as the first row's does; the
    // derivation runs on the nullable set and licenses what it licenses on that equivalent.
    {
        Builder builder{};

        builder.add_token(optional(text("a")), Token_kind::identifier, 2);
        builder.add_token(block_comment, Token_kind::comment, 1);

        EXPECT_EQ(builder.build().mandatory_core(), "*/");
    }

    // Quadruple-quote strings prove a four-byte core, one byte past the longest window the planner tries, so no
    // candidate window can hold the core and a byte after it. The filter concludes the walk's refusal without scanning,
    // and the plan is the single serial chunk.
    {
        Builder builder{};

        const auto not_quote{any_of(Set::all() - Set{'"'})};

        const auto interior{
                choice(not_quote, concat(text(R"(")"), not_quote), concat(text(R"("")"), not_quote),
                       concat(text(R"(""")"), not_quote))};

        builder.add_token(concat(text(R"("""")"), kleene(interior), text(R"("""")")), Token_kind::string, 1);
        builder.add_token(identifier_regex(), Token_kind::identifier, 2);
        builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

        const auto lexer{builder.build()};

        EXPECT_EQ(lexer.mandatory_core(), R"("""")");

        EXPECT_TRUE(certifies_no_byte(lexer));

        std::string input{};

        for (int block{0}; block < 40; ++block)
        {
            input += "name ";
            input += "\"\"\"\"text with \"\"inner\"\" quotes\nand lines\"\"\"\" ";
            input += "tail\n";
        }

        const std::vector<std::size_t> serial{0, input.size()};

        EXPECT_EQ(lexer.chunk_boundaries_with_windows(input, 6), serial);
    }
}

TEST_F(Lexer_test, Core_filtered_planning_equals_the_exhaustive_walk)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        integer,
        comment,
        line_comment,
        whitespace,
        operator_symbol
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::integer, 2);
    builder.add_token(block_comment_regex(), Token_kind::comment, 1);
    builder.add_token(line_comment_regex(), Token_kind::line_comment, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {"!", "=", "+", ";", "(", ")", "*", "/"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    ASSERT_EQ(lexer.mandatory_core(), "*/");

    ASSERT_TRUE(certifies_no_byte(lexer));

    // Closers dense at the front and absent from the tail, so one plan covers certified cuts, in-window origins away
    // from the evidence position, and refused targets falling through to the final boundary.
    std::string input{};

    for (int block{0}; block < 18; ++block)
    {
        input += "/* note\n   spans lines */\n";
        input += "total = total + 7; x = (y); /* end */\n";
    }

    for (int block{0}; block < 18; ++block)
    {
        input += "plain = code + 1; // closer-free tail\n";
    }

    expect_planner_equals_walk(lexer, input, std::nullopt);

    // A stream with no occurrence of the core at all refuses on both sides with the same single-chunk answer, the
    // filtered side from a bare byte scan that never has to certify a window.
    const std::string bare(4096, 'x');

    const auto refused{lexer.chunk_boundaries_with_windows(bare, 5)};

    EXPECT_EQ(refused, reference_window_walk(lexer, bare, 5));

    EXPECT_EQ(refused, (std::vector<std::size_t>{0, bare.size()}));
}

TEST_F(Lexer_test, Three_byte_core_planning_equals_the_walk_when_no_window_certifies)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        string,
        whitespace
    };

    Builder builder{};

    const auto not_quote{any_of(Set::all() - Set{'"'})};

    const auto interior{choice(not_quote, concat(text(R"(")"), not_quote), concat(text(R"("")"), not_quote))};

    builder.add_token(concat(text(R"(""")"), kleene(interior), text(R"(""")")), Token_kind::string, 1);
    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    // Triple-quote strings sit exactly on the refusal boundary: the core fills all but one byte of the longest window,
    // so the filter must still plan rather than refuse, and every occurrence proposes a single candidate window instead
    // of a range. The licence is real and the certificates are not: outside a string a bare closer opens a fresh string
    // that swallows the byte after it, so no window ever agrees on an origin under this token set, and both sides must
    // certify their way to the same refusal.
    ASSERT_EQ(lexer.mandatory_core(), R"(""")");

    ASSERT_TRUE(certifies_no_byte(lexer));

    // Closers dense at the front with six-quote runs whose occurrences sit one byte apart, so the filtered side
    // generates and certifies a real candidate stream before concluding what the walk concludes.
    std::string input{};

    for (int block{0}; block < 15; ++block)
    {
        input += "name \"\"\"text with \"\"inner\"\" quotes\nand lines\"\"\" ";
        input += "\"\"\"\"\"\" tail\n";
    }

    for (int block{0}; block < 15; ++block)
    {
        input += "plain words with no quotes at all\n";
    }

    const std::vector<std::size_t> serial{0, input.size()};

    expect_planner_equals_walk(lexer, input, serial);
}

TEST_F(Lexer_test, Three_byte_core_planning_cuts_where_the_closer_dies_outside_its_token)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        string,
        whitespace
    };

    Builder builder{};

    const auto not_gt{any_of(Set::all() - Set{'>'})};

    const auto interior{choice(not_gt, concat(text(">"), not_gt), concat(text(">>"), not_gt))};

    builder.add_token(concat(text("<"), kleene(interior), text(">>>")), Token_kind::string, 1);
    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    // The same three-byte shape as the triple-quote family, with one change that turns refusals into cuts: the closer's
    // byte belongs to no other token, so every history that is not inside a string dies on the window instead of
    // surviving to disagree, and the closer plus one byte certifies. This is the only core length where the refusal
    // threshold sits one byte away, so real cuts here pin that comparison exactly.
    ASSERT_EQ(lexer.mandatory_core(), ">>>");

    ASSERT_TRUE(certifies_no_byte(lexer));

    std::string input{};

    for (int block{0}; block < 15; ++block)
    {
        input += "name <text with >>inner>> marks\nand lines>>> ";
        input += "<dense>>>>\n";
    }

    for (int block{0}; block < 15; ++block)
    {
        input += "plain words with no closer at all\n";
    }

    expect_planner_equals_walk(lexer, input, std::nullopt);
}

TEST_F(Lexer_test, Single_byte_core_planning_equals_the_walk_over_the_full_length_range)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        integer,
        region,
        whitespace,
        operator_symbol
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::integer, 2);
    builder.add_token(concat(text("<@"), kleene(any_of(Set::all() - Set{';'})), text(";")), Token_kind::region, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {"=", "+", ";", "(", ")", "<", "@"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    // A single-byte core is the widest case of the candidate algebra: every occurrence proposes windows of all three
    // lengths, and a repeated closer puts occurrences one byte apart, so neighbouring occurrences propose the same
    // window and the candidate stream carries duplicates through the heap's ordering.
    ASSERT_EQ(lexer.mandatory_core(), ";");

    ASSERT_TRUE(certifies_no_byte(lexer));

    std::string input{};

    for (int block{0}; block < 15; ++block)
    {
        input += "<@ region body\nspans lines; total = total + 7;;\n";
        input += "<@ dense;;; (tight) runs;\n";
    }

    for (int block{0}; block < 15; ++block)
    {
        input += "plain = code + 1 (no closer at all)\n";
    }

    expect_planner_equals_walk(lexer, input, std::nullopt);
}

TEST_F(Lexer_test, Window_length_order_decides_the_cut_when_the_shortest_refuses)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        region,
        whitespace,
        operator_symbol
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(
            concat(text("<@"), kleene(any_of(Set::all() - Set{';'})), text(";"), kleene(any_of(Set{'x'}))),
            Token_kind::region, 1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {";", "<", "@"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    // The region swallows 'x' bytes past its closer, so the two-byte window at an occurrence refuses while the three-
    // and four-byte windows certify with different origins. The plan is then decided by which length is tried first at
    // the same position, which is exactly the ordering the heap must reproduce, every length tried and the shortest
    // first; the test pins the real cuts that ordering gives.
    ASSERT_EQ(lexer.mandatory_core(), ";");

    ASSERT_TRUE(certifies_no_byte(lexer));

    std::string input{};

    for (int block{0}; block < 15; ++block)
    {
        input += "<@ first region;x alpha beta\n";
        input += "<@ second;xxx gamma <@ third; delta\n";
    }

    for (int block{0}; block < 15; ++block)
    {
        input += "plain words with no closer at all\n";
    }

    expect_planner_equals_walk(lexer, input, std::nullopt);

    // One target landing exactly on an occurrence, so no earlier window can answer first: the two-byte window refuses,
    // the three-byte one cuts at eight, and only a walk trying lengths in ascending order finds it, since the four-byte
    // window would cut at nine. The plan is pinned, not just compared.
    const std::string micro{"<@qqqq;x a b"};

    const auto plan{lexer.chunk_boundaries_with_windows(micro, 2)};

    EXPECT_EQ(plan, reference_window_walk(lexer, micro, 2));

    EXPECT_EQ(plan, (std::vector<std::size_t>{0, 8, micro.size()}));

    // A target whose end-touching windows still fit before the input's end: the barren shortcut may refuse only floors
    // past the last occurrence, so this cut is kept.
    const std::string tight{" <@; "};

    const auto tight_plan{lexer.chunk_boundaries_with_windows(tight, 2)};

    EXPECT_EQ(tight_plan, reference_window_walk(lexer, tight, 2));

    EXPECT_EQ(tight_plan, (std::vector<std::size_t>{0, 4, tight.size()}));

    // An occurrence one byte in, tried at the longest window length: the candidate range's lower end saturates at zero
    // here, which keeps the only certifying window and its cut.
    const std::string wrap{"q;xx "};

    const auto wrap_plan{lexer.chunk_boundaries_with_windows(wrap, 5)};

    EXPECT_EQ(wrap_plan, reference_window_walk(lexer, wrap, 5));

    EXPECT_EQ(wrap_plan, (std::vector<std::size_t>{0, 4, wrap.size()}));

    // A later target landing exactly on the last recorded occurrence: the barren cache holds the offset one past it, so
    // the occurrence itself stays a candidate and the cut it certifies is kept.
    const std::string edge{"qx;b ;a;@ x  q@"};

    const auto edge_plan{lexer.chunk_boundaries_with_windows(edge, 6)};

    EXPECT_EQ(edge_plan, reference_window_walk(lexer, edge, 6));

    EXPECT_EQ(edge_plan, (std::vector<std::size_t>{0, 3, 6, 8, edge.size()}));
}

TEST_F(Lexer_test, Window_walk_advances_past_the_previous_cut)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    Builder builder{};

    builder.add_token(choice(choice(text("a"), text("aa")), choice(text("ab"), text("bc"))), Token_kind::word, 1);

    const auto lexer{builder.build()};

    ASSERT_EQ(lexer.mandatory_core(), "");

    ASSERT_TRUE(certifies_no_byte(lexer));

    // Two certifying windows sit one byte apart, and several targets funnel to the same region: each target's walk must
    // begin strictly past the previous cut, so every target finds a cut of its own. The plan is pinned cut for cut.
    std::string input(30, 'a');

    input += "bc";

    input += std::string(68, 'a');

    const auto plan{lexer.chunk_boundaries_with_windows(input, 10)};

    EXPECT_EQ(plan, reference_window_walk(lexer, input, 10));

    EXPECT_EQ(plan, (std::vector<std::size_t>{0, 30, 32, input.size()}));
}

TEST_F(Lexer_test, Window_walk_defers_to_a_certified_byte_even_when_the_input_lacks_it)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    Builder builder{};

    builder.add_token(
            choice(choice(choice(text("a"), text("aa")), choice(text("ab"), text("bc"))), text("!")), Token_kind::word,
            1);

    const auto lexer{builder.build()};

    // The exclamation mark certifies as a byte, so the token set does not lack a usable byte, and the window recovery
    // is documented for sets that do. This input carries certifiable windows around "bc" yet no exclamation mark, so
    // the plan stays the byte plan's single whole-input chunk rather than cutting at 30 and 32.
    ASSERT_TRUE(lexer.is_split_point('!'));

    std::string input(30, 'a');

    input += "bc";

    input += std::string(68, 'a');

    const auto byte_plan{lexer.chunk_boundaries(input, 10)};

    const auto window_plan{lexer.chunk_boundaries_with_windows(input, 10)};

    EXPECT_EQ(byte_plan, (std::vector<std::size_t>{0, input.size()}));

    EXPECT_EQ(window_plan, (std::vector<std::size_t>{0, input.size()}));
}

TEST_F(Lexer_test, Self_overlapping_core_planning_equals_the_walk_through_duplicate_candidates)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        integer,
        comment,
        whitespace,
        operator_symbol
    };

    Builder builder{};

    const auto not_star{any_of(Set::all() - Set{'*'})};

    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::integer, 2);
    builder.add_token(
            concat(text("(*"), kleene(choice(not_star, concat(text("*"), not_star))), text("**")), Token_kind::comment,
            1);
    builder.add_token(plus(any_of(Set::whitespace() + '\n')), Token_kind::whitespace, 1);

    for (const std::string_view op : {"=", "+", ";", "(", ")", "*"})
    {
        builder.add_token(text(op), Token_kind::operator_symbol, 3);
    }

    const auto lexer{builder.build()};

    // A doubled-byte closer overlaps itself, so a star run holds occurrences one byte apart and neighbouring
    // occurrences propose the same length-four window: the candidate stream carries duplicates the committed
    // block-comment corpus can never produce. Duplicates share their memoized verdict, so no final plan can see whether
    // the skip fired; what this run pins is that overlap-heavy proposal generation still equals the walk, and that the
    // skip drops nothing it should keep.
    ASSERT_EQ(lexer.mandatory_core(), "**");

    ASSERT_TRUE(certifies_no_byte(lexer));

    std::string input{};

    for (int block{0}; block < 15; ++block)
    {
        input += "(* note **) total = total + 7; (* spans\nlines **)\n";
        input += "runs (*a***** and (*b****** follow;\n";
    }

    for (int block{0}; block < 15; ++block)
    {
        input += "plain = code + 1; single * stars only\n";
    }

    expect_planner_equals_walk(lexer, input, std::nullopt);
}

TEST_F(Lexer_test, Long_runs_tokenize_identically_to_the_per_token_scan)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        number
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);

    const auto lexer{builder.build()};

    const std::string input{
            std::string(30, ' ') + "an_identifier_well_past_the_probe_distance " + std::string(25, '7') + " x 9 " +
            std::string(18, 'y')};

    const auto [batch, consumed]{serial_stream<Token_kind>(lexer, input)};

    EXPECT_EQ(consumed, input.size());

    const auto [single, single_consumed]{per_token_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(single_consumed, input.size());

    EXPECT_EQ(batch, single);
}

TEST_F(Lexer_test, Chunk_boundaries_land_on_certified_split_points)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        semicolon
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    std::string input{};

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_GT(boundaries[index], boundaries[index - 1]);
        EXPECT_TRUE(lexer.is_split_point(input[boundaries[index]]));
    }
}

TEST_F(Lexer_test, Chunk_boundaries_recover_windows_when_no_byte_certifies)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    // Identifier characters continue identifiers and whitespace continues runs, so no byte certifies and the byte plan
    // is the single serial chunk. The two-byte window of a whitespace byte followed by a letter certifies at origin 1,
    // whitespace cannot continue an identifier and a letter cannot continue a run, so the fallback cuts at token starts
    // the byte certificate cannot see.
    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    // Discarding identifiers gives the relaxed byte certificate plenty of bytes while the exact set stays empty, so the
    // test pins that the fallback consults the exact query and the window recovery below still happens.
    builder.set_ignored_tokens({Token_kind::identifier});

    const auto lexer{builder.build()};

    const auto window{lexer.is_split_window(" a")};

    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(*window, 1U);

    const std::string input{"alpha beta gamma delta epsilon zeta eta theta"};

    ASSERT_EQ(lexer.chunk_boundaries(input, 4), (std::vector<std::size_t>{0, input.size()}));

    const auto boundaries{lexer.chunk_boundaries_with_windows(input, 4)};

    ASSERT_GT(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());

    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Dead_branches_do_not_decertify)
{
    enum class Token_kind : std::uint8_t
    {
        never,
        letter
    };

    // any_of over an empty set denotes the empty language, so "ab" followed by it matches nothing. The states leading
    // there stay reachable, and one of them consumes 'b'. That transition can never lie on an emitted token, so it must
    // not de-certify 'b': every 'b' in an input this lexer accepts is a whole token.
    Builder builder{};

    builder.add_token(concat(text("ab"), any_of(Set{})), Token_kind::never, 1);
    builder.add_token(text("b"), Token_kind::letter, 1);

    const auto lexer{builder.build()};

    EXPECT_TRUE(lexer.is_split_point('b'));

    // 'a' is consumed only into the dead branch, so no valid input contains it and it is not a usable split point.
    EXPECT_FALSE(lexer.is_split_point('a'));

    const std::string input{"bbbbbbbb"};

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_EQ(input[boundaries[index]], 'b');
    }
}

TEST_F(Lexer_test, Planning_ignores_vacuously_certified_symbols)
{
    enum class Token_kind : std::uint8_t
    {
        letters
    };

    // Only 'a' appears in any token, so every other byte certifies vacuously: no input this lexer accepts can contain
    // one. The plan must recognize that the useful certificate is empty and return the whole input as one chunk, rather
    // than scanning for bytes that cannot occur.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::letters, 1);

    const auto lexer{builder.build()};

    // 'a' continues a token, and every other byte certifies only vacuously, so nothing is reported.
    EXPECT_FALSE(lexer.is_split_point('a'));
    EXPECT_FALSE(lexer.is_split_point('z'));

    const std::string input(4096, 'a');

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.front(), 0U);
    EXPECT_EQ(boundaries.back(), input.size());
}

TEST_F(Lexer_test, Useful_certificates_are_those_the_initial_state_consumes)
{
    enum class Token_kind : std::uint8_t
    {
        letters,
        semicolon
    };

    // ';' is certified and the initial state consumes it, so it is useful and planning may search for it.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::letters, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    // ';' is certified and the initial state consumes it; 'z' certifies only vacuously and is not reported.
    EXPECT_TRUE(lexer.is_split_point(';'));
    EXPECT_FALSE(lexer.is_split_point('z'));

    std::string input{};

    for (std::size_t index{0}; index < 512; ++index)
    {
        input += "aaa;";
    }

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(boundaries.size(), 5U);

    for (std::size_t index{1}; index + 1 < boundaries.size(); ++index)
    {
        EXPECT_EQ(input[boundaries[index]], ';');
    }
}

TEST_F(Lexer_test, Parallel_tokenization_matches_the_serial_stream)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        semicolon
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    std::string input{};

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    const auto [serial, serial_consumed]{serial_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(serial_consumed, input.size());

    constexpr std::size_t chunks{4};

    std::array<std::vector<std::pair<Token_kind, std::size_t>>, chunks> streams{};

    const auto collect{[&streams](const std::size_t chunk, const Token_kind token, const std::size_t length) {
        streams[chunk].emplace_back(token, length);
    }};

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(input, chunks, collect)};

    const auto boundaries{lexer.chunk_boundaries(input, chunks)};

    ASSERT_EQ(consumed.size(), boundaries.size() - 1);

    std::vector<std::pair<Token_kind, std::size_t>> spliced{};

    for (std::size_t chunk{0}; chunk < consumed.size(); ++chunk)
    {
        EXPECT_EQ(consumed[chunk], boundaries[chunk + 1] - boundaries[chunk]);

        spliced.insert(spliced.end(), streams[chunk].cbegin(), streams[chunk].cend());
    }

    EXPECT_EQ(spliced, serial);
}

TEST_F(Lexer_test, Parallel_tokenization_accepts_a_sink_callable_only_as_an_lvalue)
{
    enum class Token_kind : std::uint8_t
    {
        word,
        space
    };

    // The workers hold the sink by reference and call it as an lvalue, so the constraint admits this sink, which no
    // rvalue call accepts; the counter is atomic because one sink serves every chunk's thread.
    struct Lvalue_only_sink
    {
        void operator()(std::size_t, Token_kind, std::size_t) & { ++seen; }

        std::atomic<std::size_t>& seen;
    };

    Builder builder{};

    builder.add_token(plus(any_of(Set::alpha())), Token_kind::word, 1);
    builder.add_token(any_of(Set{' '}), Token_kind::space, 1);

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_point(' '));

    const std::string input{"ab cd ef gh"};

    std::atomic<std::size_t> seen{0};

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(input, 2, Lvalue_only_sink{.seen = seen})};

    const auto total{std::ranges::fold_left(consumed, std::size_t{0}, std::plus{})};

    EXPECT_EQ(total, input.size());

    // Four words and the three spaces between them.
    EXPECT_EQ(seen, 7U);
}

TEST_F(Lexer_test, Parallel_tokenization_reports_a_rejected_chunk)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace,
        semicolon
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    std::string input{};

    for (int index{0}; index < 64; ++index)
    {
        input += "alpha beta; gamma delta; ";
    }

    // A byte no token accepts, planted in the last quarter: only that chunk stops short, at exactly its offset.
    const auto poison{(input.size() * 7) / 8};

    input[poison] = '@';

    const auto consumed{lexer.tokenize_all_parallel<Token_kind>(input, 4, ignore_chunk_token)};

    const auto boundaries{lexer.chunk_boundaries(input, 4)};

    ASSERT_EQ(consumed.size(), boundaries.size() - 1);

    for (std::size_t chunk{0}; chunk + 1 < consumed.size(); ++chunk)
    {
        EXPECT_EQ(consumed[chunk], boundaries[chunk + 1] - boundaries[chunk]);
    }

    EXPECT_EQ(boundaries[consumed.size() - 1] + consumed.back(), poison);
}

TEST_F(Lexer_test, Diagnose_reports_shadowed_keywords_as_dead)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword_if,
        whitespace
    };

    Builder builder{};

    // The identifier pattern outranks the keyword, so "if" always tokenizes as an identifier and the keyword can never
    // win any input.
    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(text("if"), Token_kind::keyword_if, 2);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_EQ(dead_tokens, (std::vector<std::size_t>{std::to_underlying(Token_kind::keyword_if)}));
    EXPECT_TRUE(ties.empty());
}

TEST_F(Lexer_test, Diagnose_passes_a_healthy_grammar)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword_if,
        number,
        whitespace
    };

    Builder builder{};

    builder.add_token(text("if"), Token_kind::keyword_if, 1);
    builder.add_token(identifier_regex(), Token_kind::identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_TRUE(dead_tokens.empty());
    EXPECT_TRUE(ties.empty());
}

TEST_F(Lexer_test, Diagnose_reports_equal_priority_ties)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword_if
    };

    Builder builder{};

    // Both accept the spelling "if" at the same priority, so the build breaks the tie by registered value: the
    // identifier wins, which also leaves the keyword dead.
    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(text("if"), Token_kind::keyword_if, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_EQ(
            ties, (std::vector<std::pair<std::size_t, std::size_t>>{
                          {std::to_underlying(Token_kind::identifier), std::to_underlying(Token_kind::keyword_if)}}));
    EXPECT_EQ(dead_tokens, (std::vector<std::size_t>{std::to_underlying(Token_kind::keyword_if)}));
}

TEST_F(Lexer_test, Diagnose_reports_a_tie_that_leaves_both_tokens_alive)
{
    enum class Token_kind : std::uint8_t
    {
        keyword_if,
        identifier
    };

    Builder builder{};

    // Reversed registered values: the keyword wins its own spelling by the tie break, every other identifier spelling
    // still belongs to the identifier, so both stay alive but the tie is still an accident to report.
    builder.add_token(text("if"), Token_kind::keyword_if, 1);
    builder.add_token(identifier_regex(), Token_kind::identifier, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_EQ(
            ties, (std::vector<std::pair<std::size_t, std::size_t>>{
                          {std::to_underlying(Token_kind::keyword_if), std::to_underlying(Token_kind::identifier)}}));
    EXPECT_TRUE(dead_tokens.empty());
}

TEST_F(Lexer_test, Diagnose_ignores_equal_priorities_over_disjoint_languages)
{
    enum class Token_kind : std::uint8_t
    {
        number,
        semicolon
    };

    Builder builder{};

    // Equal priorities are only a problem when some input could go either way; disjoint token languages never meet in
    // an accepting state set.
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_TRUE(dead_tokens.empty());
    EXPECT_TRUE(ties.empty());
}

TEST_F(Lexer_test, Diagnose_accepts_an_empty_builder)
{
    const Builder builder{};

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_TRUE(dead_tokens.empty());
    EXPECT_TRUE(ties.empty());
}

TEST_F(Lexer_test, Diagnose_treats_liveness_per_token_not_per_pattern)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        keyword
    };

    Builder builder{};

    // The keyword is registered twice: the "if" pattern is fully shadowed by the identifier, the ";" pattern is not.
    // Liveness belongs to the token value, so one winning pattern keeps the token alive.
    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(text("if"), Token_kind::keyword, 2);
    builder.add_token(text(";"), Token_kind::keyword, 2);

    const auto [dead_tokens, ties]{builder.diagnose()};

    EXPECT_TRUE(dead_tokens.empty());
    EXPECT_TRUE(ties.empty());
}

TEST_F(Lexer_test, Diagnose_reports_every_pair_of_a_three_way_tie)
{
    enum class Token_kind : std::uint8_t
    {
        first,
        second,
        third
    };

    Builder builder{};

    // All three accept exactly ";" at the same priority, so every pair of the three collides.
    builder.add_token(text(";"), Token_kind::first, 1);
    builder.add_token(text(";"), Token_kind::second, 1);
    builder.add_token(text(";"), Token_kind::third, 1);

    const auto [dead_tokens, ties]{builder.diagnose()};

    const std::vector<std::pair<std::size_t, std::size_t>> expected{
            {std::to_underlying(Token_kind::first), std::to_underlying(Token_kind::second)},
            {std::to_underlying(Token_kind::first), std::to_underlying(Token_kind::third)},
            {std::to_underlying(Token_kind::second), std::to_underlying(Token_kind::third)}};

    EXPECT_EQ(ties, expected);
    EXPECT_EQ(
            dead_tokens,
            (std::vector<std::size_t>{std::to_underlying(Token_kind::second), std::to_underlying(Token_kind::third)}));
}

TEST_F(Lexer_test, Split_points_refuse_a_reentrant_start_state)
{
    enum class Token_kind : std::uint8_t
    {
        run
    };

    // kleene minimizes to an accepting start state with a self-loop, so the start state is reachable again after
    // consuming input: 'a' can continue a token mid-scan even though only the start state consumes it, and splitting
    // "aa" would turn one length-two token into two length-one tokens.
    Builder builder{};

    builder.add_token(kleene(text("a")), Token_kind::run, 1);

    const auto lexer{builder.build()};

    EXPECT_FALSE(lexer.is_split_point('a'));

    const auto boundaries{lexer.chunk_boundaries(std::string{"aa"}, 2)};

    ASSERT_EQ(boundaries.size(), 2U);
    EXPECT_EQ(boundaries.back(), 2U);
}

TEST_F(Lexer_test, Split_points_refuse_reentry_through_a_cycle)
{
    enum class Token_kind : std::uint8_t
    {
        item
    };

    // (ab)*c returns to the start state after every "ab": minimization merges the post-"ab" state with the start state,
    // so the re-entry is a cycle rather than a self-loop, and 'c' must not certify even though only the start state
    // consumes it. Splitting "abc" before the 'c' would orphan an unmatchable "ab" chunk.
    Builder builder{};

    builder.add_token(concat(kleene(text("ab")), text("c")), Token_kind::item, 1);

    const auto lexer{builder.build()};

    EXPECT_FALSE(lexer.is_split_point('c'));
    EXPECT_FALSE(lexer.is_split_point('a'));
}

TEST_F(Lexer_test, State_limit_stops_an_exploding_construction)
{
    enum class Token_kind : std::uint8_t
    {
        needle
    };

    // The classic exponential case: (a|b)* a (a|b)^12 needs a state per remembered 12-symbol suffix, around 2^12 of
    // them, which is exactly the pathology a caller accepting untrusted token sets must be able to cap.
    Builder builder{};

    builder.add_token(
            concat(kleene(any_of(Set{'a', 'b'})), text("a"), exact(any_of(Set{'a', 'b'}), 12)), Token_kind::needle, 1);

    builder.set_state_limit(256);

    EXPECT_THROW(std::ignore = builder.build(), State_limit_error);
    EXPECT_THROW(std::ignore = builder.diagnose(), State_limit_error);

    // The error carries the limit and is catchable as std::runtime_error too.
    try
    {
        std::ignore = builder.build();

        FAIL() << "build() must throw";
    }
    catch (const std::runtime_error& error)
    {
        const auto& limit_error{dynamic_cast<const State_limit_error&>(error)};

        EXPECT_EQ(limit_error.limit(), 256U);
    }

    // The same grammar builds once the cap allows its true size.
    builder.set_state_limit(0);

    const auto lexer{builder.build()};

    const auto input{std::string{"a"} + std::string(12, 'b')};

    const auto [token, length]{lexer.tokenize<Token_kind>(input)};

    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(length, 13U);
}

TEST_F(Lexer_test, The_state_limit_counts_what_determinization_discovers_and_a_nullable_set_compiles_one_more)
{
    enum class Token_kind : std::uint8_t
    {
        only
    };

    // The cap counts the states subset construction discovers. A nullable set is then compiled as its positive-width
    // equivalent, one state larger, so the table the cap bounds is one column wider than the cap: optional(a) under a
    // cap of two discovers two states and compiles three. A set of positive width compiles what it discovered and no
    // more, which is the table the promise in set_state_limit() bounds.
    Builder nullable{};

    nullable.add_token(optional(text("a")), Token_kind::only, 1);

    nullable.set_state_limit(2);

    const auto nullable_lexer{nullable.build()};

    EXPECT_TRUE(nullable_lexer.simulator().nullable());
    EXPECT_EQ(nullable_lexer.simulator().state_count(), 3U);

    Builder positive{};

    positive.add_token(text("a"), Token_kind::only, 1);

    positive.set_state_limit(2);

    const auto positive_lexer{positive.build()};

    EXPECT_FALSE(positive_lexer.simulator().nullable());
    EXPECT_EQ(positive_lexer.simulator().state_count(), 2U);
}

TEST_F(Lexer_test, Oversized_nfa_state_identifier_throws_before_the_count_wraps)
{
    // The determinizer mirrors the DFA simulator's guard: a hand-built NFA may number states sparsely, and one whose
    // initial state's identifier is the largest std::size_t, beyond the 32-bit dense index, is rejected before the
    // walk.
    const nfa::Nfa nfa{std::numeric_limits<std::size_t>::max(), {}, {}};

    EXPECT_THROW(std::ignore = determinize(nfa), std::runtime_error);
}

TEST_F(Lexer_test, State_limit_leaves_reasonable_grammars_untouched)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        number
    };

    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);

    builder.set_state_limit(64);

    const auto lexer{builder.build()};

    const auto [token, length]{lexer.tokenize<Token_kind>(std::string{"counter42"})};

    EXPECT_EQ(token, Token_kind::identifier);
    EXPECT_EQ(length, 9U);
}

TEST_F(Lexer_test, Unicode_identifiers_tokenize_through_xid_properties)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        number,
        space
    };

    Builder builder{};

    // The C-style profile on top of the exact properties: underscore may lead, which XID_Start alone excludes.
    builder.add_token(
            concat(choice(text('_'), unicode::xid_start()), kleene(unicode::xid_continue())), Token_kind::identifier,
            1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);
    builder.add_token(plus(any_of(Set{' '})), Token_kind::space, 1);

    const auto lexer{builder.build()};

    // Greek, Han with a combining-free ASCII tail, an underscore head, and a number: "αβ 漢字_2 _x9 42".
    const std::string input{"\xCE\xB1\xCE\xB2 \xE6\xBC\xA2\xE5\xAD\x97_2 _x9 42"};

    const auto [stream, consumed]{serial_stream<Token_kind>(lexer, input)};

    EXPECT_EQ(consumed, input.size());

    const std::vector<std::pair<Token_kind, std::size_t>> expected{
            {Token_kind::identifier, 4}, {Token_kind::space, 1}, {Token_kind::identifier, 8}, {Token_kind::space, 1},
            {Token_kind::identifier, 3}, {Token_kind::space, 1}, {Token_kind::number, 2},
    };

    EXPECT_EQ(stream, expected);
}

TEST_F(Lexer_test, Xid_identifiers_compete_with_keywords_and_reject_ill_formed_input)
{
    enum class Token_kind : std::uint8_t
    {
        keyword,
        identifier
    };

    Builder builder{};

    builder.add_token(text("if"), Token_kind::keyword, 1);
    builder.add_token(concat(unicode::xid_start(), kleene(unicode::xid_continue())), Token_kind::identifier, 2);

    const auto lexer{builder.build()};

    // The keyword's own spelling wins on priority; one more code point and the identifier takes over.
    const auto [keyword, keyword_length]{lexer.tokenize<Token_kind>(std::string{"if"})};

    const auto [identifier, identifier_length]{lexer.tokenize<Token_kind>(std::string{"if\xCE\xBB"})};

    EXPECT_EQ(keyword, Token_kind::keyword);
    EXPECT_EQ(identifier, Token_kind::identifier);

    // Ill-formed UTF-8 never matches an XID class: a stray continuation byte, an overlong encoding, and a truncated
    // sequence all fail at offset zero rather than tokenize as identifiers.
    for (const std::string input : {"\x80", "\xC0\xAF", "\xE6\xBC"})
    {
        const auto [token, length]{lexer.tokenize<Token_kind>(input)};

        EXPECT_FALSE(token.has_value()) << input;
        EXPECT_EQ(length, 0U) << input;
    }
}

TEST_F(Lexer_test, Xid_membership_matches_the_generated_tables_at_every_boundary)
{
    // The checked-in tables are the oracle: every interval edge and its outside neighbors go through the full pipeline,
    // covering exactly the points where membership can flip in unioning, UTF-8 expansion, lowering, determinization, or
    // minimization.
    enum class Token_kind : std::uint8_t
    {
        point
    };

    // One interval of a checked-in table, both ends included.
    struct Range
    {
        char32_t first{};

        char32_t last{};
    };

    const auto load{[](const std::string_view name) {
        std::ifstream file{std::string{SOURCE_DIR} + "/libs/regex/src/xid_ranges.inc"};

        std::vector<Range> ranges{};

        std::string line{};

        bool inside{};

        while (std::getline(file, line))
        {
            if (!inside)
            {
                inside = line.contains(name);

                continue;
            }

            unsigned long first{};

            unsigned long last{};

            const auto parsed{std::sscanf(line.c_str(), " {.first = 0x%lX, .last = 0x%lX}", &first, &last)};

            if (parsed != 2)
            {
                break;
            }

            ranges.push_back({.first = static_cast<char32_t>(first), .last = static_cast<char32_t>(last)});
        }

        return ranges;
    }};

    const auto member{[](const std::vector<Range>& ranges, const char32_t point) {
        const auto holds_point{[point](const Range& range) {
            const auto [first, last]{range};

            return point >= first && point <= last;
        }};

        return std::ranges::any_of(ranges, holds_point);
    }};

    // A checked-in table and the pattern the library compiles for its property.
    struct Property
    {
        std::string_view name{};

        Regex regex{};
    };

    const std::array properties{
            Property{.name = "xid_start_ranges", .regex = unicode::xid_start()},
            Property{.name = "xid_continue_ranges", .regex = unicode::xid_continue()}};

    for (const auto& [name, pattern] : properties)
    {
        const auto ranges{load(name)};

        ASSERT_GT(ranges.size(), 500U) << name;

        Builder builder{};

        builder.add_token(pattern, Token_kind::point, 1);

        const auto lexer{builder.build()};

        const auto check{[&](const char32_t probe) {
            if (probe > max_code_point || (probe >= first_surrogate && probe <= last_surrogate))
            {
                return;
            }

            const auto bytes{regex::utf8::encode(probe)};

            const auto [token, length]{lexer.tokenize<Token_kind>(bytes)};

            const auto matched{token.has_value() && length == bytes.size()};

            EXPECT_EQ(matched, member(ranges, probe)) << name << " U+" << std::hex << static_cast<unsigned>(probe);
        }};

        for (const auto& [first, last] : ranges)
        {
            for (const auto probe : {first, last, static_cast<char32_t>(first - 1), static_cast<char32_t>(last + 1)})
            {
                check(probe);
            }
        }
    }
}

TEST_F(Lexer_test, Chunk_boundaries_finds_adjacent_certified_bytes)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        semicolon
    };

    // Two ideal offsets can walk forward onto the same certified byte; the next search resumes past it and reaches the
    // certified byte after it, so the plan keeps every chunk the input can support.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaaaaaa;;a"};

    ASSERT_TRUE(lexer.is_split_point(';'));

    const auto four_chunks{lexer.chunk_boundaries(input, 4)};

    const auto two_chunks{lexer.chunk_boundaries(input, 2)};

    EXPECT_EQ(four_chunks, (std::vector<std::size_t>{0, 7, 8, 10}));

    EXPECT_EQ(two_chunks, (std::vector<std::size_t>{0, 7, 10}));
}

TEST_F(Lexer_test, Chunk_boundaries_caps_a_request_larger_than_the_input)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        semicolon
    };

    // A chunk needs a byte, so a request far larger than the input must neither iterate once per requested chunk nor
    // overflow the ideal-offset arithmetic. The answer is capped by what the input can actually support.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaaaaaa;;a"};

    const auto capped{lexer.chunk_boundaries(input, std::numeric_limits<std::size_t>::max())};

    EXPECT_EQ(capped, (std::vector<std::size_t>{0, 7, 8, 10}));

    const auto empty{lexer.chunk_boundaries(std::string{}, std::numeric_limits<std::size_t>::max())};

    EXPECT_EQ(empty, (std::vector<std::size_t>{0, 0}));
}

TEST_F(Lexer_test, Chunk_boundaries_divide_the_input_exactly)
{
    enum class Token_kind : std::uint8_t
    {
        any
    };

    // The ideal offsets must be exactly size * index / chunks. Computing them that way overflows for a large input cut
    // very finely, so the planner accumulates instead; this pins the accumulation against the closed form. Every byte
    // certifies here, so no boundary walks forward and the offsets are the division itself. The sizes and chunk counts
    // are deliberately coprime, which is what makes the carry fire on most iterations.
    Builder builder{};

    builder.add_token(any_of(Set::all()), Token_kind::any, 1);

    const auto lexer{builder.build()};

    for (const auto size : {std::size_t{100}, std::size_t{997}, std::size_t{1024}})
    {
        const std::string input(size, 'x');

        ASSERT_TRUE(lexer.is_split_point('x'));

        for (const auto chunks : {std::size_t{3}, std::size_t{7}, std::size_t{64}})
        {
            std::vector<std::size_t> expected{0};

            for (std::size_t index{1}; index < chunks; ++index)
            {
                expected.push_back(size * index / chunks);
            }

            expected.push_back(size);

            EXPECT_EQ(lexer.chunk_boundaries(input, chunks), expected) << "size " << size << ", chunks " << chunks;
        }
    }
}

TEST_F(Lexer_test, Parallel_tokenization_propagates_a_throwing_sink)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        semicolon
    };

    // An exception escaping a jthread's callable calls std::terminate. A throwing sink must reach the caller instead,
    // and must do so identically whether it throws on a worker chunk or on the one the caller scans itself.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 1);

    const auto lexer{builder.build()};

    const std::string input{"aaa;aaa;aaa;aaa"};

    ASSERT_GT(lexer.chunk_boundaries(input, 4).size(), 2U);

    const auto scan{[&lexer, &input](const std::size_t throwing_chunk) {
        const auto throw_on_chunk{[throwing_chunk](const std::size_t chunk, Token_kind, std::size_t) {
            if (chunk == throwing_chunk)
            {
                throw std::runtime_error{"sink"};
            }
        }};

        return lexer.tokenize_all_parallel<Token_kind>(input, 4, throw_on_chunk);
    }};

    EXPECT_THROW(scan(0), std::runtime_error);

    EXPECT_THROW(scan(lexer.chunk_boundaries(input, 4).size() - 2), std::runtime_error);
}

TEST_F(Lexer_test, Window_planning_reads_elements_never_iterator_objects)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    // Twelve spaces then eight letters puts the equal-division target at offset ten, inside the whitespace run two
    // bytes before the identifier. Planning over an iterator whose elements convert to char reads each window's bytes
    // from adjacent elements of the input, so the plan equals the plain char plan and rejoins to the serial stream.
    Builder builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{std::string(12, ' ') + "alphabet"};

    const auto boundaries{lexer.chunk_boundaries_with_windows(
            Char_convertible_iterator{.position = input.cbegin()}, Char_convertible_iterator{.position = input.cend()},
            2)};

    EXPECT_EQ(boundaries, lexer.chunk_boundaries_with_windows(input, 2));

    expect_rejoined_equals_serial<Token_kind>(lexer, input, boundaries);
}

TEST_F(Lexer_test, Every_entry_point_scans_std_byte_input_like_char_input)
{
    enum class Token_kind : std::uint8_t
    {
        identifier,
        whitespace
    };

    // The concept probes below ask only overload viability, which never instantiates a body; an element type the
    // interface admits could still be rejected by a body's implicit conversions. std::byte converts to nothing
    // implicitly, so running it through every entry point pins each body to the promised domain, and the answers must
    // match the same input spelled as char. The grammar certifies no byte but certifies a window, so the window walk
    // itself executes rather than short-circuiting.
    Builder_dbg builder{};

    builder.add_token(identifier_regex(), Token_kind::identifier, 1);
    builder.add_token(plus(any_of(Set::whitespace())), Token_kind::whitespace, 1);

    const auto lexer{builder.build()};

    const std::string input{"alpha beta gamma delta epsilon zeta eta theta"};

    std::vector<std::byte> bytes{};

    const auto as_byte{[](const char symbol) { return static_cast<std::byte>(symbol); }};

    std::ranges::transform(input, std::back_inserter(bytes), as_byte);

    const auto byte_match{lexer.tokenize<Token_kind>(bytes)};

    const auto text_match{lexer.tokenize<Token_kind>(input)};

    EXPECT_EQ(byte_match, text_match);

    std::vector<std::pair<Token_kind, std::size_t>> from_bytes{};

    const auto collect{
            [&from_bytes](const Token_kind kind, const std::size_t length) { from_bytes.emplace_back(kind, length); }};

    const auto consumed{lexer.tokenize_all<Token_kind>(bytes, collect)};

    const auto [from_text, text_consumed]{serial_stream<Token_kind>(lexer, input)};

    ASSERT_EQ(consumed, text_consumed);

    EXPECT_EQ(from_bytes, from_text);

    const auto bytes_plan{lexer.chunk_boundaries(bytes, 4)};

    const auto text_plan{lexer.chunk_boundaries(input, 4)};

    EXPECT_EQ(bytes_plan, text_plan);

    const auto boundaries{lexer.chunk_boundaries_with_windows(bytes, 4)};

    const auto text_window_plan{lexer.chunk_boundaries_with_windows(input, 4)};

    ASSERT_GT(boundaries.size(), 2U);
    EXPECT_EQ(boundaries, text_window_plan);

    std::vector<std::size_t> parallel_from_bytes(boundaries.size() - 1, 0);

    const auto count_in_chunk{[&parallel_from_bytes](const std::size_t chunk, Token_kind, const std::size_t) {
        ++parallel_from_bytes[chunk];
    }};

    const auto counted{lexer.tokenize_all_parallel<Token_kind>(bytes, 4, count_in_chunk)};

    const auto counted_from_text{lexer.tokenize_all_parallel<Token_kind>(input, 4, ignore_chunk_token)};

    EXPECT_EQ(counted, counted_from_text);

    // The iterator overloads run their own bodies rather than sharing the container ones' instantiations, so each is
    // driven explicitly; the answers must again match the char spelling.
    const auto iterator_match{lexer.tokenize<Token_kind>(bytes.begin(), bytes.end())};

    EXPECT_EQ(iterator_match, text_match);

    std::size_t through_iterators{0};

    const auto count{[&through_iterators](Token_kind, std::size_t) { ++through_iterators; }};

    const auto iterators_consumed{lexer.tokenize_all<Token_kind>(bytes.begin(), bytes.end(), count)};

    EXPECT_EQ(consumed, iterators_consumed);

    EXPECT_EQ(through_iterators, from_text.size());

    const auto byte_plan{lexer.chunk_boundaries(bytes.begin(), bytes.end(), 4)};

    EXPECT_EQ(byte_plan, text_plan);

    const auto window_plan{lexer.chunk_boundaries_with_windows(bytes.begin(), bytes.end(), 4)};

    EXPECT_EQ(window_plan, boundaries);

    const auto counted_through_iterators{
            lexer.tokenize_all_parallel<Token_kind>(bytes.begin(), bytes.end(), 4, ignore_chunk_token)};

    EXPECT_EQ(counted, counted_through_iterators);

    // The NFA scanner is its own public surface with its own body.
    const auto automaton{builder.nfa()};

    const auto [byte_token, byte_length]{nfa::Simulator::run(automaton, bytes.begin(), bytes.end())};

    const auto [text_token, text_length]{nfa::Simulator::run(automaton, input)};

    EXPECT_EQ(byte_token, text_token);
    EXPECT_EQ(byte_length, text_length);
}

TEST_F(Lexer_test, Container_concepts_require_common_const_ranges)
{
    using munch::common::concepts::Iterable;
    using munch::common::concepts::Random_access_iterable;

    static_assert(Random_access_iterable<std::string>);
    static_assert(Random_access_iterable<std::string_view>);
    static_assert(Random_access_iterable<std::vector<char>>);
    static_assert(Random_access_iterable<std::array<char, 4>>);
    static_assert(Random_access_iterable<char[4]>);

    // A counted iterator paired with the default sentinel is random access but not a common range, and a begin/end pair
    // of different types cannot instantiate the single-iterator entry points, so the concept must reject it at the
    // interface instead of failing inside the body.
    using Counted = std::ranges::subrange<std::counted_iterator<std::string::const_iterator>, std::default_sentinel_t>;

    static_assert(std::ranges::random_access_range<Counted>);
    static_assert(!Random_access_iterable<Counted>);
    static_assert(!Iterable<Counted>);

    // A filter view iterates only mutably, and every container overload takes a const reference.
    using Filtered = decltype(std::declval<std::string&>() | std::views::filter([](char) { return true; }));

    static_assert(std::ranges::range<Filtered>);
    static_assert(!Iterable<Filtered>);

    // views::common is the caller's one-line repair, and over a sized random access range it keeps random access.
    const Builder builder{};

    const auto lexer{builder.build()};

    const std::string input{"abab"};

    const auto common{
            std::ranges::subrange{std::counted_iterator{input.cbegin(), 4}, std::default_sentinel} |
            std::views::common};

    static_assert(Random_access_iterable<decltype(common)>);

    EXPECT_EQ(lexer.tokenize<int>(common), no_match<int>);

    EXPECT_EQ(lexer.chunk_boundaries(common, 2), (std::vector<std::size_t>{0, 4}));

    // The byte domain: integral elements qualify, wider ones under the documented modulo-256 reading, and so does
    // std::byte; floating and non-byte elements fall out at overload resolution instead of failing inside a scan.
    using munch::common::concepts::Random_access_byte_iterable;

    static_assert(Random_access_byte_iterable<std::string>);
    static_assert(Random_access_byte_iterable<std::vector<std::uint8_t>>);
    static_assert(Random_access_byte_iterable<std::vector<int>>);
    static_assert(Random_access_byte_iterable<std::array<std::byte, 4>>);
    static_assert(!Random_access_byte_iterable<std::vector<double>>);
    static_assert(!Random_access_byte_iterable<std::vector<std::string>>);

    // Yields bytes from mutable iteration but strings from const iteration, and is judged by its const face, the one
    // every container overload receives.
    struct Two_faced
    {
        [[nodiscard]] std::string::iterator begin() { return {}; }

        [[nodiscard]] std::string::iterator end() { return {}; }

        [[nodiscard]] std::vector<std::string>::const_iterator begin() const { return {}; }

        [[nodiscard]] std::vector<std::string>::const_iterator end() const { return {}; }
    };

    static_assert(!Random_access_byte_iterable<Two_faced>);

    // The rejection happens at the interface of every entry point: each overload asserts its own acceptance and its own
    // refusals, so no single overload can lose its constraint behind the others.
    static_assert(
            Single_scan_over<std::string> && Full_scan_over<std::string> && Byte_plan_over<std::string> &&
            Window_plan_over<std::string> && Parallel_scan_over<std::string>);

    static_assert(
            !Single_scan_over<std::vector<double>> && !Full_scan_over<std::vector<double>> &&
            !Byte_plan_over<std::vector<double>> && !Window_plan_over<std::vector<double>> &&
            !Parallel_scan_over<std::vector<double>>);

    static_assert(
            !Single_scan_over<std::vector<std::string>> && !Full_scan_over<std::vector<std::string>> &&
            !Byte_plan_over<std::vector<std::string>> && !Window_plan_over<std::vector<std::string>> &&
            !Parallel_scan_over<std::vector<std::string>>);

    static_assert(
            !Single_scan_over<Two_faced> && !Full_scan_over<Two_faced> && !Byte_plan_over<Two_faced> &&
            !Window_plan_over<Two_faced> && !Parallel_scan_over<Two_faced>);

    using Good_iterator = std::string_view::iterator;
    using Bad_iterator = std::vector<double>::const_iterator;

    static_assert(
            Single_scan_through<Good_iterator> && Full_scan_through<Good_iterator> &&
            Byte_plan_through<Good_iterator> && Window_plan_through<Good_iterator> &&
            Parallel_scan_through<Good_iterator>);

    static_assert(
            !Single_scan_through<Bad_iterator> && !Full_scan_through<Bad_iterator> &&
            !Byte_plan_through<Bad_iterator> && !Window_plan_through<Bad_iterator> &&
            !Parallel_scan_through<Bad_iterator>);

    // The library scanners beneath the Lexer hold the same line.
    static_assert(Dfa_run_through<Good_iterator> && !Dfa_run_through<Bad_iterator>);
    static_assert(Dfa_run_all_through<Good_iterator> && !Dfa_run_all_through<Bad_iterator>);
    static_assert(Dfa_scan_over<std::string> && !Dfa_scan_over<std::vector<double>> && !Dfa_scan_over<Two_faced>);
    static_assert(Nfa_scan_through<Good_iterator> && !Nfa_scan_through<Bad_iterator>);
    static_assert(Nfa_scan_over<std::string> && !Nfa_scan_over<std::vector<double>> && !Nfa_scan_over<Two_faced>);
}

TEST_F(Lexer_test, Ignored_tokens_reject_non_integral_initializers)
{
    // A floating-point initializer cannot deduce the list's element type and truncate silently: the constrained
    // overload leaves only the vector overload, where the narrowing conversion is ill-formed.
    static_assert(requires(Builder builder) { builder.set_ignored_tokens({1, 2}); });

    enum class Token_kind : std::uint8_t
    {
        comment
    };

    static_assert(requires(Builder builder) { builder.set_ignored_tokens({Token_kind::comment}); });

    // set_ignored_tokens({1.9}) is ill-formed outright: the constrained overload does not deduce a double list, and the
    // vector overload rejects the narrowing conversion. That is a hard error inside the braced list, not a substitution
    // failure, so it cannot be asserted here with a negative requires-expression.
}

TEST_F(Lexer_test, A_sink_taking_either_arity_is_called_with_two_arguments)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    // A sink accepting both arities is called with the token and its length, without the accepting-state payload. A
    // generic or variadic sink accepts both silently.
    Builder builder{};

    builder.add_token(plus(any_of(Set::alpha())), Token_kind::word, 1);

    const auto lexer{builder.build()};

    const std::string input{"abc"};

    std::size_t arity{0};

    const auto record_arity{
            [&arity]<typename... Arguments>(Arguments&&... arguments) { arity = sizeof...(arguments); }};

    std::ignore = lexer.tokenize_all<Token_kind>(input, record_arity);

    EXPECT_EQ(arity, 2U);
}

TEST_F(Lexer_test, A_token_set_is_proved_to_cut_exactly_as_itself)
{
    enum class Token_kind : std::uint8_t
    {
        pair,
        one
    };

    // The reflexive negative: both sides read the same token set, and the search proves they cut every input alike, a
    // set against itself included.
    Builder builder{};

    builder.add_token(concat(text("a"), text("a")), Token_kind::pair, 1);
    builder.add_token(text("a"), Token_kind::one, 2);

    const auto lexer{builder.build()};

    const auto [witness, exhaustive]{lexer.boundary_difference(lexer)};

    EXPECT_TRUE(exhaustive);
    EXPECT_TRUE(witness.empty());
}

TEST_F(Lexer_test, Two_token_sets_over_one_language_differ_and_the_witness_shows_where)
{
    enum class Token_kind : std::uint8_t
    {
        pair,
        one
    };

    // Both sets accept exactly the strings of a repeated, so neither can be told from the other by what it accepts;
    // they are told apart by where they cut, which is the property this decision is about.
    Builder singles{};

    singles.add_token(text("a"), Token_kind::one, 1);

    const auto single{singles.build()};

    Builder doubles{};

    doubles.add_token(concat(text("a"), text("a")), Token_kind::pair, 1);
    doubles.add_token(text("a"), Token_kind::one, 2);

    const auto paired{doubles.build()};

    const auto [witness, exhaustive]{single.boundary_difference(paired)};

    EXPECT_TRUE(exhaustive);
    ASSERT_FALSE(witness.empty());

    // The witness is a shortest one, which here leaves no choice: one byte both sets cut alike, and two bytes are one
    // token against two. Every longer input of a bytes is a differing input too, so only the value itself holds the
    // search to reading inputs by length.
    EXPECT_EQ(witness, "aa");

    const auto cuts{[](const Lexer& lexer, const std::string& input) {
        const auto [tokens, consumed]{per_token_stream<Token_kind>(lexer, input)};

        if (consumed != input.size())
        {
            return std::string{};
        }

        std::string marks(input.size(), '0');

        std::size_t at{0};

        for (const auto& [token, length] : tokens)
        {
            marks[at] = '1';

            at += length;
        }

        return marks;
    }};

    const auto mine{cuts(single, witness)};

    const auto theirs{cuts(paired, witness)};

    EXPECT_FALSE(mine.empty());
    EXPECT_FALSE(theirs.empty());
    EXPECT_NE(mine, theirs);
}

TEST_F(Lexer_test, A_renamed_token_does_not_count_as_a_different_cut)
{
    enum class Token_kind : std::uint8_t
    {
        first,
        second
    };

    // Boundaries and names are separate data: the same partition under another token id is the same segmentation, so a
    // rename alone is no difference.
    Builder one{};

    one.add_token(plus(any_of(Set::alpha())), Token_kind::first, 1);

    Builder two{};

    two.add_token(plus(any_of(Set::alpha())), Token_kind::second, 1);

    const auto [witness, exhaustive]{one.build().boundary_difference(two.build())};

    EXPECT_TRUE(exhaustive);
    EXPECT_TRUE(witness.empty());
}

TEST_F(Lexer_test, Segmentation_difference_separates_token_sets_where_boundary_difference_sees_no_difference)
{
    enum class Token_kind : std::uint8_t
    {
        first,
        second,
        third
    };

    // {a} against {a, b}: the two cut every shared input alike, the shared inputs being the runs of a, so the boundary
    // half is proved empty, and b separates them at once, an input one tokenizes and the other does not.
    Builder singles{};

    singles.add_token(text("a"), Token_kind::first, 1);

    const auto single{singles.build()};

    Builder letters{};

    letters.add_token(text("a"), Token_kind::first, 1);
    letters.add_token(text("b"), Token_kind::second, 1);

    const auto letter{letters.build()};

    const auto [same_cuts, same_cuts_settled]{single.boundary_difference(letter)};

    EXPECT_TRUE(same_cuts_settled);
    EXPECT_TRUE(same_cuts.empty());

    const auto [domain, domain_half, domain_settled]{single.segmentation_difference(letter)};

    EXPECT_TRUE(domain_settled);
    EXPECT_EQ(domain, "b");
    EXPECT_EQ(domain_half, dfa::Separation_half::domain);
    EXPECT_TRUE(separates(single, letter, domain, dfa::Separation_half::domain));

    // The same partition under other token ids is the same segmentation function: an exhaustive search with no witness,
    // and no half to name.
    Builder renamed{};

    renamed.add_token(text("a"), Token_kind::third, 1);

    const auto [none, no_half, settled]{single.segmentation_difference(renamed.build())};

    EXPECT_TRUE(settled);
    EXPECT_TRUE(none.empty());
    EXPECT_FALSE(no_half.has_value());

    // {a} against {aa}: the shortest marked run only one side accepts is a, the domain witness, where the boundary
    // route returns aa, one token against two; the two routes need not agree on the witness or its half.
    Builder pairs{};

    pairs.add_token(concat(text("a"), text("a")), Token_kind::first, 1);

    const auto pair{pairs.build()};

    const auto [pair_witness, pair_half, pair_settled]{single.segmentation_difference(pair)};

    const auto [pair_cut, pair_cut_settled]{single.boundary_difference(pair)};

    EXPECT_EQ(pair_witness, "a");
    EXPECT_EQ(pair_half, dfa::Separation_half::domain);
    EXPECT_TRUE(separates(single, pair, "a", dfa::Separation_half::domain));
    EXPECT_EQ(pair_cut, "aa");

    // {a, b} against {a, ab, b}: every input of a's and b's tokenizes under both, and ab is cut apart, two tokens
    // against one; a boundary witness here is a boundary_difference() witness.
    Builder keywords{};

    keywords.add_token(text("a"), Token_kind::first, 1);
    keywords.add_token(concat(text("a"), text("b")), Token_kind::second, 1);
    keywords.add_token(text("b"), Token_kind::third, 1);

    const auto keyword{keywords.build()};

    const auto [boundary, boundary_half, boundary_settled]{letter.segmentation_difference(keyword)};

    EXPECT_TRUE(boundary_settled);
    EXPECT_EQ(boundary, "ab");
    EXPECT_EQ(boundary_half, dfa::Separation_half::boundary);
    EXPECT_TRUE(separates(letter, keyword, boundary, dfa::Separation_half::boundary));

    const auto [keyword_cut, keyword_cut_settled]{letter.boundary_difference(keyword)};

    EXPECT_EQ(keyword_cut, "ab");
}

TEST_F(Lexer_test, The_segmentation_cap_is_a_ceiling_on_the_states_the_search_holds)
{
    enum class Token_kind : std::uint8_t
    {
        first,
        second
    };

    // {ab, abcd} against {ab, abce}: ab is one token under both, no run of three bytes is accepted by either, and abcd
    // is the first marked run only one side accepts. The cap is the most states the search may hold, so every cap below
    // the smallest one that settles the question answers nothing rather than something, and every cap from it on
    // answers the same witness; zero holds nothing, not even the state the search starts in.
    Builder ones{};

    ones.add_token(concat(text("a"), text("b")), Token_kind::first, 1);
    ones.add_token(concat(text("ab"), text("cd")), Token_kind::second, 1);

    const auto one{ones.build()};

    Builder twos{};

    twos.add_token(concat(text("a"), text("b")), Token_kind::first, 1);
    twos.add_token(concat(text("ab"), text("ce")), Token_kind::second, 1);

    const auto two{twos.build()};

    const auto [held_nothing, no_half, zero_settled]{one.segmentation_difference(two, 0)};

    EXPECT_FALSE(zero_settled);
    EXPECT_TRUE(held_nothing.empty());
    EXPECT_FALSE(no_half.has_value());

    std::size_t holds{1};

    for (;; ++holds)
    {
        const auto [witness, half, exhaustive]{one.segmentation_difference(two, holds)};

        if (exhaustive)
        {
            break;
        }

        ASSERT_TRUE(witness.empty()) << "cap " << holds;
        ASSERT_FALSE(half.has_value()) << "cap " << holds;
    }

    // Every byte of the witness but the last admits at least one state, and the search settled with the witness at the
    // cap.
    const auto [first_witness, first_half, first_settled]{one.segmentation_difference(two, holds)};

    EXPECT_GE(holds, 4U);
    EXPECT_EQ(first_witness, "abcd");

    for (std::size_t cap{holds}; cap <= holds + 8; ++cap)
    {
        const auto [witness, half, exhaustive]{one.segmentation_difference(two, cap)};

        EXPECT_TRUE(exhaustive) << "cap " << cap;
        EXPECT_EQ(witness, "abcd") << "cap " << cap;
        EXPECT_EQ(half, dfa::Separation_half::domain) << "cap " << cap;
    }

    // Two sets are proved one segmentation function only by an exhausted search, and a cap that stops the search before
    // it exhausts says nothing about them.
    const auto [stopped_witness, stopped_half, stopped_settled]{one.segmentation_difference(one, 1)};

    const auto [same_witness, same_half, same_settled]{one.segmentation_difference(one)};

    EXPECT_FALSE(stopped_settled);
    EXPECT_TRUE(same_settled);
}

TEST_F(Lexer_test, Segmentation_difference_agrees_with_the_research_oracle_on_every_literal_pair)
{
    // One of the two literal pools the research oracle decides by language equality: the fifteen one- and two-token
    // subsets of {a, b, ab, ba, aa} that segmentation_equivalence.py's census runs over, and the eight sets of
    // finitely_verifiable.py's full-equivalence sweep. Each matrix cell is the oracle's verdict on the row set against
    // the column set, as the program printed it: = for one segmentation function, otherwise the half the witness falls
    // in, d for domain and b for boundary, and the length of the shortest marked run only one side accepts.
    struct Pool
    {
        std::string_view name{};

        // One text token per literal.
        std::vector<std::vector<std::string_view>> sets{};

        // One matrix row per set, its cells separated by spaces.
        std::vector<std::string_view> verdicts{};
    };

    const std::vector<std::string_view> census{
            " = d1 d1 d1 d1 d1 d2 d2 b2 d1 d1 d1 d1 d1 d1", // {a}
            "d1  = d1 d1 d1 d1 d1 d1 d1 d2 d2 d2 d1 d1 d1", // {b}
            "d1 d1  = d2 d2 d1 d1 d1 d1 d1 d1 d1 d2 d2 d2", // {ab}
            "d1 d1 d2  = d2 d1 d1 d1 d1 d1 d1 d1 d2 d2 d2", // {ba}
            "d1 d1 d2 d2  = d1 d1 d1 d1 d1 d1 d1 d2 d2 d2", // {aa}
            "d1 d1 d1 d1 d1  = d1 d1 d1 d1 d1 d1 d1 d1 d1", // {a, b}
            "d2 d1 d1 d1 d1 d1  = d2 b2 d1 d1 d1 d1 d1 d1", // {a, ab}
            "d2 d1 d1 d1 d1 d1 d2  = b2 d1 d1 d1 d1 d1 d1", // {a, ba}
            "b2 d1 d1 d1 d1 d1 b2 b2  = d1 d1 d1 d1 d1 d1", // {a, aa}
            "d1 d2 d1 d1 d1 d1 d1 d1 d1  = d2 d2 d1 d1 d1", // {b, ab}
            "d1 d2 d1 d1 d1 d1 d1 d1 d1 d2  = d2 d1 d1 d1", // {b, ba}
            "d1 d2 d1 d1 d1 d1 d1 d1 d1 d2 d2  = d1 d1 d1", // {b, aa}
            "d1 d1 d2 d2 d2 d1 d1 d1 d1 d1 d1 d1  = d2 d2", // {ab, ba}
            "d1 d1 d2 d2 d2 d1 d1 d1 d1 d1 d1 d1 d2  = d2", // {ab, aa}
            "d1 d1 d2 d2 d2 d1 d1 d1 d1 d1 d1 d1 d2 d2  =", // {ba, aa}
    };

    const std::vector<std::string_view> sweep{
            " = d1 d1 d1 d2 d1 d1 b2", // {a}
            "d1  = d1 d1 d1 d2 d1 d1", // {b}
            "d1 d1  = d1 d1 d1 b2 d1", // {a, b}
            "d1 d1 d1  = d1 d1 d1 d1", // {ab}
            "d2 d1 d1 d1  = d1 d1 b2", // {a, ab}
            "d1 d2 d1 d1 d1  = d1 d1", // {ab, b}
            "d1 d1 b2 d1 d1 d1  = d1", // {a, ab, b}
            "b2 d1 d1 d1 b2 d1 d1  =", // {aa, a}
    };

    const std::vector<Pool> pools{
            {.name = "census",
             .sets =
                     {{"a"},
                      {"b"},
                      {"ab"},
                      {"ba"},
                      {"aa"},
                      {"a", "b"},
                      {"a", "ab"},
                      {"a", "ba"},
                      {"a", "aa"},
                      {"b", "ab"},
                      {"b", "ba"},
                      {"b", "aa"},
                      {"ab", "ba"},
                      {"ab", "aa"},
                      {"ba", "aa"}},
             .verdicts = census},
            {.name = "sweep",
             .sets = {{"a"}, {"b"}, {"a", "b"}, {"ab"}, {"a", "ab"}, {"ab", "b"}, {"a", "ab", "b"}, {"aa", "a"}},
             .verdicts = sweep}};

    const auto build{[](const std::vector<std::string_view>& tokens) {
        const std::vector<std::string> literals{tokens.begin(), tokens.end()};

        return literal_lexer(literals);
    }};

    std::size_t decided{0};

    std::size_t domains{0};

    std::size_t boundaries{0};

    for (const auto& [name, sets, verdicts] : pools)
    {
        std::vector<Lexer> lexers{};

        for (const auto& tokens : sets)
        {
            lexers.push_back(build(tokens));
        }

        const auto decide{[&](const std::size_t row, const std::size_t column, const std::string& cell) {
            const auto [witness, half, exhaustive]{lexers[row].segmentation_difference(lexers[column])};

            ASSERT_TRUE(exhaustive) << name << ' ' << row << ' ' << column;
            EXPECT_EQ(witness.empty(), cell == "=") << name << ' ' << row << ' ' << column;
            EXPECT_EQ(half.has_value(), cell != "=") << name << ' ' << row << ' ' << column;

            ++decided;

            if (cell == "=")
            {
                return;
            }

            // Both searches find a shortest witness, so the lengths agree even where the witnesses need not, and the
            // half is a fact about the witness both read the same way; the witness is checked as the oracle checks its
            // own, by scanning it under both sets.
            const auto expected{cell[0] == 'd' ? dfa::Separation_half::domain : dfa::Separation_half::boundary};

            const auto shortest{static_cast<std::size_t>(cell[1] - '0')};

            EXPECT_EQ(witness.size(), shortest) << name << ' ' << row << ' ' << column;
            EXPECT_EQ(half, expected) << name << ' ' << row << ' ' << column;
            EXPECT_TRUE(separates(lexers[row], lexers[column], witness, expected))
                    << name << ' ' << row << ' ' << column;

            if (expected == dfa::Separation_half::domain)
            {
                ++domains;
            }
            else
            {
                ++boundaries;
            }
        }};

        for (std::size_t row{0}; row < sets.size(); ++row)
        {
            std::istringstream cells{std::string{verdicts[row]}};

            for (std::size_t column{0}; column < sets.size(); ++column)
            {
                std::string cell{};

                ASSERT_TRUE(cells >> cell) << name << ' ' << row << ' ' << column;

                decide(row, column, cell);
            }
        }
    }

    // The oracle's own counts: 225 census pairs, 204 separating on the domain and 6 on the boundary, and 64 sweep
    // pairs, 50 and 6.
    EXPECT_EQ(decided, 289U);
    EXPECT_EQ(domains, 254U);
    EXPECT_EQ(boundaries, 12U);
}

TEST_F(Lexer_test, The_anchor_free_span_of_one_fixed_token_is_its_interior)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    // A token whose first byte only the initial state consumes certifies that byte, and every later byte of it is
    // consumed mid-token. The uncertified run is therefore exactly the interior, whatever the token's length, which
    // makes this a family whose answer is known in advance rather than read off the implementation.
    for (const std::string word : {"ab", "abc", "abcd", "abcde"})
    {
        Builder builder{};

        auto pattern{text(word.front())};

        for (std::size_t at{1}; at < word.size(); ++at)
        {
            pattern = concat(pattern, text(word[at]));
        }

        builder.add_token(pattern, Token_kind::word, 1);

        const auto lexer{builder.build()};

        ASSERT_TRUE(lexer.is_split_point(word.front()));
        ASSERT_FALSE(lexer.is_split_point(word[1]));

        const auto span{lexer.anchor_free_span()};

        ASSERT_TRUE(span.has_value());
        EXPECT_EQ(*span, word.size() - 1);
    }
}

TEST_F(Lexer_test, A_token_set_certifying_every_byte_leaves_no_anchor_free_position)
{
    enum class Token_kind : std::uint8_t
    {
        a,
        b
    };

    // Single-byte tokens are consumed only from the initial state, so every position of every input is an anchor and
    // the longest run without one is empty rather than absent.
    Builder builder{};

    builder.add_token(text("a"), Token_kind::a, 1);
    builder.add_token(text("b"), Token_kind::b, 2);

    const auto span{builder.build().anchor_free_span()};

    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(*span, 0U);
}

TEST_F(Lexer_test, A_run_token_leaves_the_anchor_free_span_unbounded)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        semicolon
    };

    // The run's own byte is consumed by its continuation state, so nothing certifies, and an input of that byte can be
    // any length. Unbounded is the answer a conventional grammar usually gets and is reported rather than capped.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);
    builder.add_token(text(";"), Token_kind::semicolon, 2);

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_point(';'));
    ASSERT_FALSE(lexer.is_split_point('a'));
    EXPECT_FALSE(lexer.anchor_free_span().has_value());
}

TEST_F(Lexer_test, A_window_inventory_bounds_a_span_no_certified_byte_can)
{
    enum class Token_kind : std::uint8_t
    {
        word
    };

    // One token, aab. Its a is consumed mid-token and its b only from a live state, so no byte certifies and the byte
    // form has nothing to anchor on. The windows are another matter: aab is certified at its own start, and ba at the
    // a, since b only ever ends a token. Every input is a run of aab, so the anchor-free stretch is the two interior
    // positions, the same interior the fixed-token family reaches through a certified first byte.
    Builder builder{};

    builder.add_token(concat(text("aa"), text("b")), Token_kind::word, 1);

    const auto lexer{builder.build()};

    ASSERT_FALSE(lexer.is_split_point('a'));
    ASSERT_FALSE(lexer.is_split_point('b'));
    ASSERT_FALSE(lexer.anchor_free_span().has_value());
    ASSERT_EQ(lexer.is_split_window("aab"), std::optional<std::size_t>{0});
    ASSERT_EQ(lexer.is_split_window("ba"), std::optional<std::size_t>{1});

    const std::vector<std::pair<std::string_view, std::size_t>> inventory{{"aab", 0}, {"ba", 1}};

    const auto span{lexer.anchor_free_span(inventory)};

    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(*span, 2U);
}

TEST_F(Lexer_test, The_window_span_counts_positions_a_window_reaches_back_to)
{
    enum class Token_kind : std::uint8_t
    {
        ab,
        bab
    };

    // Tokens ab and bab. The inventory anchors the last a of aba, the last b of abb and bb and the middle b of bba, so
    // an anchor is only known once the bytes after it have been read. The longest stretch is bab followed by ab, whose
    // first three positions no window reaches: the decision has to hold positions back until the windows over them are
    // settled, and to release them unanchored when the input ends first.
    Builder builder{};

    builder.add_token(concat(text("a"), text("b")), Token_kind::ab, 1);
    builder.add_token(concat(text("ba"), text("b")), Token_kind::bab, 1);

    const auto lexer{builder.build()};

    ASSERT_FALSE(lexer.anchor_free_span().has_value());
    ASSERT_EQ(lexer.is_split_window("aba"), std::optional<std::size_t>{2});
    ASSERT_EQ(lexer.is_split_window("abb"), std::optional<std::size_t>{2});
    ASSERT_EQ(lexer.is_split_window("bb"), std::optional<std::size_t>{1});
    ASSERT_EQ(lexer.is_split_window("bba"), std::optional<std::size_t>{1});

    const std::vector<std::pair<std::string_view, std::size_t>> inventory{
            {"aba", 2},
            {"abb", 2},
            {"bb", 1},
            {"bba", 1}};

    const auto span{lexer.anchor_free_span(inventory)};

    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(*span, 3U);
}

TEST_F(Lexer_test, A_window_longer_than_a_machine_word_is_decided_like_a_short_one)
{
    enum class Token_kind : std::uint8_t
    {
        a,
        b
    };

    // Tokens a and b, every byte a token. The windows ab and ba anchor the first byte of every run after the first, and
    // a^n and b^n anchor every byte of a run from its n-th on. So only the first n - 1 bytes of the input can escape
    // every anchor, and the anchor-free stretch is n - 1 for every n. The walk keeps no buffer of bytes, so a window of
    // 70 bytes costs no more in kind than one of 3.
    Builder builder{};

    builder.add_token(text("a"), Token_kind::a, 1);
    builder.add_token(text("b"), Token_kind::b, 1);

    const auto lexer{builder.build()};

    for (const std::size_t width : {3U, 70U})
    {
        const std::string as(width, 'a');

        const std::string bs(width, 'b');

        ASSERT_EQ(lexer.is_split_window(as), std::optional<std::size_t>{width - 1});
        ASSERT_EQ(lexer.is_split_window(bs), std::optional<std::size_t>{width - 1});
        ASSERT_EQ(lexer.is_split_window("ab"), std::optional<std::size_t>{1});
        ASSERT_EQ(lexer.is_split_window("ba"), std::optional<std::size_t>{1});

        const std::vector<std::pair<std::string_view, std::size_t>> inventory{
                {as, width - 1},
                {bs, width - 1},
                {"ab", 1},
                {"ba", 1}};

        const auto span{lexer.anchor_free_span(inventory)};

        ASSERT_TRUE(span.has_value());
        EXPECT_EQ(*span, width - 1);
    }
}

TEST_F(Lexer_test, A_run_token_leaves_the_window_span_unbounded_too)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        ab
    };

    // A run of a and the token ab. The a after a b starts a token, since b only ends one, so ba is certified at the a;
    // but an input of a alone never contains a b, so the windows anchor nothing on it and the stretch is as long as the
    // input. Windows change which inputs carry anchors, not whether every long input must.
    Builder builder{};

    builder.add_token(plus(any_of(Set{'a'})), Token_kind::run, 1);
    builder.add_token(concat(text("a"), text("b")), Token_kind::ab, 1);

    const auto lexer{builder.build()};

    ASSERT_FALSE(lexer.is_split_point('a'));
    ASSERT_FALSE(lexer.is_split_point('b'));
    ASSERT_EQ(lexer.is_split_window("ba"), std::optional<std::size_t>{1});

    const std::vector<std::pair<std::string_view, std::size_t>> inventory{{"ba", 1}};

    EXPECT_FALSE(lexer.anchor_free_span(inventory).has_value());
}

TEST_F(Lexer_test, A_nullable_token_set_leaves_the_anchor_free_span_unbounded)
{
    enum class Token_kind : std::uint8_t
    {
        run
    };

    // a* minimizes to an accepting, self-looping start state, so it certifies nothing and its only accepting state is
    // the initial one. Any run of a is still one token, so inputs of every length tokenize with no anchor in them; the
    // decision has to see the match through a transition into an accepting state rather than through an accepting state
    // other than the initial one, which this set does not have.
    Builder builder{};

    builder.add_token(kleene(text("a")), Token_kind::run, 1);

    const auto lexer{builder.build()};

    ASSERT_FALSE(lexer.is_split_point('a'));
    EXPECT_FALSE(lexer.anchor_free_span().has_value());
}

TEST_F(Lexer_test, A_token_payload_reaches_the_three_argument_sink_as_the_last_word_given_and_zero_where_none_was)
{
    enum class Token_kind : std::uint8_t
    {
        word,
        number,
        space
    };

    // Three tokens: the word gets a payload, the number gets one twice so the last word must win, and the space gets
    // none, which the sink sees as zero; a three-argument sink is the only place the payload surfaces.
    Builder builder{};

    builder.add_token(plus(any_of(Set::alpha())), Token_kind::word, 1);
    builder.add_token(plus(any_of(Set::digits())), Token_kind::number, 1);
    builder.add_token(text(" "), Token_kind::space, 1);

    builder.set_token_payload(Token_kind::word, 7);
    builder.set_token_payload(Token_kind::number, 11);
    builder.set_token_payload(Token_kind::number, 13);

    const auto lexer{builder.build()};

    std::vector<std::pair<Token_kind, std::uint64_t>> seen{};

    const std::string input{"ab 12 c"};

    const auto collect{[&seen](const Token_kind kind, std::size_t, const std::uint64_t payload) {
        seen.emplace_back(kind, payload);
    }};

    const auto consumed{lexer.tokenize_all<Token_kind>(input, collect)};

    EXPECT_EQ(consumed, input.size());

    const std::vector<std::pair<Token_kind, std::uint64_t>> expected{
            {Token_kind::word, 7},
            {Token_kind::space, 0},
            {Token_kind::number, 13},
            {Token_kind::space, 0},
            {Token_kind::word, 7}};

    EXPECT_EQ(seen, expected);
}

TEST_F(Lexer_test, A_state_limit_error_carries_its_limit_in_the_message_and_the_accessor)
{
    // Constructed directly rather than through an exploding grammar, so the test says what the type promises on its
    // own: a std::runtime_error whose text names the cap and whose accessor returns it.
    const State_limit_error error{42};

    EXPECT_EQ(error.limit(), 42U);
    EXPECT_TRUE(std::string_view{error.what()}.contains("42"));

    const std::runtime_error& base{error};

    EXPECT_EQ(std::string_view{base.what()}, std::string_view{error.what()});
}

TEST_F(Lexer_test, Window_at_tries_lengths_ascending_from_two_and_the_first_certificate_wins)
{
    enum class Token_kind : std::uint8_t
    {
        run,
        b
    };

    // Over {a+, b} a window ending in b certifies with the b as its origin and one of a alone refuses, since the run
    // may have begun before the window: at the start of abb the two-byte window already certifies, so its origin and
    // length come back although the three-byte one certifies too; at the start of aab the two-byte window refuses and
    // the three-byte one answers; a run of a refuses at every length, and one byte left fits no window at all.
    Builder builder{};

    builder.add_token(plus(text("a")), Token_kind::run, 1);
    builder.add_token(text("b"), Token_kind::b, 1);

    const auto lexer{builder.build()};

    ASSERT_TRUE(lexer.is_split_window("ab").has_value());
    ASSERT_TRUE(lexer.is_split_window("abb").has_value());
    ASSERT_FALSE(lexer.is_split_window("aa").has_value());

    using Found = std::optional<std::pair<std::size_t, std::size_t>>;

    Window_planner planner{};

    const auto window_at{[&](const std::string_view input, const std::size_t at) {
        return planner.window_at(lexer.simulator(), input.begin(), input.size(), at);
    }};

    EXPECT_EQ(window_at("abb", 0), Found({1, 2}));
    EXPECT_EQ(window_at("aab", 0), Found({2, 3}));
    EXPECT_EQ(window_at("aaaaa", 0), std::nullopt);
    EXPECT_EQ(window_at("ab", 1), std::nullopt);

    // At or past the input's size the search finds nothing, as the searches beginning at an offset have it: no byte is
    // left to read there.
    EXPECT_EQ(window_at("ab", 2), std::nullopt);
    EXPECT_EQ(window_at("a", 2), std::nullopt);
    EXPECT_EQ(window_at("abb", 9), std::nullopt);
    EXPECT_EQ(window_at("", 1), std::nullopt);

    // The same search over an input shorter than the bytes behind it, which a caller holding one buffer and reading a
    // part of it gives: the size is one, the position two, and the bytes at the position are live and certify a window.
    // The length available there is zero, so the search answers nothing, which shows it read none of them.
    const std::string behind{"xxabbbb"};

    EXPECT_EQ(planner.cut(lexer.simulator(), behind.cbegin(), 1, 2), std::nullopt);
    EXPECT_EQ(planner.window_at(lexer.simulator(), behind.cbegin(), 1, 2), std::nullopt);

    // The search from a floor answers at or after it, so a floor at or past the size finds nothing rather than the
    // first cut in the input, up to the largest std::size_t, where the walk's own bound, the floor plus two, does not
    // fit. The library's own caller holds its floor at the size at most; the public function bounds the floor nowhere.

    const auto cut{[&](const std::string_view input, const std::size_t floor) {
        return planner.cut(lexer.simulator(), input.begin(), input.size(), floor);
    }};

    EXPECT_EQ(cut("ab", 0), std::optional<std::size_t>{1});
    EXPECT_EQ(cut("ab", 2), std::nullopt);
    EXPECT_EQ(cut("ab", 5), std::nullopt);
    EXPECT_EQ(cut("ab", std::numeric_limits<std::size_t>::max()), std::nullopt);
    EXPECT_EQ(cut("", std::numeric_limits<std::size_t>::max()), std::nullopt);
}

#ifndef MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP
#define MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"

namespace munch::tools::benchmark
{
/**
 * @brief The token types recognized by the benchmarked lexers.
 */
enum class Token : std::size_t
{
    /**
     * @brief A whitespace run.
     */
    whitespace = 1,

    /**
     * @brief An identifier.
     */
    identifier,

    /**
     * @brief A number.
     */
    number,

    /**
     * @brief A keyword.
     */
    keyword,

    /**
     * @brief An operator.
     */
    operator_,

    /**
     * @brief A punctuation byte.
     */
    punctuation,

    /**
     * @brief A string literal.
     */
    string_,

    /**
     * @brief A comment.
     */
    comment
};

/**
 * @brief The bytes in a mebibyte, the unit every throughput is reported in.
 */
inline constexpr double bytes_per_mebibyte{1024.0 * 1024.0};

/**
 * @brief The multiplier of the fixed-seed linear congruential generator the corpora are drawn from.
 */
inline constexpr unsigned lcg_multiplier{1664525U};

/**
 * @brief The increment of the fixed-seed linear congruential generator the corpora are drawn from.
 */
inline constexpr unsigned lcg_increment{1013904223U};

/**
 * @brief The seed every corpus of the benchmark starts its linear congruential stream from.
 */
inline constexpr unsigned corpus_seed{12345U};

/**
 * @brief The low bits the benchmark's corpora drop from each draw of next_random().
 */
inline constexpr unsigned corpus_dropped_bits{16U};

/**
 * @brief The low bits the figures' corpora drop from each draw of next_random().
 */
inline constexpr unsigned figure_dropped_bits{8U};

/**
 * @brief The timed passes each benchmark runs when its command line names none.
 */
inline constexpr int default_passes{15};

/**
 * @brief The identifiers of the ASCII corpus, none of which starts with a keyword.
 *
 * A first-match alternation listing keywords before identifiers would tokenize an entry like "integer" as "int" +
 * "eger" while munch's longest match keeps it whole, so the engine comparison's validation fails loudly if an entry
 * breaks this.
 */
inline constexpr std::array<std::string_view, 6> ascii_identifiers{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

/**
 * @brief One value on a cache line of its own, so that per-chunk values written by separate worker threads do not
 *        false-share, as the parallel entry point's contract asks.
 * @tparam T The value's type.
 */
template <typename T>
struct alignas(64) Padded
{
    /**
     * @brief The value.
     */
    T value{};
};

/**
 * @brief The fixed-seed stream every corpus of the benchmark draws from: next_random() started at corpus_seed, each
 *        draw dropping corpus_dropped_bits.
 */
class Corpus_random
{
public:
    /**
     * @brief Advances the stream and draws its high bits.
     * @return The bits drawn.
     */
    [[nodiscard]] unsigned operator()() noexcept;

private:
    /**
     * @brief The stream's state.
     */
    unsigned seed_{corpus_seed};
};

/**
 * @brief The token types of the JSON lexer.
 */
enum class Json_token : std::size_t
{
    /**
     * @brief A whitespace run.
     */
    whitespace = 1,

    /**
     * @brief A string.
     */
    string,

    /**
     * @brief A number.
     */
    number,

    /**
     * @brief One of the three literal names.
     */
    literal,

    /**
     * @brief A structural character.
     */
    structural
};

/**
 * @brief One named scenario in an interleaved measurement.
 */
struct Scenario
{
    /**
     * @brief The scenario name to report.
     */
    std::string_view name{};

    /**
     * @brief The input size the pass consumes.
     */
    std::size_t bytes{0};

    /**
     * @brief One pass, returning its token count.
     */
    std::function<std::size_t()> pass{};
};

/**
 * @brief Advances a fixed-seed linear congruential stream and draws its high bits.
 *
 * The one generator every corpus of the benchmark and of the figures draws from, so a corpus is identical across runs
 * and builds.
 * @param seed The stream's state, advanced.
 * @param dropped_bits The low bits dropped from the draw, corpus_dropped_bits or figure_dropped_bits.
 * @return The advanced state shifted right by the dropped bits.
 */
constexpr unsigned next_random(unsigned& seed, const unsigned dropped_bits)
{
    seed = seed * lcg_multiplier + lcg_increment;

    return seed >> dropped_bits;
}

/**
 * @brief Returns the bytes in a number of mebibytes, the unit every corpus size is given in.
 * @param mebibytes The size in MiB.
 * @return The size in bytes.
 */
constexpr std::size_t bytes_of(const std::size_t mebibytes)
{
    return mebibytes << 20U;
}

/**
 * @brief Returns the ASCII identifier pattern: a letter or underscore, then letters, digits and underscores.
 * @return The pattern.
 */
regex::Regex ascii_identifier();

/**
 * @brief Builds a lexer for a small C-like language, exercising every regex combinator kind.
 * @param greek_identifiers Whether identifiers may also contain Greek letters, encoded as UTF-8.
 * @return The lexer.
 */
core::Lexer build_lexer(bool greek_identifiers);

/**
 * @brief Builds the same C-like lexer with the identifier pattern given.
 * @param identifier The identifier pattern.
 * @return The lexer.
 */
core::Lexer build_lexer(regex::Regex identifier);

/**
 * @brief Appends the keyword-scale token set to a builder: 100 keywords plus identifier, number, operator and
 *        punctuation patterns, approximating a real language front end.
 *
 * Shared between the construction-cost benchmark and the applicability figure, so the published row is asserted against
 * the grammar the benchmark compiles rather than against a transcription verified by eye.
 * @param builder The builder the tokens are added to.
 */
void keyword_scale_tokens(core::Builder& builder);

/**
 * @brief Generates deterministic pseudo-code of at least the given size.
 * @param size The minimum size of the input in bytes.
 * @param identifiers The identifier pool the code draws from.
 * @return The generated input.
 */
std::string generate_input(std::size_t size, std::span<const std::string_view> identifiers);

/**
 * @brief Generates deterministic pseudo-code shaped like real source: long identifiers, indentation, and larger
 *        numbers, averaging several bytes per token where generate_input() averages under two.
 * @param size The minimum size of the input in bytes.
 * @return The generated input.
 */
std::string generate_source_input(std::size_t size);

/**
 * @brief Builds a lexer for the RFC 8259 lexical forms, taken over bytes.
 *
 * JSON is the corpus where the exact certificate is empty for a reason given by the specification rather than by an
 * accident of the grammar: a string may hold any byte except a raw control byte, so a state inside one consumes most of
 * the alphabet, and no byte is safe at every occurrence. Discarding whitespace recovers tab, newline and carriage
 * return, which is what makes JSON the sharpest available test of the weaker certificate.
 * @param discard_whitespace Whether whitespace is declared discarded, enabling the relaxed certificate.
 * @return The lexer.
 */
core::Lexer build_json_lexer(bool discard_whitespace);

/**
 * @brief Generates deterministic JSON of at least the given size.
 *
 * The two shapes recognize the same documents and differ only in whitespace, so a comparison between them isolates what
 * the corpus contributes from what the grammar does: the pretty form carries newlines the relaxed certificate admits,
 * and the minified form carries none at all.
 * @param size The minimum size of the input in bytes.
 * @param pretty Whether to indent and break lines, rather than emit the document on one line.
 * @return The generated input.
 */
std::string generate_json_input(std::size_t size, bool pretty);

/**
 * @brief Returns the median of a sorted sample: the mean of its two middle values, which are one value for an odd
 *        count.
 * @param sorted The sample, ascending and not empty.
 * @return The median.
 */
[[nodiscard]] double median_of(const std::vector<double>& sorted);

/**
 * @brief Prints a scenario's summary line: its size, token count and pass count, and the best, median and worst
 *        throughput of its sorted pass times.
 * @param name The scenario name to report.
 * @param bytes The input size the pass consumes.
 * @param tokens The token count to display.
 * @param passes The number of timed passes.
 * @param sorted_seconds The pass times, ascending and not empty.
 */
void print_summary(
        std::string_view name, std::size_t bytes, std::size_t tokens, int passes,
        const std::vector<double>& sorted_seconds);

/**
 * @brief Measures the throughput of a tokenization pass over a number of runs.
 *
 * The first pass warms caches and provides the token count the timed passes are validated against. Three figures are
 * reported: the best pass estimates the least-interrupted cost of the work itself, the median shows the typical run,
 * and the worst bounds the interference the machine added, so the spread is visible instead of only the most favorable
 * pass. Every timed pass must reproduce the warmup pass's full result, not merely its token count, so a pass that
 * drifted in content rather than length still fails loudly.
 * @tparam Pass Callable running one pass and returning an equality-comparable result.
 * @tparam Count Callable projecting the token count out of a result.
 * @param name The scenario name to report.
 * @param bytes The input size the pass consumes.
 * @param passes The number of timed passes.
 * @param pass The pass to measure, returning an equality-comparable result.
 * @param count_of Projects the token count to display out of a result.
 * @return True if every pass tokenized the input completely and consistently.
 */
template <typename Pass, typename Count>
bool measure(const std::string_view name, const std::size_t bytes, const int passes, Pass&& pass, Count&& count_of)
{
    const auto expected{pass()};

    if (count_of(expected) == 0)
    {
        return false;
    }

    std::vector<double> seconds{};

    seconds.reserve(static_cast<std::size_t>(passes));

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        const auto result{pass()};

        const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

        if (result != expected)
        {
            std::printf("%s: the result changed between passes\n", std::string{name}.c_str());

            return false;
        }

        seconds.push_back(elapsed.count());
    }

    std::ranges::sort(seconds);

    const auto count{count_of(expected)};

    print_summary(name, bytes, count, passes, seconds);

    return true;
}

/**
 * @brief Measures a pass whose result is its own token count.
 * @tparam Pass Callable running one pass and returning its token count.
 * @param name The scenario name to report.
 * @param bytes The input size the pass consumes.
 * @param passes The number of timed passes.
 * @param pass The pass to measure.
 * @return True if every pass tokenized the input completely and consistently.
 */
template <typename Pass>
bool measure(const std::string_view name, const std::size_t bytes, const int passes, Pass&& pass)
{
    return measure(name, bytes, passes, std::forward<Pass>(pass), std::identity{});
}

/**
 * @brief Measures several scenarios in interleaved rounds rather than one scenario at a time.
 *
 * Running a scenario's passes consecutively lets thermal, turbo, scheduler, and host-load drift land on one scenario
 * and not its neighbours, which biases the ratios between them in a direction the measurement cannot recover. Here each
 * round runs every scenario once, in an order reshuffled per round from a fixed seed, so drift spreads across all of
 * them and the run stays reproducible.
 *
 * Every observation is written to a CSV rather than only the best, median, and worst, so the reported summary can be
 * checked against the distribution it came from.
 * @param scenarios The scenarios to measure, reported in the order given.
 * @param passes The number of rounds; each round runs every scenario once.
 * @param input_mebibytes The corpus size to record beside each observation.
 * @param observations_path The CSV to append every observation to, std::nullopt to write none.
 * @return True if every scenario reproduced its warmup result on every round.
 */
bool measure_interleaved(
        std::span<const Scenario> scenarios, int passes, std::size_t input_mebibytes,
        std::optional<std::string_view> observations_path);

/**
 * @brief Prints what this run may be cited as: the commit built from, the machine, and the invocation.
 *
 * A throughput figure is quotable only beside the tree and machine that produced it.
 * @param benchmark The name of the benchmark reporting.
 * @param passes The pass count the run was invoked with.
 * @param observations_path The CSV being written, std::nullopt when none is.
 */
void print_provenance(std::string_view benchmark, int passes, std::optional<std::string_view> observations_path);

} // namespace munch::tools::benchmark

#endif // MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP

#ifndef MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP
#define MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "munch/core/lexer.hpp"

namespace munch::tools::benchmark
{
/**
 * @brief The token types recognized by the benchmarked lexers.
 */
enum class Token : std::size_t
{
    whitespace = 1,
    identifier,
    number,
    keyword,
    operator_,
    punctuation
};

/**
 * @brief Builds a lexer for a small C-like language, exercising every regex combinator kind.
 * @param greek_identifiers Whether identifiers may also contain Greek letters, encoded as UTF-8.
 */
core::Lexer build_lexer(bool greek_identifiers);

/**
 * @brief Generates deterministic pseudo-code of at least the given size.
 * @param size The minimum size of the input in bytes.
 * @param identifiers The identifier pool the code draws from.
 * @return The generated input.
 */
std::string generate_input(std::size_t size, std::span<const char* const> identifiers);

/**
 * @brief Generates deterministic pseudo-code shaped like real source: long identifiers, indentation, and larger
 * numbers, averaging several bytes per token where generate_input() averages under two.
 * @param size The minimum size of the input in bytes.
 * @return The generated input.
 */
std::string generate_source_input(std::size_t size);

/**
 * @brief Measures the throughput of a tokenization pass over a number of runs.
 *
 * The first pass warms caches and provides the token count the timed passes are validated against. Three figures
 * are reported: the best pass estimates the least-interrupted cost of the work itself, the median shows the
 * typical run, and the worst bounds the interference the machine added, so the spread is visible instead of only
 * the most favorable pass.
 * Every timed pass must reproduce the warmup pass's full result, not merely its token count, so a pass that
 * drifted in content rather than length still fails loudly.
 * @param name The scenario name to report.
 * @param bytes The input size the pass consumes.
 * @param passes The number of timed passes.
 * @param pass The pass to measure, returning an equality-comparable result.
 * @param count_of Projects the token count to display out of a result.
 * @return True if every pass tokenized the input completely and consistently.
 */
template <typename Pass, typename Count>
bool measure(const char* name, const std::size_t bytes, const int passes, Pass&& pass, Count&& count_of)
{
    const auto expected{pass()};

    if (count_of(expected) == 0)
    {
        return false;
    }

    std::vector<double> seconds;

    seconds.reserve(static_cast<std::size_t>(passes));

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        const auto result{pass()};

        const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

        if (result != expected)
        {
            std::printf("%s: the result changed between passes\n", name);

            return false;
        }

        seconds.push_back(elapsed.count());
    }

    std::sort(seconds.begin(), seconds.end());

    const auto mib{static_cast<double>(bytes) / (1024.0 * 1024.0)};

    const auto median{(seconds[(seconds.size() - 1) / 2] + seconds[seconds.size() / 2]) / 2.0};

    std::printf(
            "%-16s %.1f MiB, %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f MiB/s\n", name, mib,
            count_of(expected), passes, mib / seconds.front(), mib / median, mib / seconds.back());

    return true;
}

/**
 * @brief Measures a pass whose result is its own token count.
 */
template <typename Pass>
bool measure(const char* name, const std::size_t bytes, const int passes, Pass&& pass)
{
    return measure(name, bytes, passes, std::forward<Pass>(pass), [](const std::size_t count) { return count; });
}

/**
 * @brief One named scenario in an interleaved measurement.
 */
struct Scenario
{
    const char* name;
    std::size_t bytes;
    std::function<std::size_t()> pass;
};

/**
 * @brief Measures several scenarios in interleaved rounds rather than one scenario at a time.
 *
 * Running a scenario's passes consecutively lets thermal, turbo, scheduler, and host-load drift land on one
 * scenario and not its neighbours, which biases the ratios between them in a direction the measurement cannot
 * recover. Here each round runs every scenario once, in an order reshuffled per round from a fixed seed, so drift
 * spreads across all of them and the run stays reproducible.
 *
 * Every observation is written to a CSV rather than only the best, median, and worst, so the reported summary can
 * be checked against the distribution it came from.
 * @param scenarios The scenarios to measure, reported in the order given.
 * @param passes The number of rounds; each round runs every scenario once.
 * @param input_mebibytes The corpus size to record beside each observation.
 * @param observations_path The CSV to append every observation to, or nullptr to write none.
 * @return True if every scenario reproduced its warmup result on every round.
 */
bool measure_interleaved(
        std::span<const Scenario> scenarios, int passes, std::size_t input_mebibytes, const char* observations_path);

} // namespace munch::tools::benchmark

#endif // MUNCH_TOOLS_BENCHMARK_INCLUDE_MUNCH_TOOLS_BENCHMARK_HARNESS_HPP

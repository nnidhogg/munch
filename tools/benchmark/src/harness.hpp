#ifndef MUNCH_TOOLS_BENCHMARK_SRC_HARNESS_HPP
#define MUNCH_TOOLS_BENCHMARK_SRC_HARNESS_HPP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <span>
#include <string>

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
 * @brief Measures the best throughput of a tokenization pass over a number of runs.
 *
 * The first pass warms caches and provides the token count the timed passes are validated against. The minimum is
 * reported, as it is the least noisy estimate of the cost of the work itself.
 * @param name The scenario name to report.
 * @param bytes The input size the pass consumes.
 * @param passes The number of timed passes.
 * @param pass The pass to measure, returning its token count.
 * @return True if every pass tokenized the input completely and consistently.
 */
template <typename Pass>
bool measure(const char* name, const std::size_t bytes, const int passes, Pass&& pass)
{
    const auto expected{pass()};

    if (expected == 0)
    {
        return false;
    }

    double best_seconds{std::numeric_limits<double>::infinity()};

    for (int index{0}; index < passes; ++index)
    {
        const auto start{std::chrono::steady_clock::now()};

        const auto tokens{pass()};

        const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

        if (tokens != expected)
        {
            std::printf("%s: token count changed between passes\n", name);

            return false;
        }

        best_seconds = std::min(best_seconds, elapsed.count());
    }

    const auto mib{static_cast<double>(bytes) / (1024.0 * 1024.0)};

    std::printf(
            "%-16s %.1f MiB, %zu tokens, best of %d passes: %.1f MiB/s\n", name, mib, expected, passes,
            mib / best_seconds);

    return true;
}

} // namespace munch::tools::benchmark

#endif // MUNCH_TOOLS_BENCHMARK_SRC_HARNESS_HPP

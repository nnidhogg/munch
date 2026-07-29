#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "munch/tools/benchmark/harness.hpp"
#include "munch/tools/tokenizer/tokenizer.hpp"

namespace
{
using namespace munch::tools::benchmark;

/**
 * @brief Tokenizes the whole input once through the Lexer.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize(const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t offset{0};

    std::size_t tokens{0};

    while (offset < input.size())
    {
        const auto [token, length]{lexer.tokenize<Token>(input.cbegin() + offset, input.cend())};

        if (!token || length == 0)
        {
            std::printf("input rejected at offset %zu\n", offset);

            return 0;
        }

        offset += length;

        ++tokens;
    }

    return tokens;
}

/**
 * @brief Tokenizes the whole input once through the Tokenizer, measuring the driver layer as well.
 *
 * Flattened because next() only inlines into the loop on its own when the token type has internal linkage, and
 * whether it inlines swings the measurement by a third.
 * @param tokenizer The tokenizer to run, rewound before the pass.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
[[gnu::flatten]] std::size_t tokenize(munch::tools::tokenizer::Tokenizer& tokenizer)
{
    tokenizer.reset();

    std::size_t tokens{0};

    for (;;)
    {
        const auto result{tokenizer.next<Token>()};

        if (result.end_of_input())
        {
            return tokens;
        }

        if (result.has_error())
        {
            std::printf("input rejected at offset %zu\n", tokenizer.offset());

            return 0;
        }

        ++tokens;
    }
}

/**
 * @brief Tokenizes the whole input once through Lexer::tokenize_all, the batch entry point.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize_all(const munch::core::Lexer& lexer, const std::string& input)
{
    std::size_t tokens{0};

    const auto consumed{lexer.tokenize_all<Token>(input, [&tokens](const Token, const std::size_t) { ++tokens; })};

    if (consumed != input.size())
    {
        std::printf("input rejected at offset %zu\n", consumed);

        return 0;
    }

    return tokens;
}

/**
 * @brief Finds chunk boundaries at the certified safe split points nearest the equal-division offsets.
 *
 * Each boundary is the first split point at or after its target offset, so every chunk starts at a byte the
 * automaton certifies can only begin a token and chunk-local tokenization equals whole-input tokenization.
 * @param lexer The lexer whose certification is consulted.
 * @param input The input to divide.
 * @param chunks The number of chunks requested; fewer are returned when no split point separates two targets.
 * @return The boundaries, from 0 to input.size() inclusive.
 */
std::vector<std::size_t> chunk_boundaries(
        const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    std::vector<std::size_t> boundaries{0};

    for (std::size_t index{1}; index < chunks; ++index)
    {
        auto offset{index * input.size() / chunks};

        while (offset < input.size() && !lexer.is_split_point(input[offset]))
        {
            ++offset;
        }

        if (offset > boundaries.back() && offset < input.size())
        {
            boundaries.push_back(offset);
        }
    }

    boundaries.push_back(input.size());

    return boundaries;
}

/**
 * @brief Tokenizes the input in chunks split at certified safe split points, one thread per chunk.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks to divide the input into.
 * @return The total number of tokens matched, or 0 if any chunk was rejected.
 */
std::size_t tokenize_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    const auto boundaries{chunk_boundaries(lexer, input, chunks)};

    std::vector<std::size_t> counts(boundaries.size() - 1, 0);

    std::vector<std::size_t> consumed(counts.size(), 0);

    {
        std::vector<std::jthread> workers;

        workers.reserve(counts.size());

        for (std::size_t index{0}; index < counts.size(); ++index)
        {
            workers.emplace_back([&lexer, &input, &boundaries, &counts, &consumed, index] {
                const auto begin{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index])};

                const auto end{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index + 1])};

                std::size_t tokens{0};

                consumed[index] =
                        lexer.tokenize_all<Token>(begin, end, [&tokens](const Token, const std::size_t) { ++tokens; });

                counts[index] = tokens;
            });
        }
    }

    for (std::size_t index{0}; index < counts.size(); ++index)
    {
        if (consumed[index] != boundaries[index + 1] - boundaries[index])
        {
            std::printf("chunk %zu rejected at offset %zu\n", index, boundaries[index] + consumed[index]);

            return 0;
        }
    }

    return std::accumulate(counts.cbegin(), counts.cend(), std::size_t{0});
}

/**
 * @brief Verifies that chunked tokenization is identical to the whole-input scan, not merely equal in count.
 *
 * Folds an order-sensitive checksum over the serial token stream and over the chunk-local streams in order; the
 * two agree exactly when every token kind appears in the same sequence.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @param chunks The number of chunks to divide the input into.
 * @return True if the chunked token stream matches the serial one.
 */
bool validate_chunked(const munch::core::Lexer& lexer, const std::string& input, const std::size_t chunks)
{
    const auto fold{[](std::uint64_t& checksum) {
        return [&checksum](const Token token, const std::size_t) {
            checksum = checksum * 31U + static_cast<std::uint64_t>(token);
        };
    }};

    std::uint64_t serial{0};

    if (lexer.tokenize_all<Token>(input, fold(serial)) != input.size())
    {
        return false;
    }

    const auto boundaries{chunk_boundaries(lexer, input, chunks)};

    std::uint64_t chunked{0};

    for (std::size_t index{0}; index + 1 < boundaries.size(); ++index)
    {
        const auto begin{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index])};

        const auto end{input.cbegin() + static_cast<std::ptrdiff_t>(boundaries[index + 1])};

        if (lexer.tokenize_all<Token>(begin, end, fold(chunked)) != static_cast<std::size_t>(end - begin))
        {
            return false;
        }
    }

    if (serial != chunked)
    {
        std::printf("chunked token stream diverged from the serial scan\n");

        return false;
    }

    return true;
}

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, the Lexer on input with Greek
 * identifiers matched through UTF-8 byte expansion, and chunked scans split at certified safe split points.
 * Usage: munch_benchmark [input size in MiB] [passes]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    constexpr const char* ascii_identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    constexpr const char* greek_identifiers[]{"foo", "αλφα", "counter", "βητα_1", "δελτα", "tmp_ω"};

    const auto ascii_lexer{build_lexer(false)};

    const auto greek_lexer{build_lexer(true)};

    const auto ascii_input{generate_input(mebibytes << 20U, ascii_identifiers)};

    const auto greek_input{generate_input(mebibytes << 20U, greek_identifiers)};

    munch::tools::tokenizer::Tokenizer tokenizer{ascii_lexer, ascii_input};

    auto ok{measure("lexer/ascii", ascii_input.size(), passes, [&ascii_lexer, &ascii_input] {
        return tokenize(ascii_lexer, ascii_input);
    })};

    ok = measure("lexer_all/ascii", ascii_input.size(), passes,
                 [&ascii_lexer, &ascii_input] { return tokenize_all(ascii_lexer, ascii_input); }) &&
         ok;

    ok = measure("tokenizer/ascii", ascii_input.size(), passes, [&tokenizer] { return tokenize(tokenizer); }) && ok;

    ok = measure("lexer_all/utf8", greek_input.size(), passes,
                 [&greek_lexer, &greek_input] { return tokenize_all(greek_lexer, greek_input); }) &&
         ok;

    const auto source_input{generate_source_input(mebibytes << 20U)};

    ok = validate_chunked(ascii_lexer, ascii_input, 8) && validate_chunked(ascii_lexer, source_input, 8) && ok;

    ok = measure("lexer_all/source", source_input.size(), passes,
                 [&ascii_lexer, &source_input] { return tokenize_all(ascii_lexer, source_input); }) &&
         ok;

    for (const std::size_t threads : {2, 4, 8})
    {
        char name[32];

        std::snprintf(name, sizeof(name), "chunked%zu/ascii", threads);

        ok = measure(name, ascii_input.size(), passes,
                     [&ascii_lexer, &ascii_input, threads] {
                         return tokenize_chunked(ascii_lexer, ascii_input, threads);
                     }) &&
             ok;

        std::snprintf(name, sizeof(name), "chunked%zu/source", threads);

        ok = measure(name, source_input.size(), passes,
                     [&ascii_lexer, &source_input, threads] {
                         return tokenize_chunked(ascii_lexer, source_input, threads);
                     }) &&
             ok;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

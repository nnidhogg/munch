#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <string>

#include "lexer/core/builder.hpp"
#include "lexer/regex/regex.hpp"

namespace
{
/**
 * @brief The token types recognized by the benchmarked lexer.
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
 */
lexer::core::Lexer build_lexer()
{
    using namespace lexer::regex;

    lexer::core::Builder builder;

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::identifier, 2);

    builder.add_token(plus(any_of(Set::digits())), Token::number, 2);

    // Keywords outrank identifiers, exercising priority resolution on equal-length matches.
    builder.add_token(choice(text("if"), text("else"), text("while"), text("return"), text("int")), Token::keyword, 1);

    builder.add_token(
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("*"), text("/"),
                   text("="), text("<"), text(">")),
            Token::operator_, 2);

    builder.add_token(any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);

    return builder.build();
}

/**
 * @brief Generates deterministic pseudo-code of at least the given size.
 * @param size The minimum size of the input in bytes.
 * @return The generated input.
 */
std::string generate_input(const std::size_t size)
{
    std::string input;

    input.reserve(size + 128);

    const char* identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    // A fixed-seed linear congruential generator keeps the input identical across runs and builds.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifiers[random() % std::size(identifiers)];
        input += " <= ";
        input += std::to_string(random() % 100000);
        input += ") { ";
        input += identifiers[random() % std::size(identifiers)];
        input += " = ";
        input += identifiers[random() % std::size(identifiers)];
        input += " + ";
        input += std::to_string(random() % 997);
        input += "; if (x1 != 42) { return counter; } }\n";
    }

    return input;
}

/**
 * @brief Tokenizes the whole input once.
 * @param lexer The lexer to run.
 * @param input The input to tokenize.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize(const lexer::core::Lexer& lexer, const std::string& input)
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

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the best of a number of passes, as the minimum is the least noisy estimate of the cost of the work itself.
 * Usage: lexer_benchmark [input size in MiB] [passes]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    const auto lexer{build_lexer()};

    const auto input{generate_input(mebibytes << 20U)};

    // The first pass warms caches and provides the token count the timed passes are validated against.
    const auto expected{tokenize(lexer, input)};

    if (expected == 0)
    {
        return EXIT_FAILURE;
    }

    double best_seconds{std::numeric_limits<double>::infinity()};

    for (int pass{0}; pass < passes; ++pass)
    {
        const auto start{std::chrono::steady_clock::now()};

        const auto tokens{tokenize(lexer, input)};

        const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

        if (tokens != expected)
        {
            std::printf("token count changed between passes\n");

            return EXIT_FAILURE;
        }

        best_seconds = std::min(best_seconds, elapsed.count());
    }

    const auto mib{static_cast<double>(input.size()) / (1024.0 * 1024.0)};

    std::printf("input: %.1f MiB, tokens: %zu, best of %d passes: %.1f MiB/s\n", mib, expected, passes,
                mib / best_seconds);

    return EXIT_SUCCESS;
}

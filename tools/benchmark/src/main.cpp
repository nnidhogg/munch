#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <span>
#include <string>

#include "lexer/core/builder.hpp"
#include "lexer/regex/regex.hpp"
#include "lexer/regex/utf8.hpp"
#include "lexer/tools/tokenizer/tokenizer.hpp"

namespace
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
lexer::core::Lexer build_lexer(const bool greek_identifiers)
{
    using namespace lexer::regex;

    const auto letter{[greek_identifiers](Regex ascii) {
        return greek_identifiers ? choice(std::move(ascii), utf8::range(U'Α', U'ω')) : std::move(ascii);
    }};

    lexer::core::Builder builder;

    builder.add_token(plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);

    builder.add_token(
            concat(letter(any_of(Set::alpha() + '_')), kleene(letter(any_of(Set::alphanum() + '_')))),
            Token::identifier, 2);

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
 * @param identifiers The identifier pool the code draws from.
 * @return The generated input.
 */
std::string generate_input(const std::size_t size, const std::span<const char* const> identifiers)
{
    std::string input;

    input.reserve(size + 128);

    // A fixed-seed linear congruential generator keeps the input identical across runs and builds.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifiers[random() % identifiers.size()];
        input += " <= ";
        input += std::to_string(random() % 100000);
        input += ") { ";
        input += identifiers[random() % identifiers.size()];
        input += " = ";
        input += identifiers[random() % identifiers.size()];
        input += " + ";
        input += std::to_string(random() % 997);
        input += "; if (x1 != 42) { return counter; } }\n";
    }

    return input;
}

/**
 * @brief Tokenizes the whole input once through the Lexer.
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

/**
 * @brief Tokenizes the whole input once through the Tokenizer, measuring the driver layer as well.
 * @param tokenizer The tokenizer to run, rewound before the pass.
 * @return The number of tokens matched, or 0 if the input was rejected.
 */
std::size_t tokenize(lexer::tools::tokenizer::Tokenizer& tokenizer)
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

    std::printf("%-16s %.1f MiB, %zu tokens, best of %d passes: %.1f MiB/s\n", name, mib, expected, passes,
                mib / best_seconds);

    return true;
}

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, and the Lexer on input with Greek
 * identifiers matched through UTF-8 byte expansion.
 * Usage: lexer_benchmark [input size in MiB] [passes]
 */
int main(const int argc, const char** argv)
{
    const std::size_t mebibytes{argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8};

    const int passes{argc > 2 ? std::atoi(argv[2]) : 15};

    constexpr const char* ascii_identifiers[]{"foo", "bar_baz", "counter", "x1", "value2", "tmp"};

    constexpr const char* greek_identifiers[]{"foo", "αλφα", "counter",
                                              "βητα_1", "δελτα",
                                              "tmp_ω"};

    const auto ascii_lexer{build_lexer(false)};

    const auto greek_lexer{build_lexer(true)};

    const auto ascii_input{generate_input(mebibytes << 20U, ascii_identifiers)};

    const auto greek_input{generate_input(mebibytes << 20U, greek_identifiers)};

    lexer::tools::tokenizer::Tokenizer tokenizer{ascii_lexer, ascii_input};

    auto ok{measure("lexer/ascii", ascii_input.size(), passes,
                    [&ascii_lexer, &ascii_input] { return tokenize(ascii_lexer, ascii_input); })};

    ok = measure("tokenizer/ascii", ascii_input.size(), passes, [&tokenizer] { return tokenize(tokenizer); }) && ok;

    ok = measure("lexer/utf8", greek_input.size(), passes,
                 [&greek_lexer, &greek_input] { return tokenize(greek_lexer, greek_input); }) &&
         ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

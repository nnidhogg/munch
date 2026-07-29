#include <cstdio>
#include <cstdlib>
#include <string>

#include "harness.hpp"
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

} // namespace

/**
 * @brief Measures tokenization throughput over generated pseudo-code.
 *
 * Reports the Lexer on ASCII input, the Tokenizer driver on the same input, and the Lexer on input with Greek
 * identifiers matched through UTF-8 byte expansion.
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

    ok = measure("tokenizer/ascii", ascii_input.size(), passes, [&tokenizer] { return tokenize(tokenizer); }) && ok;

    ok = measure("lexer/utf8", greek_input.size(), passes,
                 [&greek_lexer, &greek_input] { return tokenize(greek_lexer, greek_input); }) &&
         ok;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

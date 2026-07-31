#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/nfa/simulator.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"

using namespace munch;
using namespace munch::regex;

namespace
{
/**
 * @brief Deterministic linear congruential generator, keeping the tests reproducible across runs and platforms.
 */
class Random
{
public:
    explicit Random(const unsigned seed) : seed_{seed} {}

    unsigned next(const unsigned bound) { return seed_ = seed_ * 1664525U + 1013904223U, (seed_ >> 8U) % bound; }

private:
    unsigned seed_;
};

/**
 * @brief Builds a random regex over 'a' to 'c', drawing from every combinator kind up to the given depth.
 *
 * The budget bounds the total expanded length: counted repetitions multiply the length of their sub-pattern, and an
 * unbounded repetition followed by a long fixed tail needs a DFA exponential in that tail, so unbudgeted generation
 * makes subset construction explode.
 * @param budget The remaining expanded-length allowance, decremented as the regex grows.
 */
Regex random_regex(Random& random, const int depth, int& budget)
{
    const auto symbol{[&random] { return static_cast<char>('a' + random.next(3)); }};

    if (depth == 0 || budget <= 1 || random.next(3) == 0)
    {
        if (random.next(2) == 0)
        {
            std::string value{symbol()};

            if (budget > 1 && random.next(2) == 0)
            {
                value += symbol();
            }

            budget -= static_cast<int>(value.size());

            return text(value);
        }

        budget -= 1;

        return any_of(Set{symbol(), symbol()});
    }

    const auto repeated{[&random, depth, &budget](const unsigned count) {
        const auto before{budget};

        auto regex{random_regex(random, depth - 1, budget)};

        // The sub-pattern is expanded count times, so charge its size once per extra repetition.
        budget -= (before - budget) * (static_cast<int>(count) - 1);

        return regex;
    }};

    switch (random.next(8))
    {
    case 0:
        return concat(random_regex(random, depth - 1, budget), random_regex(random, depth - 1, budget));
    case 1:
        return choice(random_regex(random, depth - 1, budget), random_regex(random, depth - 1, budget));
    case 2:
        return kleene(random_regex(random, depth - 1, budget));
    case 3:
        return plus(random_regex(random, depth - 1, budget));
    case 4:
        return optional(random_regex(random, depth - 1, budget));
    case 5:
    {
        const auto count{random.next(3)};

        return exact(repeated(count), count);
    }
    case 6:
    {
        const auto min{random.next(2)};

        return at_least(repeated(min + 1), min);
    }
    default:
    {
        const auto min{random.next(2)};

        const auto max{min + random.next(2)};

        return range(repeated(max), min, max);
    }
    }
}

/**
 * @brief Generates a random input over 'a' to 'd'; 'd' appears in no pattern, exercising rejection.
 */
std::string random_input(Random& random)
{
    std::string input;

    for (auto length{random.next(13)}; length > 0; --length)
    {
        input += static_cast<char>('a' + random.next(4));
    }

    return input;
}

/**
 * @brief Generates a run over 'a' to 'c' that the lexer tokenizes completely, or an empty run if it finds none.
 *
 * The splicing guarantee is stated for input that tokenizes completely, so the input has to be built from pieces
 * known to tokenize rather than from arbitrary bytes.
 */
std::string tokenizable_run(Random& random, const core::Lexer& lexer)
{
    for (int attempt{0}; attempt < 8; ++attempt)
    {
        std::string candidate;

        for (auto length{1U + random.next(5)}; length > 0; --length)
        {
            candidate += static_cast<char>('a' + random.next(3));
        }

        if (lexer.tokenize_all<std::size_t>(candidate, [](std::size_t, std::size_t) {}) == candidate.size())
        {
            return candidate;
        }
    }

    return {};
}

/**
 * @brief Encodes a code point as UTF-8.
 */
std::string encode(const char32_t code_point)
{
    std::string bytes;

    if (code_point <= 0x7F)
    {
        bytes += static_cast<char>(code_point);
    }
    else if (code_point <= 0x7FF)
    {
        bytes += static_cast<char>(0xC0 | (code_point >> 6U));
        bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
    }
    else if (code_point <= 0xFFFF)
    {
        bytes += static_cast<char>(0xE0 | (code_point >> 12U));
        bytes += static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU));
        bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
    }
    else
    {
        bytes += static_cast<char>(0xF0 | (code_point >> 18U));
        bytes += static_cast<char>(0x80 | ((code_point >> 12U) & 0x3FU));
        bytes += static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU));
        bytes += static_cast<char>(0x80 | (code_point & 0x3FU));
    }

    return bytes;
}

} // namespace

TEST(Pipeline_property_test, Lexer_agrees_with_direct_nfa_simulation)
{
    Random random{7};

    for (int round{0}; round < 60; ++round)
    {
        const auto count{1U + random.next(4)};

        std::vector<Regex> patterns;

        core::Builder builder;

        for (std::size_t index{0}; index < count; ++index)
        {
            int budget{12};

            patterns.push_back(random_regex(random, 3, budget));

            builder.add_token(patterns[index], index + 1, index);
        }

        const auto lexer{builder.build()};

        std::vector<nfa::Nfa> nfas;

        for (std::size_t index{0}; index < count; ++index)
        {
            nfas.push_back(to_nfa(patterns[index]).set_accept_token(nfa::Token{index + 1, index}).build());
        }

        for (int pass{0}; pass < 40; ++pass)
        {
            const auto input{random_input(random)};

            // The reference result: the longest match over the per-pattern NFAs, ties won by the earliest
            // registration, which is the highest priority here.
            std::optional<std::size_t> best_id;

            std::size_t best_length{0};

            for (const auto& nfa : nfas)
            {
                const auto [token, length]{nfa::Simulator::run(nfa, input)};

                if (token && (!best_id || length > best_length))
                {
                    best_id = token->id();

                    best_length = length;
                }
            }

            const auto [token, length]{lexer.tokenize<std::size_t>(input)};

            ASSERT_EQ(token, best_id) << "round " << round << ", input " << input;
            ASSERT_EQ(length, best_length) << "round " << round << ", input " << input;
        }
    }
}

TEST(Pipeline_property_test, Certified_chunks_reproduce_the_serial_token_stream)
{
    using Emitted = std::pair<std::size_t, std::size_t>;

    Random random{0x5b1cU};

    std::size_t certified{0};
    std::size_t compared{0};
    std::size_t genuinely_split{0};

    for (int round{0}; round < 60; ++round)
    {
        core::Builder builder;

        const auto count{1U + random.next(4)};

        for (std::size_t index{0}; index < count; ++index)
        {
            int budget{12};

            builder.add_token(random_regex(random, 3, budget), index + 1, index);
        }

        // 's' occurs in no generated pattern, so the initial state alone consumes it. That gives the certificate
        // something to find and keeps the comparison below from degenerating into a second serial scan.
        builder.add_token(text("s"), count + 1, count);

        const auto lexer{builder.build()};

        // A nullable generated pattern minimizes to an accepting, self-looping initial state, which de-certifies
        // every byte including this one. There is nothing to splice then.
        if (!lexer.is_split_point('s'))
        {
            continue;
        }

        ++certified;

        std::string input;

        for (auto segments{4U + random.next(9)}; segments > 0; --segments)
        {
            input += tokenizable_run(random, lexer);

            input += 's';
        }

        std::vector<Emitted> serial;

        const auto consumed{lexer.tokenize_all<std::size_t>(
                input,
                [&serial](const std::size_t token, const std::size_t length) { serial.emplace_back(token, length); })};

        ASSERT_EQ(consumed, input.size()) << "round " << round << ", input " << input;

        for (const auto chunks : {2U, 3U, 5U, 8U})
        {
            const auto boundaries{lexer.chunk_boundaries(input, chunks)};

            // Sized before the scan and never resized, so each worker writes only its own vector.
            std::vector<std::vector<Emitted>> parts(chunks);

            const auto lengths{lexer.tokenize_all_parallel<std::size_t>(
                    input, chunks,
                    [&parts](const std::size_t chunk, const std::size_t token, const std::size_t length) {
                        parts[chunk].emplace_back(token, length);
                    })};

            ASSERT_EQ(lengths.size() + 1, boundaries.size()) << "round " << round << ", chunks " << chunks;

            std::vector<Emitted> spliced;

            for (std::size_t chunk{0}; chunk < lengths.size(); ++chunk)
            {
                // Every chunk begins at a certified byte, so every chunk must tokenize to its own end. A short count
                // would mean a cut landed inside a token even if the concatenated stream happened to still match.
                ASSERT_EQ(lengths[chunk], boundaries[chunk + 1] - boundaries[chunk])
                        << "round " << round << ", chunks " << chunks << ", chunk " << chunk << ", input " << input;

                spliced.insert(spliced.end(), parts[chunk].begin(), parts[chunk].end());
            }

            ASSERT_EQ(spliced, serial) << "round " << round << ", chunks " << chunks << ", input " << input;

            if (lengths.size() > 1)
            {
                ++genuinely_split;
            }
        }

        ++compared;
    }

    // Guards against the test going quietly vacuous. Without them it would still pass if every grammar certified
    // nothing, or if every plan collapsed to a single chunk, having compared the serial scan only against itself.
    EXPECT_GT(certified, 40u);
    EXPECT_GT(compared, 40u);
    EXPECT_GT(genuinely_split, 100u);
}

TEST(Pipeline_property_test, Utf8_range_matches_exactly_the_encodable_code_points)
{
    core::Builder builder;

    builder.add_token(utf8::range(0x0, 0x10FFFF), 1, 0);

    const auto lexer{builder.build()};

    Random random{11};

    for (int round{0}; round < 20000; ++round)
    {
        const auto code_point{static_cast<char32_t>(random.next(0x110000))};

        if (code_point >= 0xD800 && code_point <= 0xDFFF)
        {
            continue;
        }

        const auto bytes{encode(code_point)};

        const auto [token, length]{lexer.tokenize<std::size_t>(bytes)};

        ASSERT_EQ(token, std::optional<std::size_t>{1}) << "U+" << std::hex << static_cast<unsigned>(code_point);
        ASSERT_EQ(length, bytes.size()) << "U+" << std::hex << static_cast<unsigned>(code_point);
    }

    // Sequences no UTF-8 encoder produces: stray continuations, overlong encodings, and lead bytes past U+10FFFF.
    for (const std::string invalid :
         {"\x80", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80", "\xF0\x80\x80\x80", "\xF5\x80\x80\x80", "\xFF"})
    {
        EXPECT_EQ(lexer.tokenize<std::size_t>(invalid).token, std::nullopt);
    }
}

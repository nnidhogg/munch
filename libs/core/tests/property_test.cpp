#include <gtest/gtest.h>

#include <cstddef>
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
    /**
     * @brief Seeds the generator.
     * @param seed The first state.
     */
    explicit Random(const unsigned seed) : seed_{seed} {}

    /**
     * @brief Advances the generator and draws a value below a bound.
     * @param bound One past the largest value drawn.
     * @return The drawn value.
     */
    unsigned next(const unsigned bound)
    {
        seed_ = seed_ * multiplier + increment;

        return (seed_ >> 8U) % bound;
    }

private:
    /**
     * @brief The generator's multiplier.
     */
    static constexpr unsigned multiplier{1664525U};

    /**
     * @brief The generator's increment.
     */
    static constexpr unsigned increment{1013904223U};

    /**
     * @brief The generator's state.
     */
    unsigned seed_;
};

/**
 * @brief The largest Unicode scalar value.
 */
constexpr char32_t max_code_point{0x10FFFF};

/**
 * @brief The number of Unicode code points, surrogates included.
 */
constexpr unsigned code_point_count{max_code_point + 1};

/**
 * @brief The first surrogate code point, which UTF-8 cannot carry.
 */
constexpr char32_t first_surrogate{0xD800};

/**
 * @brief The last surrogate code point.
 */
constexpr char32_t last_surrogate{0xDFFF};

/**
 * @brief The node budget every random regex is drawn under.
 */
constexpr int regex_budget{12};

/**
 * @brief The nesting depth every random regex is drawn to.
 */
constexpr int regex_depth{3};

/**
 * @brief The random grammars the lexer pipeline tests draw.
 */
constexpr std::size_t rounds{60};

/**
 * @brief The random inputs each of those grammars is run on against direct NFA simulation.
 */
constexpr std::size_t passes{40};

/**
 * @brief The runs tokenizable_run() draws before it gives up.
 */
constexpr std::size_t attempts{8};

/**
 * @brief The random code points the UTF-8 range test draws.
 */
constexpr std::size_t code_point_rounds{20000};

/**
 * @brief Ignores every token of a scan.
 * @tparam T The token type.
 */
constexpr auto ignore_token{[]<typename T>(const T, const std::size_t) {}};

/**
 * @brief Builds a random regex over 'a' to 'c', drawing from every combinator kind up to the given depth.
 *
 * The budget bounds the total expanded length: counted repetitions multiply the length of their sub-pattern, and an
 * unbounded repetition followed by a long fixed tail needs a DFA exponential in that tail, so unbudgeted generation
 * makes subset construction explode.
 * @param random The source of every draw.
 * @param depth The nesting still allowed: at zero a leaf is drawn, a text or a set.
 * @param budget The remaining expanded-length allowance, decremented as the regex grows.
 * @return The drawn regex.
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

    // The right operand is drawn first, which fixes the patterns a seed yields.
    const auto operands{[&random, depth, &budget] {
        auto right{random_regex(random, depth - 1, budget)};

        auto left{random_regex(random, depth - 1, budget)};

        return std::pair{std::move(left), std::move(right)};
    }};

    switch (random.next(8))
    {
    case 0:
    {
        auto [left, right]{operands()};

        return concat(std::move(left), std::move(right));
    }
    case 1:
    {
        auto [left, right]{operands()};

        return choice(std::move(left), std::move(right));
    }
    case 2:
    {
        auto operand{random_regex(random, depth - 1, budget)};

        return kleene(std::move(operand));
    }
    case 3:
    {
        auto operand{random_regex(random, depth - 1, budget)};

        return plus(std::move(operand));
    }
    case 4:
    {
        auto operand{random_regex(random, depth - 1, budget)};

        return optional(std::move(operand));
    }
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
 * @param random The source of every draw.
 * @return The input, up to twelve bytes.
 */
std::string random_input(Random& random)
{
    std::string input{};

    for (auto length{random.next(13)}; length > 0; --length)
    {
        input += static_cast<char>('a' + random.next(4));
    }

    return input;
}

/**
 * @brief Generates a run over 'a' to 'c' that the lexer tokenizes completely, or an empty run if it finds none.
 *
 * The splicing guarantee is stated for input that tokenizes completely, so the input has to be built from pieces known
 * to tokenize rather than from arbitrary bytes.
 * @param random The source of every draw.
 * @param lexer The lexer the run must tokenize completely under.
 * @return The run, or an empty string when every attempt found none.
 */
std::string tokenizable_run(Random& random, const core::Lexer& lexer)
{
    for (std::size_t attempt{0}; attempt < attempts; ++attempt)
    {
        std::string candidate{};

        for (auto length{1U + random.next(5)}; length > 0; --length)
        {
            candidate += static_cast<char>('a' + random.next(3));
        }

        const auto consumed{lexer.tokenize_all<std::size_t>(candidate, ignore_token)};

        if (consumed == candidate.size())
        {
            return candidate;
        }
    }

    return {};
}

} // namespace

TEST(Pipeline_property_test, Lexer_agrees_with_direct_nfa_simulation)
{
    Random random{7};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        const auto count{1U + random.next(4)};

        std::vector<Regex> patterns{};

        core::Builder builder{};

        for (std::size_t index{0}; index < count; ++index)
        {
            int budget{regex_budget};

            patterns.push_back(random_regex(random, regex_depth, budget));

            builder.add_token(patterns[index], index + 1, index);
        }

        const auto lexer{builder.build()};

        std::vector<nfa::Nfa> nfas{};

        for (std::size_t index{0}; index < count; ++index)
        {
            const nfa::Token token{index + 1, index};

            auto automaton{to_nfa(patterns[index])};

            automaton.set_accept_token(token);

            nfas.push_back(automaton.build());
        }

        // Longest match over the per-pattern NFAs, ties won by the earliest registration, the highest priority here.
        const auto reference{[&nfas](const std::string& input) {
            std::optional<std::size_t> best_id{};

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

            return std::pair{best_id, best_length};
        }};

        for (std::size_t pass{0}; pass < passes; ++pass)
        {
            const auto input{random_input(random)};

            const auto [best_id, best_length]{reference(input)};

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

    for (std::size_t round{0}; round < rounds; ++round)
    {
        core::Builder builder{};

        const auto count{1U + random.next(4)};

        for (std::size_t index{0}; index < count; ++index)
        {
            int budget{regex_budget};

            builder.add_token(random_regex(random, regex_depth, budget), index + 1, index);
        }

        // 's' occurs in no generated pattern, so the initial state alone consumes it. That gives the certificate
        // something to find and keeps the comparison below from degenerating into a second serial scan.
        builder.add_token(text("s"), count + 1, count);

        const auto lexer{builder.build()};

        // A nullable generated pattern is compiled behind a fresh start state, and the old start, re-entered by the
        // pattern's loop, is then an ordinary live state consuming 's' mid-token, which de-certifies it. There is
        // nothing to splice then.
        if (!lexer.is_split_point('s'))
        {
            continue;
        }

        ++certified;

        std::string input{};

        for (auto segments{4U + random.next(9)}; segments > 0; --segments)
        {
            input += tokenizable_run(random, lexer);

            input += 's';
        }

        std::vector<Emitted> serial{};

        const auto collect{
                [&serial](const std::size_t token, const std::size_t length) { serial.emplace_back(token, length); }};

        const auto consumed{lexer.tokenize_all<std::size_t>(input, collect)};

        ASSERT_EQ(consumed, input.size()) << "round " << round << ", input " << input;

        for (const auto chunks : {2U, 3U, 5U, 8U})
        {
            const auto boundaries{lexer.chunk_boundaries(input, chunks)};

            // Sized before the scan and never resized, so each worker writes only its own vector.
            std::vector<std::vector<Emitted>> parts(chunks);

            const auto collect_chunk{
                    [&parts](const std::size_t chunk, const std::size_t token, const std::size_t length) {
                        parts[chunk].emplace_back(token, length);
                    }};

            const auto lengths{lexer.tokenize_all_parallel<std::size_t>(input, chunks, collect_chunk)};

            ASSERT_EQ(lengths.size() + 1, boundaries.size()) << "round " << round << ", chunks " << chunks;

            std::vector<Emitted> spliced{};

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

    // More than forty grammars certified 's' and were compared, and more than a hundred plans cut their input into
    // several chunks.
    EXPECT_GT(certified, 40U);
    EXPECT_GT(compared, 40U);
    EXPECT_GT(genuinely_split, 100U);
}

TEST(Pipeline_property_test, Utf8_range_matches_exactly_the_encodable_code_points)
{
    core::Builder builder{};

    builder.add_token(utf8::range(0x0, max_code_point), 1, 0);

    const auto lexer{builder.build()};

    Random random{11};

    for (std::size_t round{0}; round < code_point_rounds; ++round)
    {
        const auto code_point{static_cast<char32_t>(random.next(code_point_count))};

        if (code_point >= first_surrogate && code_point <= last_surrogate)
        {
            continue;
        }

        const auto bytes{utf8::encode(code_point)};

        const auto [token, length]{lexer.tokenize<std::size_t>(bytes)};

        ASSERT_EQ(token, std::optional<std::size_t>{1}) << "U+" << std::hex << static_cast<unsigned>(code_point);
        ASSERT_EQ(length, bytes.size()) << "U+" << std::hex << static_cast<unsigned>(code_point);
    }

    // Sequences no UTF-8 encoder produces: stray continuations, overlong encodings, and lead bytes past U+10FFFF.
    for (const std::string invalid :
         {"\x80", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80", "\xF0\x80\x80\x80", "\xF5\x80\x80\x80", "\xFF"})
    {
        const auto [token, length]{lexer.tokenize<std::size_t>(invalid)};

        EXPECT_EQ(token, std::nullopt);
    }
}

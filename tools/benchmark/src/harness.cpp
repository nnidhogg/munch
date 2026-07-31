#include "munch/tools/benchmark/harness.hpp"

#include <fstream>
#include <numeric>
#include <random>

#include "munch/core/builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/utf8.hpp"

namespace munch::tools::benchmark
{
core::Lexer build_lexer(const bool greek_identifiers)
{
    using namespace munch::regex;

    const auto letter{[greek_identifiers](Regex ascii) {
        return greek_identifiers ? choice(std::move(ascii), utf8::range(U'Α', U'ω')) : std::move(ascii);
    }};

    core::Builder builder;

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

std::string generate_source_input(const std::size_t size)
{
    constexpr const char* identifiers[]{
            "configuration_manager",
            "total_element_count",
            "process_next_request",
            "buffer_capacity",
            "initialize_state_machine",
            "compute_partial_checksum",
            "validation_result",
            "iterator_position",
            "acc",
            "idx"};

    std::string input;

    input.reserve(size + 256);

    // The same fixed-seed generator as generate_input(), so ports stay byte-identical.
    unsigned seed{12345};

    const auto random{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    const auto identifier{[&] { return identifiers[random() % std::size(identifiers)]; }};

    while (input.size() < size)
    {
        input += "while (";
        input += identifier();
        input += " <= ";
        input += std::to_string(random() % 10000000);
        input += ") {\n    ";
        input += identifier();
        input += " = ";
        input += identifier();
        input += " * ";
        input += identifier();
        input += " + ";
        input += std::to_string(random() % 100000);
        input += ";\n    if (";
        input += identifier();
        input += " != ";
        input += std::to_string(random() % 997);
        input += ") { return ";
        input += identifier();
        input += "; }\n}\n";
    }

    return input;
}

namespace
{
/**
 * @brief One timed pass of one scenario.
 */
struct Observation
{
    std::size_t scenario;
    int round;
    double seconds;
};
} // namespace

bool measure_interleaved(
        const std::span<const Scenario> scenarios, const int passes, const std::size_t input_mebibytes,
        const char* const observations_path)
{
    std::vector<std::size_t> expected;

    expected.reserve(scenarios.size());

    // The warmup pass also fixes the result every timed pass must reproduce, so a scenario that drifts in content
    // rather than in timing still fails loudly.
    for (const auto& scenario : scenarios)
    {
        expected.push_back(scenario.pass());

        if (expected.back() == 0)
        {
            return false;
        }
    }

    std::vector<std::vector<double>> seconds(scenarios.size());

    std::vector<std::size_t> order(scenarios.size());

    std::iota(order.begin(), order.end(), std::size_t{0});

    // Seeded rather than random: the order must vary between rounds to spread drift, and repeat exactly between
    // runs so a measurement can be reproduced.
    std::mt19937 sequence{0x5eedU};

    std::vector<Observation> observations;

    observations.reserve(scenarios.size() * static_cast<std::size_t>(passes));

    for (int round{0}; round < passes; ++round)
    {
        std::shuffle(order.begin(), order.end(), sequence);

        for (const auto index : order)
        {
            const auto start{std::chrono::steady_clock::now()};

            const auto result{scenarios[index].pass()};

            const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start};

            if (result != expected[index])
            {
                std::printf("%s: the result changed between passes\n", scenarios[index].name);

                return false;
            }

            seconds[index].push_back(elapsed.count());

            observations.push_back(Observation{.scenario = index, .round = round, .seconds = elapsed.count()});
        }
    }

    for (std::size_t index{0}; index < scenarios.size(); ++index)
    {
        auto samples{seconds[index]};

        std::sort(samples.begin(), samples.end());

        const auto mib{static_cast<double>(scenarios[index].bytes) / (1024.0 * 1024.0)};

        const auto median{(samples[(samples.size() - 1) / 2] + samples[samples.size() / 2]) / 2.0};

        std::printf(
                "%-16s %.1f MiB, %zu tokens, %d passes: best %.1f, median %.1f, worst %.1f MiB/s\n",
                scenarios[index].name, mib, expected[index], passes, mib / samples.front(), mib / median,
                mib / samples.back());
    }

    if (observations_path == nullptr)
    {
        return true;
    }

    // The CSV is appended to, and rounds restart at zero in every call, so without this the rows of separate runs
    // are separable only by position. One identifier per process keeps them apart.
    static const auto run{std::chrono::system_clock::now().time_since_epoch() / std::chrono::seconds{1}};

    const bool fresh{!std::ifstream{observations_path}.good()};

    std::ofstream csv{observations_path, std::ios::app};

    if (!csv)
    {
        std::printf("unable to write observations to %s\n", observations_path);

        return false;
    }

    if (fresh)
    {
        csv << "run,scenario,input_mib,round,seconds,mib_per_s\n";
    }

    for (const auto& observation : observations)
    {
        const auto mib{static_cast<double>(scenarios[observation.scenario].bytes) / (1024.0 * 1024.0)};

        csv << run << ',' << scenarios[observation.scenario].name << ',' << input_mebibytes << ',' << observation.round
            << ',' << observation.seconds << ',' << mib / observation.seconds << '\n';
    }

    return csv.flush().good();
}

} // namespace munch::tools::benchmark

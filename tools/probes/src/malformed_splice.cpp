// Pins the malformed-input caveat of the window planner: full per-chunk consumption does not imply the serial scan
// succeeds, so window plans require the completely-tokenizable precondition or downstream validation. The recovery
// report's motivation quotes a measurement of this hazard, and this probe is the program behind it.
//
// The hazard. chunk_boundaries_with_windows() documents that on malformed input a window cut can land inside a token of
// the serial scan's doomed suffix, and the concatenated chunk streams then contain tokens the serial scan never
// reaches. The undamaged fragments consume fully and silently; only chunks holding a locally unconsumable byte report
// short consumption, so a caller checking the per-chunk counts is flagged, and one accepting later-chunk output without
// checking swallows the overproduced stream. Continuation past a failure needs an explicit restart contract, which is
// what certified recovery supplies; this probe measures what ignoring the flags costs.
//
// What runs as a test. A deterministic generated corpus is broken by one unconsumable byte near its front, so the
// serial scan stops there. The window plan still recovers all eight chunks; the chunk holding the damage reports short
// consumption, which a caller checking per-chunk counts would catch, while the other seven consume fully and silently,
// and the spliced token count dwarfs the serial one. The probe asserts exactly that shape with both counts pinned; the
// caveat is thereby a checked behavior rather than a documentation sentence.
//
// Campaign mode. With a directory argument the probe concatenates the given extension's files in sorted order, applies
// the consumption-complete C row (deliberately mismatched to languages whose strings span lines, which is what makes
// real corpora malformed under it), and reports serial consumption, the plan, per-chunk consumption, and the
// spliced-versus-serial token counts, the ratio undefined where the serial scan commits no token; a directory that
// cannot be walked or holds no file of the extension is refused with exit status one. Figures from campaign runs are
// archived with their corpus pin; the collection this backs is paper/data/malformed-splice-2026-08.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/chunks.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/generated_identifiers.hpp"
#include "munch/tools/probes/lcg64.hpp"
#include "munch/tools/probes/study_rows.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Assertions;
using munch::tools::probes::chunks_of;
using munch::tools::probes::consumption_complete_c_row;
using munch::tools::probes::files_under;
using munch::tools::probes::Lcg64;
using munch::tools::probes::pick_identifier;
using munch::tools::probes::read_bytes;

/**
 * @brief The chunks the window plan of a spliced input is asked for.
 */
constexpr std::size_t planned_chunks{8};

/**
 * @brief What one input does under the serial scan and under the window plan's eight chunks scanned apart.
 */
struct Splice
{
    /**
     * @brief The bytes the serial scan consumes before it stops.
     */
    std::size_t serial_consumed{0};

    /**
     * @brief The tokens the serial scan commits.
     */
    std::size_t serial_tokens{0};

    /**
     * @brief The chunks the window plan cuts.
     */
    std::size_t chunks{0};

    /**
     * @brief The chunks whose own scan stops short of their end.
     */
    std::size_t incomplete_chunks{0};

    /**
     * @brief The tokens the chunks' scans commit together.
     */
    std::size_t spliced_tokens{0};
};

/**
 * @brief Scans an input serially and as the eight chunks of its window plan, each chunk scanned on its own.
 * @param lexer The lexer both scans run.
 * @param input The input.
 * @return The serial consumption and token count, the chunk count, the chunks scanned short, and the spliced token
 *         count.
 */
Splice splice(const munch::core::Lexer& lexer, const std::string_view input)
{
    std::size_t serial_tokens{0};

    std::size_t spliced_tokens{0};

    std::size_t incomplete_chunks{0};

    const auto count_serial{[&serial_tokens](const Token, const std::size_t) { ++serial_tokens; }};

    const auto count_spliced{[&spliced_tokens](const Token, const std::size_t) { ++spliced_tokens; }};

    const auto serial_consumed{lexer.tokenize_all<Token>(input, count_serial)};

    const auto bounds{lexer.chunk_boundaries_with_windows(input, planned_chunks)};

    for (const auto chunk : chunks_of(input, bounds))
    {
        const auto consumed{lexer.tokenize_all<Token>(chunk, count_spliced)};

        if (consumed != chunk.size())
        {
            ++incomplete_chunks;
        }
    }

    const auto chunks{bounds.size() - 1};

    return {.serial_consumed = serial_consumed,
            .serial_tokens = serial_tokens,
            .chunks = chunks,
            .incomplete_chunks = incomplete_chunks,
            .spliced_tokens = spliced_tokens};
}

/**
 * @brief Generates C-like text of six statement shapes drawn from an Lcg64 at a fixed seed, deterministic from that
 *        seed.
 * @param bytes The least size of the text; generation stops at the first statement reaching it.
 * @return The text.
 */
std::string generated_c(const std::size_t bytes)
{
    constexpr std::uint64_t seed{0x9E3779B97F4A7C15ULL};

    Lcg64 lcg{seed};

    std::string out{};

    while (out.size() < bytes)
    {
        constexpr std::size_t statement_shapes{6};

        const auto shape{lcg.next(statement_shapes)};

        switch (shape)
        {
        case 0:
            out += "/* invariant: ";
            out += pick_identifier(lcg);
            out += " stays in range */\n";

            break;

        case 1:
            out += "#define LIMIT_";
            out += pick_identifier(lcg);
            out += " 4096\n";

            break;

        case 2:
            out += "    ";
            out += pick_identifier(lcg);
            out += " = ";
            out += pick_identifier(lcg);
            out += " + 17;\n";

            break;

        case 3:
            out += R"(static const char* name = "a \"quoted\" piece";)"
                   "\n";

            break;

        case 4:
            out += "int ";
            out += pick_identifier(lcg);
            out += "[128]; // sized by the table\n";

            break;

        case 5:
            out += "}\n";

            break;
        }
    }

    return out;
}

/**
 * @brief Returns the campaign's line for a spliced stream: the files and bytes, the serial consumption as a share of
 * the bytes, the plan, and the spliced token count with its ratio to the serial one, which a serial scan committing no
 * token leaves undefined.
 * @param files The files concatenated.
 * @param bytes The stream's bytes, at least one.
 * @param spliced The stream's splice.
 * @return The line, its newline included.
 */
std::string summary_line(const std::size_t files, const std::size_t bytes, const Splice& spliced)
{
    const auto& [serial_consumed, serial_tokens, chunks, incomplete_chunks, spliced_tokens]{spliced};

    const auto consumed_percent{100.0 * static_cast<double>(serial_consumed) / static_cast<double>(bytes)};

    const auto ratio{static_cast<double>(spliced_tokens) / static_cast<double>(serial_tokens)};

    const auto ratio_text{serial_tokens == 0 ? std::string{"undefined"} : std::format("{:.2f}", ratio)};

    return std::format(
            "{} files, {} bytes; serial consumed {} ({:.1f}%), {} tokens; {} chunks, {} incomplete; spliced {} tokens, "
            "ratio {}\n",
            files, bytes, serial_consumed, consumed_percent, serial_tokens, chunks, incomplete_chunks, spliced_tokens,
            ratio_text);
}

/**
 * @brief Runs the pinned self-test: one unconsumable byte near the front of the generated corpus stops the serial scan,
 *        while the spliced chunks do not notice, and prints the figures and the verdict.
 *
 * A corpus holding no arithmetic statement to damage fails the test before either scan runs.
 * @param lexer The consumption-complete C row's lexer.
 * @return EXIT_SUCCESS when every assertion holds, EXIT_FAILURE otherwise.
 */
int self_test(const munch::core::Lexer& lexer)
{
    Assertions assertions{};

    constexpr std::size_t corpus_bytes{256 * 1024};

    auto corpus{generated_c(corpus_bytes)};

    constexpr std::string_view arithmetic_tail{"+ 17;"};

    constexpr std::size_t target_fraction{50};

    // The damaged byte must sit at top level, not inside a comment or string interior, which absorb control bytes; the
    // first arithmetic statement past the target offset is provably top level.
    const auto site{corpus.find(arithmetic_tail, corpus.size() / target_fraction)};

    assertions.expect(site != std::string::npos, "the generated corpus lost its arithmetic statements");

    if (site == std::string::npos)
    {
        std::cout << "assertion failures\n";

        return EXIT_FAILURE;
    }

    constexpr auto damaged_offset{arithmetic_tail.find('1')};

    corpus[site + damaged_offset] = '\x01';

    const auto [serial_consumed, serial_tokens, chunks, incomplete_chunks, spliced_tokens]{splice(lexer, corpus)};

    std::cout << std::format(
            "serial {}/{} ({} tokens), chunks {}, spliced {} tokens\n", serial_consumed, corpus.size(), serial_tokens,
            chunks, spliced_tokens);

    constexpr std::size_t serial_stop_fraction{40};

    assertions.expect(
            serial_consumed < corpus.size() / serial_stop_fraction, "the damage did not stop the serial scan early");

    assertions.expect(chunks == planned_chunks, "the malformed stream did not plan eight chunks");

    assertions.expect(incomplete_chunks == 1, "only the damaged chunk may report short consumption");

    constexpr std::size_t overproduction{20};

    assertions.expect(spliced_tokens > overproduction * serial_tokens, "splicing did not overproduce");

    assertions.expect(serial_tokens == 1487, "the pinned serial token count moved");

    assertions.expect(spliced_tokens == 62'309, "the pinned spliced token count moved");

    // A stream whose first byte no token takes commits no serial token, so the campaign's ratio is undefined.
    const auto unconsumable{summary_line(1, 2, splice(lexer, "\x01\n"))};

    assertions.expect(unconsumable.ends_with("ratio undefined\n"), "a serial scan of no token gave a ratio");

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? EXIT_FAILURE : EXIT_SUCCESS;
}

/**
 * @brief Splices the concatenation of a directory's files of one extension, in sorted order and each followed by a
 *        newline, an unreadable file read as empty, and prints the serial and spliced figures on one line; a directory
 *        holding no such file is refused with `no <extension> files under <directory>` on standard error.
 * @param lexer The lexer both scans run.
 * @param root The directory walked.
 * @param extension The extension, dot included, a file must have.
 * @return Whether the directory held a file to splice.
 */
bool campaign(const munch::core::Lexer& lexer, const std::filesystem::path& root, const std::string_view extension)
{
    const auto files{files_under(root, extension)};

    if (files.empty())
    {
        std::fprintf(stderr, "no %s files under %s\n", std::string{extension}.c_str(), root.c_str());

        return false;
    }

    std::string stream{};

    for (const auto& path : files)
    {
        const auto text{read_bytes(path).value_or("")};

        stream.append(text);

        stream += '\n';
    }

    std::cout << summary_line(files.size(), stream.size(), splice(lexer, stream));

    return true;
}

} // namespace

/**
 * @brief Builds the consumption-complete C row and splices a directory's files under it when one is given, of the
 *        extension the second argument names or `.rs`, and otherwise runs the pinned self-test.
 * @param argc The argument count.
 * @param argv The directory and the extension, both optional.
 * @return EXIT_SUCCESS after a campaign or a self-test whose assertions hold, EXIT_FAILURE when a self-test assertion
 *         fails, the directory cannot be walked, which `cannot walk the corpus: <reason>` on standard error names, or
 *         it holds no file of the extension.
 */
int main(const int argc, char** argv)
{
    munch::core::Builder builder{};

    consumption_complete_c_row(builder);

    const auto lexer{builder.build()};

    if (argc > 1)
    {
        const std::string_view extension{argc > 2 ? argv[2] : ".rs"};

        try
        {
            return campaign(lexer, argv[1], extension) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        catch (const std::filesystem::filesystem_error& error)
        {
            std::fprintf(stderr, "cannot walk the corpus: %s\n", error.what());

            return EXIT_FAILURE;
        }
    }

    return self_test(lexer);
}

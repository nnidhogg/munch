// Pins the malformed-input caveat of the window planner: full per-chunk consumption does not imply the serial scan
// succeeds, so window plans require the completely-tokenizable precondition or downstream validation. The recovery
// report's motivation quotes a measurement of this hazard, and this probe is the program behind it.
//
// The hazard. chunk_boundaries_with_windows() documents that on malformed input a window cut can land inside
// a token of the serial scan's doomed suffix, and the concatenated chunk streams then contain tokens the
// serial scan never reaches. The undamaged fragments consume fully and silently; only chunks holding a
// locally unconsumable byte report short consumption, so a caller checking the per-chunk counts is flagged,
// and one accepting later-chunk output without checking swallows the overproduced stream. Continuation past
// a failure needs an explicit restart contract, which is what certified recovery supplies; this probe
// measures what ignoring the flags costs.
//
// What runs as a test. A deterministic generated corpus is broken by one unconsumable byte near its front, so the
// serial scan stops there. The window plan still recovers all eight chunks; the chunk holding the damage reports short
// consumption, which a caller checking per-chunk counts would catch, while the other seven consume fully and silently,
// and the spliced token count dwarfs the serial one. The probe asserts exactly that shape with both counts pinned; the
// caveat is thereby a checked behavior rather than a documentation sentence.
//
// Campaign mode. With a directory argument the probe concatenates the given extension's files in sorted
// order, applies the consumption-complete C row (deliberately mismatched to languages whose strings span
// lines, which is what makes real corpora malformed under it), and reports serial consumption, the plan,
// per-chunk consumption, and the spliced-versus-serial token counts. Figures from campaign runs are archived
// with their corpus pin; the collection this backs is paper/data/malformed-splice-2026-08.

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/lcg64.hpp"
#include "munch/tools/probes/study_rows.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Assertions;
using munch::tools::probes::consumption_complete_c_row;
using munch::tools::probes::files_under;
using munch::tools::probes::Lcg64;
using munch::tools::probes::read_bytes;

/**
 * @brief What one input does under the serial scan and under the window plan's eight chunks scanned apart.
 */
struct Splice
{
    /**
     * @brief The bytes the serial scan consumes before it stops.
     */
    std::size_t serial_consumed{};

    /**
     * @brief The tokens the serial scan commits.
     */
    std::size_t serial_tokens{};

    /**
     * @brief The chunks the window plan cuts.
     */
    std::size_t chunks{};

    /**
     * @brief The chunks whose own scan stops short of their end.
     */
    std::size_t incomplete_chunks{};

    /**
     * @brief The tokens the chunks' scans commit together.
     */
    std::size_t spliced_tokens{};
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
    Splice result{};

    result.serial_consumed =
            lexer.tokenize_all<Token>(input, [&result](Token, std::size_t) { ++result.serial_tokens; });

    const auto bounds{lexer.chunk_boundaries_with_windows(input, 8)};

    result.chunks = bounds.size() - 1;

    for (std::size_t index{1}; index < bounds.size(); ++index)
    {
        const std::string_view chunk{input.data() + bounds[index - 1], bounds[index] - bounds[index - 1]};

        const auto consumed{
                lexer.tokenize_all<Token>(chunk, [&result](Token, std::size_t) { ++result.spliced_tokens; })};

        if (consumed != chunk.size())
        {
            ++result.incomplete_chunks;
        }
    }

    return result;
}

/**
 * @brief Generates C-like text of six statement shapes drawn from an Lcg64 at a fixed seed, deterministic from that
 *        seed.
 * @param bytes The least size of the text; generation stops at the first statement reaching it.
 * @return The text.
 */
std::string generated_c(const std::size_t bytes)
{
    Lcg64 lcg{0x9E3779B97F4A7C15ULL};

    static constexpr std::array<std::string_view, 8> idents{"count", "buffer", "index", "state",
                                                            "value", "table",  "next",  "size"};

    std::string out{};

    while (out.size() < bytes)
    {
        switch (lcg.next(6))
        {
        case 0:
            out += "/* invariant: ";
            out += idents[lcg.next(8)];
            out += " stays in range */\n";
            break;
        case 1:
            out += "#define LIMIT_";
            out += idents[lcg.next(8)];
            out += " 4096\n";
            break;
        case 2:
            out += "    ";
            out += idents[lcg.next(8)];
            out += " = ";
            out += idents[lcg.next(8)];
            out += " + 17;\n";
            break;
        case 3:
            out += "static const char* name = \"a \\\"quoted\\\" piece\";\n";
            break;
        case 4:
            out += "int ";
            out += idents[lcg.next(8)];
            out += "[128]; // sized by the table\n";
            break;
        default:
            out += "}\n";
            break;
        }
    }

    return out;
}

/**
 * @brief Runs the pinned self-test: one unconsumable byte near the front of the generated corpus stops the serial
 *        scan, while the spliced chunks do not notice, and prints the figures and the verdict.
 * @param lexer The consumption-complete C row's lexer.
 * @return 0 when every assertion holds, 1 otherwise.
 */
int self_test(const munch::core::Lexer& lexer)
{
    Assertions assertions{};

    auto corpus{generated_c(256 * 1024)};

    // The damaged byte must sit at top level, not inside a comment or string interior, which absorb control
    // bytes; the first arithmetic statement past the target offset is provably top level.
    const auto site{corpus.find("+ 17;", corpus.size() / 50)};

    assertions.expect(site != std::string::npos, "the generated corpus lost its arithmetic statements");

    corpus[site + 2] = '\x01';

    const auto result{splice(lexer, corpus)};

    std::cout << "serial " << result.serial_consumed << "/" << corpus.size() << " (" << result.serial_tokens
              << " tokens), chunks " << result.chunks << ", spliced " << result.spliced_tokens << " tokens\n";

    assertions.expect(result.serial_consumed < corpus.size() / 40, "the damage did not stop the serial scan early");
    assertions.expect(result.chunks == 8, "the malformed stream did not plan eight chunks");
    assertions.expect(result.incomplete_chunks == 1, "only the damaged chunk may report short consumption");
    assertions.expect(result.spliced_tokens > 20 * result.serial_tokens, "splicing did not overproduce");
    assertions.expect(result.serial_tokens == 1487, "the pinned serial token count moved");
    assertions.expect(result.spliced_tokens == 62309, "the pinned spliced token count moved");

    std::cout << (assertions.has_failures() ? "ASSERTION FAILURES\n" : "all assertions hold\n");

    return assertions.has_failures() ? 1 : 0;
}

/**
 * @brief Splices the concatenation of a directory's files of one extension, in sorted order and each followed by a
 *        newline, an unreadable file read as empty, and prints the serial and spliced figures on one line.
 * @param lexer The lexer both scans run.
 * @param root The directory walked.
 * @param extension The extension, dot included, a file must have.
 */
void campaign(const munch::core::Lexer& lexer, const std::filesystem::path& root, const std::string_view extension)
{
    const auto files{files_under(root, extension)};

    std::string stream{};

    for (const auto& path : files)
    {
        stream.append(read_bytes(path).value_or(""));

        stream += '\n';
    }

    const auto result{splice(lexer, stream)};

    std::printf(
            "%zu files, %zu bytes; serial consumed %zu (%.1f%%), %zu tokens; %zu chunks, %zu "
            "incomplete; spliced %zu tokens, ratio %.2f\n",
            files.size(), stream.size(), result.serial_consumed,
            100.0 * static_cast<double>(result.serial_consumed) / static_cast<double>(stream.size()),
            result.serial_tokens, result.chunks, result.incomplete_chunks, result.spliced_tokens,
            static_cast<double>(result.spliced_tokens) / static_cast<double>(result.serial_tokens));
}
} // namespace

/**
 * @brief Builds the consumption-complete C row and splices a directory's files under it when one is given, of the
 *        extension the second argument names or `.rs`, and otherwise runs the pinned self-test.
 * @param argc The argument count.
 * @param argv The directory and the extension, both optional.
 * @return 0 after a campaign or a self-test whose assertions hold, 1 when a self-test assertion fails.
 */
int main(const int argc, const char** argv)
{
    munch::core::Builder builder{};

    consumption_complete_c_row(builder);

    const auto lexer{builder.build()};

    if (argc > 1)
    {
        campaign(lexer, argv[1], argc > 2 ? argv[2] : ".rs");

        return 0;
    }

    return self_test(lexer);
}

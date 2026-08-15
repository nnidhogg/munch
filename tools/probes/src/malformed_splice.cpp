// Pins the malformed-input caveat of the window planner: full per-chunk consumption does not imply the
// serial scan succeeds, so window plans require the completely-tokenizable precondition or downstream
// validation. The recovery report's motivation quotes a measurement of this hazard, and this probe is the
// program behind it.
//
// The hazard. chunk_boundaries_with_windows() documents that on malformed input a window cut can land inside
// a token of the serial scan's doomed suffix, and the concatenated chunk streams then contain tokens the
// serial scan never reaches. The undamaged fragments consume fully and silently; only chunks holding a
// locally unconsumable byte report short consumption, so a caller checking the per-chunk counts is flagged,
// and one accepting later-chunk output without checking swallows the overproduced stream. Continuation past
// a failure needs an explicit restart contract, which is what certified recovery supplies; this probe
// measures what ignoring the flags costs.
//
// What runs as A test. A deterministic generated corpus is broken by one unconsumable byte near its front,
// so the serial scan stops there. The window plan still recovers all eight chunks; the chunk holding the
// damage reports short consumption, which a caller checking per-chunk counts would catch, while the other
// seven consume fully and silently, and the spliced token count dwarfs the serial one. The probe asserts
// exactly that shape with both counts pinned; the caveat is thereby a checked behavior rather than a
// documentation sentence.
//
// Campaign mode. With a directory argument the probe concatenates the given extension's files in sorted
// order, applies the consumption-complete C row (deliberately mismatched to languages whose strings span
// lines, which is what makes real corpora malformed under it), and reports serial consumption, the plan,
// per-chunk consumption, and the spliced-versus-serial token counts. Figures from campaign runs are archived
// with their corpus pin; the collection this backs is paper/data/malformed-splice-2026-08.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"

namespace
{
using figures::Token;
using namespace munch::regex;

std::size_t failures{0};

void expect(const bool condition, const char* what)
{
    if (!condition)
    {
        ++failures;

        std::cout << "FAIL: " << what << "\n";
    }
}

// The consumption-complete C row of the corpus instrument, verbatim.
munch::core::Lexer consumption_complete_c()
{
    munch::core::Builder builder;

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set::digits())), Token::Number, 2);
    builder.add_token(any_of(figures::operators()), Token::Operator, 2);
    builder.add_token(any_of(figures::punctuation() + '#' + '\\' + '@' + '`' + '$' + '\''), Token::Punctuation, 2);
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::Whitespace, 2);

    const auto escape{concat(text("\\"), any_of(Set::all()))};

    builder.add_token(
            concat(text("\""), kleene(choice(any_of(Set::all() - Set{'"', '\\', '\n'}), escape)), text("\"")),
            Token::String, 1);

    builder.add_token(
            concat(text("'"), plus(choice(any_of(Set::all() - Set{'\'', '\\', '\n'}), escape)), text("'")),
            Token::Literal, 1);

    builder.add_token(figures::line_comment(), Token::LineComment, 1);
    builder.add_token(figures::block_comment(), Token::BlockComment, 1);

    return builder.build();
}

// Deterministic C-like text, the recovery harness's LCG discipline: every run is the identical experiment.
std::string generated_c(const std::size_t bytes)
{
    std::uint64_t state{0x9E3779B97F4A7C15ULL};

    const auto next{[&state](const std::size_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;

        return static_cast<std::size_t>((state >> 33) % bound);
    }};

    static constexpr const char* idents[]{"count", "buffer", "index", "state", "value", "table", "next", "size"};

    std::string out;

    while (out.size() < bytes)
    {
        switch (next(6))
        {
        case 0:
            out += "/* invariant: ";
            out += idents[next(8)];
            out += " stays in range */\n";
            break;
        case 1:
            out += "#define LIMIT_";
            out += idents[next(8)];
            out += " 4096\n";
            break;
        case 2:
            out += "    ";
            out += idents[next(8)];
            out += " = ";
            out += idents[next(8)];
            out += " + 17;\n";
            break;
        case 3:
            out += "static const char* name = \"a \\\"quoted\\\" piece\";\n";
            break;
        case 4:
            out += "int ";
            out += idents[next(8)];
            out += "[128]; // sized by the table\n";
            break;
        default:
            out += "}\n";
            break;
        }
    }

    return out;
}

struct Splice
{
    std::size_t serial_consumed{};
    std::size_t serial_tokens{};
    std::size_t chunks{};
    std::size_t incomplete_chunks{};
    std::size_t spliced_tokens{};
};

Splice splice(const munch::core::Lexer& lexer, const std::string_view input)
{
    Splice result;

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
} // namespace

int main(const int argc, const char** argv)
{
    const auto lexer{consumption_complete_c()};

    if (argc > 1)
    {
        std::vector<std::filesystem::path> files;

        const std::string extension{argc > 2 ? argv[2] : ".rs"};

        for (const auto& entry : std::filesystem::recursive_directory_iterator{argv[1]})
        {
            if (entry.is_regular_file() && entry.path().extension() == extension)
            {
                files.push_back(entry.path());
            }
        }

        std::ranges::sort(files);

        std::string stream;

        for (const auto& path : files)
        {
            std::ifstream in{path, std::ios::binary};

            stream.append(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});

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

        return 0;
    }

    // The pinned self-test: one unconsumable byte near the front, the serial scan stops, the spliced chunks
    // do not notice.
    auto corpus{generated_c(256 * 1024)};

    // The damaged byte must sit at top level, not inside a comment or string interior, which absorb control
    // bytes; the first arithmetic statement past the target offset is provably top level.
    const auto site{corpus.find("+ 17;", corpus.size() / 50)};

    expect(site != std::string::npos, "the generated corpus lost its arithmetic statements");

    corpus[site + 2] = '\x01';

    const auto result{splice(lexer, corpus)};

    std::cout << "serial " << result.serial_consumed << "/" << corpus.size() << " (" << result.serial_tokens
              << " tokens), chunks " << result.chunks << ", spliced " << result.spliced_tokens << " tokens\n";

    expect(result.serial_consumed < corpus.size() / 40, "the damage did not stop the serial scan early");
    expect(result.chunks == 8, "the malformed stream did not plan eight chunks");
    expect(result.incomplete_chunks == 1, "only the damaged chunk may report short consumption");
    expect(result.spliced_tokens > 20 * result.serial_tokens, "splicing did not overproduce");
    expect(result.serial_tokens == 1487, "the pinned serial token count moved");
    expect(result.spliced_tokens == 62309, "the pinned spliced token count moved");

    std::cout << (failures == 0 ? "all assertions hold\n" : "ASSERTION FAILURES\n");

    return failures == 0 ? 0 : 1;
}

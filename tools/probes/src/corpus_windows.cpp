// Measures whether certified split positions occur densely enough on real corpora to plan balanced chunks,
// which is the empirical question both published reports name and neither answers: the split-points report
// proves where a byte certifies and the split-windows report proves where a window does, but occurrence on
// real inputs is a property of corpora, not of grammars, and no frequency claim exists in either paper.
//
// WHAT RUNS AS A TEST. The grammar-level facts this file asserts are corpus-free and pin the mechanism:
// the published cumulative C-like row and the RFC 8259 row certify no exact byte; the RFC 8259 row's 120
// certified two-byte windows split into 63 of the form control-whitespace-then-must-start-byte and 57 of the
// form token-final-byte-then-control-whitespace, and the SPACE byte appears in neither position at either
// end, because a space is a legal string interior and so poisons no hypothesis, where tab, newline, and
// carriage return are excluded from unescaped string interiors by RFC 8259 and kill every inside-a-string
// reading. Three-byte STRUCTURAL windows extend the same mechanism to printable text: {,"v} certifies at
// origin 1 because no JSON token starts with v, so every cloud hypothesis in which the quote closes a string
// dies on the final byte and the sole survivor has the quote opening one; the dual {t":} certifies at origin
// 2 because no JSON token ends with t. The two-byte prefixes of both refuse, as does {,"9}, whose digit can
// begin a Number and so keeps the closure hypothesis alive: the poison byte must be unable to START a token
// for the mechanism to fire. The consumption-complete C row certifies no two-byte window at all; its plans
// rest entirely on lengths three and four. A deterministic generated C-like corpus then exercises
// the shipped planner end to end: complete consumption, a full plan at eight chunks, and spliced-scan token
// equality against the serial scan, all with pinned counts, so a drifted number fails the test suite.
//
// THE CONSUMPTION-COMPLETE C ROW. Real C defeats every published study row before certification is even in
// question: the preprocessor's # begins essentially every file, so the cumulative row consumes 2.5% of a
// pinned Linux kernel sample. This instrument therefore carries its own row, the published cumulative row
// plus nine consumption fixes (#, backslash, @, backtick, $, and the apostrophe as punctuation, carriage
// return in the whitespace run, escape-carrying strings, char literals), which consumed 100% of that sample's
// 1503 files. It is consumption-faithful, not kind-faithful: hex literals and floats split into Number and
// Identifier fragments, which certification does not care about but token consumers would. It is this
// instrument's own row, not a published one.
//
// CAMPAIGN MODE. With a corpus directory argument the instrument walks its regular files (grammar chosen by
// extension: .json uses the RFC 8259 row, everything else the consumption-complete C row), concatenates them
// in sorted order, and reports per file and for the stream: bytes, consumed fraction, chunks achieved against
// requested, balance (largest chunk over ideal), and boundary-deviation quantiles against equal-division
// targets, the planning-granularity proxy for the certificate gap distribution. Rows go to the CSV path when
// given. Figures from campaign runs are quotable only under the collection ritual, archived with provenance
// beside the clean commit, exactly as the benchmark's and the recovery harness's are.

#include <algorithm>
#include <cstddef>
#include <cstdio>
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

munch::core::Lexer published_cumulative()
{
    munch::core::Builder builder;

    figures::c_like(builder, false);
    builder.add_token(figures::string_literal(), Token::String, 2);
    builder.add_token(figures::line_comment(), Token::LineComment, 1);
    builder.add_token(figures::block_comment(), Token::BlockComment, 1);

    return builder.build();
}

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

munch::core::Lexer rfc_json()
{
    munch::core::Builder builder;

    figures::json(builder);

    return builder.build();
}

std::size_t exact_bytes(const munch::core::Lexer& lexer)
{
    std::size_t count{0};

    for (int value{0}; value < 256; ++value)
    {
        count += lexer.is_split_point(static_cast<char>(value)) ? 1 : 0;
    }

    return count;
}

// The two-byte census; whitespace_anchored counts members whose first byte the grammar treats as whitespace.
struct Census
{
    std::size_t windows{0};
    std::size_t whitespace_anchored{0};
};

Census census_two(const munch::core::Lexer& lexer, const std::string_view whitespace)
{
    Census census;

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const char window[2]{static_cast<char>(first), static_cast<char>(second)};

            if (lexer.is_split_window(std::string_view{window, 2}).has_value())
            {
                ++census.windows;

                if (whitespace.find(static_cast<char>(first)) != std::string_view::npos)
                {
                    ++census.whitespace_anchored;
                }
            }
        }
    }

    return census;
}

// Deterministic C-like source: the same LCG discipline as the recovery harness, so every run performs the
// identical experiment. The shapes exercise what the consumption fixes exist for: preprocessor lines, strings
// with escapes, char literals, both comment forms, and ordinary statement text.
std::string generated_c(const std::size_t bytes)
{
    std::uint64_t state{0x2545F4914F6CDD1DULL};

    const auto next{[&state](const std::size_t bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;

        return static_cast<std::size_t>((state >> 33) % bound);
    }};

    static constexpr const char* idents[]{"count", "buffer", "index", "state", "value", "table", "next", "size"};

    std::string out;

    while (out.size() < bytes)
    {
        switch (next(8))
        {
        case 0:
            out += "#define LIMIT_";
            out += idents[next(8)];
            out += " 4096\n";
            break;
        case 1:
            out += "/* invariant: ";
            out += idents[next(8)];
            out += " stays below the table size */\n";
            break;
        case 2:
            out += "static const char* name = \"escaped \\\"quote\\\" and tab\\t\";\n";
            break;
        case 3:
            out += "if (";
            out += idents[next(8)];
            out += " != '\\n') { // resync at line end\n";
            break;
        case 4:
            out += "    ";
            out += idents[next(8)];
            out += " = ";
            out += idents[next(8)];
            out += " + 17;\n";
            break;
        case 5:
            out += "}\n";
            break;
        case 6:
            out += "int ";
            out += idents[next(8)];
            out += "[128];\n";
            break;
        default:
            out += "    call(";
            out += idents[next(8)];
            out += ", \"literal\", 3);\n";
            break;
        }
    }

    return out;
}

struct Plan
{
    std::size_t chunks{0};
    double balance{0.0};
    std::vector<std::size_t> deviations;
};

Plan plan(const munch::core::Lexer& lexer, const std::string_view input, const std::size_t chunks)
{
    Plan result;

    const auto bounds{lexer.chunk_boundaries_with_windows(input, chunks)};

    result.chunks = bounds.size() - 1;

    std::size_t largest{0};

    for (std::size_t index{1}; index < bounds.size(); ++index)
    {
        largest = std::max(largest, bounds[index] - bounds[index - 1]);
    }

    result.balance =
            static_cast<double>(largest) / (static_cast<double>(input.size()) / static_cast<double>(result.chunks));

    for (std::size_t index{1}; index + 1 < bounds.size(); ++index)
    {
        const auto target{index * input.size() / result.chunks};

        result.deviations.push_back(bounds[index] > target ? bounds[index] - target : target - bounds[index]);
    }

    std::ranges::sort(result.deviations);

    return result;
}

std::size_t quantile(const std::vector<std::size_t>& sorted, const std::size_t numerator, const std::size_t denominator)
{
    return sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, sorted.size() * numerator / denominator)];
}

// One scan counting tokens; the return is the consumed byte count exactly as tokenize_all() reports it.
std::size_t scan(const munch::core::Lexer& lexer, const std::string_view input, std::size_t& tokens)
{
    return lexer.tokenize_all<Token>(input, [&tokens](Token, std::size_t) { ++tokens; });
}

void campaign(const std::filesystem::path& root, const std::size_t chunks, const char* csv_path)
{
    const auto json_lexer{rfc_json()};

    const auto c_lexer{consumption_complete_c()};

    std::vector<std::filesystem::path> files;

    for (const auto& entry : std::filesystem::recursive_directory_iterator{root})
    {
        if (entry.is_regular_file())
        {
            files.push_back(entry.path());
        }
    }

    std::ranges::sort(files);

    std::FILE* csv{csv_path ? std::fopen(csv_path, "w") : nullptr};

    if (csv)
    {
        std::fprintf(
                csv,
                "path,grammar,bytes,consumed,chunks_requested,chunks_achieved,balance,dev_median,"
                "dev_p95,dev_max\n");
    }

    std::string stream;

    const munch::core::Lexer* stream_lexer{nullptr};

    for (const auto& path : files)
    {
        const auto& lexer{path.extension() == ".json" ? json_lexer : c_lexer};

        stream_lexer = &lexer;

        std::ifstream in{path, std::ios::binary};

        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

        std::size_t tokens{0};

        const auto consumed{scan(lexer, text, tokens)};

        const auto planned{plan(lexer, text, chunks)};

        if (csv)
        {
            std::fprintf(
                    csv, "%s,%s,%zu,%zu,%zu,%zu,%.5f,%zu,%zu,%zu\n", path.c_str(),
                    path.extension() == ".json" ? "rfc8259" : "consumption-complete-c", text.size(), consumed, chunks,
                    planned.chunks, planned.balance, quantile(planned.deviations, 1, 2),
                    quantile(planned.deviations, 95, 100), planned.deviations.empty() ? 0 : planned.deviations.back());
        }

        stream.append(text);

        stream += '\n';
    }

    if (!stream_lexer)
    {
        std::cout << "campaign: no regular files under " << root << "\n";

        return;
    }

    std::size_t serial_tokens{0};

    const auto consumed{scan(*stream_lexer, stream, serial_tokens)};

    const auto planned{plan(*stream_lexer, stream, chunks)};

    std::cout << "stream: " << files.size() << " files, " << stream.size() << " bytes, consumed " << consumed
              << ", chunks " << planned.chunks << "/" << chunks << ", balance " << planned.balance
              << ", deviations median " << quantile(planned.deviations, 1, 2) << " p95 "
              << quantile(planned.deviations, 95, 100) << " max "
              << (planned.deviations.empty() ? 0 : planned.deviations.back()) << "\n";

    if (csv)
    {
        std::fprintf(
                csv, "STREAM,%s,%zu,%zu,%zu,%zu,%.5f,%zu,%zu,%zu\n",
                stream_lexer == &json_lexer ? "rfc8259" : "consumption-complete-c", stream.size(), consumed, chunks,
                planned.chunks, planned.balance, quantile(planned.deviations, 1, 2),
                quantile(planned.deviations, 95, 100), planned.deviations.empty() ? 0 : planned.deviations.back());

        std::fclose(csv);
    }
}
} // namespace

int main(const int argc, const char** argv)
{
    if (argc > 1)
    {
        campaign(argv[1], argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 8, argc > 3 ? argv[3] : nullptr);

        return 0;
    }

    // The corpus-free grammar facts. Exact bytes: both published rows certify none, which is why the window
    // layer is load-bearing on production-shaped inputs at all.
    expect(exact_bytes(published_cumulative()) == 0, "published cumulative row certifies an exact byte");

    const auto json_lexer{rfc_json()};

    expect(exact_bytes(json_lexer) == 0, "RFC 8259 row certifies an exact byte");

    const auto json_census{census_two(json_lexer, "\t\n\r ")};

    std::cout << "json two-byte windows " << json_census.windows << ", whitespace-anchored "
              << json_census.whitespace_anchored << "\n";

    expect(json_census.windows == 120, "RFC 8259 two-byte census moved");

    expect(json_census.whitespace_anchored == 63, "the control-whitespace-first family moved");

    // Space is a legal string interior, so it poisons nothing: no two-byte window uses it on either side.
    expect(!json_lexer.is_split_window(" \"").has_value(), "space-first window certifies");

    expect(!json_lexer.is_split_window("\" ").has_value(), "space-second window certifies");

    // Control whitespace is excluded from unescaped interiors, so it anchors in both directions.
    expect(json_lexer.is_split_window("\"\t") == std::optional<std::size_t>{1},
           "token-final-then-tab window does not certify");

    // The structural poison-byte mechanism, pinned positively and negatively.
    expect(json_lexer.is_split_window(",\"v") == std::optional<std::size_t>{1},
           "structural window {,\"v} does not certify at origin 1");

    expect(json_lexer.is_split_window("t\":") == std::optional<std::size_t>{2},
           "structural window {t\":} does not certify at origin 2");

    expect(!json_lexer.is_split_window(",\"").has_value(), "two-byte prefix {,\"} certifies");

    expect(!json_lexer.is_split_window("\",").has_value(), "two-byte {\",} certifies");

    expect(!json_lexer.is_split_window(",\"9").has_value(), "{,\"9} certifies although 9 can begin a Number");

    const auto c_lexer{consumption_complete_c()};

    expect(exact_bytes(c_lexer) == 0, "consumption-complete C row certifies an exact byte");

    const auto c_census{census_two(c_lexer, "\t\n\r ")};

    expect(c_census.windows == 0, "the consumption-complete C row gained a two-byte window");

    // The generated corpus: consumption, a full plan, and spliced equality with pinned counts.
    const auto corpus{generated_c(256 * 1024)};

    expect(corpus.size() == 262194, "generated corpus size moved");

    std::size_t serial_tokens{0};

    expect(scan(c_lexer, corpus, serial_tokens) == corpus.size(), "generated corpus does not consume completely");

    expect(serial_tokens == 74838, "generated corpus token count moved");

    const auto planned{plan(c_lexer, corpus, 8)};

    std::cout << "generated corpus chunks " << planned.chunks << ", balance " << planned.balance << "\n";

    expect(planned.chunks == 8, "generated corpus does not plan eight chunks");

    expect(planned.balance < 1.10, "generated corpus balance exceeds 1.10");

    const auto bounds{c_lexer.chunk_boundaries_with_windows(corpus, 8)};

    std::size_t spliced_tokens{0};

    auto consumed_all{true};

    for (std::size_t index{1}; index < bounds.size(); ++index)
    {
        const std::string_view chunk{corpus.data() + bounds[index - 1], bounds[index] - bounds[index - 1]};

        consumed_all = scan(c_lexer, chunk, spliced_tokens) == chunk.size() && consumed_all;
    }

    expect(consumed_all, "a planned chunk of the generated corpus does not consume completely");

    expect(spliced_tokens == serial_tokens, "spliced token count differs from the serial scan");

    std::cout << (failures == 0 ? "all assertions hold\n" : "ASSERTION FAILURES\n");

    return failures == 0 ? 0 : 1;
}

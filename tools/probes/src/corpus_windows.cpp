// Measures whether certified split positions occur densely enough on real corpora to plan balanced chunks,
// which is the empirical question both published reports name and neither answers: the split-points report
// proves where a byte certifies and the split-windows report proves where a window does, but occurrence on
// real inputs is a property of corpora, not of grammars, and no frequency claim exists in either paper.
//
// What runs as a test. The grammar-level facts this file asserts are corpus-free and pin the mechanism:
// the published cumulative C-like row and the RFC 8259 row certify no exact byte; the RFC 8259 row's 120
// certified two-byte windows split into 63 of the form control-whitespace-then-must-start-byte and 57 of the
// form token-final-byte-then-control-whitespace, and the space byte appears in neither position at either
// end, because a space is a legal string interior and so poisons no hypothesis, where tab, newline, and
// carriage return are excluded from unescaped string interiors by RFC 8259 and kill every inside-a-string
// reading. Three-byte structural windows extend the same mechanism to printable text: {,"v} certifies at
// origin 1 because no JSON token starts with v, so every cloud hypothesis in which the quote closes a string
// dies on the final byte and the sole survivor has the quote opening one; the dual {t":} certifies at origin
// 2 because no JSON token ends with t. The two-byte prefixes of both refuse, as does {,"9}, whose digit can
// begin a Number and so keeps the closure hypothesis alive: the poison byte must be unable to start a token
// for the mechanism to fire. The consumption-complete C row certifies no two-byte window at all; its plans
// rest entirely on lengths three and four. A deterministic generated C-like corpus then exercises the shipped
// planner end to end: complete consumption, a full plan at eight chunks, and equality of the spliced chunks'
// concatenated (token, length) stream against the serial scan's, with the corpus size and the token count
// pinned, so a drifted number fails the test suite.
//
// The consumption-complete C row. Real C defeats every published study row before certification is even in
// question: the preprocessor's # begins essentially every file, so the cumulative row consumes 2.5% of a
// pinned Linux kernel sample. This instrument therefore carries its own row, the published cumulative row
// plus nine consumption fixes (#, backslash, @, backtick, $, and the apostrophe as punctuation, carriage
// return in the whitespace run, escape-carrying strings, char literals), which consumed 100% of that sample's
// 1503 files. It is consumption-faithful, not kind-faithful: hex literals and floats split into Number and
// Identifier fragments, which certification does not care about but token consumers would. It is this
// instrument's own row, not a published one.
//
// Campaign mode. With a corpus directory argument the instrument walks its regular files (grammar chosen by
// extension: .json uses the RFC 8259 row, everything else the consumption-complete C row), concatenates them
// in sorted order, and reports per file and for the stream: bytes, consumed fraction, chunks achieved against
// requested, balance (largest chunk over ideal), and boundary-deviation quantiles against equal-division
// targets, the planning-granularity proxy for the certificate gap distribution. Rows go to the CSV path when
// given. The stream row is one grammar's measurement, so a corpus whose extensions select both grammars is
// refused it: the per-file rows still stand, but no single certificate plans the aggregate. Figures from campaign runs
// are quoted from archives kept with their provenance beside a clean commit, as the benchmark's and the recovery
// harness's are.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

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
using munch::tools::probes::Output_file;
using munch::tools::probes::published_cumulative_row;
using munch::tools::probes::read_bytes;

/**
 * @brief The grammar a campaign reads a file under.
 */
enum class Grammar
{
    rfc8259,
    consumption_complete_c
};

/**
 * @brief The grammar a file's extension selects: `.json` the RFC 8259 row, every other the consumption-complete C row.
 * @param path The file.
 * @return The file's grammar.
 */
Grammar grammar_of(const std::filesystem::path& path)
{
    return path.extension() == ".json" ? Grammar::rfc8259 : Grammar::consumption_complete_c;
}

/**
 * @brief The grammar's name in the CSV's grammar column.
 * @param grammar The grammar.
 * @return `rfc8259` or `consumption-complete-c`.
 */
std::string_view name_of(const Grammar grammar)
{
    return grammar == Grammar::rfc8259 ? "rfc8259" : "consumption-complete-c";
}

/**
 * @brief The two rows' lexers a campaign reads files under: json the RFC 8259 row, c the consumption-complete C row.
 */
struct Row_lexers
{
    munch::core::Lexer json;

    munch::core::Lexer c;
};

/**
 * @brief Picks the RFC 8259 row's lexer for Grammar::rfc8259 and the consumption-complete C row's otherwise.
 */
const munch::core::Lexer& lexer_of(const Row_lexers& lexers, const Grammar grammar)
{
    return grammar == Grammar::rfc8259 ? lexers.json : lexers.c;
}

/**
 * @brief Scans an input, counting tokens.
 * @param lexer The lexer.
 * @param input The input.
 * @param tokens The count, incremented once per token.
 * @return The consumed byte count exactly as tokenize_all() reports it.
 */
std::size_t scan(const munch::core::Lexer& lexer, const std::string_view input, std::size_t& tokens)
{
    return lexer.tokenize_all<Token>(input, [&tokens](Token, std::size_t) { ++tokens; });
}

/**
 * @brief Scans an input, recording each token with its length.
 * @param lexer The lexer.
 * @param input The input.
 * @param tokens The stream, each token appended with its length.
 * @return The consumed byte count exactly as tokenize_all() reports it.
 */
std::size_t scan(
        const munch::core::Lexer& lexer, const std::string_view input,
        std::vector<std::pair<Token, std::size_t>>& tokens)
{
    return lexer.tokenize_all<Token>(
            input, [&tokens](const Token token, const std::size_t length) { tokens.emplace_back(token, length); });
}

/**
 * @brief One plan of an input and how evenly it cuts.
 */
struct Plan
{
    /**
     * @brief The chunks the plan achieves.
     */
    std::size_t chunks{0};

    /**
     * @brief The largest chunk over the ideal equal share.
     */
    double balance{0.0};

    /**
     * @brief Each inner boundary's distance from its equal-division target, sorted.
     */
    std::vector<std::size_t> deviations;
};

/**
 * @brief Plans an input with the shipped window planner and measures the plan against equal division.
 * @param lexer The lexer planning.
 * @param input The input.
 * @param chunks The chunks requested.
 * @return The achieved chunks, the balance, and the sorted boundary deviations.
 */
Plan plan(const munch::core::Lexer& lexer, const std::string_view input, const std::size_t chunks)
{
    Plan result{};

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

/**
 * @brief One quantile of a sorted sample in integer arithmetic, `sorted[min(n - 1, n * numerator / denominator)]`.
 * @param sorted The sample, sorted.
 * @param numerator The quantile's numerator.
 * @param denominator The quantile's denominator.
 * @return The quantile, 0 for an empty sample.
 */
std::size_t quantile(const std::vector<std::size_t>& sorted, const std::size_t numerator, const std::size_t denominator)
{
    return sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, sorted.size() * numerator / denominator)];
}

/**
 * @brief The files of a campaign concatenated, and the grammar the stream row is read under.
 */
struct Stream
{
    /**
     * @brief Every file's bytes, each followed by a newline, in sorted order.
     */
    std::string bytes;

    /**
     * @brief The first file's grammar, std::nullopt when there is no file.
     */
    std::optional<Grammar> grammar;

    /**
     * @brief Whether a later file's grammar differs from the first file's.
     */
    bool is_mixed{false};
};

/**
 * @brief Scans and plans every file under its grammar, writes one CSV row per file when the CSV is open, and
 *        concatenates the files into the stream.
 * @param files The files, sorted; an unreadable one is read as empty.
 * @param lexers The two rows' lexers.
 * @param chunks The chunks requested of each plan.
 * @param csv The CSV, written only when open.
 * @return The stream, its grammar, and whether its files mix grammars.
 */
Stream file_rows(
        const std::vector<std::filesystem::path>& files, const Row_lexers& lexers, const std::size_t chunks,
        Output_file& csv)
{
    Stream stream{};

    for (const auto& path : files)
    {
        const auto grammar{grammar_of(path)};

        const auto& lexer{lexer_of(lexers, grammar)};

        // The stream is scanned and planned with one lexer, so a second grammar in the corpus leaves it none.
        stream.is_mixed = stream.is_mixed || stream.grammar.value_or(grammar) != grammar;

        if (!stream.grammar)
        {
            stream.grammar = grammar;
        }

        const auto text{read_bytes(path).value_or("")};

        std::size_t tokens{0};

        const auto consumed{scan(lexer, text, tokens)};

        const auto planned{plan(lexer, text, chunks)};

        if (csv.is_open())
        {
            std::fprintf(
                    csv.stream(), "%s,%s,%zu,%zu,%zu,%zu,%.5f,%zu,%zu,%zu\n", path.c_str(),
                    std::string{name_of(grammar)}.c_str(), text.size(), consumed, chunks, planned.chunks,
                    planned.balance, quantile(planned.deviations, 1, 2), quantile(planned.deviations, 95, 100),
                    planned.deviations.empty() ? 0 : planned.deviations.back());
        }

        stream.bytes.append(text);

        stream.bytes += '\n';
    }

    return stream;
}

/**
 * @brief Scans and plans the stream under its one grammar, prints the stream row, and writes it to the CSV and closes
 *        the CSV when it is open.
 * @param stream The stream, of the one grammar its grammar field names.
 * @param lexer The lexer of the stream's grammar.
 * @param files The number of files in the stream.
 * @param chunks The chunks requested of the plan.
 * @param csv The CSV, written and closed only when open.
 */
void stream_row(
        const Stream& stream, const munch::core::Lexer& lexer, const std::size_t files, const std::size_t chunks,
        Output_file& csv)
{
    std::size_t serial_tokens{0};

    const auto consumed{scan(lexer, stream.bytes, serial_tokens)};

    const auto planned{plan(lexer, stream.bytes, chunks)};

    std::cout << "stream: " << files << " files, " << stream.bytes.size() << " bytes, consumed " << consumed
              << ", chunks " << planned.chunks << "/" << chunks << ", balance " << planned.balance
              << ", deviations median " << quantile(planned.deviations, 1, 2) << " p95 "
              << quantile(planned.deviations, 95, 100) << " max "
              << (planned.deviations.empty() ? 0 : planned.deviations.back()) << "\n";

    if (csv.is_open())
    {
        std::fprintf(
                csv.stream(), "STREAM,%s,%zu,%zu,%zu,%zu,%.5f,%zu,%zu,%zu\n",
                std::string{name_of(*stream.grammar)}.c_str(), stream.bytes.size(), consumed, chunks, planned.chunks,
                planned.balance, quantile(planned.deviations, 1, 2), quantile(planned.deviations, 95, 100),
                planned.deviations.empty() ? 0 : planned.deviations.back());

        std::ignore = csv.close();
    }
}

/**
 * @brief Builds the RFC 8259 row.
 * @return The row's lexer.
 */
munch::core::Lexer rfc_json()
{
    munch::core::Builder builder{};

    figures::json(builder);

    return builder.build();
}

/**
 * @brief Builds the consumption-complete C row.
 * @return The row's lexer.
 */
munch::core::Lexer consumption_complete_c()
{
    munch::core::Builder builder{};

    consumption_complete_c_row(builder);

    return builder.build();
}

/**
 * @brief Counts the bytes in a lexer's exact certificate.
 * @param lexer The lexer.
 * @return The number of bytes, of 256, that are split points.
 */
std::size_t exact_bytes(const munch::core::Lexer& lexer)
{
    std::size_t count{0};

    for (int value{0}; value < 256; ++value)
    {
        count += lexer.is_split_point(static_cast<char>(value)) ? 1 : 0;
    }

    return count;
}

/**
 * @brief The two-byte census of a lexer.
 */
struct Census
{
    /**
     * @brief The certified two-byte windows.
     */
    std::size_t windows{0};

    /**
     * @brief The certified windows whose first byte the grammar treats as whitespace.
     */
    std::size_t whitespace_anchored{0};
};

/**
 * @brief Counts the certified two-byte windows, and those whose first byte is one of the given whitespace bytes.
 * @param lexer The lexer.
 * @param whitespace The bytes the grammar treats as whitespace.
 * @return The two counts.
 */
Census census_two(const munch::core::Lexer& lexer, const std::string_view whitespace)
{
    Census census{};

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const std::array window{static_cast<char>(first), static_cast<char>(second)};

            if (lexer.is_split_window(std::string_view{window.data(), window.size()}).has_value())
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

/**
 * @brief Generates C-like source of eight statement shapes drawn from an Lcg64 at a fixed seed, deterministic from
 *        that seed: preprocessor lines, strings with escapes, char literals, both comment forms, and ordinary
 *        statement text.
 * @param bytes The least size of the source; generation stops at the first statement reaching it.
 * @return The source.
 */
std::string generated_c(const std::size_t bytes)
{
    Lcg64 lcg{0x2545F4914F6CDD1DULL};

    static constexpr std::array<std::string_view, 8> idents{"count", "buffer", "index", "state",
                                                            "value", "table",  "next",  "size"};

    std::string out{};

    while (out.size() < bytes)
    {
        switch (lcg.next(8))
        {
        case 0:
            out += "#define LIMIT_";
            out += idents[lcg.next(8)];
            out += " 4096\n";
            break;
        case 1:
            out += "/* invariant: ";
            out += idents[lcg.next(8)];
            out += " stays below the table size */\n";
            break;
        case 2:
            out += "static const char* name = \"escaped \\\"quote\\\" and tab\\t\";\n";
            break;
        case 3:
            out += "if (";
            out += idents[lcg.next(8)];
            out += " != '\\n') { // resync at line end\n";
            break;
        case 4:
            out += "    ";
            out += idents[lcg.next(8)];
            out += " = ";
            out += idents[lcg.next(8)];
            out += " + 17;\n";
            break;
        case 5:
            out += "}\n";
            break;
        case 6:
            out += "int ";
            out += idents[lcg.next(8)];
            out += "[128];\n";
            break;
        default:
            out += "    call(";
            out += idents[lcg.next(8)];
            out += ", \"literal\", 3);\n";
            break;
        }
    }

    return out;
}

/**
 * @brief A directory, root, removed with everything under it when the object is destroyed.
 */
struct Removed_on_exit
{
    std::filesystem::path root;

    /**
     * @brief Removes the directory and everything under it, ignoring a failure.
     */
    ~Removed_on_exit()
    {
        std::error_code ignored{};

        std::filesystem::remove_all(root, ignored);
    }
};

/**
 * @brief Writes a corpus mixing the two grammars, one JSON and one C file, into a new directory of a unique name
 *        made by mkdtemp beneath TMPDIR, or beneath /tmp when TMPDIR is unset or empty; exits the program with
 *        EXIT_FAILURE when mkdtemp fails.
 * @return The directory.
 */
std::filesystem::path mixed_corpus()
{
    std::string pattern{"/tmp"};

    if (const char* const base{std::getenv("TMPDIR")}; base != nullptr && *base != '\0')
    {
        pattern = base;
    }

    pattern += "/munch-corpus-windows-XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());

    buffer.push_back('\0');

    if (::mkdtemp(buffer.data()) == nullptr)
    {
        std::perror("mkdtemp");

        std::exit(EXIT_FAILURE);
    }

    const std::filesystem::path root{buffer.data()};

    std::ofstream{root / "a.json"} << "{\"k\": [1, 2]}\n";

    std::ofstream{root / "b.c"} << "int x = 1;\n";

    return root;
}

/**
 * @brief Scans and plans the files under a directory, per file and as one stream, printing the stream row and writing
 *        every row to the CSV when a path is given; the stream row is refused when there is no file or the files mix
 *        grammars.
 * @param root The directory walked.
 * @param chunks The chunks requested of each plan.
 * @param csv_path The CSV to write, std::nullopt for none.
 * @return Whether one certificate planned the aggregate stream.
 */
bool campaign(
        const std::filesystem::path& root, const std::size_t chunks,
        const std::optional<std::filesystem::path>& csv_path)
{
    const Row_lexers lexers{.json = rfc_json(), .c = consumption_complete_c()};

    const auto files{files_under(root, std::nullopt)};

    auto csv{csv_path ? Output_file{*csv_path} : Output_file{}};

    if (csv.is_open())
    {
        std::fprintf(
                csv.stream(),
                "path,grammar,bytes,consumed,chunks_requested,chunks_achieved,balance,dev_median,"
                "dev_p95,dev_max\n");
    }

    const auto stream{file_rows(files, lexers, chunks, csv)};

    if (!stream.grammar)
    {
        std::cout << "campaign: no regular files under " << root << "\n";

        return false;
    }

    if (stream.is_mixed)
    {
        std::cout << "campaign: mixed grammars under " << root
                  << ", stream row refused: no single certificate plans the aggregate\n";

        if (csv.is_open())
        {
            std::ignore = csv.close();
        }

        return false;
    }

    stream_row(stream, lexer_of(lexers, *stream.grammar), files.size(), chunks, csv);

    return true;
}

/**
 * @brief Asserts the corpus-free facts of the two published rows: neither certifies an exact byte; the RFC 8259
 *        row's two-byte census and its structural poison-byte windows, pinned positively and negatively. Prints the
 *        census.
 * @param assertions The probe's assertions.
 */
void published_row_facts(Assertions& assertions)
{
    munch::core::Builder cumulative_builder{};

    published_cumulative_row(cumulative_builder);

    assertions.expect(exact_bytes(cumulative_builder.build()) == 0, "published cumulative row certifies an exact byte");

    const auto json_lexer{rfc_json()};

    assertions.expect(exact_bytes(json_lexer) == 0, "RFC 8259 row certifies an exact byte");

    const auto json_census{census_two(json_lexer, "\t\n\r ")};

    std::cout << "json two-byte windows " << json_census.windows << ", whitespace-anchored "
              << json_census.whitespace_anchored << "\n";

    assertions.expect(json_census.windows == 120, "RFC 8259 two-byte census moved");

    assertions.expect(json_census.whitespace_anchored == 63, "the control-whitespace-first family moved");

    // Space is a legal string interior, so it poisons nothing: no two-byte window uses it on either side.
    assertions.expect(!json_lexer.is_split_window(" \"").has_value(), "space-first window certifies");

    assertions.expect(!json_lexer.is_split_window("\" ").has_value(), "space-second window certifies");

    // Control whitespace is excluded from unescaped interiors, so it anchors in both directions.
    assertions.expect(
            json_lexer.is_split_window("\"\t") == std::optional<std::size_t>{1},
            "token-final-then-tab window does not certify");

    // The structural poison-byte mechanism, pinned positively and negatively.
    assertions.expect(
            json_lexer.is_split_window(",\"v") == std::optional<std::size_t>{1},
            "structural window {,\"v} does not certify at origin 1");

    assertions.expect(
            json_lexer.is_split_window("t\":") == std::optional<std::size_t>{2},
            "structural window {t\":} does not certify at origin 2");

    assertions.expect(!json_lexer.is_split_window(",\"").has_value(), "two-byte prefix {,\"} certifies");

    assertions.expect(!json_lexer.is_split_window("\",").has_value(), "two-byte {\",} certifies");

    assertions.expect(
            !json_lexer.is_split_window(",\"9").has_value(), "{,\"9} certifies although 9 can begin a Number");
}

/**
 * @brief Asserts that the consumption-complete C row certifies no exact byte and no two-byte window.
 * @param assertions The probe's assertions.
 * @param c_lexer The consumption-complete C row's lexer.
 */
void c_row_facts(Assertions& assertions, const munch::core::Lexer& c_lexer)
{
    assertions.expect(exact_bytes(c_lexer) == 0, "consumption-complete C row certifies an exact byte");

    const auto c_census{census_two(c_lexer, "\t\n\r ")};

    assertions.expect(c_census.windows == 0, "the consumption-complete C row gained a two-byte window");
}

/**
 * @brief Asserts on the generated corpus: complete consumption, a full plan at eight chunks, and the spliced chunks'
 *        (token, length) stream equal to the serial scan's, with the corpus size and the token count pinned. Prints
 *        the plan.
 * @param assertions The probe's assertions.
 * @param c_lexer The consumption-complete C row's lexer.
 */
void generated_corpus_splices(Assertions& assertions, const munch::core::Lexer& c_lexer)
{
    const auto corpus{generated_c(256 * 1024)};

    assertions.expect(corpus.size() == 262194, "generated corpus size moved");

    std::vector<std::pair<Token, std::size_t>> serial_stream{};

    assertions.expect(
            scan(c_lexer, corpus, serial_stream) == corpus.size(), "generated corpus does not consume completely");

    assertions.expect(serial_stream.size() == 74838, "generated corpus token count moved");

    const auto planned{plan(c_lexer, corpus, 8)};

    std::cout << "generated corpus chunks " << planned.chunks << ", balance " << planned.balance << "\n";

    assertions.expect(planned.chunks == 8, "generated corpus does not plan eight chunks");

    assertions.expect(planned.balance < 1.10, "generated corpus balance exceeds 1.10");

    const auto bounds{c_lexer.chunk_boundaries_with_windows(corpus, 8)};

    std::vector<std::pair<Token, std::size_t>> spliced_stream{};

    auto consumed_all{true};

    for (std::size_t index{1}; index < bounds.size(); ++index)
    {
        const std::string_view chunk{corpus.data() + bounds[index - 1], bounds[index] - bounds[index - 1]};

        consumed_all = scan(c_lexer, chunk, spliced_stream) == chunk.size() && consumed_all;
    }

    assertions.expect(consumed_all, "a planned chunk of the generated corpus does not consume completely");

    assertions.expect(spliced_stream == serial_stream, "spliced token stream differs from the serial scan");
}

/**
 * @brief Asserts that the stream row is planned by one lexer: a corpus carrying both grammars refuses it rather than
 *        planning the aggregate under whichever came first, and a single-grammar corpus plans it.
 * @param assertions The probe's assertions.
 */
void stream_row_refuses_mixed(Assertions& assertions)
{
    const Removed_on_exit scratch{mixed_corpus()};

    assertions.expect(!campaign(scratch.root, 2, std::nullopt), "a mixed corpus planned its aggregate stream");

    std::filesystem::remove(scratch.root / "b.c");

    assertions.expect(campaign(scratch.root, 2, std::nullopt), "a single-grammar corpus refused its aggregate stream");
}
} // namespace

/**
 * @brief Runs a campaign over the directory, chunk count and CSV path given, or else the self-test of the grammar
 *        facts, the generated corpus and the mixed-corpus refusal, and prints the verdict.
 * @param argc The argument count.
 * @param argv The directory, the chunks requested (8 by default) and the CSV path, all optional.
 * @return 0 after a campaign or a self-test whose assertions hold, 1 when a self-test assertion fails.
 */
int main(const int argc, const char** argv)
{
    Assertions assertions{};

    if (argc > 1)
    {
        std::ignore = campaign(
                argv[1], argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 8,
                argc > 3 ? std::optional<std::filesystem::path>{argv[3]} : std::nullopt);

        return 0;
    }

    published_row_facts(assertions);

    const auto c_lexer{consumption_complete_c()};

    c_row_facts(assertions, c_lexer);

    generated_corpus_splices(assertions, c_lexer);

    stream_row_refuses_mixed(assertions);

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? 1 : 0;
}

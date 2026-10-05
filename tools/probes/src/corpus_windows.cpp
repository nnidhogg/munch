// Measures whether certified split positions occur densely enough on real corpora to plan balanced chunks, which is the
// empirical question both published reports name and neither answers: the split-points report proves where a byte
// certifies and the split-windows report proves where a window does, but occurrence on real inputs is a property of
// corpora, not of grammars, and no frequency claim exists in either paper.
//
// What runs as a test. The grammar-level facts this file asserts are corpus-free and pin the mechanism: the published
// cumulative C-like row and the RFC 8259 row certify no exact byte; the RFC 8259 row's 120 certified two-byte windows
// split into 63 of the form control-whitespace-then-must-start-byte and 57 of the form
// token-final-byte-then-control-whitespace, and the space byte appears in neither position at either end, because a
// space is a legal string interior and so poisons no hypothesis, where tab, newline, and carriage return are excluded
// from unescaped string interiors by RFC 8259 and kill every inside-a-string reading. Three-byte structural windows
// extend the same mechanism to printable text: {,"v} certifies at origin 1 because no JSON token starts with v, so
// every cloud hypothesis in which the quote closes a string dies on the final byte and the sole survivor has the quote
// opening one; the dual {t":} certifies at origin 2 because no JSON token ends with t. The two-byte prefixes of both
// refuse, as does {,"9}, whose digit can begin a Number and so keeps the closure hypothesis alive: the poison byte must
// be unable to start a token for the mechanism to fire. The consumption-complete C row certifies no two-byte window at
// all; its plans rest entirely on lengths three and four. A deterministic generated C-like corpus then exercises the
// shipped planner end to end: complete consumption, a full plan at eight chunks, and equality of the spliced chunks'
// concatenated (token, length) stream against the serial scan's, with the corpus size and the token count pinned, so a
// drifted number fails the test suite.
//
// The consumption-complete C row. Real C defeats every published study row before certification is even in question:
// the preprocessor's # begins essentially every file, so the cumulative row consumes 2.5% of a pinned Linux kernel
// sample. This instrument therefore carries its own row, the published cumulative row plus nine consumption fixes (#,
// backslash, @, backtick, $, and the apostrophe as punctuation, carriage return in the whitespace run, escape-carrying
// strings, char literals), which consumed 100% of that sample's 1503 files. It is consumption-faithful, not
// kind-faithful: hex literals and floats split into Number and Identifier fragments, which certification does not care
// about but token consumers would. It is this instrument's own row, not a published one.
//
// Campaign mode. With a corpus directory argument the instrument walks its regular files (grammar chosen by extension:
// .json uses the RFC 8259 row, everything else the consumption-complete C row), concatenates them in sorted order, and
// reports per file and for the stream: bytes, consumed fraction, chunks achieved against requested, balance (largest
// chunk over ideal), and boundary-deviation quantiles against equal-division targets, the planning-granularity proxy
// for the certificate gap distribution. Rows go to the CSV path when given. The stream row is one grammar's
// measurement, so a corpus whose extensions select both grammars is refused it: the per-file rows still stand, but no
// single certificate plans the aggregate. Figures from campaign runs are quoted from archives kept with their
// provenance beside a clean commit, as the benchmark's and the recovery harness's are.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
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
#include "munch/tools/probes/chunks.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/generated_identifiers.hpp"
#include "munch/tools/probes/lcg64.hpp"
#include "munch/tools/probes/study_rows.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Assertions;
using munch::tools::probes::chunks_of;
using munch::tools::probes::consumption_complete_c_row;
using munch::tools::probes::every_byte;
using munch::tools::probes::files_under;
using munch::tools::probes::Lcg64;
using munch::tools::probes::Output_file;
using munch::tools::probes::pick_identifier;
using munch::tools::probes::published_cumulative_row;
using munch::tools::probes::read_bytes;

/**
 * @brief The chunks the generated corpus is planned in, and the chunks a campaign requests when none is given.
 */
constexpr std::size_t planned_chunks{8};

/**
 * @brief The chunks the self-test's mixed-corpus campaigns request.
 */
constexpr std::size_t self_test_chunks{2};

/**
 * @brief The four whitespace bytes of RFC 8259, the bytes the two-byte census counts windows anchored on.
 */
constexpr std::string_view whitespace_bytes{"\t\n\r "};

/**
 * @brief The grammar a campaign reads a file under.
 */
enum class Grammar
{
    /**
     * @brief The RFC 8259 row.
     */
    rfc8259,

    /**
     * @brief The consumption-complete C row.
     */
    consumption_complete_c
};

/**
 * @brief The two rows' lexers a campaign reads files under: json_row the RFC 8259 row, c_row the consumption-complete C
 *        row.
 */
struct Row_lexers
{
    /**
     * @brief The RFC 8259 row's lexer.
     */
    munch::core::Lexer json_row;

    /**
     * @brief The consumption-complete C row's lexer.
     */
    munch::core::Lexer c_row;
};

/**
 * @brief A scan's tokens, each a kind and a length, in stream order.
 */
using Stream_t = std::vector<std::pair<Token, std::size_t>>;

/**
 * @brief What a counting scan of an input found.
 */
struct Token_count
{
    /**
     * @brief The consumed byte count exactly as tokenize_all() reports it.
     */
    std::size_t consumed{0};

    /**
     * @brief The tokens scanned.
     */
    std::size_t tokens{0};
};

/**
 * @brief What a recording scan of an input found.
 */
struct Token_stream
{
    /**
     * @brief The consumed byte count exactly as tokenize_all() reports it.
     */
    std::size_t consumed{0};

    /**
     * @brief Each token scanned, with its length.
     */
    Stream_t tokens{};
};

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
    std::vector<std::size_t> deviations{};

    /**
     * @brief The plan's boundaries, both ends included.
     */
    std::vector<std::size_t> bounds{};
};

/**
 * @brief The boundary deviations of a plan, summarized.
 */
struct Deviation_summary
{
    /**
     * @brief The median deviation.
     */
    std::size_t median{0};

    /**
     * @brief The 95th percentile deviation.
     */
    std::size_t p95{0};

    /**
     * @brief The largest deviation, 0 when there is none.
     */
    std::size_t largest{0};
};

/**
 * @brief The files of a campaign concatenated, and the grammar the stream row is read under.
 */
struct Campaign_stream
{
    /**
     * @brief Every file's bytes, each followed by a newline, in sorted order.
     */
    std::string bytes{};

    /**
     * @brief The first file's grammar, std::nullopt when there is no file.
     */
    std::optional<Grammar> grammar{};

    /**
     * @brief Whether a later file's grammar differs from the first file's.
     */
    bool is_mixed{false};
};

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
 * @brief A directory, root, removed with everything under it when the object is destroyed.
 */
struct Removed_on_exit
{
    /**
     * @brief Removes the directory and everything under it, ignoring a failure.
     */
    ~Removed_on_exit()
    {
        std::error_code ignored{};

        std::filesystem::remove_all(root, ignored);
    }

    /**
     * @brief The directory.
     */
    std::filesystem::path root{};
};

/**
 * @brief Returns the grammar's name in the CSV's grammar column.
 * @param grammar The grammar.
 * @return `rfc8259` or `consumption-complete-c`.
 */
std::string_view name_of(const Grammar grammar)
{
    return grammar == Grammar::rfc8259 ? "rfc8259" : "consumption-complete-c";
}

/**
 * @brief Picks the RFC 8259 row's lexer for Grammar::rfc8259 and the consumption-complete C row's otherwise.
 * @param lexers The two rows' lexers.
 * @param grammar The grammar.
 * @return The grammar's lexer.
 */
const munch::core::Lexer& lexer_of(const Row_lexers& lexers, const Grammar grammar)
{
    const auto& [json_row, c_row]{lexers};

    return grammar == Grammar::rfc8259 ? json_row : c_row;
}

/**
 * @brief Scans an input, counting tokens.
 * @param lexer The lexer.
 * @param input The input.
 * @return The consumed byte count and the number of tokens.
 */
Token_count count_tokens(const munch::core::Lexer& lexer, const std::string_view input)
{
    std::size_t tokens{0};

    const auto count{[&tokens](const Token, const std::size_t) { ++tokens; }};

    const auto consumed{lexer.tokenize_all<Token>(input, count)};

    return {.consumed = consumed, .tokens = tokens};
}

/**
 * @brief Scans an input, recording each token with its length.
 * @param lexer The lexer.
 * @param input The input.
 * @return The consumed byte count and the token stream.
 */
Token_stream stream_tokens(const munch::core::Lexer& lexer, const std::string_view input)
{
    Stream_t tokens{};

    const auto record{[&tokens](const Token token, const std::size_t length) { tokens.emplace_back(token, length); }};

    const auto consumed{lexer.tokenize_all<Token>(input, record)};

    return {.consumed = consumed, .tokens = std::move(tokens)};
}

/**
 * @brief Plans an input with the shipped window planner and measures the plan against equal division.
 * @param lexer The lexer planning.
 * @param input The input.
 * @param chunks The chunks requested.
 * @return The achieved chunks, the balance, the sorted boundary deviations and the boundaries.
 */
Plan plan(const munch::core::Lexer& lexer, const std::string_view input, const std::size_t chunks)
{
    auto bounds{lexer.chunk_boundaries_with_windows(input, chunks)};

    const auto achieved{bounds.size() - 1};

    const auto chunk_size{[](const std::size_t begin, const std::size_t end) { return end - begin; }};

    const auto sizes{bounds | std::views::adjacent_transform<2>(chunk_size)};

    const auto largest{std::ranges::max(sizes)};

    const auto ideal{static_cast<double>(input.size()) / static_cast<double>(achieved)};

    const auto balance{static_cast<double>(largest) / ideal};

    std::vector<std::size_t> deviations{};

    for (std::size_t index{1}; index + 1 < bounds.size(); ++index)
    {
        const auto target{index * input.size() / achieved};

        const auto deviation{bounds[index] > target ? bounds[index] - target : target - bounds[index]};

        deviations.push_back(deviation);
    }

    std::ranges::sort(deviations);

    return {.chunks = achieved, .balance = balance, .deviations = std::move(deviations), .bounds = std::move(bounds)};
}

/**
 * @brief Returns one quantile of a sorted sample in integer arithmetic, `sorted[min(n - 1, n * numerator /
 *        denominator)]`.
 * @param sorted The sample, sorted.
 * @param numerator The quantile's numerator.
 * @param denominator The quantile's denominator.
 * @return The quantile, 0 for an empty sample.
 */
std::size_t quantile(const std::vector<std::size_t>& sorted, const std::size_t numerator, const std::size_t denominator)
{
    if (sorted.empty())
    {
        return 0;
    }

    const auto rank{sorted.size() * numerator / denominator};

    const auto index{std::min(sorted.size() - 1, rank)};

    return sorted[index];
}

/**
 * @brief Summarizes a plan's sorted boundary deviations by their median, 95th percentile and largest.
 * @param deviations The deviations, sorted.
 * @return The summary.
 */
Deviation_summary summarize(const std::vector<std::size_t>& deviations)
{
    const auto median{quantile(deviations, 1, 2)};

    const auto p95{quantile(deviations, 95, 100)};

    const auto largest{deviations.empty() ? 0 : deviations.back()};

    return {.median = median, .p95 = p95, .largest = largest};
}

/**
 * @brief Writes one CSV row: its label, the grammar, the input's size, the bytes consumed, the chunks requested, and
 *        the plan's achieved chunks, balance and deviation summary.
 * @param csv The CSV, open.
 * @param label The row's first column: a file's path, or `STREAM`.
 * @param grammar The grammar the input is read under.
 * @param bytes The input's size.
 * @param consumed The bytes the scan consumed.
 * @param chunks The chunks requested.
 * @param planned The plan.
 */
void write_csv_row(
        Output_file& csv, const std::string& label, const Grammar grammar, const std::size_t bytes,
        const std::size_t consumed, const std::size_t chunks, const Plan& planned)
{
    const auto& [achieved, balance, deviations, bounds]{planned};

    const auto [median, p95, largest]{summarize(deviations)};

    const std::string grammar_name{name_of(grammar)};

    std::fprintf(
            csv.stream(), "%s,%s,%zu,%zu,%zu,%zu,%.5f,%zu,%zu,%zu\n", label.c_str(), grammar_name.c_str(), bytes,
            consumed, chunks, achieved, balance, median, p95, largest);
}

/**
 * @brief Scans and plans every file under its grammar, writes one CSV row per file when the CSV is open, and
 *        concatenates the files into the stream.
 * @param files The files, sorted; an unreadable one is read as empty.
 * @param lexers The two rows' lexers.
 * @param chunks The chunks requested of each plan.
 * @param csv The CSV, written only when open.
 * @return The stream, its grammar, and whether its files mix grammars.
 */
Campaign_stream file_rows(
        const std::vector<std::filesystem::path>& files, const Row_lexers& lexers, const std::size_t chunks,
        Output_file& csv)
{
    Campaign_stream stream{};

    auto& [stream_bytes, stream_grammar, is_mixed]{stream};

    const auto grammar_of{[](const std::filesystem::path& path) {
        return path.extension() == ".json" ? Grammar::rfc8259 : Grammar::consumption_complete_c;
    }};

    for (const auto& path : files)
    {
        const auto grammar{grammar_of(path)};

        const auto& lexer{lexer_of(lexers, grammar)};

        // The stream is scanned and planned with one lexer, so a second grammar in the corpus leaves it none.
        is_mixed = is_mixed || stream_grammar.value_or(grammar) != grammar;

        if (!stream_grammar)
        {
            stream_grammar = grammar;
        }

        const auto text{read_bytes(path).value_or("")};

        const auto [consumed, tokens]{count_tokens(lexer, text)};

        const auto planned{plan(lexer, text, chunks)};

        if (csv.is_open())
        {
            write_csv_row(csv, path.string(), grammar, text.size(), consumed, chunks, planned);
        }

        stream_bytes.append(text);

        stream_bytes += '\n';
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
        const Campaign_stream& stream, const munch::core::Lexer& lexer, const std::size_t files,
        const std::size_t chunks, Output_file& csv)
{
    const auto& [bytes, grammar, is_mixed]{stream};

    const auto [consumed, serial_tokens]{count_tokens(lexer, bytes)};

    const auto planned{plan(lexer, bytes, chunks)};

    const auto& [achieved, balance, deviations, bounds]{planned};

    const auto [median, p95, largest]{summarize(deviations)};

    std::cout << "stream: " << files << " files, " << bytes.size() << " bytes, consumed " << consumed << ", chunks "
              << achieved << "/" << chunks << ", balance " << balance << ", deviations median " << median << " p95 "
              << p95 << " max " << largest << "\n";

    if (csv.is_open())
    {
        write_csv_row(csv, "STREAM", *grammar, bytes.size(), consumed, chunks, planned);

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
    const auto certified{[&lexer](const char byte) { return lexer.is_split_point(byte); }};

    const auto count{std::ranges::count_if(every_byte(), certified)};

    return static_cast<std::size_t>(count);
}

/**
 * @brief Counts the certified two-byte windows, and those whose first byte is one of the given whitespace bytes.
 * @param lexer The lexer.
 * @param whitespace The bytes the grammar treats as whitespace.
 * @return The two counts.
 */
Census census_two(const munch::core::Lexer& lexer, const std::string_view whitespace)
{
    Census census{};

    auto& [windows, whitespace_anchored]{census};

    for (const auto first : every_byte())
    {
        for (const auto second : every_byte())
        {
            const std::array bytes{first, second};

            const std::string_view window{bytes.data(), bytes.size()};

            const auto certified{lexer.is_split_window(window).has_value()};

            if (!certified)
            {
                continue;
            }

            ++windows;

            if (whitespace.contains(first))
            {
                ++whitespace_anchored;
            }
        }
    }

    return census;
}

/**
 * @brief Generates C-like source of eight statement shapes drawn from an Lcg64 at a fixed seed, deterministic from that
 *        seed: preprocessor lines, strings with escapes, char literals, both comment forms, and ordinary statement
 *        text.
 * @param bytes The least size of the source; generation stops at the first statement reaching it.
 * @return The source.
 */
std::string generated_c(const std::size_t bytes)
{
    constexpr std::uint64_t seed{0x2545F4914F6CDD1DULL};

    Lcg64 lcg{seed};

    std::string out{};

    while (out.size() < bytes)
    {
        constexpr std::size_t statement_shapes{8};

        const auto shape{lcg.next(statement_shapes)};

        switch (shape)
        {
        case 0:
            out += "#define LIMIT_";
            out += pick_identifier(lcg);
            out += " 4096\n";

            break;

        case 1:
            out += "/* invariant: ";
            out += pick_identifier(lcg);
            out += " stays below the table size */\n";

            break;

        case 2:
            out += R"(static const char* name = "escaped \"quote\" and tab\t";)"
                   "\n";

            break;

        case 3:
            out += "if (";
            out += pick_identifier(lcg);
            out += R"( != '\n') { // resync at line end)"
                   "\n";

            break;

        case 4:
            out += "    ";
            out += pick_identifier(lcg);
            out += " = ";
            out += pick_identifier(lcg);
            out += " + 17;\n";

            break;

        case 5:
            out += "}\n";

            break;

        case 6:
            out += "int ";
            out += pick_identifier(lcg);
            out += "[128];\n";

            break;

        case 7:
            out += "    call(";
            out += pick_identifier(lcg);
            out += ", \"literal\", 3);\n";

            break;
        }
    }

    return out;
}

/**
 * @brief Writes a corpus mixing the two grammars, one JSON and one C file, into a new directory of a unique name made
 *        by mkdtemp beneath TMPDIR, or beneath /tmp when TMPDIR is unset or empty; exits the program with EXIT_FAILURE
 *        when mkdtemp fails.
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

    if (::mkdtemp(pattern.data()) == nullptr)
    {
        std::perror("mkdtemp");

        std::exit(EXIT_FAILURE);
    }

    const std::filesystem::path root{pattern};

    std::ofstream{root / "a.json"} << R"({"k": [1, 2]})"
                                      "\n";

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
    const Row_lexers lexers{.json_row = rfc_json(), .c_row = consumption_complete_c()};

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

    const auto& [bytes, grammar, is_mixed]{stream};

    if (!grammar)
    {
        std::cout << "campaign: no regular files under " << root << "\n";

        return false;
    }

    if (is_mixed)
    {
        std::cout << "campaign: mixed grammars under " << root
                  << ", stream row refused: no single certificate plans the aggregate\n";

        if (csv.is_open())
        {
            std::ignore = csv.close();
        }

        return false;
    }

    const auto& lexer{lexer_of(lexers, *grammar)};

    stream_row(stream, lexer, files.size(), chunks, csv);

    return true;
}

/**
 * @brief Asserts the corpus-free facts of the two published rows: neither certifies an exact byte; the RFC 8259 row's
 *        two-byte census and its structural poison-byte windows, pinned positively and negatively. Prints the census.
 * @param assertions The probe's assertions.
 */
void published_row_facts(Assertions& assertions)
{
    munch::core::Builder cumulative_builder{};

    published_cumulative_row(cumulative_builder);

    const auto cumulative_lexer{cumulative_builder.build()};

    assertions.expect(exact_bytes(cumulative_lexer) == 0, "published cumulative row certifies an exact byte");

    const auto json_lexer{rfc_json()};

    assertions.expect(exact_bytes(json_lexer) == 0, "RFC 8259 row certifies an exact byte");

    const auto [windows, whitespace_anchored]{census_two(json_lexer, whitespace_bytes)};

    std::cout << "json two-byte windows " << windows << ", whitespace-anchored " << whitespace_anchored << "\n";

    assertions.expect(windows == 120, "RFC 8259 two-byte census moved");

    assertions.expect(whitespace_anchored == 63, "the control-whitespace-first family moved");

    // Space is a legal string interior, so it poisons nothing: no two-byte window uses it on either side.
    assertions.expect(!json_lexer.is_split_window(R"( ")").has_value(), "space-first window certifies");

    assertions.expect(!json_lexer.is_split_window(R"(" )").has_value(), "space-second window certifies");

    // Control whitespace is excluded from unescaped interiors, so it anchors in both directions.
    assertions.expect(
            json_lexer.is_split_window("\"\t") == std::optional<std::size_t>{1},
            "token-final-then-tab window does not certify");

    // The structural poison-byte mechanism, pinned positively and negatively.
    assertions.expect(
            json_lexer.is_split_window(R"(,"v)") == std::optional<std::size_t>{1},
            R"(structural window {,"v} does not certify at origin 1)");

    assertions.expect(
            json_lexer.is_split_window(R"(t":)") == std::optional<std::size_t>{2},
            R"(structural window {t":} does not certify at origin 2)");

    assertions.expect(!json_lexer.is_split_window(R"(,")").has_value(), R"(two-byte prefix {,"} certifies)");

    assertions.expect(!json_lexer.is_split_window(R"(",)").has_value(), R"(two-byte {",} certifies)");

    assertions.expect(
            !json_lexer.is_split_window(R"(,"9)").has_value(), R"({,"9} certifies although 9 can begin a Number)");
}

/**
 * @brief Asserts that the consumption-complete C row certifies no exact byte and no two-byte window.
 * @param assertions The probe's assertions.
 * @param c_lexer The consumption-complete C row's lexer.
 */
void c_row_facts(Assertions& assertions, const munch::core::Lexer& c_lexer)
{
    assertions.expect(exact_bytes(c_lexer) == 0, "consumption-complete C row certifies an exact byte");

    const auto [windows, whitespace_anchored]{census_two(c_lexer, whitespace_bytes)};

    assertions.expect(windows == 0, "the consumption-complete C row gained a two-byte window");
}

/**
 * @brief Asserts on the generated corpus: complete consumption, a full plan at eight chunks, and the spliced chunks'
 *        (token, length) stream equal to the serial scan's, with the corpus size and the token count pinned. Prints the
 *        plan.
 * @param assertions The probe's assertions.
 * @param c_lexer The consumption-complete C row's lexer.
 */
void generated_corpus_splices(Assertions& assertions, const munch::core::Lexer& c_lexer)
{
    constexpr std::size_t corpus_bytes{256 * 1024};

    const auto corpus{generated_c(corpus_bytes)};

    assertions.expect(corpus.size() == 262'194, "generated corpus size moved");

    const auto [serial_consumed, serial_stream]{stream_tokens(c_lexer, corpus)};

    assertions.expect(serial_consumed == corpus.size(), "generated corpus does not consume completely");

    assertions.expect(serial_stream.size() == 74'838, "generated corpus token count moved");

    const auto [achieved, balance, deviations, bounds]{plan(c_lexer, corpus, planned_chunks)};

    std::cout << "generated corpus chunks " << achieved << ", balance " << balance << "\n";

    assertions.expect(achieved == planned_chunks, "generated corpus does not plan eight chunks");

    assertions.expect(balance < 1.10, "generated corpus balance exceeds 1.10");

    Stream_t spliced_stream{};

    auto consumed_all{true};

    for (const auto chunk : chunks_of(corpus, bounds))
    {
        const auto [consumed, tokens]{stream_tokens(c_lexer, chunk)};

        spliced_stream.insert(spliced_stream.end(), tokens.begin(), tokens.end());

        consumed_all = consumed == chunk.size() && consumed_all;
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

    assertions.expect(
            !campaign(scratch.root, self_test_chunks, std::nullopt), "a mixed corpus planned its aggregate stream");

    std::filesystem::remove(scratch.root / "b.c");

    assertions.expect(
            campaign(scratch.root, self_test_chunks, std::nullopt),
            "a single-grammar corpus refused its aggregate stream");
}

} // namespace

/**
 * @brief Runs a campaign over the directory, chunk count and CSV path given, or else the self-test of the grammar
 *        facts, the generated corpus and the mixed-corpus refusal, and prints the verdict.
 * @param argc The argument count.
 * @param argv The directory, the chunks requested (8 by default) and the CSV path, all optional.
 * @return EXIT_SUCCESS after a campaign or a self-test whose assertions hold, EXIT_FAILURE when a self-test assertion
 *         fails.
 */
int main(const int argc, char** argv)
{
    Assertions assertions{};

    if (argc > 1)
    {
        const std::size_t chunks{argc > 2 ? std::strtoull(argv[2], nullptr, 10) : planned_chunks};

        const auto csv_path{argc > 3 ? std::optional<std::filesystem::path>{argv[3]} : std::nullopt};

        std::ignore = campaign(argv[1], chunks, csv_path);

        return EXIT_SUCCESS;
    }

    published_row_facts(assertions);

    const auto c_lexer{consumption_complete_c()};

    c_row_facts(assertions, c_lexer);

    generated_corpus_splices(assertions, c_lexer);

    stream_row_refuses_mixed(assertions);

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? EXIT_FAILURE : EXIT_SUCCESS;
}

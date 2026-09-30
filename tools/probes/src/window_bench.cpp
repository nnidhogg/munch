// Measures what a certified split window is worth as a parallel cut, on the grammar the window search rescues:
// C-like with string literals, whose exact single-byte certificate is empty, so byte planning cannot cut it. The probe
// plans chunk boundaries at window-recovered origins, proves the chunked stream equals the serial one before any
// clock starts, and only then times the comparison; it does the same on the split-friendly grammar with strings,
// where newline is exactly certified, pricing the window plan against the shipped byte planner on one corpus.
//
// What it checks. The premise that the first grammar certifies no byte and the second certifies newline; that every
// planned boundary lands on a token start of the serial scan; that the chunks' concatenated (kind, length) stream
// equals the serial one element for element; and that every timed pass reproduces the serial token count and
// checksum. Any failure exits 1, as does a failed CSV open or write.
//
// Usage: munch_window_bench [size MiB [passes [csv path | -]]] [occurrence file...]
// A first argument std::atoi reads as positive is the size (16 MiB by default), then a second so read is the passes
// (5), then a third is the CSV the observations are appended to, `-` for none; every remaining argument is a file
// whose certified window occurrences are counted, and a file that cannot be read is skipped. Every number printed is
// run-local: the CSV and stdout carry commit and dirty-state provenance, and no figure from a casual run may be
// quoted without the collect.sh ritual on a quiet machine.
//
// The window model is window_model's, the one window_gate.cpp states and proves in its header comment: the
// representation lemma, the soundness argument and the quotient. The gate asserts the model against the scanner, and
// this probe asserts that every boundary it plans lands on a token start of the serial scan it then reproduces.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/tools/benchmark/provenance.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Builder_dbg;
using munch::tools::probes::certified_pairs;
using munch::tools::probes::is_init_reentrant;
using munch::tools::probes::live_states;
using munch::tools::probes::read_bytes;
using munch::tools::probes::States_t;

/**
 * @brief One scan's observable workload: its tokens counted, their kinds hashed in stream order, its bytes consumed.
 */
struct Tally
{
    /**
     * @brief The tokens the scan emitted, one per call of its sink.
     */
    std::size_t tokens{0};

    /**
     * @brief The token kinds in stream order as a polynomial hash of base 31.
     */
    std::size_t checksum{0};

    /**
     * @brief The bytes the scan consumed, as tokenize_all() reports them.
     */
    std::size_t consumed{0};
};

/**
 * @brief The chunks every plan asks for.
 */
constexpr std::size_t kChunks{8};

/**
 * @brief The origin table's entry for a two-byte window that is not certified.
 */
constexpr unsigned char kNotCertified{0xff};

/**
 * @brief The serial scan of a corpus, the stream every plan must reproduce.
 */
struct Reference
{
    /**
     * @brief Every token's kind, in stream order.
     */
    std::vector<unsigned char> kinds;

    /**
     * @brief Every token's length, in stream order.
     */
    std::vector<std::uint32_t> lengths;

    /**
     * @brief Per offset of the corpus, whether a token begins there.
     */
    std::vector<bool> begins;

    /**
     * @brief The serial scan's token count, checksum and consumed bytes, which every chunked scan must equal.
     */
    Tally tally;
};

/**
 * @brief The origin table of the certified two-byte windows.
 */
struct Origins
{
    /**
     * @brief Per two-byte window, at the first byte times 256 plus the second, the offset inside it at which a token
     *        begins, kNotCertified where the window is not certified.
     */
    std::vector<unsigned char> of_pair;

    /**
     * @brief The count of certified two-byte windows, the entries of of_pair that are not kNotCertified.
     */
    std::size_t windows{0};
};

/**
 * @brief One timed scan: its tally and its wall-clock time.
 */
struct Timed_scan
{
    /**
     * @brief What the scan returned: its token count, checksum and consumed bytes.
     */
    Tally tally;

    /**
     * @brief The scan's wall-clock time.
     */
    std::chrono::steady_clock::duration elapsed{};
};

/**
 * @brief One of the benchmark's two grammars, C-like with string literals, compiled: the automaton the window model
 *        walks and the lexer that scans.
 */
struct Bench_grammar
{
    /**
     * @brief The automaton compiled from the grammar, not unrolled, which the window model walks.
     */
    munch::dfa::Dfa dfa;

    /**
     * @brief The lexer built from the same grammar.
     */
    munch::core::Lexer lexer;

    /**
     * @brief The automaton's trim states.
     */
    States_t live;

    /**
     * @brief Whether a live transition re-enters the initial state.
     */
    bool reentrant{false};
};

/**
 * @brief The benchmark's command line: the generated corpus's size, the timed passes, the CSV and the occurrence
 *        files.
 */
struct Bench_options
{
    /**
     * @brief The generated corpus's size in MiB.
     */
    std::size_t size_mib{16};

    /**
     * @brief The timed passes of each measurement.
     */
    int passes{5};

    /**
     * @brief The CSV the observations are appended to, empty for none.
     */
    std::string csv_path;

    /**
     * @brief The files whose certified window occurrences are counted.
     */
    std::vector<std::string> occurrence_files;
};

/**
 * @brief The observations a run appends to its CSV, one row per timed plan or pass under the run's stamp, the commit
 *        and the dirty state; a failed open or write is printed and fails the run.
 */
class Observations
{
public:
    /**
     * @brief Starts a run stamped with the current time.
     * @param csv_path The CSV appended to, empty for none.
     * @param input_mib The generated corpus's size in MiB, written on every row.
     */
    Observations(std::string csv_path, std::size_t input_mib);

    /**
     * @brief Appends one row to the CSV, the header first when the file is absent or empty, at round-trip precision;
     *        nothing without a CSV. Prints `CSV OPEN FAILED` or `CSV WRITE FAILED` with the path when the open or the
     *        write fails.
     * @param scenario The scenario's name.
     * @param pass The pass index, -1 for a plan.
     * @param seconds The timed seconds.
     * @param mib_s The throughput in MiB/s, 0 for a plan.
     */
    void row(std::string_view scenario, int pass, double seconds, double mib_s);

    /**
     * @brief Whether every open and write of the CSV so far succeeded.
     * @return False after the first failure.
     */
    [[nodiscard]] bool is_healthy() const noexcept { return healthy_; }

private:
    /**
     * @brief The CSV appended to, empty for none.
     */
    std::string csv_path_;

    /**
     * @brief The generated corpus's size in MiB.
     */
    std::size_t input_mib_;

    /**
     * @brief The run's stamp, the time the run started in seconds since the epoch.
     */
    std::size_t run_;

    /**
     * @brief Whether every open and write so far succeeded.
     */
    bool healthy_{true};
};

/**
 * @brief The origin table's index of a two-byte window.
 * @param first The window's first byte.
 * @param second The window's second byte.
 * @return The first byte times 256 plus the second.
 */
std::size_t pair_index(const char first, const char second)
{
    return static_cast<std::size_t>(static_cast<unsigned char>(first)) * 256 +
           static_cast<std::size_t>(static_cast<unsigned char>(second));
}

/**
 * @brief 31 raised to a power, modulo 2 to the width of std::size_t, by repeated squaring.
 * @param exponent The power.
 * @return The power of 31, which splices a chunk's checksum after the checksums before it.
 */
std::size_t pow31(std::size_t exponent)
{
    std::size_t result{1};

    std::size_t base{31};

    for (; exponent != 0; exponent >>= 1U)
    {
        if ((exponent & 1U) != 0)
        {
            result *= base;
        }

        base *= base;
    }

    return result;
}

/**
 * @brief Whether a scan reproduced the reference stream: the same bytes consumed, the same tokens counted, the same
 *        checksum.
 * @param scanned The scan's tally.
 * @param reference The serial reference's tally.
 * @return True when the three agree.
 */
bool is_same_stream(const Tally& scanned, const Tally& reference)
{
    return scanned.consumed == reference.consumed && scanned.checksum == reference.checksum &&
           scanned.tokens == reference.tokens;
}

/**
 * @brief Runs one scan under the steady clock.
 * @param scan The scan, a callable returning its Tally.
 * @return The scan's tally and its time.
 */
template <typename Scan>
Timed_scan timed(const Scan& scan)
{
    const auto started{std::chrono::steady_clock::now()};

    const auto tally{scan()};

    return {.tally = tally, .elapsed = std::chrono::steady_clock::now() - started};
}

/**
 * @brief Fills the origin table from every certified two-byte window of an automaton.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 * @return The origin table and the count of certified windows.
 */
Origins origins_of(const munch::dfa::Dfa& dfa, const States_t& live, const bool reentrant)
{
    Origins origins{.of_pair = std::vector<unsigned char>(256 * 256, kNotCertified), .windows = 0};

    for (const auto& [pair, at] : certified_pairs(dfa, live, reentrant))
    {
        origins.of_pair[pair_index(pair[0], pair[1])] = static_cast<unsigned char>(at);

        ++origins.windows;
    }

    return origins;
}

/**
 * @brief Every offset a certified window occurrence says a token begins at: per position whose two bytes form a
 *        certified window, the position plus the window's origin.
 * @param bytes The bytes scanned for occurrences.
 * @param origins The origin table.
 * @return The offsets, one per occurrence, in increasing order of position.
 */
std::vector<std::size_t> window_boundaries(const std::string& bytes, const Origins& origins)
{
    std::vector<std::size_t> boundaries{};

    for (std::size_t at{0}; at + 1 < bytes.size(); ++at)
    {
        if (const auto origin{origins.of_pair[pair_index(bytes[at], bytes[at + 1])]}; origin != kNotCertified)
        {
            boundaries.push_back(at + origin);
        }
    }

    return boundaries;
}

/**
 * @brief Generates the deterministic source-shaped corpus both measurements scan: identifier-heavy lines of numbers,
 *        `+` operators, `;` punctuation and string literals, drawn from a fixed 32-bit stream, cut back to its last
 *        complete line and padded with newlines to the size asked.
 * @param bytes The corpus size.
 * @return The corpus, the same bytes on every run.
 */
std::string source_corpus(const std::size_t bytes)
{
    std::string out{};

    out.reserve(bytes + 128);

    unsigned seed{0x2c9277b5U};

    const auto next{[&seed] {
        seed = seed * 1664525U + 1013904223U;

        return (seed >> 16U) & 0x7fffU;
    }};

    static constexpr std::string_view words[]{"count", "offset", "state", "token", "chunk", "origin", "table", "index"};

    while (out.size() < bytes)
    {
        const auto pieces{3 + next() % 6};

        for (std::size_t piece{0}; piece < pieces; ++piece)
        {
            switch (next() % 8)
            {
            case 0:
                out += std::to_string(next());

                break;

            case 1:
                out += '"';

                out += words[next() % 8];

                out += ' ';

                out += words[next() % 8];

                out += '"';

                break;

            case 2:
                out += "+ ";

                out += words[next() % 8];

                break;

            case 3:
                out += words[next() % 8];

                out += ';';

                break;

            default:
                out += words[next() % 8];

                out += ' ';

                break;
            }

            out += ' ';
        }

        out += '\n';
    }

    // Cuts back to the last complete line, so no string literal is left open, then pads with newlines to the size.
    const auto last_newline{out.rfind('\n', bytes - 1)};

    out.resize(last_newline + 1);

    out.append(bytes - out.size(), '\n');

    return out;
}

/**
 * @brief Scans a corpus serially, recording every token's kind and length, the offsets at which tokens begin, and the
 *        tally the chunked scans are compared with.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @return The serial reference; its tally's consumed count is below the corpus size when the corpus does not
 *         tokenize.
 */
Reference serial_reference(const munch::core::Lexer& lexer, const std::string& corpus)
{
    Reference reference{.kinds{}, .lengths{}, .begins = std::vector<bool>(corpus.size(), false), .tally{}};

    std::size_t offset{0};

    reference.tally.consumed =
            lexer.tokenize_all<Token>(corpus, [&reference, &offset](const Token token, const std::size_t length) {
                reference.begins[offset] = true;

                offset += length;

                reference.kinds.push_back(static_cast<unsigned char>(token));

                reference.lengths.push_back(static_cast<std::uint32_t>(length));

                reference.tally.checksum = reference.tally.checksum * 31 + static_cast<std::size_t>(token);

                ++reference.tally.tokens;
            });

    return reference;
}

/**
 * @brief The chunk edges of a window plan: 0, per interior division of kChunks equal parts the first boundary at or
 *        after it that lies past the edge before, and the corpus size.
 * @param corpus_size The corpus size.
 * @param boundaries The window boundaries, in increasing order.
 * @return The edges, at most kChunks + 1 of them, increasing.
 */
std::vector<std::size_t> nearest_edges(const std::size_t corpus_size, const std::vector<std::size_t>& boundaries)
{
    std::vector<std::size_t> edges{0};

    for (std::size_t chunk{1}; chunk < kChunks; ++chunk)
    {
        const auto desired{corpus_size * chunk / kChunks};

        const auto nearest{std::ranges::lower_bound(boundaries, desired)};

        if (nearest != boundaries.end() && *nearest > edges.back())
        {
            edges.push_back(*nearest);
        }
    }

    edges.push_back(corpus_size);

    return edges;
}

/**
 * @brief The first interior edge of a plan at which no token of the serial scan begins.
 * @param edges The plan's chunk edges.
 * @param reference The serial reference.
 * @return The edge, std::nullopt when every interior edge begins a token.
 */
std::optional<std::size_t> first_non_token_start(const std::vector<std::size_t>& edges, const Reference& reference)
{
    for (std::size_t edge{1}; edge + 1 < edges.size(); ++edge)
    {
        if (!reference.begins[edges[edge]])
        {
            return edges[edge];
        }
    }

    return std::nullopt;
}

/**
 * @brief Scans the corpus in the chunks the edges delimit, one jthread per interior chunk and the last chunk on the
 *        calling thread, each chunk hashing and counting its tokens into its own cache-line-aligned slot, and splices
 *        the chunks' tallies in stream order after the join. Every timed chunked path, byte-planned and
 *        window-planned alike, scans through it.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param edges The chunk edges, at least two.
 * @return The spliced tally: equal to the serial one exactly when the chunked stream hashes and counts alike.
 */
Tally chunked_scan(const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges)
{
    struct alignas(64) Padded
    {
        Tally tally;
    };

    std::vector<Padded> tallies(edges.size() - 1);

    {
        std::vector<std::jthread> workers{};

        for (std::size_t chunk{0}; chunk + 2 < edges.size(); ++chunk)
        {
            workers.emplace_back([&, chunk] {
                const std::string_view piece{corpus.data() + edges[chunk], edges[chunk + 1] - edges[chunk]};

                auto& mine{tallies[chunk].tally};

                mine.consumed = lexer.tokenize_all<Token>(piece, [&mine](const Token token, const std::size_t) {
                    mine.checksum = mine.checksum * 31 + static_cast<std::size_t>(token);

                    ++mine.tokens;
                });
            });
        }

        const auto last{edges.size() - 2};

        const std::string_view piece{corpus.data() + edges[last], edges[last + 1] - edges[last]};

        auto& mine{tallies[last].tally};

        mine.consumed = lexer.tokenize_all<Token>(piece, [&mine](const Token token, const std::size_t) {
            mine.checksum = mine.checksum * 31 + static_cast<std::size_t>(token);

            ++mine.tokens;
        });
    }

    Tally total{};

    for (const auto& [tally] : tallies)
    {
        total.checksum = total.checksum * pow31(tally.tokens) + tally.checksum;

        total.tokens += tally.tokens;

        total.consumed += tally.consumed;
    }

    return total;
}

/**
 * @brief The untimed proof of a plan: the chunks' concatenated (kind, length) stream equals the serial one element for
 *        element, and every chunk consumes whole.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param edges The plan's chunk edges.
 * @param reference The serial reference.
 * @return True when every chunk consumes whole and the streams are equal.
 */
bool exact_match(
        const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges,
        const Reference& reference)
{
    std::size_t ordinal{0};

    auto matched{true};

    for (std::size_t chunk{0}; chunk + 1 < edges.size(); ++chunk)
    {
        const std::string_view piece{corpus.data() + edges[chunk], edges[chunk + 1] - edges[chunk]};

        const auto consumed{lexer.tokenize_all<Token>(piece, [&](const Token token, const std::size_t length) {
            matched = matched && ordinal < reference.kinds.size() &&
                      static_cast<unsigned char>(token) == reference.kinds[ordinal] &&
                      length == reference.lengths[ordinal];

            ++ordinal;
        })};

        if (consumed != piece.size())
        {
            return false;
        }
    }

    return matched && ordinal == reference.kinds.size();
}

/**
 * @brief Times two scans of one corpus pass by pass, the first scan first on even passes and second on odd ones, and
 *        reports each pass's two times once both have run.
 * @param passes The passes.
 * @param reference The serial reference's tally every scan must reproduce.
 * @param first The first scan, a callable returning its Tally.
 * @param second The second scan, a callable returning its Tally.
 * @param report Called after each pass with the pass index and the first and the second scan's times.
 * @return True when every scan of every pass reproduced the reference.
 */
template <typename First, typename Second, typename Report>
bool timed_passes(
        const int passes, const Tally& reference, const First& first, const Second& second, const Report& report)
{
    auto agreed{true};

    for (int pass{0}; pass < passes; ++pass)
    {
        Timed_scan first_run{};

        Timed_scan second_run{};

        if (pass % 2 == 0)
        {
            first_run = timed(first);

            second_run = timed(second);
        }
        else
        {
            second_run = timed(second);

            first_run = timed(first);
        }

        agreed = agreed && is_same_stream(first_run.tally, reference) && is_same_stream(second_run.tally, reference);

        report(pass, first_run.elapsed, second_run.elapsed);
    }

    return agreed;
}

/**
 * @brief A wall-clock duration in seconds.
 * @param elapsed The duration.
 * @return The seconds.
 */
double elapsed_s(const std::chrono::steady_clock::duration elapsed)
{
    return std::chrono::duration<double>(elapsed).count();
}

/**
 * @brief The throughput of scanning some bytes in a duration.
 * @param bytes The bytes scanned.
 * @param elapsed The duration.
 * @return The MiB per second.
 */
double mib_per_s(const std::size_t bytes, const std::chrono::steady_clock::duration elapsed)
{
    const auto seconds{std::chrono::duration<double>(elapsed).count()};

    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

/**
 * @brief Compiles C-like with string literals, the strings at priority 2.
 * @param split_friendly Whether the C-like base is the split-friendly one, newline its own token.
 * @return The automaton, the lexer, the trim states and whether the initial state is re-entered.
 */
Bench_grammar bench_grammar(const bool split_friendly)
{
    Builder_dbg builder{};

    figures::c_like(builder, split_friendly);

    builder.add_token(figures::string_literal(), Token::String, 2);

    auto dfa{builder.dfa()};

    auto live{live_states(dfa)};

    const auto reentrant{is_init_reentrant(dfa, live)};

    return {.dfa = std::move(dfa), .lexer = builder.build(), .live = std::move(live), .reentrant = reentrant};
}

/**
 * @brief Reads the command line positionally: a first argument std::atoi reads as positive is the size in MiB, then a
 *        second one read so is the passes, then a third is the CSV path, `-` for none; every argument after those
 *        taken is an occurrence file.
 * @param arguments The arguments after the program name.
 * @return The options, each one not given at its default: 16 MiB, 5 passes, no CSV.
 */
Bench_options options_of(const std::vector<std::string>& arguments)
{
    Bench_options options{};

    std::size_t taken{0};

    if (!arguments.empty() && std::atoi(arguments[0].c_str()) > 0)
    {
        options.size_mib = static_cast<std::size_t>(std::atoi(arguments[0].c_str()));

        taken = 1;

        if (arguments.size() > 1 && std::atoi(arguments[1].c_str()) > 0)
        {
            options.passes = std::atoi(arguments[1].c_str());

            taken = 2;

            if (arguments.size() > 2)
            {
                options.csv_path = arguments[2] == "-" ? std::string{} : arguments[2];

                taken = 3;
            }
        }
    }

    options.occurrence_files.assign(arguments.begin() + static_cast<std::ptrdiff_t>(taken), arguments.end());

    return options;
}

/**
 * @brief Prints the run's banner: the commit with its dirty state, the corpus size, the passes and whether the
 *        observations are recorded.
 * @param options The run's options.
 */
void print_banner(const Bench_options& options)
{
    using munch::tools::benchmark::kCommit;

    using munch::tools::benchmark::kDirty;

    std::printf("window-recovered parallel cuts, preview of the split-windows benchmark campaign\n");

    std::printf(
            "  commit %s%s, %zu MiB, %d passes%s\n", kCommit, kDirty ? " (uncommitted changes present)" : "",
            options.size_mib, options.passes,
            options.csv_path.empty() ? ", DEV RUN, observations discarded" : ", observations recorded");
}

/**
 * @brief Checks the premise of the no-byte measurement: the lexer certifies no byte exactly. Prints `PREMISE MOVED`
 *        with the first byte it certifies.
 * @param lexer The lexer.
 * @return True when no byte is exactly certified.
 */
bool has_no_exact_byte(const munch::core::Lexer& lexer)
{
    for (int symbol{0}; symbol < 256; ++symbol)
    {
        if (lexer.is_split_point(static_cast<char>(symbol)))
        {
            std::printf(
                    "  PREMISE MOVED: byte %d is exactly certified, this grammar no longer needs windows\n", symbol);

            return false;
        }
    }

    return true;
}

/**
 * @brief Finds every certified two-byte window of a grammar into the origin table, timing the search, and prints the
 *        count and the time; prints that nothing is measured when no window is certified.
 * @param grammar The grammar.
 * @return The origin table, std::nullopt when no window is certified.
 */
std::optional<Origins> window_census(const Bench_grammar& grammar)
{
    const auto search_started{std::chrono::steady_clock::now()};

    auto origins{origins_of(grammar.dfa, grammar.live, grammar.reentrant)};

    const auto search_elapsed{std::chrono::steady_clock::now() - search_started};

    std::printf(
            "  certified two-byte windows: %zu, found in %.1f ms of post-construction analysis\n", origins.windows,
            std::chrono::duration<double, std::milli>(search_elapsed).count());

    if (origins.windows == 0)
    {
        std::printf("  no window certified, nothing to measure\n");

        return std::nullopt;
    }

    return origins;
}

/**
 * @brief Prints, per readable file, how often a certified window occurs in its bytes and the mean gap between
 *        occurrences; a file that cannot be opened prints nothing.
 * @param files The files.
 * @param origins The origin table.
 */
void report_occurrences(const std::vector<std::string>& files, const Origins& origins)
{
    for (const auto& file : files)
    {
        if (const auto data{read_bytes(file)})
        {
            const auto occurrences{window_boundaries(*data, origins).size()};

            std::printf(
                    "  %-40s %zu occurrences over %zu bytes, mean gap %.1f\n", file.c_str(), occurrences, data->size(),
                    occurrences ? static_cast<double>(data->size()) / occurrences : 0.0);
        }
    }
}

/**
 * @brief The measurement on the grammar no byte certifies: plans at most eight chunks at window-recovered origins,
 * holds the chunked stream to the serial one before any clock starts, and times the serial scan against the chunked one
 *        pass by pass, printing each figure and recording it.
 * @param grammar The C-like grammar with string literals.
 * @param origins Its origin table.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when the corpus tokenizes, the plan cuts at token starts and every scan reproduces the serial stream.
 */
bool no_byte_measurement(
        const Bench_grammar& grammar, const Origins& origins, const Bench_options& options, Observations& observations)
{
    const auto& lexer{grammar.lexer};

    const auto corpus{source_corpus(options.size_mib << 20U)};

    const auto reference{serial_reference(lexer, corpus)};

    if (reference.tally.consumed != corpus.size())
    {
        std::printf("  CORPUS NOT TOKENIZABLE: consumed %zu of %zu\n", reference.tally.consumed, corpus.size());

        return false;
    }

    const auto window_plan_started{std::chrono::steady_clock::now()};

    const auto boundaries{window_boundaries(corpus, origins)};

    std::printf(
            "  corpus: %zu bytes, %zu window occurrences, mean gap %.1f bytes\n", corpus.size(), boundaries.size(),
            boundaries.empty() ? 0.0 : static_cast<double>(corpus.size()) / boundaries.size());

    const auto edges{nearest_edges(corpus.size(), boundaries)};

    const auto window_plan_elapsed{std::chrono::steady_clock::now() - window_plan_started};

    if (const auto edge{first_non_token_start(edges, reference)})
    {
        std::printf("  BOUNDARY %zu IS NOT A TOKEN START, the certificate or the planner is wrong\n", *edge);

        return false;
    }

    if (!is_same_stream(chunked_scan(lexer, corpus, edges), reference.tally) ||
        !exact_match(lexer, corpus, edges, reference))
    {
        std::printf("  STREAMS DISAGREE: the window cut does not reproduce the serial scan\n");

        return false;
    }

    std::printf(
            "  streams identical: %zu tokens, spliced checksum equal, %zu chunks; window plan %.2f ms\n",
            reference.tally.tokens, edges.size() - 1, elapsed_s(window_plan_elapsed) * 1e3);

    observations.row("plan-window-no-byte", -1, elapsed_s(window_plan_elapsed), 0.0);

    Tally timed_serial{};

    const auto agreed{timed_passes(
            options.passes, reference.tally,
            [&] {
                timed_serial = {};

                timed_serial.consumed = lexer.tokenize_all<Token>(corpus, [&](const Token token, const std::size_t) {
                    timed_serial.checksum = timed_serial.checksum * 31 + static_cast<std::size_t>(token);

                    ++timed_serial.tokens;
                });

                return timed_serial;
            },
            [&] { return chunked_scan(lexer, corpus, edges); },
            [&](const int pass, const auto serial_elapsed, const auto chunked_elapsed) {
                observations.row(
                        "serial-no-byte", pass, elapsed_s(serial_elapsed), mib_per_s(corpus.size(), serial_elapsed));

                observations.row(
                        "window-scan-no-byte", pass, elapsed_s(chunked_elapsed),
                        mib_per_s(corpus.size(), chunked_elapsed));

                std::printf(
                        "  pass %d: serial %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, speedup %.2fx\n", pass,
                        mib_per_s(corpus.size(), serial_elapsed), edges.size() - 1,
                        mib_per_s(corpus.size(), chunked_elapsed),
                        elapsed_s(serial_elapsed) / elapsed_s(chunked_elapsed));
            })};

    if (!agreed)
    {
        std::printf("  A TIMED PASS DISAGREED with the serial reference\n");

        return false;
    }

    return true;
}

Observations::Observations(std::string csv_path, const std::size_t input_mib)
    : csv_path_{std::move(csv_path)}, input_mib_{input_mib}, run_{static_cast<std::size_t>(std::time(nullptr))}
{}

void Observations::row(const std::string_view scenario, const int pass, const double seconds, const double mib_s)
{
    if (csv_path_.empty())
    {
        return;
    }

    std::ifstream probe{csv_path_};

    const auto fresh{!probe.good() || probe.peek() == std::ifstream::traits_type::eof()};

    probe.close();

    std::ofstream csv{csv_path_, std::ios::app};

    if (!csv)
    {
        std::printf("  CSV OPEN FAILED: %s\n", csv_path_.c_str());

        healthy_ = false;

        return;
    }

    csv << std::setprecision(std::numeric_limits<double>::max_digits10);

    if (fresh)
    {
        csv << "run,commit,dirty,scenario,input_mib,pass,seconds,mib_per_s\n";
    }

    using munch::tools::benchmark::kCommit;

    using munch::tools::benchmark::kDirty;

    csv << run_ << ',' << kCommit << ',' << (kDirty ? "yes" : "no") << ',' << scenario << ',' << input_mib_ << ','
        << pass << ',' << seconds << ',' << mib_s << '\n';

    csv.flush();

    if (!csv)
    {
        std::printf("  CSV WRITE FAILED: %s\n", csv_path_.c_str());

        healthy_ = false;
    }
}

/**
 * @brief The same measurement on a grammar carrying both certificates, split-friendly C-like plus strings: newline is
 *        exactly certified there, so the shipped byte planner and the window planner cut the same corpus, both plans
 *        are held to the serial stream before any clock starts, and then their scans are timed against each other.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when newline is exactly certified, the corpus tokenizes, both plans cut at token starts and every
 *         scan of both plans reproduces the serial stream.
 */
bool byte_versus_window(const Bench_options& options, Observations& observations)
{
    std::printf("\nbyte-certified versus window-recovered cuts, same grammar, same corpus\n");

    const auto grammar{bench_grammar(true)};

    const auto& lexer{grammar.lexer};

    if (!lexer.is_split_point('\n'))
    {
        std::printf("  PREMISE MOVED: newline is no longer exactly certified on the split-friendly grammar\n");

        return false;
    }

    const auto corpus{source_corpus(options.size_mib << 20U)};

    const auto reference{serial_reference(lexer, corpus)};

    if (reference.tally.consumed != corpus.size())
    {
        std::printf(
                "  CORPUS NOT TOKENIZABLE by the split-friendly grammar: %zu of %zu\n", reference.tally.consumed,
                corpus.size());

        return false;
    }

    const auto origins{origins_of(grammar.dfa, grammar.live, grammar.reentrant)};

    const auto window_plan_started{std::chrono::steady_clock::now()};

    const auto edges{nearest_edges(corpus.size(), window_boundaries(corpus, origins))};

    const auto window_plan_elapsed{std::chrono::steady_clock::now() - window_plan_started};

    if (const auto edge{first_non_token_start(edges, reference)})
    {
        std::printf("  BOUNDARY %zu IS NOT A TOKEN START on the split-friendly grammar\n", *edge);

        return false;
    }

    const auto byte_plan_started{std::chrono::steady_clock::now()};

    const auto edges_byte{lexer.chunk_boundaries(corpus, kChunks)};

    const auto byte_plan_elapsed{std::chrono::steady_clock::now() - byte_plan_started};

    if (const auto edge{first_non_token_start(edges_byte, reference)})
    {
        std::printf("  BYTE BOUNDARY %zu IS NOT A TOKEN START\n", *edge);

        return false;
    }

    if (!is_same_stream(chunked_scan(lexer, corpus, edges), reference.tally) ||
        !is_same_stream(chunked_scan(lexer, corpus, edges_byte), reference.tally) ||
        !exact_match(lexer, corpus, edges, reference) || !exact_match(lexer, corpus, edges_byte, reference))
    {
        std::printf("  STREAMS DISAGREE between the planners and the serial scan\n");

        return false;
    }

    std::printf(
            "  %zu windows; both plans reproduce the serial stream of %zu tokens; plans: byte %.2f ms for %zu "
            "chunks, window %.2f ms for %zu\n",
            origins.windows, reference.tally.tokens, elapsed_s(byte_plan_elapsed) * 1e3, edges_byte.size() - 1,
            elapsed_s(window_plan_elapsed) * 1e3, edges.size() - 1);

    observations.row("plan-byte-split-friendly", -1, elapsed_s(byte_plan_elapsed), 0.0);

    observations.row("plan-window-split-friendly", -1, elapsed_s(window_plan_elapsed), 0.0);

    const auto agreed{timed_passes(
            options.passes, reference.tally, [&] { return chunked_scan(lexer, corpus, edges_byte); },
            [&] { return chunked_scan(lexer, corpus, edges); },
            [&](const int pass, const auto byte_elapsed, const auto window_elapsed) {
                observations.row(
                        "byte-scan-split-friendly", pass, elapsed_s(byte_elapsed),
                        mib_per_s(corpus.size(), byte_elapsed));

                observations.row(
                        "window-scan-split-friendly", pass, elapsed_s(window_elapsed),
                        mib_per_s(corpus.size(), window_elapsed));

                std::printf(
                        "  pass %d: byte-scan x%zu %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, ratio %.2f\n", pass,
                        edges_byte.size() - 1, mib_per_s(corpus.size(), byte_elapsed), edges.size() - 1,
                        mib_per_s(corpus.size(), window_elapsed), elapsed_s(byte_elapsed) / elapsed_s(window_elapsed));
            })};

    if (!agreed)
    {
        std::printf("  A TIMED PASS DISAGREED with the serial reference\n");

        return false;
    }

    return true;
}
} // namespace

/**
 * @brief Runs the benchmark: the premise that no byte is exactly certified, the window census, the occurrence files,
 *        the no-byte measurement and the byte-versus-window measurement, in turn.
 * @param argc The argument count.
 * @param argv The size in MiB, the passes, the CSV path or `-`, and the occurrence files, all optional.
 * @return 0 when every premise and stream equality holds and every CSV write succeeded, 1 otherwise.
 */
int main(const int argc, char** argv)
{
    std::vector<std::string> arguments{};

    for (int arg{1}; arg < argc; ++arg)
    {
        arguments.emplace_back(argv[arg]);
    }

    const auto options{options_of(arguments)};

    Observations observations{options.csv_path, options.size_mib};

    print_banner(options);

    const auto grammar{bench_grammar(false)};

    if (!has_no_exact_byte(grammar.lexer))
    {
        return 1;
    }

    const auto origins{window_census(grammar)};

    if (!origins)
    {
        return 1;
    }

    report_occurrences(options.occurrence_files, *origins);

    if (!no_byte_measurement(grammar, *origins, options, observations) || !byte_versus_window(options, observations))
    {
        return 1;
    }

    std::printf("\ndev-grade preview only: archived figures require the collect.sh ritual on a quiet machine\n");

    return observations.is_healthy() ? 0 : 1;
}

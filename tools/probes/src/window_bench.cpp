// Measures what a certified split window is worth as a parallel cut, on the grammar the window search rescues: C-like
// with string literals, whose exact single-byte certificate is empty, so byte planning cannot cut it. The probe plans
// chunk boundaries at window-recovered origins, proves the chunked stream equals the serial one before any clock
// starts, and only then times the comparison; it does the same on the split-friendly grammar with strings, where
// newline is exactly certified, pricing the window plan against the shipped byte planner on one corpus.
//
// What it checks. The premise that the first grammar certifies no byte and the second certifies newline; that every
// planned boundary lands on a token start of the serial scan; that the chunks' concatenated (kind, length) stream
// equals the serial one element for element; and that every timed pass reproduces the serial token count and checksum.
// Any failure exits 1, as does a failed CSV open or write.
//
// Usage: munch_window_bench [size MiB [passes [csv path | -]]] [occurrence file...]
// A first argument std::atoi reads as positive is the size (16 MiB by default), then a second so read is the passes
// (5), then a third is the CSV the observations are appended to, `-` for none; every remaining argument is a file whose
// certified window occurrences are counted, and a file that cannot be read is skipped. Every number printed is
// run-local: the CSV and stdout carry commit and dirty-state provenance, and no figure from a casual run may be quoted
// without the collect.sh ritual on a quiet machine.
//
// The window model is window_model's, the one window_gate.cpp states and proves in its header comment: the
// representation lemma, the soundness argument and the quotient. The gate asserts the model against the scanner, and
// this probe asserts that every boundary it plans lands on a token start of the serial scan it then reproduces.

#include <algorithm>
#include <array>
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
#include <ranges>
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
using munch::tools::benchmark::commit;
using munch::tools::benchmark::dirty;
using munch::tools::probes::Builder_dbg;
using munch::tools::probes::byte_count;
using munch::tools::probes::certified_pairs;
using munch::tools::probes::every_byte;
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
     * @brief Equal when the token count, the checksum and the consumed bytes all match.
     */
    bool operator==(const Tally&) const = default;

    /**
     * @brief The tokens the scan emitted, one per call of its sink.
     */
    std::size_t tokens{0};

    /**
     * @brief The token kinds in stream order as a polynomial hash of base checksum_base.
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
constexpr std::size_t chunk_count{8};

/**
 * @brief The base of the polynomial hash a tally folds its token kinds into.
 */
constexpr std::size_t checksum_base{31};

/**
 * @brief The seed of the stream the source-shaped corpus is drawn from.
 */
constexpr unsigned source_corpus_seed{0x2C9277B5U};

/**
 * @brief The origin table's entry for a two-byte window that is not certified.
 */
constexpr unsigned char not_certified{0xFF};

/**
 * @brief The bytes in a mebibyte, the unit the corpus size and every throughput are given in.
 */
constexpr std::size_t bytes_per_mib{std::size_t{1} << 20U};

/**
 * @brief The serial scan of a corpus, the stream every plan must reproduce.
 */
struct Reference
{
    /**
     * @brief Every token's kind, in stream order.
     */
    std::vector<unsigned char> kinds{};

    /**
     * @brief Every token's length, in stream order.
     */
    std::vector<std::uint32_t> lengths{};

    /**
     * @brief Per offset of the corpus, whether a token begins there.
     */
    std::vector<bool> begins{};

    /**
     * @brief The serial scan's token count, checksum and consumed bytes, which every chunked scan must equal.
     */
    Tally tally{};
};

/**
 * @brief The origin table of the certified two-byte windows.
 */
struct Origins
{
    /**
     * @brief Per two-byte window, at the first byte times 256 plus the second, the offset inside it at which a token
     *        begins, not_certified where the window is not certified.
     */
    std::vector<unsigned char> of_pair{};

    /**
     * @brief The count of certified two-byte windows, the entries of of_pair that are not not_certified.
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
    Tally tally{};

    /**
     * @brief The scan's wall-clock time.
     */
    std::chrono::steady_clock::duration elapsed{};
};

/**
 * @brief A window plan of a corpus and the time it took.
 */
struct Window_plan
{
    /**
     * @brief The plan's chunk edges.
     */
    std::vector<std::size_t> edges{};

    /**
     * @brief The plan's wall-clock time.
     */
    std::chrono::steady_clock::duration elapsed{};
};

/**
 * @brief The run's generated corpus and its serial scan.
 */
struct Measured_corpus
{
    /**
     * @brief The corpus, the same bytes on every run.
     */
    std::string corpus{};

    /**
     * @brief The corpus's serial reference.
     */
    Reference reference{};
};

/**
 * @brief The throughputs of one pass's two scans.
 */
struct Pass_rates
{
    /**
     * @brief The first scan's throughput in MiB/s.
     */
    double first_mib_s{0.0};

    /**
     * @brief The second scan's throughput in MiB/s.
     */
    double second_mib_s{0.0};
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
    States_t live{};

    /**
     * @brief Whether a live transition re-enters the initial state.
     */
    bool reentrant{false};
};

/**
 * @brief The benchmark's command line: the generated corpus's size, the timed passes, the CSV and the occurrence files.
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
    std::string csv_path{};

    /**
     * @brief The files whose certified window occurrences are counted.
     */
    std::vector<std::string> occurrence_files{};
};

/**
 * @brief The corpus's 32-bit linear congruential stream, each draw fifteen of the advanced state's high bits.
 *
 * The stream is the one recovery_lcg's Lcg advances, but its draws are bits 16 to 30 of the state alone, a sequence
 * none of Lcg's draws yields, so the corpus keeps its own.
 */
class Corpus_stream
{
public:
    /**
     * @brief Starts the stream at a seed.
     * @param seed The initial state.
     */
    explicit Corpus_stream(const unsigned seed) noexcept : state_{seed} {}

    /**
     * @brief Advances the state and draws fifteen of its high bits.
     * @return Bits 16 to 30 of the advanced state.
     */
    unsigned next() noexcept
    {
        state_ = state_ * multiplier + increment;

        return (state_ >> 16U) & 0x7FFFU;
    }

private:
    /**
     * @brief The multiplier of the state's advance.
     */
    static constexpr unsigned multiplier{1664525U};

    /**
     * @brief The increment of the state's advance.
     */
    static constexpr unsigned increment{1013904223U};

    /**
     * @brief The current state.
     */
    unsigned state_;
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
     *        nothing without a CSV. Prints `CSV open failed` or `CSV write failed` with the path when the open or the
     *        write fails.
     * @param scenario The scenario's name.
     * @param pass The pass index, -1 for a plan.
     * @param seconds The timed seconds.
     * @param mib_s The throughput in MiB/s, 0 for a plan.
     */
    void row(std::string_view scenario, int pass, double seconds, double mib_s);

    /**
     * @brief Returns whether every open and write of the CSV so far succeeded.
     * @return False after the first failure.
     */
    [[nodiscard]] bool is_healthy() const noexcept;

private:
    /**
     * @brief The CSV appended to, empty for none.
     */
    std::string csv_path_{};

    /**
     * @brief The generated corpus's size in MiB.
     */
    std::size_t input_mib_{};

    /**
     * @brief The run's stamp, the time the run started in seconds since the epoch.
     */
    std::size_t run_{};

    /**
     * @brief Whether every open and write so far succeeded.
     */
    bool healthy_{true};
};

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
        std::printf("  CSV open failed: %s\n", csv_path_.c_str());

        healthy_ = false;

        return;
    }

    csv << std::setprecision(std::numeric_limits<double>::max_digits10);

    if (fresh)
    {
        csv << "run,commit,dirty,scenario,input_mib,pass,seconds,mib_per_s\n";
    }

    csv << run_ << ',' << commit << ',' << (dirty ? "yes" : "no") << ',' << scenario << ',' << input_mib_ << ',' << pass
        << ',' << seconds << ',' << mib_s << '\n';

    csv.flush();

    if (!csv)
    {
        std::printf("  CSV write failed: %s\n", csv_path_.c_str());

        healthy_ = false;
    }
}

bool Observations::is_healthy() const noexcept
{
    return healthy_;
}

/**
 * @brief Returns the origin table's index of a two-byte window.
 * @param first The window's first byte.
 * @param second The window's second byte.
 * @return The first byte times 256 plus the second.
 */
std::size_t pair_index(const char first, const char second)
{
    const auto high{static_cast<std::size_t>(static_cast<unsigned char>(first))};

    const auto low{static_cast<std::size_t>(static_cast<unsigned char>(second))};

    return high * byte_count + low;
}

/**
 * @brief Raises checksum_base to a power, modulo 2 to the width of std::size_t, by repeated squaring.
 * @param exponent The power.
 * @return The power of checksum_base, which splices a chunk's checksum after the checksums before it.
 */
std::size_t checksum_power(std::size_t exponent)
{
    std::size_t result{1};

    std::size_t base{checksum_base};

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
 * @brief Runs one scan under the steady clock.
 * @tparam Scan The scan's type.
 * @param scan The scan, a callable returning its Tally.
 * @return The scan's tally and its time.
 */
template <typename Scan>
Timed_scan timed(const Scan& scan)
{
    const auto started{std::chrono::steady_clock::now()};

    const auto tally{scan()};

    const auto elapsed{std::chrono::steady_clock::now() - started};

    return {.tally = tally, .elapsed = elapsed};
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
    Origins origins{.of_pair = std::vector(byte_count * byte_count, not_certified), .windows = 0};

    auto& [of_pair, windows]{origins};

    for (const auto& [pair, at] : certified_pairs(dfa, live, reentrant))
    {
        of_pair[pair_index(pair[0], pair[1])] = static_cast<unsigned char>(at);

        ++windows;
    }

    return origins;
}

/**
 * @brief Returns every offset a certified window occurrence says a token begins at: per position whose two bytes form a
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
        if (const auto origin{origins.of_pair[pair_index(bytes[at], bytes[at + 1])]}; origin != not_certified)
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

    Corpus_stream stream{source_corpus_seed};

    static constexpr std::array<std::string_view, 8> words{"count", "offset", "state", "token",
                                                           "chunk", "origin", "table", "index"};

    const auto pick{[&stream] { return words[stream.next() % words.size()]; }};

    while (out.size() < bytes)
    {
        const auto pieces{3U + stream.next() % 6U};

        for (std::size_t piece{0}; piece < pieces; ++piece)
        {
            const auto shape{stream.next() % 8U};

            switch (shape)
            {
            case 0:
                out += std::to_string(stream.next());

                break;

            case 1:
                out += '"';
                out += pick();
                out += ' ';
                out += pick();
                out += '"';

                break;

            case 2:
                out += "+ ";
                out += pick();

                break;

            case 3:
                out += pick();
                out += ';';

                break;

            default:
                out += pick();
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
 * @return The serial reference; its tally's consumed count is below the corpus size when the corpus does not tokenize.
 */
Reference serial_reference(const munch::core::Lexer& lexer, const std::string& corpus)
{
    Reference reference{.kinds = {}, .lengths = {}, .begins = std::vector<bool>(corpus.size(), false), .tally = {}};

    std::size_t offset{0};

    const auto record{[&reference, &offset](const Token token, const std::size_t length) {
        reference.begins[offset] = true;

        offset += length;

        reference.kinds.push_back(static_cast<unsigned char>(token));

        reference.lengths.push_back(static_cast<std::uint32_t>(length));

        reference.tally.checksum = reference.tally.checksum * checksum_base + std::to_underlying(token);

        ++reference.tally.tokens;
    }};

    reference.tally.consumed = lexer.tokenize_all<Token>(corpus, record);

    return reference;
}

/**
 * @brief Returns the chunk edges of a window plan: 0, per interior division of chunk_count equal parts the first
 *        boundary at or after it that lies past the edge before, and the corpus size.
 * @param corpus_size The corpus size.
 * @param boundaries The window boundaries, in increasing order.
 * @return The edges, at most chunk_count + 1 of them, increasing.
 */
std::vector<std::size_t> nearest_edges(const std::size_t corpus_size, const std::vector<std::size_t>& boundaries)
{
    std::vector<std::size_t> edges{0};

    for (std::size_t chunk{1}; chunk < chunk_count; ++chunk)
    {
        const auto desired{corpus_size * chunk / chunk_count};

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
 * @brief Returns the first interior edge of a plan at which no token of the serial scan begins.
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
 *        the chunks' tallies in stream order after the join. Every timed chunked path, byte-planned and window-planned
 *        alike, scans through it.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param edges The chunk edges, at least two.
 * @return The spliced tally: equal to the serial one exactly when the chunked stream hashes and counts alike.
 */
Tally chunked_scan(const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges)
{
    // One chunk's tally alone on its cache line, so the chunks' scans never share a line.
    struct alignas(64) Padded
    {
        Tally tally{};
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
                    mine.checksum = mine.checksum * 31U + std::to_underlying(token);

                    ++mine.tokens;
                });
            });
        }

        const auto last{edges.size() - 2};

        const std::string_view piece{corpus.data() + edges[last], edges[last + 1] - edges[last]};

        auto& mine{tallies[last].tally};

        mine.consumed = lexer.tokenize_all<Token>(piece, [&mine](const Token token, const std::size_t) {
            mine.checksum = mine.checksum * 31U + std::to_underlying(token);

            ++mine.tokens;
        });
    }

    Tally total{};

    for (const auto& [tally] : tallies)
    {
        total.checksum = total.checksum * checksum_power(tally.tokens) + tally.checksum;

        total.tokens += tally.tokens;

        total.consumed += tally.consumed;
    }

    return total;
}

/**
 * @brief Returns whether the untimed proof of a plan holds: the chunks' concatenated (kind, length) stream equals the
 *        serial one element for element, and every chunk consumes whole.
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
        const auto piece{std::string_view{corpus}.substr(edges[chunk], edges[chunk + 1] - edges[chunk])};

        const auto match{[&](const Token token, const std::size_t length) {
            matched = matched && ordinal < reference.kinds.size() &&
                      static_cast<unsigned char>(token) == reference.kinds[ordinal] &&
                      length == reference.lengths[ordinal];

            ++ordinal;
        }};

        const auto consumed{lexer.tokenize_all<Token>(piece, match)};

        if (consumed != piece.size())
        {
            return false;
        }
    }

    return matched && ordinal == reference.kinds.size();
}

/**
 * @brief Times two scans of one corpus pass by pass, the first scan first on even passes and second on odd ones,
 *        reports each pass's two times once both have run, and prints `a timed pass disagreed` when a scan did not
 *        reproduce the reference; the last step of both measurements.
 * @tparam First The type of the first scan.
 * @tparam Second The type of the second scan.
 * @tparam Report The type of the per-pass report.
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

        agreed = agreed && first_run.tally == reference && second_run.tally == reference;

        report(pass, first_run.elapsed, second_run.elapsed);
    }

    if (!agreed)
    {
        std::printf("  a timed pass disagreed with the serial reference\n");

        return false;
    }

    return true;
}

/**
 * @brief Returns a wall-clock duration in seconds.
 * @param elapsed The duration.
 * @return The seconds.
 */
double elapsed_s(const std::chrono::steady_clock::duration elapsed)
{
    return std::chrono::duration<double>(elapsed).count();
}

/**
 * @brief Returns a wall-clock duration in milliseconds.
 * @param elapsed The duration.
 * @return The milliseconds.
 */
double elapsed_ms(const std::chrono::steady_clock::duration elapsed)
{
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

/**
 * @brief Returns the throughput of scanning some bytes in a duration.
 * @param bytes The bytes scanned.
 * @param elapsed The duration.
 * @return The MiB per second.
 */
double mib_per_s(const std::size_t bytes, const std::chrono::steady_clock::duration elapsed)
{
    const auto seconds{elapsed_s(elapsed)};

    return static_cast<double>(bytes) / static_cast<double>(bytes_per_mib) / seconds;
}

/**
 * @brief Returns the mean gap between certified window occurrences over some bytes.
 * @param bytes The bytes the occurrences were found in.
 * @param occurrences The occurrences.
 * @return The bytes per occurrence, zero when there is none.
 */
double mean_gap(const std::size_t bytes, const std::size_t occurrences)
{
    return occurrences == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(occurrences);
}

/**
 * @brief Generates the run's corpus at the size the options give and scans it serially; the first step of both
 *        measurements.
 * @param lexer The lexer of the measurement's grammar.
 * @param options The run's options.
 * @return The corpus and its serial reference.
 */
Measured_corpus measured_corpus(const munch::core::Lexer& lexer, const Bench_options& options)
{
    auto corpus{source_corpus(options.size_mib * bytes_per_mib)};

    auto reference{serial_reference(lexer, corpus)};

    return {.corpus = std::move(corpus), .reference = std::move(reference)};
}

/**
 * @brief Records one pass's two times as one observation each, the first scan's first, and returns their throughputs.
 * @param observations The run's observations.
 * @param corpus_size The bytes each scan covered.
 * @param pass The pass.
 * @param first_scenario The first scan's scenario name.
 * @param first_elapsed The first scan's time.
 * @param second_scenario The second scan's scenario name.
 * @param second_elapsed The second scan's time.
 * @return The two throughputs.
 */
Pass_rates record_pass(
        Observations& observations, const std::size_t corpus_size, const int pass,
        const std::string_view first_scenario, const std::chrono::steady_clock::duration first_elapsed,
        const std::string_view second_scenario, const std::chrono::steady_clock::duration second_elapsed)
{
    const auto first_mib_s{mib_per_s(corpus_size, first_elapsed)};

    const auto second_mib_s{mib_per_s(corpus_size, second_elapsed)};

    observations.row(first_scenario, pass, elapsed_s(first_elapsed), first_mib_s);

    observations.row(second_scenario, pass, elapsed_s(second_elapsed), second_mib_s);

    return {.first_mib_s = first_mib_s, .second_mib_s = second_mib_s};
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

    builder.add_token(figures::string_literal(), Token::string, 2);

    auto dfa{builder.dfa()};

    auto live{live_states(dfa)};

    const auto reentrant{is_init_reentrant(dfa, live)};

    auto lexer{builder.build()};

    return {.dfa = std::move(dfa), .lexer = std::move(lexer), .live = std::move(live), .reentrant = reentrant};
}

/**
 * @brief Reads the command line positionally: a first argument std::atoi reads as positive is the size in MiB, then a
 *        second one read so is the passes, then a third is the CSV path, `-` for none; every argument after those taken
 *        is an occurrence file.
 * @param arguments The arguments after the program name.
 * @return The options, each one not given at its default: 16 MiB, 5 passes, no CSV.
 */
Bench_options options_of(const std::vector<std::string>& arguments)
{
    Bench_options options{};

    const auto with_files{[&arguments, &options](const std::size_t taken) {
        const auto files{arguments | std::views::drop(taken)};

        options.occurrence_files.assign(files.begin(), files.end());

        return options;
    }};

    const auto number_at{[&arguments](const std::size_t index) { return std::atoi(arguments[index].c_str()); }};

    const auto size_mib{arguments.empty() ? 0 : number_at(0)};

    if (size_mib <= 0)
    {
        return with_files(0);
    }

    options.size_mib = static_cast<std::size_t>(size_mib);

    const auto passes{arguments.size() <= 1 ? 0 : number_at(1)};

    if (passes <= 0)
    {
        return with_files(1);
    }

    options.passes = passes;

    if (arguments.size() <= 2)
    {
        return with_files(2);
    }

    options.csv_path = arguments[2] == "-" ? std::string{} : arguments[2];

    return with_files(3);
}

/**
 * @brief Prints the run's banner: the commit with its dirty state, the corpus size, the passes and whether the
 *        observations are recorded.
 * @param options The run's options.
 */
void print_banner(const Bench_options& options)
{
    std::printf("window-recovered parallel cuts, preview of the split-windows benchmark campaign\n");

    const std::string commit_text{commit};

    const auto dirty_note{dirty ? " (uncommitted changes present)" : ""};

    const auto csv_note{options.csv_path.empty() ? ", dev run, observations discarded" : ", observations recorded"};

    std::printf(
            "  commit %s%s, %zu MiB, %d passes%s\n", commit_text.c_str(), dirty_note, options.size_mib, options.passes,
            csv_note);
}

/**
 * @brief Checks the premise of the no-byte measurement: the lexer certifies no byte exactly. Prints `premise moved`
 *        with the first byte it certifies.
 * @param lexer The lexer.
 * @return True when no byte is exactly certified.
 */
bool has_no_exact_byte(const munch::core::Lexer& lexer)
{
    for (const auto symbol : every_byte())
    {
        if (lexer.is_split_point(symbol))
        {
            const int value{static_cast<unsigned char>(symbol)};

            std::printf("  premise moved: byte %d is exactly certified, this grammar no longer needs windows\n", value);

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
    const auto& [dfa, lexer, live, reentrant]{grammar};

    const auto search_started{std::chrono::steady_clock::now()};

    auto origins{origins_of(dfa, live, reentrant)};

    const auto search_elapsed{std::chrono::steady_clock::now() - search_started};

    const auto search_ms{elapsed_ms(search_elapsed)};

    std::printf(
            "  certified two-byte windows: %zu, found in %.1f ms of post-construction analysis\n", origins.windows,
            search_ms);

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
        const auto data{read_bytes(file)};

        if (!data)
        {
            continue;
        }

        const auto occurrences{window_boundaries(*data, origins).size()};

        const auto gap{mean_gap(data->size(), occurrences)};

        std::printf(
                "  %-40s %zu occurrences over %zu bytes, mean gap %.1f\n", file.c_str(), occurrences, data->size(),
                gap);
    }
}

/**
 * @brief Plans a corpus's chunks at window-recovered origins under the steady clock: finds the window boundaries, hands
 *        them to an inspection, and cuts at the nearest edges.
 *
 * The inspection runs inside the timed span, so whatever it reports is timed with the plan.
 * @tparam Inspect The inspection's type.
 * @param corpus The corpus.
 * @param origins The origin table.
 * @param inspect Called with the window boundaries before the edges are cut.
 * @return The plan's edges and its time.
 */
template <typename Inspect>
Window_plan window_plan(const std::string& corpus, const Origins& origins, const Inspect& inspect)
{
    const auto started{std::chrono::steady_clock::now()};

    const auto boundaries{window_boundaries(corpus, origins)};

    inspect(boundaries);

    auto edges{nearest_edges(corpus.size(), boundaries)};

    const auto elapsed{std::chrono::steady_clock::now() - started};

    return {.edges = std::move(edges), .elapsed = elapsed};
}

/**
 * @brief Returns whether a plan reproduces the serial scan, untimed: its chunked scan's tally equals the serial one,
 *        and its chunks' (kind, length) stream equals the serial one element for element.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param edges The plan's chunk edges.
 * @param reference The serial reference.
 * @return True when both hold.
 */
bool reproduces(
        const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges,
        const Reference& reference)
{
    return chunked_scan(lexer, corpus, edges) == reference.tally && exact_match(lexer, corpus, edges, reference);
}

/**
 * @brief Checks the measurement on the grammar no byte certifies: plans at most eight chunks at window-recovered
 *        origins, holds the chunked stream to the serial one before any clock starts, and times the serial scan against
 *        the chunked one pass by pass, printing each figure and recording it.
 * @param grammar The C-like grammar with string literals.
 * @param origins Its origin table.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when the corpus tokenizes, the plan cuts at token starts and every scan reproduces the serial stream.
 */
bool no_byte_measurement(
        const Bench_grammar& grammar, const Origins& origins, const Bench_options& options, Observations& observations)
{
    const auto& [dfa, lexer, live, reentrant]{grammar};

    const auto [corpus, reference]{measured_corpus(lexer, options)};

    if (reference.tally.consumed != corpus.size())
    {
        std::printf("  corpus not tokenizable: consumed %zu of %zu\n", reference.tally.consumed, corpus.size());

        return false;
    }

    const auto print_occurrences{[&corpus](const std::vector<std::size_t>& boundaries) {
        const auto gap{mean_gap(corpus.size(), boundaries.size())};

        std::printf(
                "  corpus: %zu bytes, %zu window occurrences, mean gap %.1f bytes\n", corpus.size(), boundaries.size(),
                gap);
    }};

    const auto [edges, window_plan_elapsed]{window_plan(corpus, origins, print_occurrences)};

    if (const auto edge{first_non_token_start(edges, reference)})
    {
        std::printf("  boundary %zu is not a token start, the certificate or the planner is wrong\n", *edge);

        return false;
    }

    if (!reproduces(lexer, corpus, edges, reference))
    {
        std::printf("  streams disagree: the window cut does not reproduce the serial scan\n");

        return false;
    }

    const auto chunks{edges.size() - 1};

    std::printf(
            "  streams identical: %zu tokens, spliced checksum equal, %zu chunks; window plan %.2f ms\n",
            reference.tally.tokens, chunks, elapsed_ms(window_plan_elapsed));

    observations.row("plan-window-no-byte", -1, elapsed_s(window_plan_elapsed), 0.0);

    Tally timed_serial{};

    // Hashes and counts as the chunked scan does.
    const auto serial_scan{[&] {
        timed_serial = {};

        timed_serial.consumed = lexer.tokenize_all<Token>(corpus, [&](const Token token, const std::size_t) {
            timed_serial.checksum = timed_serial.checksum * checksum_base + std::to_underlying(token);

            ++timed_serial.tokens;
        });

        return timed_serial;
    }};

    const auto window_scan{[&] { return chunked_scan(lexer, corpus, edges); }};

    const auto report{[&](const int pass, const std::chrono::steady_clock::duration serial_elapsed,
                          const std::chrono::steady_clock::duration chunked_elapsed) {
        const auto [serial_mib_s, chunked_mib_s]{record_pass(
                observations, corpus.size(), pass, "serial-no-byte", serial_elapsed, "window-scan-no-byte",
                chunked_elapsed)};

        const auto speedup{elapsed_s(serial_elapsed) / elapsed_s(chunked_elapsed)};

        std::printf(
                "  pass %d: serial %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, speedup %.2fx\n", pass, serial_mib_s,
                chunks, chunked_mib_s, speedup);
    }};

    return timed_passes(options.passes, reference.tally, serial_scan, window_scan, report);
}

/**
 * @brief Checks the same measurement on a grammar carrying both certificates, split-friendly C-like plus strings:
 *        newline is exactly certified there, so the shipped byte planner and the window planner cut the same corpus,
 *        both plans are held to the serial stream before any clock starts, and then their scans are timed against each
 *        other.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when newline is exactly certified, the corpus tokenizes, both plans cut at token starts and every scan
 *         of both plans reproduces the serial stream.
 */
bool byte_versus_window(const Bench_options& options, Observations& observations)
{
    std::printf("\nbyte-certified versus window-recovered cuts, same grammar, same corpus\n");

    constexpr auto split_friendly{true};

    const auto grammar{bench_grammar(split_friendly)};

    const auto& [dfa, lexer, live, reentrant]{grammar};

    if (!lexer.is_split_point('\n'))
    {
        std::printf("  premise moved: newline is no longer exactly certified on the split-friendly grammar\n");

        return false;
    }

    const auto [corpus, reference]{measured_corpus(lexer, options)};

    if (reference.tally.consumed != corpus.size())
    {
        std::printf(
                "  corpus not tokenizable by the split-friendly grammar: %zu of %zu\n", reference.tally.consumed,
                corpus.size());

        return false;
    }

    const auto origins{origins_of(dfa, live, reentrant)};

    const auto inspect_nothing{[](const std::vector<std::size_t>&) {}};

    const auto [edges, window_plan_elapsed]{window_plan(corpus, origins, inspect_nothing)};

    if (const auto edge{first_non_token_start(edges, reference)})
    {
        std::printf("  boundary %zu is not a token start on the split-friendly grammar\n", *edge);

        return false;
    }

    const auto byte_plan_started{std::chrono::steady_clock::now()};

    const auto edges_byte{lexer.chunk_boundaries(corpus, chunk_count)};

    const auto byte_plan_elapsed{std::chrono::steady_clock::now() - byte_plan_started};

    if (const auto edge{first_non_token_start(edges_byte, reference)})
    {
        std::printf("  byte boundary %zu is not a token start\n", *edge);

        return false;
    }

    const auto window_reproduces{reproduces(lexer, corpus, edges, reference)};

    const auto byte_reproduces{reproduces(lexer, corpus, edges_byte, reference)};

    if (!window_reproduces || !byte_reproduces)
    {
        std::printf("  streams disagree between the planners and the serial scan\n");

        return false;
    }

    const auto byte_chunks{edges_byte.size() - 1};

    const auto window_chunks{edges.size() - 1};

    std::printf(
            "  %zu windows; both plans reproduce the serial stream of %zu tokens; plans: byte %.2f ms for %zu "
            "chunks, window %.2f ms for %zu\n",
            origins.windows, reference.tally.tokens, elapsed_ms(byte_plan_elapsed), byte_chunks,
            elapsed_ms(window_plan_elapsed), window_chunks);

    observations.row("plan-byte-split-friendly", -1, elapsed_s(byte_plan_elapsed), 0.0);

    observations.row("plan-window-split-friendly", -1, elapsed_s(window_plan_elapsed), 0.0);

    const auto byte_scan{[&] { return chunked_scan(lexer, corpus, edges_byte); }};

    const auto window_scan{[&] { return chunked_scan(lexer, corpus, edges); }};

    const auto report{[&](const int pass, const std::chrono::steady_clock::duration byte_elapsed,
                          const std::chrono::steady_clock::duration window_elapsed) {
        const auto [byte_mib_s, window_mib_s]{record_pass(
                observations, corpus.size(), pass, "byte-scan-split-friendly", byte_elapsed,
                "window-scan-split-friendly", window_elapsed)};

        const auto ratio{elapsed_s(byte_elapsed) / elapsed_s(window_elapsed)};

        std::printf(
                "  pass %d: byte-scan x%zu %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, ratio %.2f\n", pass, byte_chunks,
                byte_mib_s, window_chunks, window_mib_s, ratio);
    }};

    return timed_passes(options.passes, reference.tally, byte_scan, window_scan, report);
}

} // namespace

/**
 * @brief Runs the benchmark: the premise that no byte is exactly certified, the window census, the occurrence files,
 *        the no-byte measurement and the byte-versus-window measurement, in turn.
 * @param argc The argument count.
 * @param argv The size in MiB, the passes, the CSV path or `-`, and the occurrence files, all optional.
 * @return EXIT_SUCCESS when every premise and stream equality holds and every CSV write succeeded, EXIT_FAILURE
 *         otherwise.
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

    constexpr auto split_friendly{false};

    const auto grammar{bench_grammar(split_friendly)};

    const auto& [dfa, lexer, live, reentrant]{grammar};

    if (!has_no_exact_byte(lexer))
    {
        return EXIT_FAILURE;
    }

    const auto origins{window_census(grammar)};

    if (!origins)
    {
        return EXIT_FAILURE;
    }

    report_occurrences(options.occurrence_files, *origins);

    if (!no_byte_measurement(grammar, *origins, options, observations))
    {
        return EXIT_FAILURE;
    }

    if (!byte_versus_window(options, observations))
    {
        return EXIT_FAILURE;
    }

    std::printf("\ndev-grade preview only: archived figures require the collect.sh ritual on a quiet machine\n");

    return observations.is_healthy() ? EXIT_SUCCESS : EXIT_FAILURE;
}

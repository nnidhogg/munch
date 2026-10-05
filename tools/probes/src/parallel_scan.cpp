// The parallel scanner over certified anchors: the splitting theorem run as a program, not a policy.
//
// Parallel lexers reach an exact result by enumerating the states a chunk might start in and resolving afterwards,
// paying for that enumeration per chunk. This probe splits at certified anchors instead: positions where a decision
// procedure over the grammar has proved that every completely tokenizable context places a token start, so a chunk
// scanned independently from one reproduces the sequential segmentation exactly, no speculation window and no fixup
// pass. The anchor table is computed outside this repository by the certification instruments and consumed here as
// (window, origin) pairs over byte classes; the byte classifier below is built from the very sets grammars.hpp defines,
// so the two derivations cannot drift apart silently.
//
// The probe runs the conventional C-like row exactly as the study composes it, scans the corpus sequentially, derives
// the anchor positions from the table, verifies every anchor lands on the sequential boundary set, then for each worker
// count snaps ideal cuts to their nearest anchors, scans every chunk in its own thread against the shared lexer, and
// requires the concatenated boundary stream byte-identical to the sequential one. Equality is asserted, not reported: a
// disagreement is a failed run. Wall-clock figures are printed on lines prefixed "timing:" and are machine-dependent by
// nature; every other line is stable.
//
// Usage: munch_parallel_scan <corpus file> <anchor table file> [worker counts...]

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/study_rows.hpp"

namespace
{
using namespace munch;

using figures::Token;

/**
 * @brief The times each timed scan runs, the fastest kept.
 */
constexpr std::size_t timing_repetitions{5};

/**
 * @brief The first argument holding a worker count, past the program name, the corpus and the table.
 */
constexpr int first_worker_argument{3};

/**
 * @brief The worker counts timed when the arguments give none.
 */
constexpr std::array<std::size_t, 4> default_worker_counts{8, 16, 32, 64};

/**
 * @brief One line of the anchor table: a window over byte classes and the origin it certifies.
 */
struct Window
{
    /**
     * @brief The window's class letters.
     */
    std::string classes{};

    /**
     * @brief The offset inside the window of the certified token start.
     */
    std::size_t origin{};
};

/**
 * @brief The cuts of one worker count's split and the farthest any ideal cut moved to reach its anchor.
 */
struct Split
{
    /**
     * @brief The chunk boundaries, 0 and the corpus size included, ascending and distinct.
     */
    std::vector<std::size_t> cuts{};

    /**
     * @brief The largest distance in bytes between an ideal cut and the anchor it snapped to.
     */
    std::size_t snap_max{0};
};

/**
 * @brief Reads a whole decimal number and nothing else, so a count that is not one is refused rather than read as zero.
 * @param text The text.
 * @return The number, std::nullopt when the text is not wholly one.
 */
std::optional<std::size_t> parse_count(const std::string_view text)
{
    const auto* const end{text.data() + text.size()};

    std::size_t value{0};

    const auto [stopped, error]{std::from_chars(text.data(), end, value)};

    if (error != std::errc{} || stopped != end)
    {
        return std::nullopt;
    }

    return value;
}

/**
 * @brief Builds the conventional C-like row exactly as the study composes it: the C-like base with string literals and
 *        line comments.
 * @return The lexer.
 */
core::Lexer build_conventional_row()
{
    core::Builder builder{};

    tools::probes::conventional_row(builder);

    return builder.build();
}

/**
 * @brief Classifies a byte by the nine-class abstraction of the conventional row, built from the sets the grammar's
 *        tokens are built from: c_like's identifier start, digits and blank, its newline, the string's quote, the
 *        comment's slash, then its operators and punctuation.
 * @param c The byte to classify.
 * @return The class letter the anchor table's windows are written in.
 */
char byte_class(const char c)
{
    static const auto identifier_start{regex::Set::alpha() + '_'};

    if (identifier_start.symbols().contains(c))
    {
        return 'L';
    }

    static const auto digits{regex::Set::digits()};

    if (digits.symbols().contains(c))
    {
        return 'D';
    }

    static const regex::Set blank{' ', '\t'};

    if (blank.symbols().contains(c))
    {
        return 'S';
    }

    if (c == '\n')
    {
        return 'N';
    }

    if (c == '"')
    {
        return 'Q';
    }

    if (c == '/')
    {
        return 'C';
    }

    static const auto& operator_symbols{figures::operators().symbols()};

    if (operator_symbols.contains(c))
    {
        return 'O';
    }

    static const auto& punctuation_symbols{figures::punctuation().symbols()};

    if (punctuation_symbols.contains(c))
    {
        return 'P';
    }

    return 'X';
}

/**
 * @brief Returns the token starts of the sequential scan; a corpus that does not tokenize completely ends the run with
 *        a diagnostic.
 * @param lexer The lexer.
 * @param input The input.
 * @return The token starts, ascending.
 */
std::vector<std::size_t> boundaries(const core::Lexer& lexer, const std::string_view input)
{
    std::vector<std::size_t> begins{};

    std::size_t at{0};

    const auto note_start{[&begins, &at](const Token, const std::size_t length) {
        begins.push_back(at);

        at += length;
    }};

    const auto consumed{lexer.tokenize_all<Token>(input, note_start)};

    if (consumed != input.size())
    {
        std::fprintf(stderr, "corpus not completely tokenizable: %zu of %zu\n", consumed, input.size());

        std::exit(EXIT_FAILURE);
    }

    return begins;
}

/**
 * @brief Reads a whole file as bytes; a file that cannot be read ends the run with a diagnostic.
 * @param path The file.
 * @return Its bytes.
 */
std::string read_file(const std::filesystem::path& path)
{
    auto bytes{tools::probes::read_bytes(path)};

    if (!bytes)
    {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());

        std::exit(EXIT_FAILURE);
    }

    return std::move(*bytes);
}

/**
 * @brief Reads the anchor table, one `window origin` pair per line, empty lines and `#` comments skipped; a table that
 *        cannot be read, holds a malformed line or holds no window ends the run with a diagnostic.
 * @param path The table.
 * @return The windows, in table order.
 */
std::vector<Window> read_anchor_table(const std::filesystem::path& path)
{
    std::ifstream stream{path};

    if (!stream)
    {
        std::fprintf(stderr, "cannot read the anchor table at %s\n", path.string().c_str());

        std::exit(EXIT_FAILURE);
    }

    std::vector<Window> windows{};

    std::string line{};

    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        std::istringstream fields{line};

        Window window{};

        auto& [classes, origin]{window};

        if (!(fields >> classes >> origin) || origin >= classes.size())
        {
            std::fprintf(stderr, "anchor table line is not `window origin`: %s\n", line.c_str());

            std::exit(EXIT_FAILURE);
        }

        windows.push_back(std::move(window));
    }

    if (windows.empty())
    {
        std::fprintf(stderr, "the anchor table at %s holds no windows\n", path.string().c_str());

        std::exit(EXIT_FAILURE);
    }

    return windows;
}

/**
 * @brief Reads the worker counts from the arguments past the corpus and the table, 8, 16, 32 and 64 when none is given;
 *        a count that is not a positive whole number is refused with a diagnostic.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The worker counts, std::nullopt after a refusal.
 */
std::optional<std::vector<std::size_t>> worker_counts_of(const int argc, char** argv)
{
    std::vector<std::size_t> worker_counts{};

    for (int argument{first_worker_argument}; argument < argc; ++argument)
    {
        const auto count{parse_count(argv[argument])};

        if (!count || *count == 0)
        {
            std::fprintf(stderr, "worker count must be a positive whole number: %s\n", argv[argument]);

            return std::nullopt;
        }

        worker_counts.push_back(*count);
    }

    if (worker_counts.empty())
    {
        worker_counts.assign(default_worker_counts.begin(), default_worker_counts.end());
    }

    return worker_counts;
}

/**
 * @brief Returns the anchor positions the table witnesses in a class string: every occurrence's position plus its
 *        origin, the string's two ends left out.
 * @param classes The corpus as class letters.
 * @param windows The table's windows.
 * @return The anchors, ascending and distinct.
 */
std::vector<std::size_t> anchor_positions(const std::string& classes, const std::vector<Window>& windows)
{
    std::set<std::size_t> anchors{};

    for (const auto& [window_classes, origin] : windows)
    {
        for (auto found{classes.find(window_classes)}; found != std::string::npos;
             found = classes.find(window_classes, found + 1))
        {
            const auto position{found + origin};

            if (position > 0 && position < classes.size())
            {
                anchors.insert(position);
            }
        }
    }

    return {anchors.begin(), anchors.end()};
}

/**
 * @brief Checks that every anchor lands on the sequential boundary set, the table's guarantee checked against the
 *        library's own scan rather than trusted; the first anchor off it is reported with a diagnostic.
 * @param anchors The anchors.
 * @param sequential The sequential scan's token starts.
 * @return True when every anchor is a sequential token start.
 */
bool check_anchors(const std::vector<std::size_t>& anchors, const std::vector<std::size_t>& sequential)
{
    const std::set<std::size_t> boundary_set{sequential.begin(), sequential.end()};

    for (const auto anchor : anchors)
    {
        if (!boundary_set.contains(anchor))
        {
            std::fprintf(stderr, "anchor %zu is not on the sequential boundary set\n", anchor);

            return false;
        }
    }

    return true;
}

/**
 * @brief Times a run several times and keeps the fastest.
 * @tparam Run The run's type, callable with no argument.
 * @param repetitions The runs timed.
 * @param run The run.
 * @return The fastest run's wall-clock time in milliseconds.
 */
template <typename Run>
double best_of_runs(const std::size_t repetitions, const Run& run)
{
    double best{0.0};

    for (std::size_t repetition{0}; repetition < repetitions; ++repetition)
    {
        const auto began{std::chrono::steady_clock::now()};

        run();

        const std::chrono::duration<double, std::milli> took{std::chrono::steady_clock::now() - began};

        if (repetition == 0 || took.count() < best)
        {
            best = took.count();
        }
    }

    return best;
}

/**
 * @brief Returns the anchor nearest an ideal cut, the one above it on a tie, and the last anchor past every one.
 * @param anchors The anchors, ascending and nonempty.
 * @param ideal The ideal cut.
 * @return The anchor.
 */
std::size_t nearest_anchor(const std::vector<std::size_t>& anchors, const std::size_t ideal)
{
    const auto after{std::ranges::lower_bound(anchors, ideal)};

    if (after == anchors.end())
    {
        return anchors.back();
    }

    if (after == anchors.begin())
    {
        return *after;
    }

    const auto below{*std::prev(after)};

    return ideal - below < *after - ideal ? below : *after;
}

/**
 * @brief Splits a corpus for a worker count: each of the workers' ideal cuts snaps to its nearest anchor, and a cut
 *        equal to the one before it is dropped.
 * @param anchors The anchors, ascending and nonempty.
 * @param size The corpus size.
 * @param workers The worker count.
 * @return The cuts and the largest snap.
 */
Split snap_to_anchors(const std::vector<std::size_t>& anchors, const std::size_t size, const std::size_t workers)
{
    Split split{.cuts = {0}, .snap_max = 0};

    auto& [cuts, snap_max]{split};

    for (std::size_t cut{1}; cut < workers; ++cut)
    {
        const auto ideal{size * cut / workers};

        const auto nearest{nearest_anchor(anchors, ideal)};

        const auto distance{nearest > ideal ? nearest - ideal : ideal - nearest};

        snap_max = std::max(snap_max, distance);

        if (cuts.back() != nearest)
        {
            cuts.push_back(nearest);
        }
    }

    cuts.push_back(size);

    return split;
}

/**
 * @brief Scans one chunk, recording its token starts in the corpus's coordinates; a chunk that does not tokenize
 *        completely ends the run with a diagnostic.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param cuts The chunk boundaries.
 * @param chunk The chunk's index.
 * @param begins The chunk's token starts, replaced.
 */
void scan_chunk(
        const core::Lexer& lexer, const std::string_view corpus, const std::vector<std::size_t>& cuts,
        const std::size_t chunk, std::vector<std::size_t>& begins)
{
    begins.clear();

    std::size_t at{cuts[chunk]};

    const auto piece{corpus.substr(cuts[chunk], cuts[chunk + 1] - cuts[chunk])};

    const auto consumed{lexer.tokenize_all<Token>(piece, [&](const Token, const std::size_t length) {
        begins.push_back(at);

        at += length;
    })};

    if (consumed != piece.size())
    {
        std::fprintf(stderr, "chunk %zu not completely tokenizable\n", chunk);

        std::exit(EXIT_FAILURE);
    }
}

/**
 * @brief Scans every chunk in its own thread, each recording its token starts in the corpus's coordinates.
 * @param lexer The lexer.
 * @param corpus The corpus.
 * @param cuts The chunk boundaries.
 * @param chunk_begins Each chunk's token starts, one entry per chunk, replaced.
 */
void scan_chunks(
        const core::Lexer& lexer, const std::string_view corpus, const std::vector<std::size_t>& cuts,
        std::vector<std::vector<std::size_t>>& chunk_begins)
{
    std::vector<std::thread> threads{};

    for (std::size_t chunk{0}; chunk < cuts.size() - 1; ++chunk)
    {
        threads.emplace_back([&, chunk] { scan_chunk(lexer, corpus, cuts, chunk, chunk_begins[chunk]); });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }
}

} // namespace

/**
 * @brief Splits the corpus at the table's anchors for every worker count, scans the chunks in parallel and holds the
 *        concatenated boundary stream to the sequential scan, printing the figures and the timings.
 * @param argc The argument count.
 * @param argv The corpus, the anchor table and the worker counts, 8, 16, 32 and 64 when none is given.
 * @return EXIT_SUCCESS when every configuration reproduces the sequential scan, EXIT_FAILURE on a usage error or a
 *         disagreement.
 */
int main(const int argc, char** argv)
{
    if (argc < first_worker_argument)
    {
        std::fprintf(stderr, "usage: munch_parallel_scan <corpus file> <anchor table file> [worker counts...]\n");

        return EXIT_FAILURE;
    }

    const auto corpus{read_file(argv[1])};

    const auto windows{read_anchor_table(argv[2])};

    const auto worker_counts{worker_counts_of(argc, argv)};

    if (!worker_counts)
    {
        return EXIT_FAILURE;
    }

    const auto lexer{build_conventional_row()};

    const auto sequential{boundaries(lexer, corpus)};

    std::string classes(corpus.size(), '\0');

    std::ranges::transform(corpus, classes.begin(), byte_class);

    const auto anchors{anchor_positions(classes, windows)};

    if (!check_anchors(anchors, sequential))
    {
        return EXIT_FAILURE;
    }

    std::printf("corpus: %zu bytes, %zu sequential token starts\n", corpus.size(), sequential.size());

    // No anchor means no chunk boundary exists, and a split at an ideal cut would read past an empty table.
    if (anchors.empty())
    {
        std::fprintf(stderr, "no certified anchor in the corpus, so it cannot be split into chunks\n");

        return EXIT_FAILURE;
    }

    std::printf(
            "anchors: %zu from %zu table windows, every one on the sequential boundary set\n", anchors.size(),
            windows.size());

    const auto scan_sequentially{[&] { boundaries(lexer, corpus); }};

    const auto sequential_ms{best_of_runs(timing_repetitions, scan_sequentially)};

    std::printf("timing: sequential scan %.2f ms best of five\n", sequential_ms);

    const std::string_view view{corpus};

    for (const auto workers : *worker_counts)
    {
        const auto [cuts, snap_max]{snap_to_anchors(anchors, corpus.size(), workers)};

        const auto chunks{cuts.size() - 1};

        std::vector<std::vector<std::size_t>> chunk_begins(chunks);

        const auto scan_in_parallel{[&] { scan_chunks(lexer, view, cuts, chunk_begins); }};

        const auto parallel_ms{best_of_runs(timing_repetitions, scan_in_parallel)};

        std::vector<std::size_t> chunked{};

        std::ranges::copy(chunk_begins | std::views::join, std::back_inserter(chunked));

        if (chunked != sequential)
        {
            std::fprintf(stderr, "chunked segmentation differs from the sequential scan at %zu workers\n", workers);

            return EXIT_FAILURE;
        }

        const auto speedup{sequential_ms / parallel_ms};

        std::printf(
                "workers %zu: %zu chunks, snap max %zu bytes, chunked boundary stream "
                "byte-identical to the sequential scan\n",
                workers, chunks, snap_max);

        std::printf(
                "timing: workers %zu parallel scan %.2f ms best of five, speedup %.2f\n", workers, parallel_ms,
                speedup);
    }

    std::printf("the split theorem held on every configuration: no speculation and no fixup pass\n");

    return EXIT_SUCCESS;
}

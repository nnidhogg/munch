// The parallel scanner over certified anchors: the splitting theorem run as a program, not a policy.
//
// Parallel lexers reach an exact result by enumerating the states a chunk might start in and
// resolving afterwards, paying for that enumeration per chunk. This probe splits at
// certified anchors instead: positions where a decision procedure over the grammar has proved that every
// completely tokenizable context places a token start, so a chunk scanned independently from one
// reproduces the sequential segmentation exactly, no speculation window and no fixup pass. The anchor
// table is computed outside this repository by the certification instruments and consumed here as
// (window, origin) pairs over byte classes; the byte classifier below is built from the very sets
// grammars.hpp defines, so the two derivations cannot drift apart silently.
//
// The probe runs the conventional C-like row exactly as the study composes it, scans the corpus
// sequentially, derives the anchor positions from the table, verifies every anchor lands on the
// sequential boundary set, then for each worker count snaps ideal cuts to their nearest anchors, scans
// every chunk in its own thread against the shared lexer, and requires the concatenated boundary stream
// byte-identical to the sequential one. Equality is asserted, not reported: a disagreement is a failed
// run. Wall-clock figures are printed on lines prefixed "timing:" and are machine-dependent by nature;
// every other line is stable.
//
// Usage: munch_parallel_scan <corpus file> <anchor table file> [worker counts...]

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"

namespace
{
using namespace munch;

using figures::Token;

core::Lexer build_conventional_row()
{
    core::Builder b;

    figures::c_like(b, false);

    b.add_token(figures::string_literal(), Token::String, 2);

    b.add_token(figures::line_comment(), Token::LineComment, 1);

    return b.build();
}

/**
 * @brief The nine-class byte abstraction of the conventional row, built from the grammar's own sets.
 * @param c The byte to classify.
 * @return The class letter the anchor table's windows are written in.
 */
char byte_class(const char c)
{
    static const auto& operator_symbols{figures::operators().symbols()};

    static const auto& punctuation_symbols{figures::punctuation().symbols()};

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
    {
        return 'L';
    }

    if (c >= '0' && c <= '9')
    {
        return 'D';
    }

    if (c == ' ' || c == '\t')
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

    if (operator_symbols.contains(c))
    {
        return 'O';
    }

    if (punctuation_symbols.contains(c))
    {
        return 'P';
    }

    return 'X';
}

std::vector<std::size_t> boundaries(const core::Lexer& lexer, const std::string_view input)
{
    std::vector<std::size_t> begins;

    std::size_t at{0};

    const auto consumed{lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t length) {
        begins.push_back(at);

        at += length;
    })};

    if (consumed != input.size())
    {
        std::fprintf(stderr, "corpus not completely tokenizable: %zu of %zu\n", consumed, input.size());

        std::exit(EXIT_FAILURE);
    }

    return begins;
}

std::string read_file(const char* path)
{
    std::ifstream stream{path, std::ios::binary};

    if (!stream)
    {
        std::fprintf(stderr, "cannot read %s\n", path);

        std::exit(EXIT_FAILURE);
    }

    std::ostringstream out;

    out << stream.rdbuf();

    return std::move(out).str();
}

struct Window
{
    std::string classes{};

    std::size_t origin{};
};

std::vector<Window> read_anchor_table(const char* path)
{
    std::ifstream stream{path};

    if (!stream)
    {
        std::fprintf(stderr, "cannot read the anchor table at %s\n", path);

        std::exit(EXIT_FAILURE);
    }

    std::vector<Window> windows;

    std::string line;

    while (std::getline(stream, line))
    {
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        std::istringstream fields{line};

        Window window;

        if (!(fields >> window.classes >> window.origin) || window.origin >= window.classes.size())
        {
            std::fprintf(stderr, "anchor table line is not `window origin`: %s\n", line.c_str());

            std::exit(EXIT_FAILURE);
        }

        windows.push_back(std::move(window));
    }

    if (windows.empty())
    {
        std::fprintf(stderr, "the anchor table at %s holds no windows\n", path);

        std::exit(EXIT_FAILURE);
    }

    return windows;
}

std::vector<std::size_t> anchor_positions(const std::string& classes, const std::vector<Window>& windows)
{
    std::set<std::size_t> anchors;

    for (const auto& window : windows)
    {
        for (auto found{classes.find(window.classes)}; found != std::string::npos;
             found = classes.find(window.classes, found + 1))
        {
            const auto position{found + window.origin};

            if (position > 0 && position < classes.size())
            {
                anchors.insert(position);
            }
        }
    }

    return {anchors.begin(), anchors.end()};
}

double best_of_runs(const std::size_t repetitions, const auto& run)
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

} // namespace

int main(const int argc, const char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: munch_parallel_scan <corpus file> <anchor table file> [worker counts...]\n");

        return EXIT_FAILURE;
    }

    const auto corpus{read_file(argv[1])};

    const auto windows{read_anchor_table(argv[2])};

    std::vector<std::size_t> worker_counts;

    for (int argument{3}; argument < argc; ++argument)
    {
        worker_counts.push_back(static_cast<std::size_t>(std::strtoul(argv[argument], nullptr, 10)));
    }

    if (worker_counts.empty())
    {
        worker_counts = {8, 16, 32, 64};
    }

    const auto lexer{build_conventional_row()};

    const auto sequential{boundaries(lexer, corpus)};

    std::string classes(corpus.size(), '\0');

    std::transform(corpus.begin(), corpus.end(), classes.begin(), byte_class);

    const auto anchors{anchor_positions(classes, windows)};

    // Every anchor must land on the sequential boundary set: the table's guarantee, checked against
    // the library's own scan rather than trusted.
    std::set<std::size_t> boundary_set{sequential.begin(), sequential.end()};

    for (const auto anchor : anchors)
    {
        if (!boundary_set.contains(anchor))
        {
            std::fprintf(stderr, "anchor %zu is not on the sequential boundary set\n", anchor);

            return EXIT_FAILURE;
        }
    }

    std::printf("corpus: %zu bytes, %zu sequential token starts\n", corpus.size(), sequential.size());

    std::printf(
            "anchors: %zu from %zu table windows, every one on the sequential boundary set\n", anchors.size(),
            windows.size());

    // No anchor means no chunk boundary exists, and a split at an ideal cut would read past an empty table.
    if (anchors.empty())
    {
        std::fprintf(stderr, "no certified anchor in the corpus, so it cannot be split into chunks\n");

        return EXIT_FAILURE;
    }

    const auto sequential_ms{best_of_runs(5, [&] { boundaries(lexer, corpus); })};

    std::printf("timing: sequential scan %.2f ms best of five\n", sequential_ms);

    const std::string_view view{corpus};

    for (const auto workers : worker_counts)
    {
        std::vector<std::size_t> cuts{0};

        std::size_t snap_max{0};

        for (std::size_t k{1}; k < workers; ++k)
        {
            const auto ideal{corpus.size() * k / workers};

            const auto after{std::lower_bound(anchors.begin(), anchors.end(), ideal)};

            std::size_t nearest{after != anchors.end() ? *after : anchors.back()};

            if (after != anchors.begin() && after != anchors.end())
            {
                const auto below{*std::prev(after)};

                if (ideal - below < *after - ideal)
                {
                    nearest = below;
                }
            }
            else if (after == anchors.end())
            {
                nearest = anchors.back();
            }

            const auto distance{nearest > ideal ? nearest - ideal : ideal - nearest};

            snap_max = std::max(snap_max, distance);

            if (cuts.back() != nearest)
            {
                cuts.push_back(nearest);
            }
        }

        cuts.push_back(corpus.size());

        std::vector<std::vector<std::size_t>> chunk_begins(cuts.size() - 1);

        const auto scan_chunks{[&] {
            std::vector<std::thread> threads;

            for (std::size_t chunk{0}; chunk < cuts.size() - 1; ++chunk)
            {
                threads.emplace_back([&, chunk] {
                    chunk_begins[chunk].clear();

                    std::size_t at{cuts[chunk]};

                    const auto piece{view.substr(cuts[chunk], cuts[chunk + 1] - cuts[chunk])};

                    const auto consumed{lexer.tokenize_all<Token>(piece, [&](const Token, const std::size_t length) {
                        chunk_begins[chunk].push_back(at);

                        at += length;
                    })};

                    if (consumed != piece.size())
                    {
                        std::fprintf(stderr, "chunk %zu not completely tokenizable\n", chunk);

                        std::exit(EXIT_FAILURE);
                    }
                });
            }

            for (auto& thread : threads)
            {
                thread.join();
            }
        }};

        const auto parallel_ms{best_of_runs(5, scan_chunks)};

        std::vector<std::size_t> chunked;

        for (const auto& begins : chunk_begins)
        {
            chunked.insert(chunked.end(), begins.begin(), begins.end());
        }

        if (chunked != sequential)
        {
            std::fprintf(stderr, "chunked segmentation differs from the sequential scan at %zu workers\n", workers);

            return EXIT_FAILURE;
        }

        std::printf(
                "workers %zu: %zu chunks, snap max %zu bytes, chunked boundary stream "
                "byte-identical to the sequential scan\n",
                workers, cuts.size() - 1, snap_max);

        std::printf(
                "timing: workers %zu parallel scan %.2f ms best of five, speedup %.2f\n", workers, parallel_ms,
                sequential_ms / parallel_ms);
    }

    std::printf("the split theorem held on every configuration: no speculation and no fixup pass\n");

    return EXIT_SUCCESS;
}

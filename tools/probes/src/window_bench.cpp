// Measures what a certified split window is worth as a parallel cut, on the grammar the window search rescues:
// C-like with string literals, whose exact single-byte certificate is empty. Byte planning cannot help here;
// this probe plans boundaries at window-recovered origins, PROVES the chunked stream equals the serial one
// before any clock starts, and only then times the comparison.
//
// Every number printed here is run-local. The program stamps commit and dirty-state provenance into its CSV and
// stdout, collect.sh records the environment, and paper/data/ holds the committed campaign archives; even so, no
// figure from a casual run may be quoted anywhere without the collect.sh ritual on a quiet machine. The
// assertions are the point; the throughput lines only accompany them.
//
// The window model mirrored below is the one window_gate.cpp states and proves in full, with a confirmed second
// adversarial read; see that header for the representation lemma, the soundness argument, and the quotient. This
// copy exists so the two probes stay standalone, and it is kept honest twice over: the gate asserts the model
// against the scanner, and this probe additionally asserts that every boundary it plans lands on a token start of
// the serial scan it then reproduces exactly.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/tools/benchmark/provenance.hpp"

namespace
{
using figures::Token;

using munch::dfa::Dfa;

class Builder_dbg : public munch::core::Builder
{
public:
    using Builder::dfa;
};

using States_t = std::set<Dfa::State_t>;

constexpr std::size_t kBefore{static_cast<std::size_t>(-1)};

using Trajectory_t = std::pair<Dfa::State_t, std::size_t>;

using Cloud_t = std::set<Trajectory_t>;

States_t trim(const Dfa& dfa)
{
    States_t reachable{dfa.init_state()};

    std::deque<Dfa::State_t> pending{dfa.init_state()};

    while (!pending.empty())
    {
        const auto state{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && !reachable.contains(*next))
            {
                reachable.insert(*next);

                pending.push_back(*next);
            }
        }
    }

    States_t co_accessible;

    for (const auto state : reachable)
    {
        if (dfa.has_accept_token(state))
        {
            co_accessible.insert(state);
        }
    }

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (const auto state : reachable)
        {
            if (co_accessible.contains(state))
            {
                continue;
            }

            for (int symbol{0}; symbol < 256; ++symbol)
            {
                if (const auto next{dfa.advance(state, static_cast<char>(symbol))};
                    next && co_accessible.contains(*next))
                {
                    co_accessible.insert(state);

                    grew = true;

                    break;
                }
            }
        }
    }

    return co_accessible;
}

bool init_reentrant(const Dfa& dfa, const States_t& live)
{
    for (const auto state : live)
    {
        for (int symbol{0}; symbol < 256; ++symbol)
        {
            if (const auto next{dfa.advance(state, static_cast<char>(symbol))}; next && *next == dfa.init_state())
            {
                return true;
            }
        }
    }

    return false;
}

std::optional<Cloud_t> step(
        const Dfa& dfa, const States_t& live, const Cloud_t& from, const char symbol, const std::size_t at,
        const bool reentrant)
{
    const auto restart{dfa.advance(dfa.init_state(), symbol)};

    const auto restart_ok{restart && live.contains(*restart)};

    auto accepting{false};

    for (const auto& [state, origin] : from)
    {
        accepting = accepting || dfa.has_accept_token(state);
    }

    Cloud_t next;

    for (const auto& [state, origin] : from)
    {
        if (const auto direct{dfa.advance(state, symbol)}; direct && live.contains(*direct))
        {
            const auto begins{state == dfa.init_state() && !reentrant};

            next.emplace(*direct, begins ? at : origin);
        }
    }

    if (restart_ok && accepting)
    {
        next.emplace(*restart, at);
    }

    return next.empty() ? std::nullopt : std::optional{next};
}

std::optional<std::size_t> predicted(
        const Dfa& dfa, const States_t& live, const std::string& window, const bool reentrant)
{
    Cloud_t cloud;

    for (const auto state : live)
    {
        cloud.emplace(state, kBefore);
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto next{step(dfa, live, cloud, window[at], at, reentrant)};

        if (!next)
        {
            return std::nullopt;
        }

        cloud = *next;
    }

    const auto origin{cloud.begin()->second};

    if (origin == kBefore)
    {
        return std::nullopt;
    }

    for (const auto& [state, at] : cloud)
    {
        if (at != origin)
        {
            return std::nullopt;
        }
    }

    return origin;
}

/**
 * @brief A deterministic source-shaped corpus: identifier-heavy lines with numbers, operators, punctuation and
 *        string literals, every line newline-terminated, seeded so every run generates identical bytes.
 */
std::string source_corpus(const std::size_t bytes)
{
    std::string out;

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

    // Truncating can cut a string literal open, and no single-byte repair closes it; several sizes reproduce an
    // untokenizable tail. Cut back to the last complete line instead, then pad with newlines, which both grammars
    // tokenize, so every requested size is valid by construction.
    const auto last_newline{out.rfind('\n', bytes - 1)};

    out.resize(last_newline + 1);

    out.append(bytes - out.size(), '\n');

    return out;
}

struct Tally
{
    std::size_t tokens{0};

    std::size_t checksum{0};
};

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
 * @brief Corpus size, pass count, and CSV path for the campaign; overridable from the command line so the CI run
 *        stays small while the archived run uses collect.sh sizes. The CSV mirrors the harness schema, commit and
 *        dirty riding on every row so a row separated from its file still says which tree produced it.
 */
std::size_t g_size_mib{16};

int g_passes{5};

std::string g_csv;

std::size_t g_run{0};

/**
 * @brief Sticky CSV health: a single failed open, write, or flush fails the whole run at exit, the same class of
 *        hardening the main harness carries, since a silently truncated observations file poisons an archive.
 */
bool g_csv_ok{true};

void csv_row(const std::string_view scenario, const int pass, const double seconds, const double mib_s)
{
    if (g_csv.empty())
    {
        return;
    }

    std::ifstream probe{g_csv};

    const auto fresh{!probe.good() || probe.peek() == std::ifstream::traits_type::eof()};

    probe.close();

    std::ofstream csv{g_csv, std::ios::app};

    if (!csv)
    {
        std::printf("  CSV OPEN FAILED: %s\n", g_csv.c_str());

        g_csv_ok = false;

        return;
    }

    // Round-trip precision: six digits silently rounds throughput, the defect the harness fixed once already.
    csv << std::setprecision(std::numeric_limits<double>::max_digits10);

    if (fresh)
    {
        csv << "run,commit,dirty,scenario,input_mib,pass,seconds,mib_per_s\n";
    }

    using munch::tools::benchmark::kCommit;

    using munch::tools::benchmark::kDirty;

    csv << g_run << ',' << kCommit << ',' << (kDirty ? "yes" : "no") << ',' << scenario << ',' << g_size_mib << ','
        << pass << ',' << seconds << ',' << mib_s << '\n';

    csv.flush();

    if (!csv)
    {
        std::printf("  CSV WRITE FAILED: %s\n", g_csv.c_str());

        g_csv_ok = false;
    }
}

double mib_per_s(const std::size_t bytes, const std::chrono::steady_clock::duration elapsed)
{
    const auto seconds{std::chrono::duration<double>(elapsed).count()};

    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

double elapsed_s(const std::chrono::steady_clock::duration elapsed)
{
    return std::chrono::duration<double>(elapsed).count();
}

/**
 * @brief Scans the corpus in the chunks the edges delimit: one jthread per interior chunk, the last chunk on the
 *        calling thread, per-chunk tallies spliced in stream order.
 *
 * This is the one scan machinery every timed path shares, byte-planned and window-planned alike, so a throughput
 * ratio compares plans and nothing else. The tally work is the observable workload on every path, its results are
 * validated by the callers after every timed pass, and being used is what keeps the compiler from discarding it.
 */
std::size_t chunked_scan(
        const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges, Tally& total)
{
    struct alignas(64) Padded
    {
        Tally tally;

        std::size_t consumed{0};
    };

    std::vector<Padded> tallies(edges.size() - 1);

    {
        std::vector<std::jthread> workers;

        for (std::size_t chunk{0}; chunk + 2 < edges.size(); ++chunk)
        {
            workers.emplace_back([&, chunk] {
                const std::string_view piece{corpus.data() + edges[chunk], edges[chunk + 1] - edges[chunk]};

                auto& mine{tallies[chunk]};

                mine.consumed = lexer.tokenize_all<Token>(piece, [&mine](const Token token, const std::size_t) {
                    mine.tally.checksum = mine.tally.checksum * 31 + static_cast<std::size_t>(token);

                    ++mine.tally.tokens;
                });
            });
        }

        const auto last{edges.size() - 2};

        const std::string_view piece{corpus.data() + edges[last], edges[last + 1] - edges[last]};

        auto& mine{tallies[last]};

        mine.consumed = lexer.tokenize_all<Token>(piece, [&mine](const Token token, const std::size_t) {
            mine.tally.checksum = mine.tally.checksum * 31 + static_cast<std::size_t>(token);

            ++mine.tally.tokens;
        });
    }

    total = {};

    std::size_t covered{0};

    for (std::size_t chunk{0}; chunk + 1 < edges.size(); ++chunk)
    {
        covered += tallies[chunk].consumed;

        total.checksum = total.checksum * pow31(tallies[chunk].tally.tokens) + tallies[chunk].tally.checksum;

        total.tokens += tallies[chunk].tally.tokens;
    }

    return covered;
}

/**
 * @brief Whether a timed pass reproduced the reference stream; a mismatch is reported once by the caller.
 */
bool agrees(const Tally& timed, const Tally& reference)
{
    return timed.checksum == reference.checksum && timed.tokens == reference.tokens;
}

/**
 * @brief The exact untimed proof: the chunked concatenation must match the serial (kind, length) stream element
 *        for element, exactly as the engine-comparison harness proves agreement once before timing.
 *
 * The hashes inside the timed passes are the sanity signal; this is the proof, and it is why streams with the
 * same kinds but different token boundaries cannot pass. The reference costs five bytes per token, which the CI
 * default carries lightly and a 512 MiB campaign machine must budget for.
 */
bool exact_match(
        const munch::core::Lexer& lexer, const std::string& corpus, const std::vector<std::size_t>& edges,
        const std::vector<unsigned char>& kinds, const std::vector<std::uint32_t>& lengths)
{
    std::size_t ordinal{0};

    auto matched{true};

    for (std::size_t chunk{0}; chunk + 1 < edges.size(); ++chunk)
    {
        const std::string_view piece{corpus.data() + edges[chunk], edges[chunk + 1] - edges[chunk]};

        const auto consumed{lexer.tokenize_all<Token>(piece, [&](const Token token, const std::size_t length) {
            matched = matched && ordinal < kinds.size() && static_cast<unsigned char>(token) == kinds[ordinal] &&
                      length == lengths[ordinal];

            ++ordinal;
        })};

        if (consumed != piece.size())
        {
            return false;
        }
    }

    return matched && ordinal == kinds.size();
}

/**
 * @brief The same measurement on a grammar carrying both certificates: split-friendly C-like plus strings.
 *
 * Newline is exactly certified there, so the shipped byte planner and the window planner run on the same corpus
 * and the window path's cost is priced against the native one, which is the comparison the campaign owes. Both
 * paths are stream-equality asserted against the serial scan before any clock starts.
 */
bool byte_versus_window()
{
    std::printf("\nbyte-certified versus window-recovered cuts, same grammar, same corpus\n");

    Builder_dbg builder;

    figures::c_like(builder, true);

    builder.add_token(figures::string_literal(), Token::String, 2);

    const auto dfa{builder.dfa()};

    const auto lexer{builder.build()};

    const auto live{trim(dfa)};

    const auto reentrant{init_reentrant(dfa, live)};

    if (!lexer.is_split_point('\n'))
    {
        std::printf("  PREMISE MOVED: newline is no longer exactly certified on the split-friendly grammar\n");

        return false;
    }

    const auto corpus{source_corpus(g_size_mib << 20u)};

    std::vector<bool> begins(corpus.size(), false);

    std::vector<unsigned char> kinds;

    std::vector<std::uint32_t> lengths;

    Tally serial{};

    std::size_t offset{0};

    const auto consumed{lexer.tokenize_all<Token>(corpus, [&](const Token token, const std::size_t length) {
        begins[offset] = true;

        offset += length;

        kinds.push_back(static_cast<unsigned char>(token));

        lengths.push_back(static_cast<std::uint32_t>(length));

        serial.checksum = serial.checksum * 31 + static_cast<std::size_t>(token);

        ++serial.tokens;
    })};

    if (consumed != corpus.size())
    {
        std::printf("  CORPUS NOT TOKENIZABLE by the split-friendly grammar: %zu of %zu\n", consumed, corpus.size());

        return false;
    }

    std::vector<unsigned char> origin_of(256 * 256, 0xff);

    std::size_t windows{0};

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const std::string pair{static_cast<char>(first), static_cast<char>(second)};

            if (const auto at{predicted(dfa, live, pair, reentrant)})
            {
                origin_of[static_cast<std::size_t>(first) * 256 + static_cast<std::size_t>(second)] =
                        static_cast<unsigned char>(*at);

                ++windows;
            }
        }
    }

    const auto window_plan_started{std::chrono::steady_clock::now()};

    std::vector<std::size_t> boundaries;

    for (std::size_t at{0}; at + 1 < corpus.size(); ++at)
    {
        const auto first{static_cast<unsigned char>(corpus[at])};

        const auto second{static_cast<unsigned char>(corpus[at + 1])};

        if (const auto origin{origin_of[static_cast<std::size_t>(first) * 256 + second]}; origin != 0xff)
        {
            boundaries.push_back(at + origin);
        }
    }

    constexpr std::size_t kChunks{8};

    std::vector<std::size_t> edges{0};

    for (std::size_t chunk{1}; chunk < kChunks; ++chunk)
    {
        const auto desired{corpus.size() * chunk / kChunks};

        const auto nearest{std::lower_bound(boundaries.begin(), boundaries.end(), desired)};

        if (nearest != boundaries.end() && *nearest > edges.back())
        {
            edges.push_back(*nearest);
        }
    }

    edges.push_back(corpus.size());

    const auto window_plan_elapsed{std::chrono::steady_clock::now() - window_plan_started};

    for (std::size_t edge{1}; edge + 1 < edges.size(); ++edge)
    {
        if (!begins[edges[edge]])
        {
            std::printf("  BOUNDARY %zu IS NOT A TOKEN START on the split-friendly grammar\n", edges[edge]);

            return false;
        }
    }

    // The byte plan through the public planner, timed, so the comparison charges each side its own planning once
    // and the per-pass ratio compares scan against scan through the identical machinery.
    const auto byte_plan_started{std::chrono::steady_clock::now()};

    const auto edges_byte{lexer.chunk_boundaries(corpus, kChunks)};

    const auto byte_plan_elapsed{std::chrono::steady_clock::now() - byte_plan_started};

    for (std::size_t edge{1}; edge + 1 < edges_byte.size(); ++edge)
    {
        if (!begins[edges_byte[edge]])
        {
            std::printf("  BYTE BOUNDARY %zu IS NOT A TOKEN START\n", edges_byte[edge]);

            return false;
        }
    }

    Tally via_window{};

    Tally via_byte{};

    if (chunked_scan(lexer, corpus, edges, via_window) != corpus.size() || !agrees(via_window, serial) ||
        chunked_scan(lexer, corpus, edges_byte, via_byte) != corpus.size() || !agrees(via_byte, serial) ||
        !exact_match(lexer, corpus, edges, kinds, lengths) || !exact_match(lexer, corpus, edges_byte, kinds, lengths))
    {
        std::printf("  STREAMS DISAGREE between the planners and the serial scan\n");

        return false;
    }

    std::printf(
            "  %zu windows; both plans reproduce the serial stream of %zu tokens; plans: byte %.2f ms for %zu "
            "chunks, window %.2f ms for %zu\n",
            windows, serial.tokens, elapsed_s(byte_plan_elapsed) * 1e3, edges_byte.size() - 1,
            elapsed_s(window_plan_elapsed) * 1e3, edges.size() - 1);

    csv_row("plan-byte-split-friendly", -1, elapsed_s(byte_plan_elapsed), 0.0);

    csv_row("plan-window-split-friendly", -1, elapsed_s(window_plan_elapsed), 0.0);

    auto agreed{true};

    for (int pass{0}; pass < g_passes; ++pass)
    {
        // Alternate which path runs first each pass, so thermal and frequency drift is shared rather than
        // consistently charged to the second position, the same reason the main harness varies its order.
        Tally timed_byte{};

        Tally timed_window{};

        std::chrono::steady_clock::duration byte_elapsed{};

        std::chrono::steady_clock::duration window_elapsed{};

        const auto run_byte{[&] {
            const auto started{std::chrono::steady_clock::now()};

            const auto covered{chunked_scan(lexer, corpus, edges_byte, timed_byte)};

            byte_elapsed = std::chrono::steady_clock::now() - started;

            agreed = agreed && covered == corpus.size();
        }};

        const auto run_window{[&] {
            const auto started{std::chrono::steady_clock::now()};

            const auto covered{chunked_scan(lexer, corpus, edges, timed_window)};

            window_elapsed = std::chrono::steady_clock::now() - started;

            agreed = agreed && covered == corpus.size();
        }};

        if (pass % 2 == 0)
        {
            run_byte();

            run_window();
        }
        else
        {
            run_window();

            run_byte();
        }

        agreed = agreed && agrees(timed_byte, serial) && agrees(timed_window, serial);

        csv_row("byte-scan-split-friendly", pass, elapsed_s(byte_elapsed), mib_per_s(corpus.size(), byte_elapsed));

        csv_row("window-scan-split-friendly", pass, elapsed_s(window_elapsed),
                mib_per_s(corpus.size(), window_elapsed));

        std::printf(
                "  pass %d: byte-scan x%zu %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, ratio %.2f\n", pass,
                edges_byte.size() - 1, mib_per_s(corpus.size(), byte_elapsed), edges.size() - 1,
                mib_per_s(corpus.size(), window_elapsed), elapsed_s(byte_elapsed) / elapsed_s(window_elapsed));
    }

    if (!agreed)
    {
        std::printf("  A TIMED PASS DISAGREED with the serial reference\n");

        return false;
    }

    return true;
}
} // namespace

int main(int argc, char** argv)
{
    // Positional knobs mirror the modal benchmark: size in MiB, passes, CSV path ("-" for none), then any number
    // of files for the occurrence statistics. Argument-free runs keep the CI-sized defaults.
    int consumed_args{1};

    if (argc > 1 && std::atoi(argv[1]) > 0)
    {
        g_size_mib = static_cast<std::size_t>(std::atoi(argv[1]));

        consumed_args = 2;

        if (argc > 2 && std::atoi(argv[2]) > 0)
        {
            g_passes = std::atoi(argv[2]);

            consumed_args = 3;

            if (argc > 3)
            {
                g_csv = std::string_view{argv[3]} == "-" ? std::string{} : std::string{argv[3]};

                consumed_args = 4;
            }
        }
    }

    g_run = static_cast<std::size_t>(std::time(nullptr));

    using munch::tools::benchmark::kCommit;

    using munch::tools::benchmark::kDirty;

    std::printf("window-recovered parallel cuts, preview of the split-windows benchmark campaign\n");

    std::printf(
            "  commit %s%s, %zu MiB, %d passes%s\n", kCommit, kDirty ? " (uncommitted changes present)" : "",
            g_size_mib, g_passes, g_csv.empty() ? ", DEV RUN, observations discarded" : ", observations recorded");

    Builder_dbg builder;

    figures::c_like(builder, false);

    builder.add_token(figures::string_literal(), Token::String, 2);

    const auto dfa{builder.dfa()};

    const auto lexer{builder.build()};

    const auto live{trim(dfa)};

    const auto reentrant{init_reentrant(dfa, live)};

    // The premise: this grammar is one the published planner cannot serve at all.
    for (int symbol{0}; symbol < 256; ++symbol)
    {
        if (lexer.is_split_point(static_cast<char>(symbol)))
        {
            std::printf(
                    "  PREMISE MOVED: byte %d is exactly certified, this grammar no longer needs windows\n", symbol);

            return 1;
        }
    }

    // Every certified two-byte window, as a 256x256 origin table for the occurrence scan. The search timing is
    // the planning-cost figure the campaign owes: the whole price of window planning, paid once per grammar in a
    // post-construction analysis of the compiled tables.
    const auto search_started{std::chrono::steady_clock::now()};

    std::vector<unsigned char> origin_of(256 * 256, 0xff);

    std::size_t windows{0};

    for (int first{0}; first < 256; ++first)
    {
        for (int second{0}; second < 256; ++second)
        {
            const std::string pair{static_cast<char>(first), static_cast<char>(second)};

            if (const auto at{predicted(dfa, live, pair, reentrant)})
            {
                origin_of[static_cast<std::size_t>(first) * 256 + static_cast<std::size_t>(second)] =
                        static_cast<unsigned char>(*at);

                ++windows;
            }
        }
    }

    const auto search_elapsed{std::chrono::steady_clock::now() - search_started};

    std::printf(
            "  certified two-byte windows: %zu, found in %.1f ms of post-construction analysis\n", windows,
            std::chrono::duration<double, std::milli>(search_elapsed).count());

    if (windows == 0)
    {
        std::printf("  no window certified, nothing to measure\n");

        return 1;
    }

    // Optional real-corpus statistics: how often the certified windows occur in the named files, bytes only.
    // Occurrence frequency is the campaign's representativeness question, and it needs no tokenizability.
    for (int arg{consumed_args}; arg < argc; ++arg)
    {
        if (auto* file{std::fopen(argv[arg], "rb")})
        {
            std::string data;

            char buffer[1 << 16];

            for (std::size_t got{0}; (got = std::fread(buffer, 1, sizeof buffer, file)) > 0;)
            {
                data.append(buffer, got);
            }

            std::fclose(file);

            std::size_t occurrences{0};

            for (std::size_t at{0}; at + 1 < data.size(); ++at)
            {
                const auto first{static_cast<unsigned char>(data[at])};

                const auto second{static_cast<unsigned char>(data[at + 1])};

                occurrences += origin_of[static_cast<std::size_t>(first) * 256 + second] != 0xff ? 1 : 0;
            }

            std::printf(
                    "  %-40s %zu occurrences over %zu bytes, mean gap %.1f\n", argv[arg], occurrences, data.size(),
                    occurrences ? static_cast<double>(data.size()) / occurrences : 0.0);
        }
    }

    const auto corpus{source_corpus(g_size_mib << 20u)};

    // The serial reference: the token stream this whole exercise must reproduce, plus the token-start bitmap the
    // planned boundaries are asserted against.
    std::vector<bool> begins(corpus.size(), false);

    std::vector<unsigned char> kinds;

    std::vector<std::uint32_t> lengths;

    Tally serial{};

    std::size_t offset{0};

    const auto consumed{lexer.tokenize_all<Token>(corpus, [&](const Token token, const std::size_t length) {
        begins[offset] = true;

        offset += length;

        kinds.push_back(static_cast<unsigned char>(token));

        lengths.push_back(static_cast<std::uint32_t>(length));

        serial.checksum = serial.checksum * 31 + static_cast<std::size_t>(token);

        ++serial.tokens;
    })};

    if (consumed != corpus.size())
    {
        std::printf("  CORPUS NOT TOKENIZABLE: consumed %zu of %zu\n", consumed, corpus.size());

        return 1;
    }

    // Plan boundaries: every window occurrence yields the offset the model certifies, and the chunk edges are the
    // occurrences nearest the equal divisions, exactly the shape the shipped byte planner uses.
    const auto window_plan_started{std::chrono::steady_clock::now()};

    std::vector<std::size_t> boundaries;

    for (std::size_t at{0}; at + 1 < corpus.size(); ++at)
    {
        const auto first{static_cast<unsigned char>(corpus[at])};

        const auto second{static_cast<unsigned char>(corpus[at + 1])};

        if (const auto origin{origin_of[static_cast<std::size_t>(first) * 256 + second]}; origin != 0xff)
        {
            boundaries.push_back(at + origin);
        }
    }

    std::printf(
            "  corpus: %zu bytes, %zu window occurrences, mean gap %.1f bytes\n", corpus.size(), boundaries.size(),
            boundaries.empty() ? 0.0 : static_cast<double>(corpus.size()) / boundaries.size());

    constexpr std::size_t kChunks{8};

    std::vector<std::size_t> edges{0};

    for (std::size_t chunk{1}; chunk < kChunks; ++chunk)
    {
        const auto desired{corpus.size() * chunk / kChunks};

        const auto nearest{std::lower_bound(boundaries.begin(), boundaries.end(), desired)};

        if (nearest != boundaries.end() && *nearest > edges.back())
        {
            edges.push_back(*nearest);
        }
    }

    edges.push_back(corpus.size());

    const auto window_plan_elapsed{std::chrono::steady_clock::now() - window_plan_started};

    for (std::size_t edge{1}; edge + 1 < edges.size(); ++edge)
    {
        if (!begins[edges[edge]])
        {
            std::printf("  BOUNDARY %zu IS NOT A TOKEN START, the certificate or the planner is wrong\n", edges[edge]);

            return 1;
        }
    }

    // The chunked scan: one jthread per interior chunk, the last chunk on this thread, per-chunk tallies spliced
    // in stream order so equality with the serial checksum means the streams are identical token for token.
    Tally parallel{};

    if (chunked_scan(lexer, corpus, edges, parallel) != corpus.size() || !agrees(parallel, serial) ||
        !exact_match(lexer, corpus, edges, kinds, lengths))
    {
        std::printf("  STREAMS DISAGREE: the window cut does not reproduce the serial scan\n");

        return 1;
    }

    std::printf(
            "  streams identical: %zu tokens, spliced checksum equal, %zu chunks; window plan %.2f ms\n", serial.tokens,
            edges.size() - 1, elapsed_s(window_plan_elapsed) * 1e3);

    csv_row("plan-window-no-byte", -1, elapsed_s(window_plan_elapsed), 0.0);

    auto agreed{true};

    // Only now the clocks. Every pass carries the identical observable tally workload on both paths and is
    // validated against the reference afterwards; medians belong to the archived campaign, not here.
    for (int pass{0}; pass < g_passes; ++pass)
    {
        // Alternate which path runs first each pass, sharing thermal and frequency drift instead of charging it
        // to a fixed position, the same reason the main harness varies its order.
        Tally timed_serial{};

        Tally timed{};

        std::size_t serial_covered{0};

        std::chrono::steady_clock::duration serial_elapsed{};

        std::chrono::steady_clock::duration chunked_elapsed{};

        // The timed serial callback must do exactly the work the chunked callbacks do, checksum and count, or
        // the baseline is unequal and the ratio lies; coverage comes from the driver's return value instead of
        // a per-token accumulator in the timed region.
        const auto run_serial{[&] {
            const auto started{std::chrono::steady_clock::now()};

            serial_covered = lexer.tokenize_all<Token>(corpus, [&](const Token token, const std::size_t) {
                timed_serial.checksum = timed_serial.checksum * 31 + static_cast<std::size_t>(token);

                ++timed_serial.tokens;
            });

            serial_elapsed = std::chrono::steady_clock::now() - started;
        }};

        const auto run_chunked{[&] {
            const auto started{std::chrono::steady_clock::now()};

            const auto covered{chunked_scan(lexer, corpus, edges, timed)};

            chunked_elapsed = std::chrono::steady_clock::now() - started;

            agreed = agreed && covered == corpus.size();
        }};

        if (pass % 2 == 0)
        {
            run_serial();

            run_chunked();
        }
        else
        {
            run_chunked();

            run_serial();
        }

        agreed = agreed && agrees(timed_serial, serial) && serial_covered == corpus.size() && agrees(timed, serial);

        csv_row("serial-no-byte", pass, elapsed_s(serial_elapsed), mib_per_s(corpus.size(), serial_elapsed));

        csv_row("window-scan-no-byte", pass, elapsed_s(chunked_elapsed), mib_per_s(corpus.size(), chunked_elapsed));

        std::printf(
                "  pass %d: serial %7.1f MiB/s, window-scan x%zu %7.1f MiB/s, speedup %.2fx\n", pass,
                mib_per_s(corpus.size(), serial_elapsed), edges.size() - 1, mib_per_s(corpus.size(), chunked_elapsed),
                elapsed_s(serial_elapsed) / elapsed_s(chunked_elapsed));
    }

    if (!agreed)
    {
        std::printf("  A TIMED PASS DISAGREED with the serial reference\n");

        return 1;
    }

    if (!byte_versus_window())
    {
        return 1;
    }

    std::printf("\ndev-grade preview only: archived figures require the collect.sh ritual on a quiet machine\n");

    return g_csv_ok ? 0 : 1;
}

// Measures what verifier::chunk_plan() is worth as a worker contract for byte-pair encoding: how many cuts it finds on
// actual text against the context budget it may certify windows within, how far apart they lie, what finding them
// costs, and whether workers that each encode their chunk alone beat the sequential encoding once the plan, the workers
// and the stitching are all paid for. It is written to answer either way: every figure is printed as measured, and a
// plan that loses to the sequential encoding is reported with the figures that say why.
//
// The tables. The first 64 merges of GPT-2's table, read from the token automaton fixture, whose origin radius is
// infinite, so its cuts are the certified windows that occur; its prefix of 41 merges, the longest of finite radius,
// whose uniform window of width R + Q_s + 1 is a cut at every byte; and the merges (a, b), (a, ab), under which no
// window of a's is certified at its last byte while the uniform window cuts every run of them, over a generated text of
// runs. R comes from ripple_radius(), Q_s from rule_ceilings(), and the plans are made on the token automata, the merge
// tower of the 64 merges planning a slice for the cost of the verifier route. These are raw byte-pair tables over the
// whole text: GPT-2's encoder applies its merges to each piece of its pre-tokenizer, and that pipeline, its token ids
// and its timing against the production tokenizers belong to the production tokenizers' pre-tokenized pipelines,
// outside this probe, and are not measured here.
//
// What it checks, as hard assertions. The probe's encoder, one pass per merge in rank order, equal to the encoder that
// applies the lowest-ranked merge at its leftmost occurrence on a slice of every corpus; every cut of every plan a
// boundary of the sequential encoding; and on every run of every contract plan the stitched tokens, each worker
// encoding the bytes from its chunk's first to its read end alone and keeping the tokens below its cut, equal to the
// sequential encoding token for token. Any failure exits 1, as does a corpus that cannot be read and a CSV that cannot
// be written.
//
// What it measures. Per table and corpus: the cuts a plan of one chunk per byte finds at each budget, as a share of the
// sequential encoding's boundaries and per KiB, the median, 95th percentile and longest gap between consecutive cuts,
// the windows asked for and decided, so the cache's reuse, and the planning time; then per chunk count at one budget,
// the chunks achieved, the planning time, the time to decide the plan's evidence windows again alone, the workers' time
// with the stitching, the sequential time, the end-to-end speedup, the worker imbalance (the largest chunk over the
// ideal) and the fallback volume (the bytes scanned past their floors for want of a cut). Times are the best of the
// passes.
//
// Usage: munch_planner_bench [bytes [passes [csv path | -]]] [corpus...]
// A first argument that is wholly a positive whole number caps every corpus at that many bytes (none by default), then
// a second one so is the passes (3), then a third is the CSV the observations are appended to, `-` for none; every
// remaining argument is a corpus, a file read whole or a directory read as its C++ sources, every .cpp and .hpp under
// it in sorted order, and with none the corpus is munch's own sources under libs. Every number printed is run-local:
// the CSV and stdout carry commit and dirty-state provenance, and a figure is quoted with the machine it ran on.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "munch/tools/benchmark/provenance.hpp"
#include "munch/tools/probes/arguments.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/tools/probes/lcg64.hpp"
#include "munch/verifier/chunk_plan.hpp"
#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/ripple_radius.hpp"
#include "munch/verifier/rule_ceilings.hpp"
#include "munch/verifier/token_automaton.hpp"
#include "munch/verifier/verifier_decisions.hpp"

namespace
{
using munch::tools::benchmark::commit;
using munch::tools::benchmark::dirty;
using munch::tools::probes::files_under;
using munch::tools::probes::Lcg64;
using munch::tools::probes::positive_count;
using munch::tools::probes::read_bytes;
using munch::verifier::Certified_window;
using munch::verifier::Chunk_plan;
using munch::verifier::Finite_ripple;
using munch::verifier::Merge;
using munch::verifier::Plan_request;
using munch::verifier::Token_automaton;
using munch::verifier::Uniform_radii;
using munch::verifier::Verifier;

/**
 * @brief A token of a table, a byte by its value or a product by its merge's rank past the bytes.
 */
using Token_t = std::uint32_t;

/**
 * @brief An encoding, its tokens in order.
 */
using Tokens_t = std::vector<Token_t>;

/**
 * @brief The context budgets the supply is measured at, the widest window certified on the text.
 */
constexpr std::array<std::size_t, 6> budgets{1, 2, 3, 4, 6, 8};

/**
 * @brief The chunk counts the contract is measured at.
 */
constexpr std::array<std::size_t, 3> chunk_counts{2, 4, 8};

/**
 * @brief The budget the contract is measured at.
 */
constexpr std::size_t contract_budget{4};

/**
 * @brief The slice of each corpus the merge tower plans, its decisions being searches over the tower's product.
 */
constexpr std::size_t tower_bytes{2048};

/**
 * @brief The slice of each corpus the probe's encoder is held to the leftmost-lowest encoder on.
 */
constexpr std::size_t reference_bytes{4096};

/**
 * @brief The size of the generated text of runs of a.
 */
constexpr std::size_t runs_bytes{65536};

/**
 * @brief The longest run of a in the generated text.
 */
constexpr std::size_t longest_run{48};

/**
 * @brief The seed of the generated text of runs of a.
 */
constexpr std::uint64_t runs_seed{0x2026101318ULL};

/**
 * @brief The merges of the GPT-2 prefix of finite origin radius, the longest such.
 */
constexpr std::size_t finite_prefix{41};

/**
 * @brief The milliseconds in a second, the unit the times are printed in.
 */
constexpr double milliseconds{1000.0};

/**
 * @brief The bytes in a kibibyte, the unit the cuts are counted per.
 */
constexpr double bytes_per_kib{1024.0};

/**
 * @brief The fixture the GPT-2 table is read from, and the universe line naming it.
 */
constexpr std::string_view table_fixture{"/libs/verifier/tests/data/token_automaton_reference.txt"};

/**
 * @brief The prefix of the fixture line holding the 64 merges of GPT-2, each a left and a right part in hexadecimal.
 */
constexpr std::string_view table_line{"universe gpt2-64 tower "};

/**
 * @brief The benchmark's command line: the cap on every corpus, the timed passes, the CSV and the corpora.
 */
struct Bench_options
{
    /**
     * @brief The bytes every corpus is capped at, none when std::nullopt.
     */
    std::optional<std::size_t> cap{};

    /**
     * @brief The timed passes of each measurement.
     */
    std::size_t passes{3};

    /**
     * @brief The CSV the observations are appended to, empty for none.
     */
    std::string csv_path{};

    /**
     * @brief The corpora, each a file or a directory of sources.
     */
    std::vector<std::string> corpora{};
};

/**
 * @brief A corpus measured: its name and its bytes.
 */
struct Corpus
{
    /**
     * @brief The name it is printed under, its path or the generated text's.
     */
    std::string name{};

    /**
     * @brief The bytes.
     */
    std::string bytes{};
};

/**
 * @brief A corpus's sequential encoding: its tokens, the boundary after each, and the best time of encoding it.
 */
struct Sequential
{
    /**
     * @brief The tokens.
     */
    Tokens_t tokens{};

    /**
     * @brief The index after each token, ascending.
     */
    std::vector<std::size_t> boundaries{};

    /**
     * @brief The best time of the passes, in seconds.
     */
    double seconds{};
};

/**
 * @brief The cuts a plan of one chunk per byte finds, their gaps, and what finding them cost.
 */
struct Supply
{
    /**
     * @brief The cuts.
     */
    std::size_t cuts{};

    /**
     * @brief The median gap between consecutive cuts, the input's ends counted as cuts.
     */
    std::size_t median_gap{};

    /**
     * @brief The 95th percentile of the gaps.
     */
    std::size_t p95_gap{};

    /**
     * @brief The longest gap.
     */
    std::size_t longest_gap{};

    /**
     * @brief The window decisions asked for.
     */
    std::size_t asked{};

    /**
     * @brief The distinct windows decided.
     */
    std::size_t decided{};

    /**
     * @brief The planning time, in seconds.
     */
    double seconds{};
};

/**
 * @brief The contract at one chunk count: the chunks achieved, the best times of planning, of deciding the evidence
 *        again and of the workers with the stitching, the imbalance and the fallback volume.
 */
struct Contract
{
    /**
     * @brief The chunks the plan achieved.
     */
    std::size_t achieved{};

    /**
     * @brief The distinct windows the plan decided.
     */
    std::size_t decided{};

    /**
     * @brief The best planning time, in seconds.
     */
    double plan_seconds{};

    /**
     * @brief The best time of deciding the plan's evidence windows again alone, in seconds.
     */
    double certify_seconds{};

    /**
     * @brief The best time of the workers and the stitching, in seconds.
     */
    double worker_seconds{};

    /**
     * @brief The largest chunk over the ideal, the input's size over the chunks asked for.
     */
    double imbalance{};

    /**
     * @brief The bytes scanned past their floors for want of a cut, the dropped targets' to the input's end.
     */
    std::size_t fallback{};
};

/**
 * @brief The encoder a worker and the sequential encoding run: one left-to-right pass per merge in rank order, each
 *        joining every occurrence of its parts it reaches, which under the ordering condition is the encoder that
 *        applies the lowest-ranked merge present at its leftmost occurrence.
 */
class Encoder
{
public:
    /**
     * @brief Resolves the table's parts to tokens.
     * @param merges The admissible merge table in rank order.
     */
    explicit Encoder(std::span<const Merge> merges);

    /**
     * @brief Encodes a text.
     * @param text The text.
     * @return The tokens.
     */
    [[nodiscard]] Tokens_t encoded(std::string_view text) const;

    /**
     * @brief Returns a token's length in bytes.
     * @param token The token.
     * @return The length.
     */
    [[nodiscard]] std::size_t length(Token_t token) const;

    /**
     * @brief Returns a token's spelling.
     * @param token The token.
     * @return The spelling.
     */
    [[nodiscard]] const std::string& spelling(Token_t token) const;

private:
    /**
     * @brief The number of bytes, which the tokens carry before the products.
     */
    static constexpr Token_t byte_count{256};

    /**
     * @brief The parts of each merge, by rank.
     */
    std::vector<std::pair<Token_t, Token_t>> parts_{};

    /**
     * @brief The spelling of every token, by token.
     */
    std::vector<std::string> spellings_{};
};

/**
 * @brief The observations a run appends to its CSV, one row per figure under the run's stamp, the commit and the dirty
 *        state; a failed open or write is printed and fails the run.
 */
class Observations
{
public:
    /**
     * @brief Starts a run stamped with the current time.
     * @param csv_path The CSV appended to, empty for none.
     */
    explicit Observations(std::string csv_path);

    /**
     * @brief Appends one row to the CSV, the header first when the file is absent or empty, at round-trip precision;
     *        nothing without a CSV. Prints `CSV open failed` or `CSV write failed` with the path when the open or the
     *        write fails.
     * @param corpus The corpus's name.
     * @param table The table's name.
     * @param budget The budget, zero where none applies.
     * @param chunks The chunks asked for, zero for a plan of one chunk per byte.
     * @param figure The figure's name.
     * @param value The figure.
     */
    void row(
            std::string_view corpus, std::string_view table, std::size_t budget, std::size_t chunks,
            std::string_view figure, double value);

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
     * @brief The run's stamp, the time the run started in seconds since the epoch.
     */
    std::size_t run_{};

    /**
     * @brief Whether every open and write so far succeeded.
     */
    bool healthy_{true};
};

Encoder::Encoder(const std::span<const Merge> merges)
{
    for (Token_t byte{0}; byte < byte_count; ++byte)
    {
        const std::string spelling{static_cast<char>(byte)};

        spellings_.push_back(spelling);
    }

    const auto token_of{[this](const std::string& spelling) {
        const auto found{std::ranges::find(spellings_, spelling)};

        return static_cast<Token_t>(found - spellings_.begin());
    }};

    for (const auto& [left, right] : merges)
    {
        const auto left_token{token_of(left)};
        const auto right_token{token_of(right)};

        parts_.emplace_back(left_token, right_token);
        spellings_.push_back(left + right);
    }
}

Tokens_t Encoder::encoded(const std::string_view text) const
{
    Tokens_t tokens{};

    tokens.reserve(text.size());

    for (const auto byte : text)
    {
        tokens.push_back(static_cast<unsigned char>(byte));
    }

    Tokens_t joined{};

    joined.reserve(text.size());

    for (std::size_t rank{0}; rank < parts_.size(); ++rank)
    {
        const auto [left, right]{parts_[rank]};

        const auto product{static_cast<Token_t>(byte_count + rank)};

        joined.clear();

        for (std::size_t index{0}; index < tokens.size(); ++index)
        {
            const auto joins{index + 1 < tokens.size() && tokens[index] == left && tokens[index + 1] == right};

            if (joins)
            {
                joined.push_back(product);
                ++index;

                continue;
            }

            joined.push_back(tokens[index]);
        }

        std::swap(tokens, joined);
    }

    return tokens;
}

std::size_t Encoder::length(const Token_t token) const
{
    return spellings_[token].size();
}

const std::string& Encoder::spelling(const Token_t token) const
{
    return spellings_[token];
}

Observations::Observations(std::string csv_path)
    : csv_path_{std::move(csv_path)}, run_{static_cast<std::size_t>(std::time(nullptr))}
{}

void Observations::row(
        const std::string_view corpus, const std::string_view table, const std::size_t budget, const std::size_t chunks,
        const std::string_view figure, const double value)
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
        csv << "run,commit,dirty,corpus,table,budget,chunks,figure,value\n";
    }

    csv << run_ << ',' << commit << ',' << (dirty ? "yes" : "no") << ',' << corpus << ',' << table << ',' << budget
        << ',' << chunks << ',' << figure << ',' << value << '\n';

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
 * @brief Returns a wall-clock duration in seconds.
 * @param elapsed The duration.
 * @return The seconds.
 */
double elapsed_s(const std::chrono::steady_clock::duration elapsed)
{
    return std::chrono::duration<double>(elapsed).count();
}

/**
 * @brief Returns the best time of running a step some passes, and the step's result from the last pass.
 * @tparam Step The step's type.
 * @param passes The passes, at least one.
 * @param step The step.
 * @return The result and the best time in seconds.
 */
template <typename Step>
auto best_of(const std::size_t passes, const Step& step)
{
    auto best{std::numeric_limits<double>::max()};

    decltype(step()) result{};

    for (std::size_t pass{0}; pass < passes; ++pass)
    {
        const auto started{std::chrono::steady_clock::now()};

        result = step();

        const auto elapsed{std::chrono::steady_clock::now() - started};

        best = std::min(best, elapsed_s(elapsed));
    }

    return std::pair{std::move(result), best};
}

/**
 * @brief Returns the bytes a text of hexadecimal digit pairs spells.
 * @param digits The digits, two per byte.
 * @return The bytes.
 */
std::string unhex(const std::string_view digits)
{
    std::string bytes{};

    for (std::size_t at{0}; at + 1 < digits.size(); at += 2)
    {
        const std::string pair{digits.substr(at, 2)};

        const auto value{std::stoi(pair, nullptr, 16)};

        bytes.push_back(static_cast<char>(value));
    }

    return bytes;
}

/**
 * @brief Reads the 64 merges of GPT-2 from the token automaton fixture, each part written in hexadecimal.
 * @return The merges in rank order, std::nullopt when the fixture or its line is not there.
 */
std::optional<std::vector<Merge>> gpt2_merges()
{
    std::ifstream file{std::string{SOURCE_DIR} + std::string{table_fixture}};

    for (std::string line{}; std::getline(file, line);)
    {
        if (!line.starts_with(table_line))
        {
            continue;
        }

        std::istringstream fields{line.substr(table_line.size())};

        std::vector<Merge> merges{};

        for (std::string merge{}; fields >> merge;)
        {
            const auto comma{merge.find(',')};

            const auto left{unhex(std::string_view{merge}.substr(0, comma))};
            const auto right{unhex(std::string_view{merge}.substr(comma + 1))};

            merges.push_back({.left = left, .right = right});
        }

        return merges;
    }

    return std::nullopt;
}

/**
 * @brief Reads a corpus: a file whole, or a directory as every .cpp and .hpp under it in sorted order, concatenated,
 *        capped at a number of bytes.
 * @param path The file or directory.
 * @param cap The bytes the corpus is capped at, none when std::nullopt.
 * @return The corpus, std::nullopt when a file cannot be read.
 */
std::optional<Corpus> corpus_of(const std::string& path, const std::optional<std::size_t> cap)
{
    std::vector<std::filesystem::path> files{path};

    if (std::filesystem::is_directory(path))
    {
        files = files_under(path, ".cpp");

        const auto headers{files_under(path, ".hpp")};

        files.insert(files.end(), headers.begin(), headers.end());

        std::ranges::sort(files);
    }

    std::string bytes{};

    for (const auto& file : files)
    {
        const auto read{read_bytes(file)};

        if (!read)
        {
            return std::nullopt;
        }

        bytes += *read;
    }

    if (cap && bytes.size() > *cap)
    {
        bytes.resize(*cap);
    }

    return Corpus{.name = path, .bytes = std::move(bytes)};
}

/**
 * @brief Generates a text of runs of a, each of one to longest_run bytes and followed by a b, drawn from an Lcg64 at a
 *        fixed seed.
 * @return The text, runs_bytes long.
 */
std::string runs_text()
{
    Lcg64 lcg{runs_seed};

    std::string text{};

    while (text.size() < runs_bytes)
    {
        const auto run{1 + lcg.next(longest_run)};

        text.append(run, 'a');
        text.push_back('b');
    }

    text.resize(runs_bytes);

    return text;
}

/**
 * @brief Returns the encoding of a text by the leftmost-lowest encoder: at every step the lowest-ranked merge present
 *        anywhere is applied at its leftmost occurrence, until none applies.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @return The tokens' spellings.
 */
std::vector<std::string> leftmost_lowest(const std::vector<Merge>& merges, const std::string_view text)
{
    std::vector<std::string> sequence{};

    for (const auto byte : text)
    {
        sequence.push_back(std::string{byte});
    }

    for (auto merged{true}; merged;)
    {
        merged = false;

        for (const auto& [left, right] : merges)
        {
            const auto is_pair{[&](const std::string& first, const std::string& second) {
                return first == left && second == right;
            }};

            const auto at{std::ranges::adjacent_find(sequence, is_pair)};

            if (at == sequence.end())
            {
                continue;
            }

            *at = left + right;
            sequence.erase(at + 1);
            merged = true;

            break;
        }
    }

    return sequence;
}

/**
 * @brief Returns whether the probe's encoder agrees with the leftmost-lowest encoder on a corpus's first bytes,
 *        printing the disagreement when not.
 * @param merges The merge table in rank order.
 * @param encoder The probe's encoder over the table.
 * @param corpus The corpus.
 * @return True when the two encodings of the slice are equal.
 */
bool encoder_agrees(const std::vector<Merge>& merges, const Encoder& encoder, const Corpus& corpus)
{
    const auto slice{std::string_view{corpus.bytes}.substr(0, reference_bytes)};

    const auto expected{leftmost_lowest(merges, slice)};

    std::vector<std::string> found{};

    for (const auto token : encoder.encoded(slice))
    {
        found.push_back(encoder.spelling(token));
    }

    if (found != expected)
    {
        std::printf("  the probe's encoder disagrees with the leftmost-lowest encoder on %s\n", corpus.name.c_str());

        return false;
    }

    return true;
}

/**
 * @brief Encodes a corpus sequentially some passes and returns its tokens, their boundaries and the best time.
 * @param encoder The encoder.
 * @param corpus The corpus.
 * @param passes The passes.
 * @return The sequential encoding.
 */
Sequential sequential_of(const Encoder& encoder, const Corpus& corpus, const std::size_t passes)
{
    const auto encode{[&] { return encoder.encoded(corpus.bytes); }};

    auto [tokens, seconds]{best_of(passes, encode)};

    std::vector<std::size_t> boundaries{};

    std::size_t end{0};

    for (const auto token : tokens)
    {
        end += encoder.length(token);
        boundaries.push_back(end);
    }

    return {.tokens = std::move(tokens), .boundaries = std::move(boundaries), .seconds = seconds};
}

/**
 * @brief Returns the first cut of a plan that is not a boundary of the sequential encoding.
 * @param plan The plan.
 * @param sequential The sequential encoding.
 * @return The cut, std::nullopt when every cut is a boundary.
 */
std::optional<std::size_t> false_cut(const Chunk_plan& plan, const Sequential& sequential)
{
    for (const auto& [begin, owned_end, read_end, evidence] : plan.chunks)
    {
        if (!std::ranges::binary_search(sequential.boundaries, owned_end))
        {
            return owned_end;
        }
    }

    return std::nullopt;
}

/**
 * @brief Returns a quantile of sorted values, the value at that fraction of their count, rounded down.
 * @param sorted The values, ascending, at least one.
 * @param numerator The fraction's numerator.
 * @param denominator The fraction's denominator.
 * @return The quantile.
 */
std::size_t quantile(const std::vector<std::size_t>& sorted, const std::size_t numerator, const std::size_t denominator)
{
    const auto at{(sorted.size() - 1) * numerator / denominator};

    return sorted[at];
}

/**
 * @brief Plans a corpus at one chunk per byte and returns the cuts found with their gaps and costs, holding every cut
 *        to the sequential boundaries and printing the first that is not one.
 * @tparam Plan The planner's type, called with a request.
 * @param plan The planner.
 * @param corpus The corpus.
 * @param sequential Its sequential encoding.
 * @param budget The budget.
 * @return The supply, std::nullopt when a cut is not a boundary.
 */
template <typename Plan>
std::optional<Supply> supply_of(
        const Plan& plan, const Corpus& corpus, const Sequential& sequential, const std::size_t budget)
{
    const Plan_request request{.chunks = corpus.bytes.size(), .budget = budget, .supplied = {}};

    const auto make{[&] { return plan(request); }};

    const auto [found, seconds]{best_of(1, make)};

    if (const auto cut{false_cut(found, sequential)})
    {
        std::printf("  cut %zu is not a boundary of the sequential encoding, the plan is wrong\n", *cut);

        return std::nullopt;
    }

    std::vector<std::size_t> gaps{};

    for (const auto& [begin, owned_end, read_end, evidence] : found.chunks)
    {
        gaps.push_back(owned_end - begin);
    }

    std::ranges::sort(gaps);

    return Supply{
            .cuts = found.chunks.size() - 1,
            .median_gap = quantile(gaps, 1, 2),
            .p95_gap = quantile(gaps, 95, 100),
            .longest_gap = gaps.back(),
            .asked = found.windows_asked,
            .decided = found.windows_decided,
            .seconds = seconds};
}

/**
 * @brief Encodes one chunk as its worker does: the bytes from its first to its read end alone, keeping the tokens below
 *        its cut.
 * @param encoder The encoder.
 * @param corpus The corpus.
 * @param chunk The chunk.
 * @return The tokens the worker keeps.
 */
Tokens_t worker_tokens(const Encoder& encoder, const Corpus& corpus, const munch::verifier::Planned_chunk& chunk)
{
    const auto& [begin, owned_end, read_end, evidence]{chunk};

    const auto read{std::string_view{corpus.bytes}.substr(begin, read_end - begin)};

    auto tokens{encoder.encoded(read)};

    std::size_t kept{0};

    for (auto spent{begin}; kept < tokens.size() && spent < owned_end; ++kept)
    {
        spent += encoder.length(tokens[kept]);
    }

    tokens.resize(kept);

    return tokens;
}

/**
 * @brief Runs a plan's workers, one jthread per chunk but the last, which the calling thread encodes, and stitches
 *        their tokens in chunk order.
 * @param encoder The encoder.
 * @param corpus The corpus.
 * @param plan The plan.
 * @return The stitched tokens.
 */
Tokens_t stitched(const Encoder& encoder, const Corpus& corpus, const Chunk_plan& plan)
{
    const auto& chunks{plan.chunks};

    std::vector<Tokens_t> pieces(chunks.size());

    {
        std::vector<std::jthread> workers{};

        for (std::size_t index{0}; index + 1 < chunks.size(); ++index)
        {
            workers.emplace_back([&, index] { pieces[index] = worker_tokens(encoder, corpus, chunks[index]); });
        }

        pieces.back() = worker_tokens(encoder, corpus, chunks.back());
    }

    Tokens_t tokens{};

    for (const auto& piece : pieces)
    {
        tokens.insert(tokens.end(), piece.begin(), piece.end());
    }

    return tokens;
}

/**
 * @brief Returns the bytes a plan scans past its floors for want of a cut: from each target's floor to the cut it
 *        found, and from the first target with none to the input's end.
 * @param plan The plan.
 * @param size The input's size.
 * @param chunks The chunks asked for.
 * @return The fallback volume.
 */
std::size_t fallback_of(const Chunk_plan& plan, const std::size_t size, const std::size_t chunks)
{
    const auto usable{std::min(chunks, size)};

    std::size_t volume{0};

    std::size_t begin{0};

    for (std::size_t index{1}; index < usable; ++index)
    {
        const auto target{index * (size / usable) + index * (size % usable) / usable};

        const auto floor{std::max(target, begin + 1)};

        if (index >= plan.chunks.size())
        {
            return volume + (size - floor);
        }

        const auto cut{plan.chunks[index - 1].owned_end};

        volume += cut - floor;
        begin = cut;
    }

    return volume;
}

/**
 * @brief Returns the largest chunk of a plan over the ideal, the input's size over the chunks asked for.
 * @param plan The plan.
 * @param size The input's size.
 * @param chunks The chunks asked for.
 * @return The imbalance.
 */
double imbalance_of(const Chunk_plan& plan, const std::size_t size, const std::size_t chunks)
{
    std::size_t largest{0};

    for (const auto& [begin, owned_end, read_end, evidence] : plan.chunks)
    {
        largest = std::max(largest, owned_end - begin);
    }

    const auto ideal{static_cast<double>(size) / static_cast<double>(chunks)};

    return static_cast<double>(largest) / ideal;
}

/**
 * @brief Measures the contract at one chunk count: plans, decides the evidence windows again alone, runs the workers
 *        and stitches, each the best of the passes, holding every run's stitched tokens to the sequential encoding and
 *        every cut to its boundaries, printing the first failure.
 * @tparam Plan The planner's type, called with a request.
 * @tparam Certify The evidence decision's type, called with a plan.
 * @param plan The planner.
 * @param certify Decides a plan's evidence windows again.
 * @param encoder The encoder.
 * @param corpus The corpus.
 * @param sequential Its sequential encoding.
 * @param chunks The chunks asked for.
 * @param passes The passes.
 * @return The contract, std::nullopt when a cut is not a boundary or a stitched run differs.
 */
template <typename Plan, typename Certify>
std::optional<Contract> contract_of(
        const Plan& plan, const Certify& certify, const Encoder& encoder, const Corpus& corpus,
        const Sequential& sequential, const std::size_t chunks, const std::size_t passes)
{
    const Plan_request request{.chunks = chunks, .budget = contract_budget, .supplied = {}};

    const auto make{[&] { return plan(request); }};

    const auto [found, plan_seconds]{best_of(passes, make)};

    if (const auto cut{false_cut(found, sequential)})
    {
        std::printf("  cut %zu is not a boundary of the sequential encoding, the plan is wrong\n", *cut);

        return std::nullopt;
    }

    const auto recertify{[&] { return certify(found); }};

    const auto [certified, certify_seconds]{best_of(passes, recertify)};

    if (!certified)
    {
        std::printf("  an evidence window of the plan no longer certifies its cut\n");

        return std::nullopt;
    }

    auto agrees{true};

    const auto work{[&] {
        auto tokens{stitched(encoder, corpus, found)};

        agrees = agrees && tokens == sequential.tokens;

        return tokens.size();
    }};

    const auto [token_count, worker_seconds]{best_of(passes, work)};

    if (!agrees)
    {
        std::printf("  the stitched tokens differ from the sequential encoding at %zu chunks\n", chunks);

        return std::nullopt;
    }

    const auto size{corpus.bytes.size()};

    return Contract{
            .achieved = found.chunks.size(),
            .decided = found.windows_decided,
            .plan_seconds = plan_seconds,
            .certify_seconds = certify_seconds,
            .worker_seconds = worker_seconds,
            .imbalance = imbalance_of(found, size, chunks),
            .fallback = fallback_of(found, size, chunks)};
}

/**
 * @brief Returns whether a plan's evidence windows are certified again, each decided alone by miscovering() on the
 *        automaton at its origin.
 * @param automaton The token automaton.
 * @param corpus The corpus.
 * @param plan The plan.
 * @return True when every evidence window is certified at its origin.
 */
bool automaton_certifies(const Token_automaton& automaton, const Corpus& corpus, const Chunk_plan& plan)
{
    for (const auto& [begin, owned_end, read_end, evidence] : plan.chunks)
    {
        if (!std::holds_alternative<Certified_window>(evidence))
        {
            continue;
        }

        const auto& [window_begin, width, origin]{std::get<Certified_window>(evidence)};

        const auto window{std::string_view{corpus.bytes}.substr(window_begin, width)};

        if (munch::verifier::miscovering(automaton, window, origin))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Returns whether a plan's evidence windows are certified again on a verifier, each decided alone by
 *        pinned_cut() at its last byte and dependence() at its origin.
 * @param verifier The verifier.
 * @param corpus The corpus.
 * @param plan The plan.
 * @return True when every evidence window pins its origin with every certified cut independent.
 */
bool verifier_certifies(const Verifier& verifier, const Corpus& corpus, const Chunk_plan& plan)
{
    for (const auto& [begin, owned_end, read_end, evidence] : plan.chunks)
    {
        if (!std::holds_alternative<Certified_window>(evidence))
        {
            continue;
        }

        const auto& [window_begin, width, origin]{std::get<Certified_window>(evidence)};

        const auto window{std::string_view{corpus.bytes}.substr(window_begin, width)};

        const auto pinned{munch::verifier::pinned_cut(verifier, window, width - 1)};

        const auto pins{std::holds_alternative<std::size_t>(pinned) && std::get<std::size_t>(pinned) == origin};

        if (!pins || munch::verifier::dependence(verifier, window, origin))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief A table the probe plans over: its name, its merges, its encoder, its token automaton and its uniform radii.
 */
struct Bench_table
{
    /**
     * @brief The name it is printed under.
     */
    std::string name{};

    /**
     * @brief The merges in rank order.
     */
    std::vector<Merge> merges{};

    /**
     * @brief The radii of its uniform window, none when its origin radius is infinite.
     */
    std::optional<Uniform_radii> radii{};
};

/**
 * @brief Returns a table with its uniform radii, printing them with the rule ceilings: the origin radius by
 *        ripple_radius(), the right radii by rule_ceilings() and the ceilings beside them.
 * @param name The table's name.
 * @param merges The merges in rank order.
 * @return The table.
 */
Bench_table bench_table(std::string name, std::vector<Merge> merges)
{
    const auto ripple{munch::verifier::ripple_radius(merges)};

    const auto ceilings{munch::verifier::rule_ceilings(merges)};

    const auto finite{std::holds_alternative<Finite_ripple>(ripple)};

    const auto radius{finite ? std::get<Finite_ripple>(ripple).radius : std::size_t{0}};

    const auto radius_text{finite ? std::to_string(radius) : std::string{"infinite"}};

    const auto ceiling_text{ceilings.radius_ceiling ? std::to_string(*ceilings.radius_ceiling) : std::string{"none"}};

    std::printf(
            "\ntable %s: %zu merges, origin radius %s (ceiling %s), right span radius %zu, right start radius %zu "
            "(ceiling %zu)\n",
            name.c_str(), merges.size(), radius_text.c_str(), ceiling_text.c_str(), ceilings.span_radius,
            ceilings.start_radius, ceilings.start_ceiling);

    const auto radii{
            finite ? std::optional{Uniform_radii{.radius = radius, .start_radius = ceilings.start_radius}} :
                     std::nullopt};

    if (radii)
    {
        std::printf("  uniform window of width %zu\n", radii->radius + radii->start_radius + 1);
    }

    return {.name = std::move(name), .merges = std::move(merges), .radii = radii};
}

/**
 * @brief Prints and records the supply of one route at every budget, returning false at the first plan with a cut that
 *        is not a boundary.
 * @tparam Plan The planner's type, called with a request.
 * @param route The route's name.
 * @param plan The planner.
 * @param table The table.
 * @param corpus The corpus.
 * @param sequential Its sequential encoding.
 * @param observations The run's observations.
 * @return True when every cut of every plan is a boundary.
 */
template <typename Plan>
bool supply_rows(
        const std::string_view route, const Plan& plan, const Bench_table& table, const Corpus& corpus,
        const Sequential& sequential, Observations& observations)
{
    const auto kib{static_cast<double>(corpus.bytes.size()) / bytes_per_kib};

    const auto boundaries{static_cast<double>(sequential.boundaries.size())};

    for (const auto budget : budgets)
    {
        const auto supply{supply_of(plan, corpus, sequential, budget)};

        if (!supply)
        {
            return false;
        }

        const auto& [cuts, median_gap, p95_gap, longest_gap, asked, decided, seconds]{*supply};

        const auto share{static_cast<double>(cuts) / boundaries};

        const auto reuse{asked == 0 ? 0.0 : 1.0 - static_cast<double>(decided) / static_cast<double>(asked)};

        std::printf(
                "  %-9s budget %zu: %8zu cuts, %5.1f%% of boundaries, %7.1f per KiB, gap median %zu p95 %zu "
                "longest %zu, %zu decided of %zu asked (reuse %.1f%%), %.1f ms\n",
                std::string{route}.c_str(), budget, cuts, 100.0 * share, static_cast<double>(cuts) / kib, median_gap,
                p95_gap, longest_gap, decided, asked, 100.0 * reuse, milliseconds * seconds);

        const auto figure{[&](const std::string_view name, const double value) {
            const auto column{std::string{route} + "-" + std::string{name}};

            observations.row(corpus.name, table.name, budget, 0, column, value);
        }};

        figure("cuts", static_cast<double>(cuts));
        figure("longest-gap", static_cast<double>(longest_gap));
        figure("decided", static_cast<double>(decided));
        figure("plan-seconds", seconds);
    }

    return true;
}

/**
 * @brief Prints and records the contract of one route at every chunk count, returning false at the first plan whose cut
 *        is not a boundary or whose stitched run differs.
 * @tparam Plan The planner's type, called with a request.
 * @tparam Certify The evidence decision's type, called with a plan.
 * @param route The route's name.
 * @param plan The planner.
 * @param certify Decides a plan's evidence windows again.
 * @param table The table.
 * @param corpus The corpus.
 * @param sequential Its sequential encoding.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when every plan holds.
 */
template <typename Plan, typename Certify>
bool contract_rows(
        const std::string_view route, const Plan& plan, const Certify& certify, const Bench_table& table,
        const Corpus& corpus, const Sequential& sequential, const Bench_options& options, Observations& observations)
{
    const Encoder encoder{table.merges};

    for (const auto chunks : chunk_counts)
    {
        const auto contract{contract_of(plan, certify, encoder, corpus, sequential, chunks, options.passes)};

        if (!contract)
        {
            return false;
        }

        const auto& [achieved, decided, plan_seconds, certify_seconds, worker_seconds, imbalance, fallback]{*contract};

        const auto end_to_end{plan_seconds + worker_seconds};

        const auto speedup{sequential.seconds / end_to_end};

        const auto fallback_share{static_cast<double>(fallback) / static_cast<double>(corpus.bytes.size())};

        std::printf(
                "  %-9s x%zu: %zu chunks, plan %.3f ms, decided %zu, evidence alone %.3f ms, workers and "
                "stitching %.3f ms, sequential %.3f ms, speedup %.3gx, imbalance %.2f, fallback %zu bytes (%.1f%%)\n",
                std::string{route}.c_str(), chunks, achieved, milliseconds * plan_seconds, decided,
                milliseconds * certify_seconds, milliseconds * worker_seconds, milliseconds * sequential.seconds,
                speedup, imbalance, fallback, 100.0 * fallback_share);

        const auto figure{[&](const std::string_view name, const double value) {
            const auto column{std::string{route} + "-" + std::string{name}};

            observations.row(corpus.name, table.name, contract_budget, chunks, column, value);
        }};

        figure("chunks", static_cast<double>(achieved));
        figure("decided", static_cast<double>(decided));
        figure("plan-seconds", plan_seconds);
        figure("evidence-seconds", certify_seconds);
        figure("worker-seconds", worker_seconds);
        figure("sequential-seconds", sequential.seconds);
        figure("imbalance", imbalance);
        figure("fallback", static_cast<double>(fallback));
    }

    return true;
}

/**
 * @brief Measures one table on one corpus: the encoder held to the leftmost-lowest encoder, the sequential encoding,
 *        the supply and the contract on the token automaton with and without the uniform window, and, for the table
 *        the tower is asked of, the verifier route on a slice.
 * @param table The table.
 * @param corpus The corpus.
 * @param with_tower Whether the merge tower also plans a slice.
 * @param options The run's options.
 * @param observations The run's observations.
 * @return True when every assertion holds.
 */
bool measure(
        const Bench_table& table, const Corpus& corpus, const bool with_tower, const Bench_options& options,
        Observations& observations)
{
    const Encoder encoder{table.merges};

    if (!encoder_agrees(table.merges, encoder, corpus))
    {
        return false;
    }

    const auto sequential{sequential_of(encoder, corpus, options.passes)};

    std::printf(
            "corpus %s under %s: %zu bytes, %zu tokens, sequential %.3f ms\n", corpus.name.c_str(), table.name.c_str(),
            corpus.bytes.size(), sequential.tokens.size(), milliseconds * sequential.seconds);

    const auto automaton{munch::verifier::token_automaton(table.merges)};

    const auto windows{[&](const Plan_request& request) {
        return munch::verifier::chunk_plan(automaton, corpus.bytes, request, std::nullopt);
    }};

    const auto uniform{[&](const Plan_request& request) {
        return munch::verifier::chunk_plan(automaton, corpus.bytes, request, table.radii);
    }};

    const auto on_automaton{[&](const Chunk_plan& plan) { return automaton_certifies(automaton, corpus, plan); }};

    if (!supply_rows("windows", windows, table, corpus, sequential, observations))
    {
        return false;
    }

    if (table.radii && !supply_rows("uniform", uniform, table, corpus, sequential, observations))
    {
        return false;
    }

    if (!contract_rows("windows", windows, on_automaton, table, corpus, sequential, options, observations))
    {
        return false;
    }

    if (table.radii &&
        !contract_rows("uniform", uniform, on_automaton, table, corpus, sequential, options, observations))
    {
        return false;
    }

    if (!with_tower)
    {
        return true;
    }

    const auto tower{munch::verifier::merge_tower(table.merges)};

    const Corpus slice{.name = corpus.name + " (slice)", .bytes = corpus.bytes.substr(0, tower_bytes)};

    const auto slice_sequential{sequential_of(encoder, slice, options.passes)};

    const auto on_tower{
            [&](const Plan_request& request) { return munch::verifier::chunk_plan(tower, slice.bytes, request); }};

    const auto on_verifier{[&](const Chunk_plan& plan) { return verifier_certifies(tower, slice, plan); }};

    std::printf("  the merge tower of %zu states plans the first %zu bytes\n", tower.state_count(), slice.bytes.size());

    return contract_rows("tower", on_tower, on_verifier, table, slice, slice_sequential, options, observations);
}

/**
 * @brief Reads the command line positionally: a first argument that is wholly a positive whole number is the cap in
 *        bytes, then a second one so is the passes, then a third is the CSV path, `-` for none; every argument after
 *        those taken is a corpus.
 * @param arguments The arguments after the program name.
 * @return The options, each one not given at its default: no cap, 3 passes, no CSV, munch's own sources.
 */
Bench_options options_of(const std::vector<std::string>& arguments)
{
    Bench_options options{};

    const auto with_corpora{[&arguments, &options](const std::size_t taken) {
        const auto corpora{arguments | std::views::drop(taken)};

        options.corpora.assign(corpora.begin(), corpora.end());

        if (options.corpora.empty())
        {
            options.corpora.push_back(std::string{SOURCE_DIR} + "/libs");
        }

        return options;
    }};

    const auto number_at{[&arguments](const std::size_t index) {
        return index < arguments.size() ? positive_count(arguments[index]) : std::nullopt;
    }};

    const auto cap{number_at(0)};

    if (!cap)
    {
        return with_corpora(0);
    }

    options.cap = cap;

    const auto passes{number_at(1)};

    if (!passes)
    {
        return with_corpora(1);
    }

    options.passes = *passes;

    if (arguments.size() <= 2)
    {
        return with_corpora(2);
    }

    options.csv_path = arguments[2] == "-" ? std::string{} : arguments[2];

    return with_corpora(3);
}

/**
 * @brief Prints the run's banner: the commit with its dirty state, the cap, the passes and whether the observations are
 *        recorded.
 * @param options The run's options.
 */
void print_banner(const Bench_options& options)
{
    std::printf("chunk plans for byte-pair encoding against the sequential encoding\n");

    const std::string commit_text{commit};

    const auto dirty_note{dirty ? " (uncommitted changes present)" : ""};

    const auto cap_text{
            options.cap ? std::to_string(*options.cap) + " bytes per corpus" : std::string{"whole corpora"}};

    const auto csv_note{options.csv_path.empty() ? ", dev run, observations discarded" : ", observations recorded"};

    std::printf(
            "  commit %s%s, %s, %zu passes, %u hardware threads%s\n", commit_text.c_str(), dirty_note, cap_text.c_str(),
            options.passes, std::thread::hardware_concurrency(), csv_note);
}

} // namespace

/**
 * @brief Runs the benchmark: the tables with their radii, then every corpus under the GPT-2 tables, the merge tower of
 *        the 64 merges on a slice of each, and the generated text of runs under (a, b), (a, ab).
 * @param argc The argument count.
 * @param argv The cap in bytes, the passes, the CSV path or `-`, and the corpora, all optional.
 * @return EXIT_SUCCESS when every assertion holds, every corpus was read and every CSV write succeeded, EXIT_FAILURE
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

    Observations observations{options.csv_path};

    print_banner(options);

    auto merges{gpt2_merges()};

    if (!merges)
    {
        std::fprintf(stderr, "cannot read the GPT-2 table from %s%s\n", SOURCE_DIR, table_fixture.data());

        return EXIT_FAILURE;
    }

    std::vector prefix(merges->begin(), merges->begin() + finite_prefix);

    const auto whole_table{bench_table("gpt2-64", std::move(*merges))};

    const auto prefix_table{bench_table("gpt2-41", std::move(prefix))};

    const auto runs_table{bench_table("ab-aab", {{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}})};

    for (const auto& path : options.corpora)
    {
        const auto corpus{corpus_of(path, options.cap)};

        if (!corpus)
        {
            std::fprintf(stderr, "cannot read corpus %s\n", path.c_str());

            return EXIT_FAILURE;
        }

        std::printf("\n");

        if (!measure(whole_table, *corpus, true, options, observations))
        {
            return EXIT_FAILURE;
        }

        std::printf("\n");

        if (!measure(prefix_table, *corpus, false, options, observations))
        {
            return EXIT_FAILURE;
        }
    }

    const Corpus runs{.name = "generated runs of a", .bytes = runs_text()};

    std::printf("\n");

    if (!measure(runs_table, runs, false, options, observations))
    {
        return EXIT_FAILURE;
    }

    std::printf("\nall assertions hold; run-local figures, quoted with the machine they ran on\n");

    return observations.is_healthy() ? EXIT_SUCCESS : EXIT_FAILURE;
}

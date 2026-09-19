// Recomputes what munch's own decisions reach of the certified-splitting paper's emissions, so that every line it
// recomputes can be held against the library line by line; what it does not reach the comparison names.
//
// The paper's data directory holds emissions written by explorations/certification in the research repository:
// splitting_measurements.py, campaign_inventory.py, frozen_inventory.py, depth_series.py, budget_coverage.py and the
// wide cutoff sweep of explore_certification.py. Each figure there is a decision of the Python's own deciders over a
// token set and a corpus slice, and this probe makes the same decisions with munch: the byte certificate is
// Lexer::is_split_point(), the exact window certificate is window_counterexample() exhaustive with no witness, the
// occurring reading is window_occurrence(), the anchor supply is tools/audit's supply() over a report holding those
// decisions, the gap corollary is anchor_free_span(), and the differential is boundary_difference(). Beside the exact
// window decision the conservative one, is_split_window(), is reported on a line of its own, so a reader sees what
// the shipped model gives where the paper's decider is the exact verifier.
//
// Every line written is in the format of the committed emission it recomputes, one file per emission under the
// output directory, and the comparison script, analysis/paper4-recompute/compare.py, diffs the two directories line
// by line. Nothing here is typed from the paper: the token sets are rebuilt as the Python builds them, the trained
// byte-pair vocabulary by the same merge procedure with the same tie rule and the GPT-2 subset from the same merge
// table, and each list is digested so that the configuration line's digest is what proves the token set is the
// paper's; the edit trials replay the Python's own seeded generator, ported here, so the draws are the paper's.
//
// Usage: munch_paper4_recompute <output directory> [<twitter.json> <gpt2-merges.txt> <campaign corpus directory>]
//                               [section...]
//
// The corpus is jsonexamples/twitter.json of simdjson v3.10.1, the merge table is the GPT-2 release's, and the
// campaign corpora are the recovery paper's archived r6 corpora, the four c-like rows; none is redistributed here,
// and the research repository's gate, scripts/check-paper4.sh, names where each lives. A section name restricts the
// run to it: vocabularies, budget, depth, frozen, campaign, sweep; with none every section runs. With the output
// directory alone the probe runs the sections that need no external file, the UTF-8 shape's sync distance and the
// wide cutoff sweep, which is what the test entry runs.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/supply.hpp"

namespace
{
using munch::core::Lexer;
using munch::tools::audit::Certified_window;
using munch::tools::audit::Report;

// The paper's byte-pair experiment: the corpus prefix the vocabulary is trained on, the evaluation and calibration
// slices after it, the merge depths, the GPT-2 prefix, the window budget, the width the edit trials probe past it,
// the sampled inventory the exhaustive one replaced, the edit design and the campaign's edit design.
constexpr std::size_t train_bytes{65536};
constexpr std::size_t eval_bytes{16384};
constexpr std::size_t calibration_bytes{16384};
constexpr std::size_t merges_local{384};
constexpr std::array<std::size_t, 4> depths{48, 96, 192, 384};
constexpr std::size_t gpt2_merges{50000};
constexpr std::size_t gpt2_prefix{4096};
constexpr std::array<std::size_t, 3> widths{2, 3, 4};
constexpr std::size_t probe_width{5};
constexpr std::size_t legacy_per_width{20};
constexpr std::uint32_t edit_seed{20260826};
constexpr std::size_t edit_trials_wanted{200};
constexpr std::size_t edit_attempts_cap{4000};
constexpr std::uint32_t campaign_edit_seed{20260827};
constexpr std::size_t campaign_cross_class_edits{60};
constexpr std::size_t campaign_same_class_edits{12};
constexpr std::size_t campaign_slice_bytes{32768};
constexpr std::size_t block_bytes{1024};

// The campaign corpora are named after the archive they were drawn for, the recovery paper's r6 collection.
constexpr std::string_view campaign_archive{"recovery-quality-six-rows-512k-500-r6.csv"};

/**
 * @brief One certified pair as the paper's inventories hold it: the window's bytes and the origin.
 */
struct Pair
{
    std::string window;

    std::size_t origin;

    auto operator<=>(const Pair&) const = default;
};

/**
 * @brief The lines of one emission, written to the file of the committed emission's name.
 */
struct Emission
{
    std::string file;

    std::vector<std::string> lines;
};

/**
 * @brief SHA-256 of a byte string, as hexadecimal, the digest the paper names token lists and slices by.
 * @param text The bytes.
 * @return The 64 hexadecimal digits.
 */
[[nodiscard]] std::string sha256(const std::string_view text)
{
    static constexpr std::array<std::uint32_t, 64> round_constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // The message, padded: a one bit, zeros to 56 mod 64, and the bit length big-endian.
    std::string padded{text};

    padded.push_back(static_cast<char>(0x80));

    while (padded.size() % 64 != 56)
    {
        padded.push_back('\0');
    }

    const auto bits{static_cast<std::uint64_t>(text.size()) * 8};

    for (int shift{56}; shift >= 0; shift -= 8)
    {
        padded.push_back(static_cast<char>((bits >> static_cast<unsigned>(shift)) & 0xFF));
    }

    const auto rotate{
            [](const std::uint32_t value, const unsigned bits) { return (value >> bits) | (value << (32 - bits)); }};

    for (std::size_t block{0}; block < padded.size(); block += 64)
    {
        std::array<std::uint32_t, 64> schedule{};

        for (std::size_t word{0}; word < 16; ++word)
        {
            for (std::size_t byte{0}; byte < 4; ++byte)
            {
                schedule[word] = (schedule[word] << 8) | static_cast<unsigned char>(padded[block + (word * 4) + byte]);
            }
        }

        for (std::size_t word{16}; word < 64; ++word)
        {
            const auto s0{
                    rotate(schedule[word - 15], 7) ^ rotate(schedule[word - 15], 18) ^ (schedule[word - 15] >> 3)};

            const auto s1{rotate(schedule[word - 2], 17) ^ rotate(schedule[word - 2], 19) ^ (schedule[word - 2] >> 10)};

            schedule[word] = schedule[word - 16] + s0 + schedule[word - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h]{state};

        for (std::size_t round{0}; round < 64; ++round)
        {
            const auto s1{rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)};

            const auto choice{(e & f) ^ (~e & g)};

            const auto t1{h + s1 + choice + round_constants[round] + schedule[round]};

            const auto s0{rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)};

            const auto majority{(a & b) ^ (a & c) ^ (b & c)};

            const auto t2{s0 + majority};

            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::string hex;

    for (const auto word : state)
    {
        hex += std::format("{:08x}", word);
    }

    return hex;
}

/**
 * @brief A byte string as Python encodes the latin-1 text it reads the corpus into: UTF-8, every byte from 0x80 up
 *        two bytes, which is what the paper's token digests are taken over.
 * @param bytes The bytes.
 * @return The UTF-8 encoding.
 */
[[nodiscard]] std::string utf8_of_latin1(const std::string_view bytes)
{
    std::string out;

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (value < 0x80)
        {
            out.push_back(byte);
        }
        else
        {
            out.push_back(static_cast<char>(0xC0 | (value >> 6U)));
            out.push_back(static_cast<char>(0x80 | (value & 0x3FU)));
        }
    }

    return out;
}

/**
 * @brief The first sixteen hexadecimal digits of the paper's token-list digest: SHA-256 over the tokens joined by a
 *        zero byte, in the Python's UTF-8 encoding of its latin-1 strings.
 * @param tokens The token list, repeats included, in the order the Python holds it.
 * @return The digest prefix the configuration line carries.
 */
[[nodiscard]] std::string token_digest(const std::vector<std::string>& tokens)
{
    std::string joined;

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        if (index > 0)
        {
            joined.push_back('\0');
        }

        joined += tokens[index];
    }

    return sha256(utf8_of_latin1(joined)).substr(0, 16);
}

/**
 * @brief Reads a whole file as bytes.
 * @param path The file.
 * @return Its bytes.
 * @throws std::runtime_error If the file cannot be read.
 */
[[nodiscard]] std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};

    if (!in)
    {
        throw std::runtime_error{std::format("cannot read {}", path.string())};
    }

    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief A note on the run, to stderr with the elapsed time, never among the figures.
 * @param text The note.
 */
void note(const std::string_view text)
{
    static const auto began{std::chrono::steady_clock::now()};

    const auto elapsed{std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count()};

    std::cerr << std::format("[{:8.1f}s] {}\n", elapsed, text);
}

/**
 * @brief Writes an emission's lines to its file under the output directory, a newline after each, echoes them, and
 *        names the file in the run's manifest, which the comparison reads so that a file an earlier run left in the
 *        directory is no part of this one.
 * @param out The output directory.
 * @param emission The emission.
 * @throws std::runtime_error If the file or the manifest cannot be written.
 */
void write(const std::filesystem::path& out, const Emission& emission)
{
    std::ofstream file{out / emission.file, std::ios::binary};

    if (!file)
    {
        throw std::runtime_error{std::format("cannot write {}", (out / emission.file).string())};
    }

    for (const auto& line : emission.lines)
    {
        file << line << '\n';

        std::cout << line << '\n';
    }

    std::cout.flush();

    // The manifest names what this run wrote, so a name goes in it once the file it names is written and closed: a
    // name recorded before the writing would stand for a file an earlier run left behind, and the comparison would
    // read that one as this run's.
    file.close();

    if (!file)
    {
        throw std::runtime_error{std::format("cannot write {}", (out / emission.file).string())};
    }

    std::ofstream manifest{out / "manifest.txt", std::ios::binary | std::ios::app};

    if (!manifest)
    {
        throw std::runtime_error{std::format("cannot write {}", (out / "manifest.txt").string())};
    }

    manifest << emission.file << '\n';

    note(std::format("wrote {}", emission.file));
}

/**
 * @brief The paper's percentile: the order statistic at floor(q n) of the values ascending, capped at the last.
 * @param values The values, in any order.
 * @param q The quantile, as a fraction.
 * @return The percentile.
 */
template <typename T>
[[nodiscard]] T percentile(std::vector<T> values, const double q)
{
    std::ranges::sort(values);

    const auto index{static_cast<std::size_t>(q * static_cast<double>(values.size()))};

    return values[std::min(values.size() - 1, index)];
}

/**
 * @brief A mean to three decimals, as edit_records.py prints one.
 * @param values The values.
 * @return The mean, formatted.
 */
[[nodiscard]] std::string mean(const std::vector<std::size_t>& values)
{
    double sum{0.0};

    for (const auto value : values)
    {
        sum += static_cast<double>(value);
    }

    return std::format("{:.3f}", sum / static_cast<double>(values.size()));
}

/**
 * @brief A density printed as the Python's exact_density prints one: six decimals with the trailing zeros trimmed.
 * @param value The density.
 * @return The text.
 */
[[nodiscard]] std::string exact_density(const double value)
{
    auto text{std::format("{:.6f}", value)};

    while (text.ends_with('0'))
    {
        text.pop_back();
    }

    if (text.ends_with('.'))
    {
        text.pop_back();
    }

    return text;
}

/**
 * @brief Python's random.Random, ported: the Mersenne twister seeded by its init_by_array from one integer, with
 *        randrange() and choice() drawing through its rejection loop over getrandbits(), so that the edit trials
 *        replay the paper's draws exactly.
 */
class Python_random
{
public:
    /**
     * @brief Seeds as random.Random(seed) does for an integer below 2^32: init_by_array over the one word.
     * @param seed The seed.
     */
    explicit Python_random(const std::uint32_t seed)
    {
        state_[0] = 19650218U;

        for (std::size_t index{1}; index < size; ++index)
        {
            state_[index] =
                    1812433253U * (state_[index - 1] ^ (state_[index - 1] >> 30U)) + static_cast<std::uint32_t>(index);
        }

        std::size_t i{1};

        for (std::size_t round{0}; round < size; ++round)
        {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30U)) * 1664525U)) + seed;

            if (++i >= size)
            {
                state_[0] = state_[size - 1];

                i = 1;
            }
        }

        for (std::size_t round{0}; round < size - 1; ++round)
        {
            state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30U)) * 1566083941U)) -
                        static_cast<std::uint32_t>(i);

            if (++i >= size)
            {
                state_[0] = state_[size - 1];

                i = 1;
            }
        }

        state_[0] = 0x80000000U;
    }

    /**
     * @brief random.randrange(n): a uniform draw below n by rejection over the draw's bit length.
     * @param n The exclusive bound, positive.
     * @return The draw.
     */
    [[nodiscard]] std::size_t randrange(const std::size_t n)
    {
        unsigned bits{0};

        while ((static_cast<std::size_t>(1) << bits) <= n)
        {
            ++bits;
        }

        auto draw{getrandbits(bits)};

        while (draw >= n)
        {
            draw = getrandbits(bits);
        }

        return draw;
    }

private:
    static constexpr std::size_t size{624};

    /**
     * @brief The next 32-bit output of the twister.
     * @return The output.
     */
    [[nodiscard]] std::uint32_t next()
    {
        if (index_ >= size)
        {
            for (std::size_t k{0}; k < size; ++k)
            {
                const auto y{(state_[k] & 0x80000000U) | (state_[(k + 1) % size] & 0x7FFFFFFFU)};

                state_[k] = state_[(k + 397) % size] ^ (y >> 1U) ^ ((y & 1U) != 0 ? 0x9908B0DFU : 0U);
            }

            index_ = 0;
        }

        auto y{state_[index_++]};

        y ^= y >> 11U;
        y ^= (y << 7U) & 0x9D2C5680U;
        y ^= (y << 15U) & 0xEFC60000U;
        y ^= y >> 18U;

        return y;
    }

    /**
     * @brief random.getrandbits(k) for k up to 32: the top k bits of one output.
     * @param bits The bit count, one to 32.
     * @return The draw.
     */
    [[nodiscard]] std::size_t getrandbits(const unsigned bits) { return next() >> (32U - bits); }

    std::array<std::uint32_t, size> state_{};

    std::size_t index_{size};
};

/**
 * @brief The tie accounting of a byte-pair training run, which the paper states as identifying the vocabulary.
 */
struct Merge_stats
{
    std::size_t merges{};

    std::size_t tied_steps{};

    std::size_t first_occurrence_disagreements{};
};

/**
 * @brief Trains byte-pair merges as vocab_experiment.py's train_bpe does: the most frequent adjacent pair is merged,
 *        every adjacent pair counted, overlapping ones included, ties going to the shorter concatenation and among
 *        those to the lexicographically greatest pair, and the merge applied left to right without overlap.
 * @param text The training text.
 * @param merges How many merges to make.
 * @param stats Receives the tie accounting.
 * @return The merges, in order.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::string>> train_bpe(
        const std::string_view text, const std::size_t merges, Merge_stats& stats)
{
    std::vector<std::string> sequence;

    sequence.reserve(text.size());

    for (const auto byte : text)
    {
        sequence.emplace_back(1, byte);
    }

    std::vector<std::pair<std::string, std::string>> table;

    for (std::size_t step{0}; step < merges; ++step)
    {
        std::map<std::pair<std::string, std::string>, std::size_t> counts;

        std::map<std::pair<std::string, std::string>, std::size_t> first;

        for (std::size_t at{0}; at + 1 < sequence.size(); ++at)
        {
            std::pair<std::string, std::string> pair{sequence[at], sequence[at + 1]};

            first.try_emplace(pair, at);

            ++counts[pair];
        }

        if (counts.empty())
        {
            break;
        }

        // Python's max over (count, -len(l + r), (l, r)): the largest count, then the shortest merged string, then
        // the greatest pair, strings compared by code point, which is unsigned byte order here.
        const auto length{[](const auto& entry) { return entry.first.first.size() + entry.first.second.size(); }};

        auto best{counts.begin()};

        for (auto it{std::next(counts.begin())}; it != counts.end(); ++it)
        {
            const auto better{
                    it->second > best->second ||
                    (it->second == best->second &&
                     (length(*it) < length(*best) || (length(*it) == length(*best) && it->first > best->first)))};

            if (better)
            {
                best = it;
            }
        }

        const auto count{best->second};

        std::vector<std::pair<std::string, std::string>> contenders;

        for (const auto& [pair, n] : counts)
        {
            if (n == count)
            {
                contenders.push_back(pair);
            }
        }

        if (contenders.size() > 1)
        {
            ++stats.tied_steps;

            const auto earliest{
                    std::ranges::min_element(contenders, {}, [&first](const auto& pair) { return first.at(pair); })};

            if (*earliest != best->first)
            {
                ++stats.first_occurrence_disagreements;
            }
        }

        if (count < 2)
        {
            break;
        }

        const auto [left, right]{best->first};

        table.emplace_back(left, right);

        const auto merged{left + right};

        std::vector<std::string> out;

        out.reserve(sequence.size());

        for (std::size_t at{0}; at < sequence.size();)
        {
            if (at + 1 < sequence.size() && sequence[at] == left && sequence[at + 1] == right)
            {
                out.push_back(merged);

                at += 2;
            }
            else
            {
                out.push_back(std::move(sequence[at]));

                ++at;
            }
        }

        sequence = std::move(out);
    }

    stats.merges = table.size();

    return table;
}

/**
 * @brief A trained vocabulary as the paper holds it: the 256 single bytes and then each merge's concatenation.
 * @param merges The merge table, in order.
 * @param depth How many merges the vocabulary takes, a prefix of the table since the procedure is sequential.
 * @return The token list, 256 + depth long.
 */
[[nodiscard]] std::vector<std::string> local_tokens(
        const std::vector<std::pair<std::string, std::string>>& merges, const std::size_t depth)
{
    std::vector<std::string> tokens;

    for (int value{0}; value < 256; ++value)
    {
        tokens.emplace_back(1, static_cast<char>(value));
    }

    for (std::size_t rank{0}; rank < depth; ++rank)
    {
        tokens.push_back(merges[rank].first + merges[rank].second);
    }

    return tokens;
}

/**
 * @brief The GPT-2 subset as the paper holds it: the 256 single bytes and then the first 4,096 merges of the release's
 *        table, each decoded from the file's byte-to-unicode spelling back to bytes.
 * @param merges_text The text of gpt2-merges.txt.
 * @return The token list, 4,352 long.
 * @throws std::runtime_error If the file is not the one the paper names, by its header, its rule count or a rule's
 *         shape.
 */
[[nodiscard]] std::vector<std::string> build_gpt2(const std::string_view merges_text)
{
    std::vector<std::string_view> lines;

    for (std::size_t at{0}; at <= merges_text.size();)
    {
        const auto end{merges_text.find('\n', at)};

        lines.push_back(merges_text.substr(at, end == std::string_view::npos ? merges_text.size() - at : end - at));

        if (end == std::string_view::npos)
        {
            break;
        }

        at = end + 1;
    }

    if (lines.size() < 2 || lines.front() != "#version: 0.2" || !lines.back().empty())
    {
        throw std::runtime_error{"gpt2-merges.txt must open with the '#version: 0.2' header and end with a newline"};
    }

    if (lines.size() - 2 != gpt2_merges)
    {
        throw std::runtime_error{
                std::format("gpt2-merges.txt carries {} rules, not {}", lines.size() - 2, gpt2_merges)};
    }

    // The release's byte-to-unicode map, inverted: the printable bytes stand for themselves and the rest are
    // numbered from U+0100 in byte order.
    std::map<std::uint32_t, unsigned char> byte_of;

    std::uint32_t extra{256};

    for (std::uint32_t value{0}; value < 256; ++value)
    {
        const auto kept{(value >= '!' && value <= '~') || (value >= 0xA1 && value <= 0xAC) || value >= 0xAE};

        byte_of[kept ? value : extra++] = static_cast<unsigned char>(value);
    }

    const auto decode{[&byte_of](const std::string_view field) {
        std::string bytes;

        for (std::size_t at{0}; at < field.size();)
        {
            const auto lead{static_cast<unsigned char>(field[at])};

            std::uint32_t point{};

            std::size_t length{};

            if (lead < 0x80)
            {
                point = lead;
                length = 1;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                point = lead & 0x1FU;
                length = 2;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                point = lead & 0x0FU;
                length = 3;
            }
            else
            {
                throw std::runtime_error{"gpt2-merges.txt holds a code point beyond the byte map"};
            }

            for (std::size_t tail{1}; tail < length; ++tail)
            {
                point = (point << 6U) | (static_cast<unsigned char>(field[at + tail]) & 0x3FU);
            }

            bytes.push_back(static_cast<char>(byte_of.at(point)));

            at += length;
        }

        return bytes;
    }};

    std::vector<std::string> tokens;

    for (int value{0}; value < 256; ++value)
    {
        tokens.emplace_back(1, static_cast<char>(value));
    }

    for (std::size_t rank{0}; rank < gpt2_prefix; ++rank)
    {
        const auto rule{lines[rank + 1]};

        const auto space{rule.find(' ')};

        if (space == std::string_view::npos || rule.find(' ', space + 1) != std::string_view::npos)
        {
            throw std::runtime_error{std::format("gpt2-merges.txt rule {} is not a pair", rule)};
        }

        tokens.push_back(decode(rule.substr(0, space)) + decode(rule.substr(space + 1)));
    }

    return tokens;
}

/**
 * @brief Compiles a token list as a lexer, each distinct token a literal at its own priority in list order.
 * @param tokens The tokens.
 * @return The lexer.
 */
[[nodiscard]] Lexer compile(const std::vector<std::string>& tokens)
{
    munch::core::Builder builder;

    std::set<std::string> seen;

    for (const auto& token : tokens)
    {
        if (seen.insert(token).second)
        {
            builder.add_token(munch::regex::text(token), seen.size() - 1, seen.size() - 1);
        }
    }

    return builder.build();
}

/**
 * @brief The bytes some token carries, the paper's alphabet, ascending.
 * @param tokens The tokens.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet(const std::vector<std::string>& tokens)
{
    std::set<unsigned char> bytes;

    for (const auto& token : tokens)
    {
        for (const auto byte : token)
        {
            bytes.insert(static_cast<unsigned char>(byte));
        }
    }

    return {bytes.begin(), bytes.end()};
}

/**
 * @brief The width-one pairs of the bytes the lexer certifies exactly, ascending: Lexer::is_split_point() over
 *        every byte.
 * @param lexer The lexer.
 * @return The certified bytes as pairs at origin zero.
 */
[[nodiscard]] std::vector<Pair> certified_bytes(const Lexer& lexer)
{
    std::vector<Pair> pairs;

    for (int value{0}; value < 256; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)))
        {
            pairs.push_back(Pair{.window = std::string(1, static_cast<char>(value)), .origin = 0});
        }
    }

    return pairs;
}

/**
 * @brief The distinct windows of one width occurring in a text, ascending.
 * @param text The text.
 * @param width The width.
 * @return The windows.
 */
[[nodiscard]] std::set<std::string> windows_of_width(const std::string_view text, const std::size_t width)
{
    std::set<std::string> windows;

    for (std::size_t at{0}; at + width <= text.size(); ++at)
    {
        windows.emplace(text.substr(at, width));
    }

    return windows;
}

/**
 * @brief The distinct windows of the budget's widths occurring in a text, ascending, the candidate set the paper
 *        decides exhaustively.
 * @param text The text.
 * @return The windows.
 */
[[nodiscard]] std::vector<std::string> occurring_windows(const std::string_view text)
{
    std::set<std::string> windows;

    for (const auto width : widths)
    {
        windows.merge(windows_of_width(text, width));
    }

    return {windows.begin(), windows.end()};
}

/**
 * @brief What deciding a set of pairs found: the pairs certified exactly, the pairs the conservative model certifies
 *        among them, and the decisions the exact search left unsettled at its cap.
 */
struct Decided
{
    std::vector<Pair> exact;

    std::vector<Pair> conservative;

    std::vector<Pair> unsettled;
};

/**
 * @brief Decides pairs, exactly by window_counterexample() and conservatively by is_split_window(), the pairs shared
 * out over every hardware thread.
 * @param lexer The lexer.
 * @param pairs The pairs.
 * @return What each decision certified and the exact decisions the cap stopped, each list sorted.
 */
[[nodiscard]] Decided decide(const Lexer& lexer, const std::vector<Pair>& pairs)
{
    // Zero for refused, one for certified, two for unsettled, one slot per pair so the threads never share one.
    std::vector<unsigned char> verdicts(pairs.size(), 0);

    std::atomic<std::size_t> next{0};

    const auto worker{[&] {
        for (auto index{next.fetch_add(1)}; index < pairs.size(); index = next.fetch_add(1))
        {
            const auto& [window, origin]{pairs[index]};

            const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

            verdicts[index] = !exhaustive ? 2 : witness.empty() ? 1 : 0;
        }
    }};

    std::vector<std::thread> threads;

    for (unsigned thread{0}; thread < std::max(1U, std::thread::hardware_concurrency()); ++thread)
    {
        threads.emplace_back(worker);
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    Decided decided;

    for (std::size_t index{0}; index < pairs.size(); ++index)
    {
        if (verdicts[index] == 1)
        {
            decided.exact.push_back(pairs[index]);
        }
        else if (verdicts[index] == 2)
        {
            decided.unsettled.push_back(pairs[index]);
        }

        if (lexer.is_split_window(pairs[index].window) == pairs[index].origin)
        {
            decided.conservative.push_back(pairs[index]);
        }
    }

    std::ranges::sort(decided.exact);
    std::ranges::sort(decided.conservative);
    std::ranges::sort(decided.unsettled);

    return decided;
}

/**
 * @brief Every origin of every window, the pairs an exhaustive inventory decides.
 * @param windows The windows.
 * @return The pairs, in window order.
 */
[[nodiscard]] std::vector<Pair> every_origin(const std::vector<std::string>& windows)
{
    std::vector<Pair> pairs;

    for (const auto& window : windows)
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            pairs.push_back(Pair{.window = window, .origin = origin});
        }
    }

    return pairs;
}

/**
 * @brief An inventory indexed for matching: the origins certified for each window, by window, and the widths held.
 */
struct Inventory
{
    std::map<std::string, std::vector<std::size_t>, std::less<>> origins_of;

    std::set<std::size_t> lengths;

    /**
     * @brief Indexes the pairs.
     * @param certified The pairs.
     */
    explicit Inventory(const std::vector<Pair>& certified)
    {
        for (const auto& [window, origin] : certified)
        {
            origins_of[window].push_back(origin);

            lengths.insert(window.size());
        }
    }

    /**
     * @brief The origins certified for the windows beginning at a position of a text: for each held width whose
     *        window there is in the inventory, that window's origins.
     * @param text The text.
     * @param at The position.
     * @param take Receives each origin.
     */
    void origins_at(
            const std::string_view text, const std::size_t at, const std::function<void(std::size_t)>& take) const
    {
        for (const auto length : lengths)
        {
            if (at + length > text.size())
            {
                break;
            }

            const auto found{origins_of.find(text.substr(at, length))};

            if (found == origins_of.end())
            {
                continue;
            }

            for (const auto origin : found->second)
            {
                take(origin);
            }
        }
    }
};

/**
 * @brief The anchor positions an inventory witnesses in a text: for every occurrence of a certified window the
 *        position origin bytes into it, the text's two ends left out, ascending and distinct.
 * @param certified The inventory, width-one pairs standing for certified bytes.
 * @param text The text.
 * @return The anchors.
 */
[[nodiscard]] std::vector<std::size_t> anchor_positions(const std::vector<Pair>& certified, const std::string_view text)
{
    const Inventory inventory{certified};

    std::vector<bool> anchored(text.size(), false);

    for (std::size_t at{0}; at < text.size(); ++at)
    {
        inventory.origins_at(text, at, [&anchored, at](const std::size_t origin) {
            if (at + origin > 0)
            {
                anchored[at + origin] = true;
            }
        });
    }

    std::vector<std::size_t> anchors;

    for (std::size_t at{0}; at < anchored.size(); ++at)
    {
        if (anchored[at])
        {
            anchors.push_back(at);
        }
    }

    return anchors;
}

/**
 * @brief The library's supply figures for an inventory on a text: tools/audit's supply() over a report holding the
 *        certified bytes and the wider pairs, its count held against anchor_positions().
 *
 * With no classes the width-one pairs are the report's exact bytes and the rest its windows; with classes, the
 * campaign's case, every pair is a window over class representatives and the report's classes carry it to every
 * member, the width-one ones included, since a class window's members are not one byte.
 * @param certified The inventory.
 * @param text The text, as the classes read it.
 * @param classes The byte classes the windows stand for, none for byte windows.
 * @return The supply of the bytes and windows together.
 * @throws std::runtime_error If supply() and anchor_positions() disagree on the anchor count.
 */
[[nodiscard]] munch::tools::audit::Anchors library_supply(
        const std::vector<Pair>& certified, const std::string_view text,
        const std::vector<std::vector<unsigned char>>& classes = {})
{
    Report report;

    report.classes = classes;

    for (const auto& [window, origin] : certified)
    {
        if (window.size() == 1 && classes.empty())
        {
            report.exact.push_back(static_cast<unsigned char>(window.front()));
        }
        else
        {
            report.windows.push_back(Certified_window{.window = window, .origin = origin});
        }
    }

    std::ranges::sort(report.exact);

    const auto found{munch::tools::audit::supply(report, text)};

    const auto anchors{found.windows ? *found.windows : found.exact};

    // The positions are matched over class representatives where the report matches over classes.
    std::string canonical{text};

    for (const auto& members : classes)
    {
        for (auto& byte : canonical)
        {
            if (std::ranges::find(members, static_cast<unsigned char>(byte)) != members.end())
            {
                byte = static_cast<char>(members.front());
            }
        }
    }

    if (anchors.count != anchor_positions(certified, canonical).size())
    {
        throw std::runtime_error{"supply() and the probe's anchor positions disagree"};
    }

    return anchors;
}

/**
 * @brief Anchors per KiB as the paper prints it, from the library's supply.
 * @param certified The inventory.
 * @param text The text.
 * @return The density.
 */
[[nodiscard]] double density(const std::vector<Pair>& certified, const std::string_view text)
{
    return library_supply(certified, text).per_kibibyte;
}

/**
 * @brief The longest anchorless stretch: the paper's count of unanchored positions, the runs before the first anchor
 *        and after the last included.
 * @param anchors The anchors, ascending.
 * @param length The text's length.
 * @return The stretch.
 */
[[nodiscard]] std::size_t longest_stretch(const std::vector<std::size_t>& anchors, const std::size_t length)
{
    if (anchors.empty())
    {
        return length;
    }

    auto stretch{std::max(anchors.front(), length - anchors.back() - 1)};

    for (std::size_t index{1}; index < anchors.size(); ++index)
    {
        stretch = std::max(stretch, anchors[index] - anchors[index - 1] - 1);
    }

    return stretch;
}

/**
 * @brief The worker-snap distances: how far each of the ideal cuts for the worker count moves to reach the nearest
 *        anchor.
 * @param anchors The anchors, ascending and non-empty.
 * @param length The input's length.
 * @param workers The worker count.
 * @return The distances, one per ideal cut.
 */
[[nodiscard]] std::vector<std::size_t> snap_distances(
        const std::vector<std::size_t>& anchors, const std::size_t length, const std::size_t workers)
{
    std::vector<std::size_t> distances;

    for (std::size_t k{1}; k < workers; ++k)
    {
        const auto cut{length * k / workers};

        const auto above{std::ranges::lower_bound(anchors, cut)};

        auto nearest{std::numeric_limits<std::size_t>::max()};

        if (above != anchors.end())
        {
            nearest = std::min(nearest, *above - cut);
        }

        if (above != anchors.begin())
        {
            nearest = std::min(nearest, cut - *std::prev(above));
        }

        distances.push_back(nearest);
    }

    return distances;
}

/**
 * @brief The token starts of the maximal-munch scan of a text, or nothing when the scan does not reach its end.
 * @param lexer The lexer.
 * @param text The text.
 * @return The starts, or std::nullopt.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> token_starts(const Lexer& lexer, const std::string_view text)
{
    std::vector<std::size_t> starts;

    std::size_t at{0};

    const auto consumed{lexer.tokenize_all<std::size_t>(text, [&](const std::size_t, const std::size_t length) {
        starts.push_back(at);

        at += length;
    })};

    if (consumed != text.size())
    {
        return std::nullopt;
    }

    return starts;
}

/**
 * @brief One attempted edit of the paper's edit trials, as splitting-edit-trials.csv records it.
 */
struct Edit_record
{
    std::size_t position{};

    unsigned char new_byte{};

    bool accepted{};

    bool untokenizable{};

    std::size_t left_admissible{};

    std::size_t right_anchor{};

    std::size_t frozen_anchor{};

    std::size_t moved{};

    std::size_t hull{};

    std::size_t admissible{};

    std::size_t unoccupied{};

    std::optional<std::size_t> probe_anchor{};
};

/**
 * @brief The theorem's q: the least anchor past the edit witnessed by an occurrence lying wholly after it.
 * @param inventory The inventory.
 * @param edited The edited text.
 * @param edit The edited position.
 * @return The anchor, or the text's length when none.
 */
[[nodiscard]] std::size_t least_shared_suffix_anchor(
        const Inventory& inventory, const std::string_view edited, const std::size_t edit)
{
    auto best{edited.size()};

    for (auto start{edit + 1}; start < best; ++start)
    {
        inventory.origins_at(
                edited, start, [&best, start](const std::size_t origin) { best = std::min(best, start + origin); });
    }

    return best;
}

/**
 * @brief The edit trials of splitting_measurements.py replayed over munch: the same seeded draws, each accepted edit
 *        retokenized by the lexer, the moved boundaries held inside the theorem's window under the ceiling inventory,
 *        the frozen inventory's endpoint beside it, and the width-five probe decided by window_counterexample().
 * @param lexer The lexer.
 * @param longest The longest token's length, the theorem's L.
 * @param sigma The alphabet the replacement byte is drawn from, ascending.
 * @param ceiling The ceiling inventory.
 * @param frozen The frozen inventory.
 * @param sample The evaluation slice.
 * @return The records, in draw order.
 * @throws std::runtime_error If a moved boundary leaves the theorem's window, which would refute the theorem.
 */
[[nodiscard]] std::vector<Edit_record> edit_trials(
        const Lexer& lexer, const std::size_t longest, const std::vector<unsigned char>& sigma,
        const std::vector<Pair>& ceiling, const std::vector<Pair>& frozen, const std::string_view sample)
{
    const auto sequential{token_starts(lexer, sample)};

    if (!sequential)
    {
        throw std::runtime_error{"the evaluation slice does not tokenize"};
    }

    const std::set<std::size_t> before(sequential->begin(), sequential->end());

    const Inventory ceiling_index{ceiling};

    const Inventory frozen_index{frozen};

    Python_random chooser{edit_seed};

    std::map<Pair, bool> memo;

    std::vector<Edit_record> records;

    std::size_t measured{0};

    for (std::size_t attempts{0}; measured < edit_trials_wanted && attempts < edit_attempts_cap; ++attempts)
    {
        const auto position{chooser.randrange(sample.size())};

        const auto other{sigma[chooser.randrange(sigma.size())]};

        Edit_record record{.position = position, .new_byte = other};

        if (other == static_cast<unsigned char>(sample[position]))
        {
            records.push_back(record);

            continue;
        }

        std::string edited{sample};

        edited[position] = static_cast<char>(other);

        const auto starts{token_starts(lexer, edited)};

        if (!starts)
        {
            record.untokenizable = true;

            records.push_back(record);

            continue;
        }

        const std::set<std::size_t> after(starts->begin(), starts->end());

        std::vector<std::size_t> moved;

        std::ranges::set_symmetric_difference(before, after, std::back_inserter(moved));

        const auto anchor{least_shared_suffix_anchor(ceiling_index, edited, position)};

        for (const auto boundary : moved)
        {
            if (boundary + longest <= position || boundary >= anchor)
            {
                throw std::runtime_error{
                        std::format("a moved boundary at {} lies outside the theorem's window", boundary)};
            }
        }

        // The width-five probe: every window beginning after the edit whose origin would name an anchor before the
        // one the budget found, decided once each across the trials.
        std::vector<Pair> undecided;

        const auto last_start{std::min(anchor, sample.size() - probe_width + 1)};

        for (auto start{position + 1}; start < last_start; ++start)
        {
            for (std::size_t origin{0}; origin < probe_width && start + origin < anchor; ++origin)
            {
                const Pair pair{.window = edited.substr(start, probe_width), .origin = origin};

                if (!memo.contains(pair))
                {
                    undecided.push_back(pair);
                }
            }
        }

        std::ranges::sort(undecided);

        undecided.erase(std::ranges::unique(undecided).begin(), undecided.end());

        if (!undecided.empty())
        {
            const auto [exact, conservative, unsettled]{decide(lexer, undecided)};

            if (!unsettled.empty())
            {
                throw std::runtime_error{"a width-five probe decision was unsettled at the cap"};
            }

            for (const auto& pair : undecided)
            {
                memo[pair] = std::ranges::binary_search(exact, pair);
            }
        }

        std::optional<std::size_t> earlier;

        for (auto start{position + 1}; start < last_start; ++start)
        {
            for (std::size_t origin{0}; origin < probe_width && start + origin < anchor; ++origin)
            {
                if (memo.at(Pair{.window = edited.substr(start, probe_width), .origin = origin}) &&
                    (!earlier || start + origin < *earlier))
                {
                    earlier = start + origin;
                }
            }
        }

        const auto low{std::max<std::size_t>(1, position + 1 > longest ? position + 1 - longest : 0)};

        record.accepted = true;
        record.left_admissible = low;
        record.right_anchor = anchor;
        record.frozen_anchor = least_shared_suffix_anchor(frozen_index, edited, position);
        record.moved = moved.size();
        record.hull = moved.empty() ? 0 : moved.back() - moved.front() + 1;
        record.admissible = anchor - low;
        record.unoccupied = record.admissible - moved.size();
        record.probe_anchor = earlier;

        records.push_back(record);

        ++measured;
    }

    return records;
}

/**
 * @brief The paper's edit lines, computed from the records as edit_records.py computes them.
 * @param name The vocabulary's name.
 * @param records The records.
 * @param lines Receives the lines.
 */
void report_edits(const std::string_view name, const std::vector<Edit_record>& records, std::vector<std::string>& lines)
{
    std::vector<Edit_record> accepted;

    std::size_t untokenizable{0};

    std::size_t repeated{0};

    for (const auto& record : records)
    {
        if (record.accepted)
        {
            accepted.push_back(record);
        }
        else if (record.untokenizable)
        {
            ++untokenizable;
        }
        else
        {
            ++repeated;
        }
    }

    const auto column{[&accepted](const auto& field, const bool changed_only) {
        std::vector<std::size_t> values;

        for (const auto& record : accepted)
        {
            if (!changed_only || record.moved > 0)
            {
                values.push_back(field(record));
            }
        }

        return values;
    }};

    const auto hulls{column([](const Edit_record& record) { return record.hull; }, false)};

    const auto conditional{column([](const Edit_record& record) { return record.hull; }, true)};

    const auto moved{column([](const Edit_record& record) { return record.moved; }, false)};

    const auto moved_changed{column([](const Edit_record& record) { return record.moved; }, true)};

    const auto slacks{column([](const Edit_record& record) { return record.admissible - record.hull; }, false)};

    const auto unoccupied{column([](const Edit_record& record) { return record.unoccupied; }, false)};

    lines.push_back(std::format(
            "{} | edits measured: {} accepted of {} attempted, {} refused because the edited input stopped tokenizing "
            "and {} because the draw repeated the original byte",
            name, accepted.size(), records.size(), untokenizable, repeated));
    lines.push_back(std::format(
            "{} | edits moving no boundary: {} of {}", name, accepted.size() - conditional.size(), accepted.size()));
    lines.push_back(std::format("{} | boundary-difference hull p50: {}", name, percentile(hulls, 0.5)));
    lines.push_back(std::format("{} | boundary-difference hull p90: {}", name, percentile(hulls, 0.9)));
    lines.push_back(std::format("{} | boundary-difference hull max: {}", name, std::ranges::max(hulls)));
    lines.push_back(std::format("{} | boundary-difference hull mean: {}", name, mean(hulls)));
    lines.push_back(std::format(
            "{} | boundary-difference hull conditional on a change: n {}, p50 {}, p90 {}, max {}, mean {}", name,
            conditional.size(), percentile(conditional, 0.5), percentile(conditional, 0.9),
            std::ranges::max(conditional), mean(conditional)));
    lines.push_back(std::format(
            "{} | moved boundaries per edit: mean {} conditional on a change, {} unconditionally", name,
            mean(moved_changed), mean(moved)));
    lines.push_back(std::format("{} | hull slack p50: {}", name, percentile(slacks, 0.5)));
    lines.push_back(std::format("{} | hull slack p90: {}", name, percentile(slacks, 0.9)));
    lines.push_back(std::format("{} | unoccupied admissible p50: {}", name, percentile(unoccupied, 0.5)));
    lines.push_back(std::format("{} | unoccupied admissible p90: {}", name, percentile(unoccupied, 0.9)));

    const auto improved{
            std::ranges::count_if(accepted, [](const Edit_record& record) { return record.probe_anchor.has_value(); })};

    lines.push_back(std::format(
            "{} | trials whose anchor improves at width five: {} of {}, each an occurrence lying wholly in the "
            "unchanged suffix",
            name, improved, accepted.size()));
}

/**
 * @brief The paper's paired comparison of the two vocabularies' trials, trial by trial.
 * @param left_name The first vocabulary's name.
 * @param left The first vocabulary's records.
 * @param right_name The second vocabulary's name.
 * @param right The second vocabulary's records.
 * @param lines Receives the lines.
 * @throws std::runtime_error If the accepted trials do not pair at the same edits.
 */
void measure_pairing(
        const std::string_view left_name, const std::vector<Edit_record>& left, const std::string_view right_name,
        const std::vector<Edit_record>& right, std::vector<std::string>& lines)
{
    const auto accepted_of{[](const std::vector<Edit_record>& records) {
        std::vector<Edit_record> accepted;

        std::ranges::copy_if(records, std::back_inserter(accepted), [](const Edit_record& r) { return r.accepted; });

        return accepted;
    }};

    const auto ones{accepted_of(left)};

    const auto twos{accepted_of(right)};

    if (ones.size() != twos.size())
    {
        throw std::runtime_error{"the trials must pair"};
    }

    std::vector<long> hull_diffs;

    std::vector<long> width_diffs;

    std::map<std::pair<bool, bool>, std::size_t> movement;

    for (std::size_t index{0}; index < ones.size(); ++index)
    {
        const auto& one{ones[index]};

        const auto& two{twos[index]};

        if (one.position != two.position || one.new_byte != two.new_byte)
        {
            throw std::runtime_error{"the trials must pair at the same edit"};
        }

        hull_diffs.push_back(static_cast<long>(one.hull) - static_cast<long>(two.hull));
        width_diffs.push_back(static_cast<long>(one.admissible) - static_cast<long>(two.admissible));

        ++movement[{one.moved > 0, two.moved > 0}];
    }

    const auto sign{[&hull_diffs](const auto test) { return std::ranges::count_if(hull_diffs, test); }};

    lines.push_back(std::format(
            "{} against {} | paired hull difference p50: {}, p90 {} over {} paired trials", left_name, right_name,
            percentile(hull_diffs, 0.5), percentile(hull_diffs, 0.9), hull_diffs.size()));
    lines.push_back(std::format(
            "{} against {} | paired hull difference sign: negative {}, zero {}, positive {} of {}", left_name,
            right_name, sign([](const long d) { return d < 0; }), sign([](const long d) { return d == 0; }),
            sign([](const long d) { return d > 0; }), hull_diffs.size()));
    lines.push_back(std::format(
            "{} against {} | paired admissible-width difference p50: {}, p90 {}", left_name, right_name,
            percentile(width_diffs, 0.5), percentile(width_diffs, 0.9)));
    lines.push_back(std::format(
            "{} against {} | paired movement: both move {}, {} only {}, {} only {}, neither {} of {}", left_name,
            right_name, movement[{true, true}], left_name, movement[{true, false}], right_name, movement[{false, true}],
            movement[{false, false}], hull_diffs.size()));
}

/**
 * @brief The edit-slack table's rows for one vocabulary: hull slack and unoccupied count under the ceiling and the
 *        frozen inventory, as write_slack_table() computes them.
 * @param label The row label.
 * @param records The records.
 * @param rows Receives the two rows.
 */
void slack_rows(const std::string_view label, const std::vector<Edit_record>& records, std::vector<std::string>& rows)
{
    std::vector<std::size_t> ceiling_slack;
    std::vector<std::size_t> frozen_slack;
    std::vector<std::size_t> ceiling_unoccupied;
    std::vector<std::size_t> frozen_unoccupied;

    for (const auto& record : records)
    {
        if (!record.accepted)
        {
            continue;
        }

        const auto frozen_width{record.frozen_anchor - record.left_admissible};

        ceiling_slack.push_back(record.admissible - record.hull);
        frozen_slack.push_back(frozen_width - record.hull);
        ceiling_unoccupied.push_back(record.unoccupied);
        frozen_unoccupied.push_back(frozen_width - record.moved);
    }

    rows.push_back(std::format(
            R"({} & hull slack & {} & {} & {} & {} \\)", label, percentile(ceiling_slack, 0.5),
            percentile(ceiling_slack, 0.9), percentile(frozen_slack, 0.5), percentile(frozen_slack, 0.9)));
    rows.push_back(std::format(
            R"({} & unoccupied count & {} & {} & {} & {} \\)", label, percentile(ceiling_unoccupied, 0.5),
            percentile(ceiling_unoccupied, 0.9), percentile(frozen_unoccupied, 0.5),
            percentile(frozen_unoccupied, 0.9)));
}

/**
 * @brief The n-grams of one width ranked as Counter.most_common ranks them: by frequency over every occurrence,
 *        ties in order of first occurrence.
 * @param text The text.
 * @param width The width.
 * @return The n-grams with their counts, ranked.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> frequency_ranking(
        const std::string_view text, const std::size_t width)
{
    std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> counts;

    for (std::size_t at{0}; at + width <= text.size(); ++at)
    {
        auto& [count, first]{counts[std::string{text.substr(at, width)}]};

        if (count == 0)
        {
            first = at;
        }

        ++count;
    }

    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> entries(counts.begin(), counts.end());

    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return a.second.first != b.second.first ? a.second.first > b.second.first : a.second.second < b.second.second;
    });

    std::vector<std::pair<std::string, std::size_t>> ranked;

    for (const auto& [window, entry] : entries)
    {
        ranked.emplace_back(window, entry.first);
    }

    return ranked;
}

/**
 * @brief The paper's sampled-inventory lines: the twenty most frequent n-grams per width decided, the supply they
 *        give, and the sensitivity of that figure to the tie at the twentieth rank, every tied selection scored.
 * @param name The vocabulary's name.
 * @param lexer The lexer.
 * @param sample The slice.
 * @param bytes The certified bytes as pairs.
 * @param lines Receives the four lines.
 */
void legacy_sample_lines(
        const std::string_view name, const Lexer& lexer, const std::string_view sample, const std::vector<Pair>& bytes,
        std::vector<std::string>& lines)
{
    std::vector<std::string> chosen;

    std::vector<std::vector<std::string>> heads;

    std::vector<std::vector<std::string>> bands;

    std::vector<std::size_t> slots;

    for (const auto width : widths)
    {
        const auto ranked{frequency_ranking(sample, width)};

        for (std::size_t rank{0}; rank < std::min(legacy_per_width, ranked.size()); ++rank)
        {
            chosen.push_back(ranked[rank].first);
        }

        std::vector<std::string> head;

        std::vector<std::string> band;

        if (ranked.size() <= legacy_per_width)
        {
            for (const auto& [window, count] : ranked)
            {
                head.push_back(window);
            }
        }
        else
        {
            const auto cutoff{ranked[legacy_per_width - 1].second};

            for (const auto& [window, count] : ranked)
            {
                if (count > cutoff)
                {
                    head.push_back(window);
                }
                else if (count == cutoff)
                {
                    band.push_back(window);
                }
            }
        }

        slots.push_back(legacy_per_width - head.size());
        heads.push_back(std::move(head));
        bands.push_back(std::move(band));
    }

    const auto [pairs, conservative, unsettled]{decide(lexer, every_origin(chosen))};

    if (!unsettled.empty())
    {
        throw std::runtime_error{"a sampled-inventory decision was unsettled at the cap"};
    }

    auto with_bytes{bytes};

    with_bytes.insert(with_bytes.end(), pairs.begin(), pairs.end());

    std::size_t decided_count{0};

    for (const auto& window : chosen)
    {
        decided_count += window.size();
    }

    lines.push_back(std::format(
            "{} | sampled inventory supply: {} certified of {} decided over {} candidates, the {} most frequent slice "
            "n-grams per width, {:.1f} anchors per KiB against the exhaustive inventory's own figure",
            name, pairs.size(), decided_count, chosen.size(), legacy_per_width, density(pairs, sample)));
    lines.push_back(std::format(
            "{} | sampled inventory with the certified bytes: {:.1f} anchors per KiB, the sampled wider half added to "
            "the exact singleton half",
            name, density(with_bytes, sample)));

    // Every window a selection could pick, decided once, then every selection consistent with the ties scored.
    std::vector<std::string> contested;

    for (const auto& group : heads)
    {
        contested.insert(contested.end(), group.begin(), group.end());
    }

    for (const auto& group : bands)
    {
        contested.insert(contested.end(), group.begin(), group.end());
    }

    const auto verdicts{decide(lexer, every_origin(contested))};

    if (!verdicts.unsettled.empty())
    {
        throw std::runtime_error{"a tie-family decision was unsettled at the cap"};
    }

    const Inventory certified_of{verdicts.exact};

    std::set<std::size_t> counts;

    std::set<double> wider_supply;

    std::set<double> whole_supply;

    std::size_t selections{0};

    const auto score{[&](const std::vector<std::string>& selected_windows) {
        std::vector<Pair> selected;

        for (const auto& window : selected_windows)
        {
            const auto found{certified_of.origins_of.find(window)};

            if (found != certified_of.origins_of.end())
            {
                for (const auto origin : found->second)
                {
                    selected.push_back(Pair{.window = window, .origin = origin});
                }
            }
        }

        counts.insert(selected.size());

        wider_supply.insert(density(selected, sample));

        auto whole{bytes};

        whole.insert(whole.end(), selected.begin(), selected.end());

        whole_supply.insert(density(whole, sample));

        ++selections;
    }};

    // The product of the bands' combinations, one width's choice at a time.
    std::vector<std::string> selected_windows;

    for (const auto& group : heads)
    {
        selected_windows.insert(selected_windows.end(), group.begin(), group.end());
    }

    const std::function<void(std::size_t)> choose{[&](const std::size_t width_index) {
        if (width_index == bands.size())
        {
            score(selected_windows);

            return;
        }

        const auto& band{bands[width_index]};

        const auto free{slots[width_index]};

        std::vector<char> take(band.size(), 0);

        std::fill(take.begin(), take.begin() + static_cast<long>(free), 1);

        do
        {
            const auto mark{selected_windows.size()};

            for (std::size_t member{0}; member < band.size(); ++member)
            {
                if (take[member] != 0)
                {
                    selected_windows.push_back(band[member]);
                }
            }

            choose(width_index + 1);

            selected_windows.resize(mark);
        } while (std::ranges::prev_permutation(take).found);
    }};

    choose(0);

    std::string band_sizes;

    for (std::size_t index{0}; index < widths.size(); ++index)
    {
        band_sizes += std::format(
                "{}width {}: {} tied for {} of the {} slots", index == 0 ? "" : "; ", widths[index],
                bands[index].size(), slots[index], legacy_per_width);
    }

    lines.push_back(std::format(
            "{} | sampled inventory selection rule: rank distinct n-grams by frequency over every occurrence, the "
            "final "
            "n-gram included, and break frequency ties by first occurrence; the rank-{} frequency is itself tied at "
            "every width ({}), and over all {} selections consistent with those ties the certified-pair count runs "
            "from {} to {}",
            name, legacy_per_width, band_sizes, selections, *counts.begin(), *counts.rbegin()));

    const auto spread{[](const std::set<double>& values) {
        return values.size() == 1 ?
                       exact_density(*values.begin()) :
                       std::format("{} to {}", exact_density(*values.begin()), exact_density(*values.rbegin()));
    }};

    std::vector<std::string> moved;

    if (counts.size() > 1)
    {
        moved.emplace_back("the certified-pair count");
    }

    if (wider_supply.size() > 1 || whole_supply.size() > 1)
    {
        moved.emplace_back("the supply behind it");
    }

    std::string verdict;

    for (std::size_t index{0}; index < moved.size(); ++index)
    {
        verdict += (index == 0 ? "" : " and ") + moved[index];
    }

    lines.push_back(std::format(
            "{} | sampled inventory tie invariance: over all {} selections the sampled wider half supplies {} anchors "
            "per KiB and the same half added to the exact certified bytes supplies {}, so the tie rule moves {}",
            name, selections, spread(wider_supply), spread(whole_supply),
            moved.empty() ? "neither the certified-pair count nor the supply behind it" : verdict));
}

/**
 * @brief Whether every token's newline, where one has any, sits where a cut before it or after it is sound: before
 *        it when no token carries the byte past its first position, which is the byte certificate, and after it when
 *        no token carries the byte before its last position, read off the compiled tables as the state a newline
 *        leads a live state into having no live move onward.
 * @param lexer The lexer.
 * @param name The row's name.
 * @param lines Receives the two lines, none when no token holds a newline.
 */
void newline_lines(const Lexer& lexer, const std::string_view name, std::vector<std::string>& lines)
{
    const auto& simulator{lexer.simulator()};

    bool consumed{false};

    bool after_sound{true};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        if (!simulator.is_live(state))
        {
            continue;
        }

        const auto into{simulator.step(state, '\n')};

        if (!into || !simulator.is_live(*into))
        {
            continue;
        }

        consumed = true;

        for (int value{0}; value < 256; ++value)
        {
            const auto onward{simulator.step(*into, static_cast<unsigned char>(value))};

            if (onward && simulator.is_live(*onward))
            {
                after_sound = false;
            }
        }
    }

    if (!consumed)
    {
        return;
    }

    lines.push_back(std::format("{} | newline at-split sound: {}", name, lexer.is_split_point('\n') ? "yes" : "no"));
    lines.push_back(std::format("{} | newline after-split sound: {}", name, after_sound ? "yes" : "no"));
}

/**
 * @brief The sync-distance line: anchor_free_span() over the certified bytes, the paper's gap corollary.
 * @param lexer The lexer.
 * @param name The row's name.
 * @param lines Receives the line.
 */
void sync_line(const Lexer& lexer, const std::string_view name, std::vector<std::string>& lines)
{
    const auto span{lexer.anchor_free_span()};

    lines.push_back(std::format(
            "{} | sync distance: {}", name, span ? std::to_string(*span) : "unbounded, a quiet cycle exists"));
}

/**
 * @brief The UTF-8 shape's token set, the paper's designed contrast: one token per encoded length, with letters
 *        standing for the lead bytes and c for a continuation byte.
 * @return The lexer.
 */
[[nodiscard]] Lexer utf8_shape()
{
    munch::core::Builder builder;

    std::size_t index{0};

    for (const auto* token : {"a", "2c", "3cc", "4ccc"})
    {
        builder.add_token(munch::regex::text(token), index, index);

        ++index;
    }

    return builder.build();
}

/**
 * @brief One measured vocabulary: its name, tokens, lexer, certified bytes and the exhaustive inventory over the
 *        evaluation slice.
 */
struct Vocabulary
{
    std::string name;

    std::string label;

    std::vector<std::string> tokens;

    Lexer lexer;

    std::vector<Pair> bytes;

    std::vector<std::string> windows;

    Decided decided;

    /**
     * @brief The ceiling inventory: the certified bytes and the exact wider pairs.
     * @return The inventory.
     */
    [[nodiscard]] std::vector<Pair> certified() const
    {
        auto pairs{bytes};

        pairs.insert(pairs.end(), decided.exact.begin(), decided.exact.end());

        return pairs;
    }
};

/**
 * @brief Compiles a vocabulary and decides every window occurring in the slice at every origin.
 * @param name The name.
 * @param label The table label.
 * @param tokens The tokens.
 * @param sample The evaluation slice.
 * @return The vocabulary.
 */
[[nodiscard]] Vocabulary measure_vocabulary(
        const std::string_view name, const std::string_view label, std::vector<std::string> tokens,
        const std::string_view sample)
{
    auto lexer{compile(tokens)};

    note(std::format("{}: {} tokens compiled to {} states", name, tokens.size(), lexer.simulator().state_count()));

    auto bytes{certified_bytes(lexer)};

    auto windows{occurring_windows(sample)};

    auto decided{decide(lexer, every_origin(windows))};

    note(std::format(
            "{}: {} windows decided, {} pairs certified exactly, {} by the conservative model, {} unsettled", name,
            windows.size(), decided.exact.size(), decided.conservative.size(), decided.unsettled.size()));

    return Vocabulary{
            .name = std::string{name},
            .label = std::string{label},
            .tokens = std::move(tokens),
            .lexer = std::move(lexer),
            .bytes = std::move(bytes),
            .windows = std::move(windows),
            .decided = std::move(decided)};
}

/**
 * @brief The supply of one inventory as the paper prints it for a named row: anchors per KiB, the gap figures and
 *        the longest anchorless stretch.
 * @param name The row's name.
 * @param certified The inventory.
 * @param text The slice.
 * @param lines Receives the lines.
 * @return The anchors.
 */
std::vector<std::size_t> supply_lines(
        const std::string_view name, const std::vector<Pair>& certified, const std::string_view text,
        std::vector<std::string>& lines)
{
    const auto anchors{anchor_positions(certified, text)};

    const auto [count, per_kibibyte, gaps]{library_supply(certified, text)};

    lines.push_back(std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte));

    if (gaps)
    {
        lines.push_back(std::format("{} | anchor gap p50: {}", name, gaps->median));
        lines.push_back(std::format("{} | anchor gap p90: {}", name, gaps->ninetieth));
        lines.push_back(std::format("{} | anchor gap max: {}", name, gaps->longest));
    }

    lines.push_back(std::format("{} | longest anchorless stretch: {}", name, longest_stretch(anchors, text.size())));

    return anchors;
}

/**
 * @brief The evaluation-slice emissions of splitting_measurements.py: splitting-stats.txt, the supply table and the
 *        edit-slack table, for the two vocabularies, with the trained vocabulary's shallower sibling for the
 *        divergence lines.
 * @param vocabularies The two vocabularies, local-384 first.
 * @param stats The trained vocabulary's tie accounting.
 * @param shallow The local-48 tokens.
 * @param sample The evaluation slice.
 * @param calibration The calibration slice.
 * @return The three emissions.
 */
[[nodiscard]] std::vector<Emission> vocabulary_emissions(
        const std::vector<Vocabulary>& vocabularies, const Merge_stats& stats, const std::vector<std::string>& shallow,
        const std::string_view sample, const std::string_view calibration)
{
    std::vector<std::string> lines;

    lines.push_back(std::format(
            "local-384 | merge tie rule: {} of {} merge steps had a most-frequent tie, and {} of those would have "
            "merged a different pair under first occurrence",
            stats.tied_steps, stats.merges, stats.first_occurrence_disagreements));

    std::vector<std::vector<Edit_record>> trials;

    std::vector<std::string> table_rows;

    std::vector<std::string> slack_table_rows;

    for (const auto& vocabulary : vocabularies)
    {
        const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

        lines.push_back(std::format(
                "{} | configuration: {} tokens digest {}, evaluation slice bytes [{}, {}) digest {}, window budget "
                "widths {} to {}",
                name, tokens.size(), token_digest(tokens), train_bytes, train_bytes + sample.size(),
                sha256(sample).substr(0, 16), widths.front(), widths.back()));
        lines.push_back(std::format(
                "{} | certified bytes: {} of {} alphabet bytes, exact by the interior-byte lemma", name, bytes.size(),
                alphabet(tokens).size()));

        std::size_t decisions{0};

        for (const auto& window : windows)
        {
            decisions += window.size();
        }

        lines.push_back(std::format(
                "{} | window pairs: {} certified of {} decided over every one of the {} windows of width two to four "
                "occurring in the slice, exhaustive",
                name, decided.exact.size(), decisions, windows.size()));
        lines.push_back(std::format(
                "{} | window pairs by the conservative model: {} certified of the {} pairs, is_split_window() on the "
                "same windows{}",
                name, decided.conservative.size(), decisions,
                decided.unsettled.empty() ?
                        "" :
                        std::format(", and {} exact decisions unsettled at the cap", decided.unsettled.size())));

        std::vector<double> curve;

        std::string curve_text;

        for (std::size_t budget{1}; budget <= widths.back(); ++budget)
        {
            auto through{bytes};

            for (const auto& pair : decided.exact)
            {
                if (pair.window.size() <= budget)
                {
                    through.push_back(pair);
                }
            }

            curve.push_back(density(through, sample));

            curve_text += std::format("{}H={}: {:.1f}", budget == 1 ? "" : ", ", budget, curve.back());
        }

        lines.push_back(std::format(
                "{} | supply by window budget: {} anchors per KiB, each row exact for the slice through its budget",
                name, curve_text));
        lines.push_back(std::format(
                "{} | wider half beyond the certified bytes: {:.1f} anchors per KiB", name,
                curve.back() - curve.front()));

        const auto certified{vocabulary.certified()};

        const auto anchors{supply_lines(name, certified, sample, lines)};

        for (const auto workers : {8, 64})
        {
            const auto distances{snap_distances(anchors, sample.size(), workers)};

            lines.push_back(std::format("{} | snap max at {} workers: {}", name, workers, std::ranges::max(distances)));
            lines.push_back(std::format("{} | snap p50 at {} workers: {}", name, workers, percentile(distances, 0.5)));
        }

        // The frozen inventory as the edit trials see it: the certified bytes and the pairs whose windows
        // the calibration slice holds.
        std::vector<Pair> frozen;

        const auto calibration_windows{occurring_windows(calibration)};

        for (const auto& pair : certified)
        {
            if (pair.window.size() == 1 || std::ranges::binary_search(calibration_windows, pair.window))
            {
                frozen.push_back(pair);
            }
        }

        const auto sigma{alphabet(tokens)};

        const auto longest{std::ranges::max(tokens, {}, &std::string::size).size()};

        lines.push_back(std::format(
                "{} | edit protocol: seed {} reset for this vocabulary, positions drawn uniformly over the slice and "
                "replacement bytes uniformly over its {}-byte alphabet, a draw repeating the original byte or leaving "
                "the input untokenizable rejected and redrawn, percentiles the order statistic at floor(q*n) capped at "
                "the last element",
                name, edit_seed, sigma.size()));

        trials.push_back(edit_trials(lexer, longest, sigma, certified, frozen, sample));

        note(std::format("{}: {} edits attempted", name, trials.back().size()));

        report_edits(name, trials.back(), lines);

        slack_rows(label, trials.back(), slack_table_rows);

        std::vector<std::size_t> per_block;

        for (std::size_t start{0}; start + block_bytes <= sample.size(); start += block_bytes)
        {
            per_block.push_back(static_cast<std::size_t>(std::ranges::count_if(
                    anchors, [start](const std::size_t a) { return start <= a && a < start + block_bytes; })));
        }

        lines.push_back(std::format(
                "{} | anchors per block over {} blocks of {} bytes: {} to {}, p50 {}", name, per_block.size(),
                block_bytes, std::ranges::min(per_block), std::ranges::max(per_block), percentile(per_block, 0.5)));

        legacy_sample_lines(name, lexer, sample, bytes, lines);

        sync_line(lexer, name, lines);

        newline_lines(lexer, name, lines);

        std::vector<std::string> values;

        for (const auto* key :
             {"anchors per KiB", "anchor gap p50", "anchor gap p90", "anchor gap max", "snap max at 8 workers"})
        {
            const auto prefix{std::format("{} | {}:", name, key)};

            const auto found{std::ranges::find_if(
                    lines, [&prefix](const std::string& line) { return line.starts_with(prefix); })};

            values.push_back(found->substr(prefix.size() + 1));
        }

        table_rows.push_back(std::format(
                R"({} & {} & {} & {} & {} & {} \\)", label, values[0], values[1], values[2], values[3], values[4]));
    }

    const auto& local{vocabularies.front()};

    const auto& gpt2{vocabularies.back()};

    measure_pairing(local.name, trials.front(), gpt2.name, trials.back(), lines);

    {
        const auto left{anchor_positions(local.certified(), sample)};

        const auto right{anchor_positions(gpt2.certified(), sample)};

        std::vector<std::size_t> shared;

        std::ranges::set_intersection(left, right, std::back_inserter(shared));

        const auto both{shared.size()};

        const auto only_left{left.size() - both};

        const auto only_right{right.size() - both};

        const auto interior{sample.size() - 1};

        const auto in_union{both + only_left + only_right};

        lines.push_back(std::format(
                "{} against {} | anchor overlap: both {}, {} only {}, {} only {}, neither {} of {} interior positions, "
                "Jaccard {:.3f}",
                local.name, gpt2.name, both, local.name, only_left, gpt2.name, only_right,
                interior - both - only_left - only_right, interior,
                in_union == 0 ? 0.0 : static_cast<double>(both) / static_cast<double>(in_union)));
    }

    sync_line(utf8_shape(), "utf8-shape", lines);

    {
        const auto shallow_lexer{compile(shallow)};

        std::size_t agree{0};

        std::size_t total{0};

        bool witnessed{false};

        for (std::size_t offset{0}; offset < eval_bytes - 64; offset += 256)
        {
            const auto piece{sample.substr(offset, 64)};

            const auto a{token_starts(shallow_lexer, piece)};

            const auto b{token_starts(local.lexer, piece)};

            if (!a || !b)
            {
                continue;
            }

            ++total;

            if (*a == *b)
            {
                ++agree;
            }
            else
            {
                witnessed = true;
            }
        }

        lines.push_back(std::format("local-48 against local-384 | slices agreeing: {} of {}", agree, total));
        lines.push_back(std::format("local-48 against local-384 | divergence witnessed: {}", witnessed ? "yes" : "no"));
    }

    std::vector<std::string> table{
            R"(\begin{tabular}{@{}lrrrrr@{}})", R"(\toprule)",
            R"(Vocabulary & anchors/KiB & gap p50 & gap p90 & gap max & snap max (8) \\)", R"(\midrule)"};

    table.insert(table.end(), table_rows.begin(), table_rows.end());

    table.insert(table.end(), {R"(\bottomrule)", R"(\end{tabular})"});

    std::vector<std::string> slack_table{
            R"(\begin{tabular}{@{}llrrrr@{}})",
            R"(\toprule)",
            R"( & & \multicolumn{2}{c}{ceiling inventory} & \multicolumn{2}{c}{frozen inventory} \\)",
            R"(\cmidrule(lr){3-4}\cmidrule(lr){5-6})",
            R"(Vocabulary & Statistic & p50 & p90 & p50 & p90 \\)",
            R"(\midrule)"};

    slack_table.insert(slack_table.end(), slack_table_rows.begin(), slack_table_rows.end());

    slack_table.insert(slack_table.end(), {R"(\bottomrule)", R"(\end{tabular})"});

    return {Emission{.file = "splitting-stats.txt", .lines = std::move(lines)},
            Emission{.file = "splitting-supply-table.tex", .lines = std::move(table)},
            Emission{.file = "edit-slack-table.tex", .lines = std::move(slack_table)}};
}

/**
 * @brief budget_coverage.py's emission: what each width bought, read off the decided inventories.
 * @param vocabularies The vocabularies.
 * @param sample The evaluation slice.
 * @return The emission.
 */
[[nodiscard]] Emission budget_emission(const std::vector<Vocabulary>& vocabularies, const std::string_view sample)
{
    std::vector<std::string> lines;

    for (const auto& vocabulary : vocabularies)
    {
        const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

        std::size_t previous{0};

        for (std::size_t budget{1}; budget <= widths.back(); ++budget)
        {
            auto through{bytes};

            std::size_t certified_here{0};

            for (const auto& pair : decided.exact)
            {
                if (pair.window.size() <= budget)
                {
                    through.push_back(pair);
                }

                if (pair.window.size() == budget)
                {
                    ++certified_here;
                }
            }

            const auto anchors{anchor_positions(through, sample).size()};

            if (budget == 1)
            {
                lines.push_back(std::format(
                        "{} | budget H=1: {} anchor positions from {} certified bytes of {} in the alphabet, settled "
                        "by "
                        "the interior-byte lemma rather than by a search",
                        name, anchors, bytes.size(), alphabet(tokens).size()));
            }
            else
            {
                const auto decided_here{windows_of_width(sample, budget).size() * budget};

                lines.push_back(std::format(
                        "{} | budget H={}: {} anchor positions, {} gained over the width before, {} pairs certified of "
                        "{} decided at this width alone",
                        name, budget, anchors, anchors - previous, certified_here, decided_here));
            }

            previous = anchors;
        }

        lines.push_back(std::format(
                "{} | the curve is reported through width {}, the declared budget, and the gain at that last width is "
                "the final row above; what lies past the budget is not estimated here beyond the width-five probe the "
                "edit trials run",
                name, widths.back()));
    }

    return Emission{.file = "budget-coverage-stats.txt", .lines = std::move(lines)};
}

/**
 * @brief depth_series.py's emission: supply against merge depth, the three shallower vocabularies decided here and
 *        the deepest being the trained vocabulary already decided.
 * @param merges The trained merge table.
 * @param local The trained vocabulary, decided.
 * @param sample The evaluation slice.
 * @return The emission.
 */
[[nodiscard]] Emission depth_emission(
        const std::vector<std::pair<std::string, std::string>>& merges, const Vocabulary& local,
        const std::string_view sample)
{
    std::vector<std::string> lines{std::format(
            "series: byte-pair vocabularies over the same 256-symbol fallback base, trained on the same first {} "
            "corpus "
            "bytes, measured on the same evaluation slice [{}, {}) through the same width budget; merge depth is the "
            "only thing that varies",
            train_bytes, train_bytes, train_bytes + sample.size())};

    std::vector<double> densities;

    for (const auto depth : depths)
    {
        const auto name{std::format("depth {}", depth)};

        std::optional<Vocabulary> own;

        if (depth != merges_local)
        {
            own = measure_vocabulary(name, name, local_tokens(merges, depth), sample);
        }

        const auto& row{own ? *own : local};

        const auto certified{row.certified()};

        const auto anchors{anchor_positions(certified, sample)};

        const auto [count, per_kibibyte, gaps]{library_supply(certified, sample)};

        lines.push_back(std::format(
                "{} | tokens: {}, certified bytes {}, wider pairs {}", name, row.tokens.size(), row.bytes.size(),
                row.decided.exact.size()));
        lines.push_back(std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte));
        lines.push_back(std::format(
                "{} | anchor gap p50: {}, p90 {}, max {}", name, gaps->median, gaps->ninetieth, gaps->longest));
        lines.push_back(
                std::format("{} | longest anchorless stretch: {}", name, longest_stretch(anchors, sample.size())));

        if (!row.decided.unsettled.empty())
        {
            lines.push_back(
                    std::format("{} | exact decisions unsettled at the cap: {}", name, row.decided.unsettled.size()));
        }

        densities.push_back(per_kibibyte);
    }

    std::string curve;

    for (std::size_t index{0}; index < depths.size(); ++index)
    {
        curve += std::format("{}{}: {:.1f}", index == 0 ? "" : ", ", depths[index], densities[index]);
    }

    lines.push_back(std::format(
            "series | supply against depth: {} anchors per KiB, falling by a factor of {:.2f} from depth {} to depth "
            "{}",
            curve, densities.front() / densities.back(), depths.front(), depths.back()));

    const auto monotone{std::ranges::is_sorted(densities, std::ranges::greater_equal{})};

    lines.push_back(std::format(
            "series | the curve is {} over the depths measured, which is a fact about these four vocabularies on this "
            "slice and not a law",
            monotone ? "monotone decreasing" : "not monotone"));

    return Emission{.file = "depth-series-stats.txt", .lines = std::move(lines)};
}

/**
 * @brief frozen_inventory.py's two emissions: the inventory frozen on the calibration slice applied to the
 *        evaluation slice beside the post-hoc ceiling, and the transfer as a distribution over the slice's blocks.
 * @param vocabularies The vocabularies.
 * @param sample The evaluation slice.
 * @param calibration The calibration slice.
 * @return The two emissions.
 */
[[nodiscard]] std::vector<Emission> frozen_emissions(
        const std::vector<Vocabulary>& vocabularies, const std::string_view sample, const std::string_view calibration)
{
    const auto start{train_bytes + sample.size()};

    std::vector<std::string> lines{std::format(
            "calibration slice: bytes [{}, {}), disjoint from the training bytes and from the evaluation slice [{}, "
            "{})",
            start, start + calibration.size(), train_bytes, train_bytes + sample.size())};

    const auto calibration_windows{occurring_windows(calibration)};

    std::vector<std::string> only_here;

    std::size_t decisions{0};

    std::array<std::size_t, widths.size()> by_width{};

    for (const auto& window : calibration_windows)
    {
        if (!std::ranges::binary_search(vocabularies.front().windows, window))
        {
            only_here.push_back(window);

            decisions += window.size();

            ++by_width[window.size() - widths.front()];
        }
    }

    std::string by_width_text;

    for (std::size_t index{0}; index < widths.size(); ++index)
    {
        by_width_text += std::format("{}{} of width {}", index == 0 ? "" : ", ", by_width[index], widths[index]);
    }

    std::vector<std::string> table{
            R"(\begin{tabular}{@{}lrrr@{}})",
            R"(\toprule)",
            R"( & \multicolumn{3}{c}{share of the block's ceiling supply retained, per cent} \\)",
            R"(\cmidrule(lr){2-4})",
            R"(Vocabulary & lowest block & upper median & highest block \\)",
            R"(\midrule)"};

    for (const auto& vocabulary : vocabularies)
    {
        const auto& [name, label, tokens, lexer, bytes, windows, decided]{vocabulary};

        // The calibration-only windows decided fresh, the shared ones carrying the evaluation run's verdicts.
        const auto fresh{decide(lexer, every_origin(only_here))};

        note(std::format(
                "{}: {} calibration-only windows decided, {} pairs certified, {} unsettled", name, only_here.size(),
                fresh.exact.size(), fresh.unsettled.size()));

        auto frozen{bytes};

        for (const auto& pair : decided.exact)
        {
            if (std::ranges::binary_search(calibration_windows, pair.window))
            {
                frozen.push_back(pair);
            }
        }

        frozen.insert(frozen.end(), fresh.exact.begin(), fresh.exact.end());

        const auto ceiling{vocabulary.certified()};

        const auto frozen_anchors{anchor_positions(frozen, sample)};

        const auto ceiling_anchors{anchor_positions(ceiling, sample)};

        const auto frozen_density{density(frozen, sample)};

        const auto oracle_density{density(ceiling, sample)};

        lines.push_back(std::format(
                "{} | frozen hybrid inventory: {} certified pairs, {} certified bytes from the token set alone and {} "
                "calibrated pairs of widths two to four from the calibration slice; {} of the (window, origin) "
                "decisions behind the calibrated pairs are ones the evaluation slice never poses, from {} "
                "calibration-only windows ({}); the evaluation slice poses every other one of them, and certification "
                "does not depend on the slice, so those verdicts are the same whichever run reached them",
                name, frozen.size(), bytes.size(), frozen.size() - bytes.size(), decisions, only_here.size(),
                by_width_text));
        lines.push_back(std::format(
                "{} | frozen supply on the evaluation slice: {:.1f} anchors per KiB, longest anchorless stretch {}",
                name, frozen_density, longest_stretch(frozen_anchors, sample.size())));
        lines.push_back(std::format(
                "{} | post-hoc ceiling on the same slice: {:.1f} anchors per KiB, longest anchorless stretch {}", name,
                oracle_density, longest_stretch(ceiling_anchors, sample.size())));
        lines.push_back(std::format(
                "{} | frozen supply as a share of the ceiling: {:.1f} per cent", name,
                oracle_density == 0.0 ? 0.0 : 100.0 * frozen_density / oracle_density));

        if (!fresh.unsettled.empty())
        {
            lines.push_back(std::format(
                    "{} | calibration-only exact decisions unsettled at the cap: {}", name, fresh.unsettled.size()));
        }

        std::vector<double> shares;

        for (std::size_t block{0}; block + block_bytes <= sample.size(); block += block_bytes)
        {
            const auto in_block{[block](const std::vector<std::size_t>& anchors) {
                return std::ranges::count_if(
                        anchors, [block](const std::size_t a) { return block <= a && a < block + block_bytes; });
            }};

            const auto held{in_block(ceiling_anchors)};

            if (held == 0)
            {
                throw std::runtime_error{std::format("the block at {} has no ceiling supply", block)};
            }

            shares.push_back(100.0 * static_cast<double>(in_block(frozen_anchors)) / static_cast<double>(held));
        }

        table.push_back(std::format(
                R"({} & {:.1f} & {:.1f} & {:.1f} \\)", label, std::ranges::min(shares), percentile(shares, 0.5),
                std::ranges::max(shares)));
    }

    table.insert(table.end(), {R"(\bottomrule)", R"(\end{tabular})"});

    return {Emission{.file = "frozen-inventory-stats.txt", .lines = std::move(lines)},
            Emission{.file = "frozen-transfer-table.tex", .lines = std::move(table)}};
}

/**
 * @brief One campaign row: its name, corpus file, grammar, the class letters in the paper's order and the letter of
 *        each byte.
 */
struct Campaign_row
{
    std::string name;

    std::string corpus;

    std::function<void(munch::core::Builder&)> grammar;

    std::string sigma;

    std::function<char(unsigned char)> classify;

    std::vector<Pair> extra_windows;
};

/**
 * @brief The class letter every C-like row shares, or nothing where the row's own refinement decides.
 * @param byte The byte.
 * @return The letter, or std::nullopt.
 */
[[nodiscard]] std::optional<char> core_class(const unsigned char byte)
{
    const auto c{static_cast<char>(byte)};

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

    if (figures::punctuation().symbols().contains(c))
    {
        return 'P';
    }

    return std::nullopt;
}

/**
 * @brief The four campaign rows, the grammars composed as recovery_quality.cpp composes them and the classes as
 *        campaign_inventory.py spells them.
 * @return The rows.
 */
[[nodiscard]] std::vector<Campaign_row> campaign_rows()
{
    const auto conventional_class{[](const unsigned char byte) {
        const auto c{static_cast<char>(byte)};

        return core_class(byte).value_or(
                c == '"'                                   ? 'Q' :
                c == '/'                                   ? 'C' :
                figures::operators().symbols().contains(c) ? 'O' :
                                                             'X');
    }};

    const auto block_class{[](const unsigned char byte) {
        const auto c{static_cast<char>(byte)};

        return core_class(byte).value_or(
                c == '/'                                   ? 'C' :
                c == '*'                                   ? 'A' :
                figures::operators().symbols().contains(c) ? 'O' :
                                                             'X');
    }};

    const auto bare_class{[](const unsigned char byte) {
        return core_class(byte).value_or(figures::operators().symbols().contains(static_cast<char>(byte)) ? 'O' : 'X');
    }};

    std::vector<Pair> close_shaped;

    for (const auto left : std::string_view{"LDSNACOPX"})
    {
        for (const auto right : std::string_view{"LDSNACOPX"})
        {
            close_shaped.push_back(Pair{.window = std::format("{}AC{}", left, right), .origin = 3});
        }
    }

    return {Campaign_row{
                    .name = "campaign conventional",
                    .corpus = "corpus-c-like-conventional-with-strings-and-line-comments.bin",
                    .grammar =
                            [](munch::core::Builder& b) {
                                figures::c_like(b, false);
                                b.add_token(figures::string_literal(), figures::Token::String, 2);
                                b.add_token(figures::line_comment(), figures::Token::LineComment, 1);
                            },
                    .sigma = "LDSNQCOPX",
                    .classify = conventional_class,
                    .extra_windows = {}},
            Campaign_row{
                    .name = "campaign split-friendly",
                    .corpus = "corpus-c-like-split-friendly-with-strings-and-line-comments.bin",
                    .grammar =
                            [](munch::core::Builder& b) {
                                figures::c_like(b, true);
                                b.add_token(figures::string_literal(), figures::Token::String, 2);
                                b.add_token(figures::line_comment(), figures::Token::LineComment, 1);
                            },
                    .sigma = "LDSNQCOPX",
                    .classify = conventional_class,
                    .extra_windows = {}},
            Campaign_row{
                    .name = "campaign block",
                    .corpus = "corpus-c-like-conventional-plus-block-comments-alone.bin",
                    .grammar =
                            [](munch::core::Builder& b) {
                                figures::c_like(b, false);
                                b.add_token(figures::block_comment(), figures::Token::BlockComment, 1);
                            },
                    .sigma = "LDSNACOPX",
                    .classify = block_class,
                    .extra_windows = close_shaped},
            Campaign_row{
                    .name = "campaign bare",
                    .corpus = "corpus-c-like-bare--identifiers-numbers-operators-punctuation.bin",
                    .grammar = [](munch::core::Builder& b) { figures::c_like(b, false); },
                    .sigma = "LDSNOPX",
                    .classify = bare_class,
                    .extra_windows = {}}};
}

/**
 * @brief The byte classes of a lexer's tables, as tools/audit's report enumerates them: two bytes are one class when
 *        every state moves on both to the same state; each class ascending, the classes by lowest byte.
 * @param lexer The lexer.
 * @return The classes.
 */
[[nodiscard]] std::vector<std::vector<unsigned char>> byte_classes(const Lexer& lexer)
{
    const auto& simulator{lexer.simulator()};

    std::map<std::vector<std::optional<std::size_t>>, std::vector<unsigned char>> by_signature;

    for (int value{0}; value < 256; ++value)
    {
        std::vector<std::optional<std::size_t>> signature;

        for (std::size_t state{0}; state < simulator.state_count(); ++state)
        {
            signature.push_back(simulator.step(state, static_cast<unsigned char>(value)));
        }

        by_signature[std::move(signature)].push_back(static_cast<unsigned char>(value));
    }

    std::vector<std::vector<unsigned char>> classes;

    for (auto& [signature, members] : by_signature)
    {
        classes.push_back(std::move(members));
    }

    std::ranges::sort(classes, {}, [](const std::vector<unsigned char>& members) { return members.front(); });

    return classes;
}

/**
 * @brief What the campaign's edit study needs of a measured row: its lexer, its corpus, the corpus over class
 *        representatives, the letter of each byte, the letters in the paper's order and the inventory over
 *        representatives.
 */
struct Measured_row
{
    std::string name;

    Lexer lexer;

    std::string corpus;

    std::string class_text;

    std::function<char(unsigned char)> classify;

    std::string sigma;

    std::vector<Pair> certified;
};

/**
 * @brief The campaign's between-anchors edit study on one row, campaign_inventory.py's draws replayed: cross-class
 *        single-byte substitutions over a line-ended slice of the corpus, every moved boundary held strictly between
 *        the flanking anchors the inventory witnesses in the shared prefix and suffix, and same-class substitutions
 *        held to move nothing.
 * @param row The row.
 * @param lines Receives the two lines.
 * @throws std::runtime_error If a moved boundary escapes the flanking anchors or a same-class substitution moves one.
 */
void campaign_edit_lines(const Measured_row& row, std::vector<std::string>& lines)
{
    const auto& [name, lexer, corpus, class_text, classify, sigma, certified]{row};

    // The slice ends at a line end, since a raw cut can open a string or comment it never closes.
    const auto slice{std::string_view{corpus}.substr(0, corpus.rfind('\n', campaign_slice_bytes - 1) + 1)};

    const auto slice_classes{std::string_view{class_text}.substr(0, slice.size())};

    const auto base_starts{token_starts(lexer, slice)};

    if (!base_starts)
    {
        throw std::runtime_error{std::format("{}: the edit slice does not tokenize", name)};
    }

    const std::set<std::size_t> base(base_starts->begin(), base_starts->end());

    // The replacement byte of each letter, in the order campaign_inventory.py lists them.
    const std::vector<std::pair<char, char>> replacements{{'L', 'x'}, {'D', '7'}, {'S', ' '}, {'N', '\n'},  {'Q', '"'},
                                                          {'C', '/'}, {'O', '+'}, {'P', ';'}, {'X', '\x01'}};

    const auto replacement_of{[&replacements](const char letter) {
        return std::ranges::find(replacements, letter, &std::pair<char, char>::first)->second;
    }};

    // Every occurrence of every inventory window in the slice's class string, found once.
    std::vector<std::pair<Pair, std::vector<std::size_t>>> occurrences;

    for (const auto& pair : certified)
    {
        std::vector<std::size_t> found;

        for (auto at{slice_classes.find(pair.window)}; at != std::string_view::npos;
             at = slice_classes.find(pair.window, at + 1))
        {
            found.push_back(at);
        }

        occurrences.emplace_back(pair, std::move(found));
    }

    const auto moved_after{[&](const std::string_view edited) {
        const auto starts{token_starts(lexer, edited)};

        std::optional<std::vector<std::size_t>> moved;

        if (starts)
        {
            const std::set<std::size_t> after(starts->begin(), starts->end());

            moved.emplace();

            std::ranges::set_symmetric_difference(base, after, std::back_inserter(*moved));
        }

        return moved;
    }};

    Python_random chooser{campaign_edit_seed};

    std::vector<std::size_t> radii;

    std::size_t draws{0};

    std::size_t redrawn{0};

    while (radii.size() < campaign_cross_class_edits)
    {
        ++draws;

        const auto position{chooser.randrange(slice.size())};

        std::string others;

        for (const auto letter : sigma)
        {
            if (letter != classify(static_cast<unsigned char>(slice[position])))
            {
                others.push_back(letter);
            }
        }

        const auto target{others[chooser.randrange(others.size())]};

        std::string edited{slice};

        edited[position] = replacement_of(target);

        const auto moved{moved_after(edited)};

        if (!moved)
        {
            ++redrawn;

            continue;
        }

        // The flanking anchors: the greatest witnessed wholly before the edit and the least witnessed wholly after.
        std::size_t low{0};

        auto high{slice.size()};

        for (const auto& [pair, found] : occurrences)
        {
            for (const auto q : found)
            {
                const auto anchor{q + pair.origin};

                if (q + pair.window.size() <= position && anchor > 0 && anchor <= position)
                {
                    low = std::max(low, anchor);
                }

                if (q > position && anchor > position)
                {
                    high = std::min(high, anchor);
                }
            }
        }

        for (const auto boundary : *moved)
        {
            if (boundary <= low || boundary >= high)
            {
                throw std::runtime_error{
                        std::format("{}: a moved boundary at {} escapes the flanking anchors", name, boundary)};
            }
        }

        radii.push_back(moved->empty() ? 0 : moved->back() - moved->front() + 1);
    }

    std::size_t unchanged{0};

    std::size_t position_draws{0};

    while (unchanged < campaign_same_class_edits)
    {
        ++position_draws;

        const auto position{chooser.randrange(slice.size())};

        const auto letter{classify(static_cast<unsigned char>(slice[position]))};

        const auto same{std::ranges::find_if(replacements, [&](const std::pair<char, char>& entry) {
            return classify(static_cast<unsigned char>(entry.second)) == letter && entry.second != slice[position];
        })};

        if (same == replacements.end())
        {
            continue;
        }

        std::string edited{slice};

        edited[position] = same->second;

        const auto moved{moved_after(edited)};

        if (!moved || !moved->empty())
        {
            throw std::runtime_error{
                    std::format("{}: a same-class substitution at {} moved a boundary", name, position)};
        }

        ++unchanged;
    }

    lines.push_back(std::format(
            "{} | edit bound: {} cross-class substitutions retained from {} draws, {} redrawn as untokenizable, on a "
            "{}-byte slice, every moved boundary between the flanking anchors, boundary-difference hull p50 {} and max "
            "{}",
            name, radii.size(), draws, redrawn, slice.size(), percentile(radii, 0.5), std::ranges::max(radii)));
    lines.push_back(std::format(
            "{} | class invariance: {} same-class substitutions retained from {} position draws moved no boundary",
            name, unchanged, position_draws));
}

/**
 * @brief campaign_inventory.py's three emissions over the four c-like rows: the stats file, the conventional row's
 *        anchor table and the supply table.
 * @param corpora The directory holding the archived campaign corpora.
 * @return The three emissions.
 * @throws std::runtime_error If a row's byte classes are not the paper's letters, or a corpus does not tokenize.
 */
[[nodiscard]] std::vector<Emission> campaign_emissions(const std::filesystem::path& corpora)
{
    std::vector<std::string> lines;

    std::vector<std::string> anchor_table{
            "# certified (window, origin) pairs of the conventional row, over classes",
            "# L alpha or underscore, D digit, S space or tab, N newline, Q quote,",
            "# C slash, O other operator, P punctuation, X everything else"};

    std::vector<std::string> table_rows;

    std::size_t certified_total{0};

    std::size_t vacuous_total{0};

    std::vector<Measured_row> edit_rows;

    for (const auto& [name, corpus_file, grammar, sigma, classify, extra_windows] : campaign_rows())
    {
        munch::core::Builder builder;

        grammar(builder);

        const auto lexer{builder.build()};

        const auto corpus{read_file(corpora / std::format("{}.{}", campaign_archive, corpus_file))};

        // The paper's letters against the library's classes: every letter must lie inside one class of the tables,
        // so that its bytes move every state alike and a window over letters decides as its representatives do. The
        // letters may be finer than the tables' classes, as they are where a row treats the newline like a blank.
        const auto table_classes{byte_classes(lexer)};

        std::array<std::size_t, 256> class_of{};

        for (std::size_t index{0}; index < table_classes.size(); ++index)
        {
            for (const auto member : table_classes[index])
            {
                class_of[member] = index;
            }
        }

        std::map<char, std::vector<unsigned char>> members_of;

        for (int value{0}; value < 256; ++value)
        {
            members_of[classify(static_cast<unsigned char>(value))].push_back(static_cast<unsigned char>(value));
        }

        if (members_of.size() != sigma.size())
        {
            throw std::runtime_error{
                    std::format("{}: {} letters for {} declared", name, members_of.size(), sigma.size())};
        }

        std::map<std::size_t, std::string> letters_in_class;

        std::map<char, unsigned char> representative;

        std::array<char, 256> canonical{};

        std::vector<std::vector<unsigned char>> classes;

        for (const auto& [letter, members] : members_of)
        {
            for (const auto member : members)
            {
                if (class_of[member] != class_of[members.front()])
                {
                    throw std::runtime_error{std::format("{}: the letter {} straddles two byte classes", name, letter)};
                }

                canonical[member] = static_cast<char>(members.front());
            }

            letters_in_class[class_of[members.front()]].push_back(letter);

            representative.emplace(letter, members.front());

            classes.push_back(members);
        }

        std::ranges::sort(classes, {}, [](const std::vector<unsigned char>& members) { return members.front(); });

        std::string merged;

        for (const auto& [index, letters] : letters_in_class)
        {
            if (letters.size() > 1)
            {
                merged += std::format("{}{}", merged.empty() ? ", the tables not telling " : " nor ", letters);
            }
        }

        const auto of_letters{[&representative](const std::string_view letters) {
            std::string bytes;

            for (const auto letter : letters)
            {
                bytes.push_back(static_cast<char>(representative.at(letter)));
            }

            return bytes;
        }};

        const auto to_letters{[&classify](const std::string_view bytes) {
            std::string letters;

            for (const auto byte : bytes)
            {
                letters.push_back(classify(static_cast<unsigned char>(byte)));
            }

            return letters;
        }};

        std::string class_text{corpus};

        for (auto& byte : class_text)
        {
            byte = canonical[static_cast<unsigned char>(byte)];
        }

        const auto byte_starts{token_starts(lexer, corpus)};

        const auto class_starts{token_starts(lexer, class_text)};

        if (!byte_starts || !class_starts || *byte_starts != *class_starts)
        {
            throw std::runtime_error{std::format("{}: the class abstraction does not commute with the scan", name)};
        }

        lines.push_back(std::format(
                "{} | abstraction: {} corpus bytes, byte and class scans agree on all {} token starts", name,
                corpus.size(), byte_starts->size()));
        lines.push_back(std::format(
                "{} | byte classes: the row's {} letters each lie inside one of the tables' {} classes{}", name,
                sigma.size(), table_classes.size(), merged.empty() ? "" : merged + " apart"));

        // Every class window to length two at every origin, in the paper's enumeration order.
        std::vector<Pair> candidates;

        for (const auto letter : sigma)
        {
            candidates.push_back(Pair{.window = std::string(1, letter), .origin = 0});
        }

        for (const auto left : sigma)
        {
            for (const auto right : sigma)
            {
                for (std::size_t origin{0}; origin < 2; ++origin)
                {
                    candidates.push_back(Pair{.window = std::format("{}{}", left, right), .origin = origin});
                }
            }
        }

        const auto over_bytes{[&of_letters](const std::vector<Pair>& over_letters) {
            std::vector<Pair> bytes;

            for (const auto& [window, origin] : over_letters)
            {
                bytes.push_back(Pair{.window = of_letters(window), .origin = origin});
            }

            return bytes;
        }};

        const auto byte_candidates{over_bytes(candidates)};

        const auto decided{decide(lexer, byte_candidates)};

        if (!decided.unsettled.empty())
        {
            throw std::runtime_error{std::format("{}: a class-window decision was unsettled at the cap", name)};
        }

        // The certified pairs in candidate order, over letters and over bytes alike.
        std::vector<Pair> certified;

        std::vector<Pair> certified_bytes_order;

        for (std::size_t index{0}; index < candidates.size(); ++index)
        {
            if (std::ranges::binary_search(decided.exact, byte_candidates[index]))
            {
                certified.push_back(candidates[index]);
                certified_bytes_order.push_back(byte_candidates[index]);
            }
        }

        std::string singles;

        for (const auto& [window, origin] : certified)
        {
            if (window.size() == 1)
            {
                singles += std::format("{}'{}'", singles.empty() ? "" : ", ", window);
            }
        }

        lines.push_back(std::format(
                "{} | certified pairs: {} over class windows to length two, certified single classes at origin zero: "
                "{}",
                name, certified.size(), singles.empty() ? "none" : std::format("[{}]", singles)));
        lines.push_back(std::format(
                "{} | certified pairs by the conservative model: {} of the {} class pairs to length two, "
                "is_split_window() on the representatives",
                name, decided.conservative.size(), candidates.size()));

        const auto two_class_certified{
                std::ranges::any_of(certified, [](const Pair& p) { return p.window.size() == 2; })};

        if (!two_class_certified)
        {
            std::size_t refused{0};

            std::size_t pairs{0};

            std::optional<std::pair<std::string, Pair>> deepest;

            for (std::size_t index{0}; index < candidates.size(); ++index)
            {
                if (candidates[index].window.size() != 2)
                {
                    continue;
                }

                ++pairs;

                const auto [witness, exhaustive]{
                        lexer.window_counterexample(byte_candidates[index].window, byte_candidates[index].origin)};

                if (witness.empty())
                {
                    continue;
                }

                ++refused;

                if (!deepest || witness.size() > deepest->first.size())
                {
                    deepest = std::pair{witness, candidates[index]};
                }
            }

            if (deepest)
            {
                lines.push_back(std::format(
                        "{} | no two-class window certifies: all {} of {} two-class pairs are refuted, and the deepest "
                        "refutation the decider synthesizes runs to {} classes, {} for the window {} at origin {}",
                        name, refused, pairs, deepest->first.size(), to_letters(deepest->first), deepest->second.window,
                        deepest->second.origin));
            }
        }

        if (!extra_windows.empty())
        {
            const auto extra_bytes{over_bytes(extra_windows)};

            const auto extra{decide(lexer, extra_bytes)};

            if (!extra.unsettled.empty())
            {
                throw std::runtime_error{std::format("{}: a budget-extension decision was unsettled at the cap", name)};
            }

            for (std::size_t index{0}; index < extra_windows.size(); ++index)
            {
                if (std::ranges::binary_search(extra.exact, extra_bytes[index]))
                {
                    certified.push_back(extra_windows[index]);
                    certified_bytes_order.push_back(extra_bytes[index]);
                }
            }

            lines.push_back(std::format(
                    "{} | budget extension: {} of {} close-shaped four-class windows certify at origin three", name,
                    extra.exact.size(), extra_windows.size()));
            lines.push_back(std::format(
                    "{} | budget extension by the conservative model: {} of {} close-shaped four-class windows, "
                    "is_split_window() on the representatives",
                    name, extra.conservative.size(), extra_windows.size()));
        }

        std::string vacuous;

        std::size_t vacuous_count{0};

        for (std::size_t index{0}; index < certified.size(); ++index)
        {
            const auto [witness, exhaustive]{lexer.window_occurrence(certified_bytes_order[index].window)};

            if (!exhaustive)
            {
                throw std::runtime_error{std::format("{}: an occurrence search was unsettled at the cap", name)};
            }

            if (witness.empty())
            {
                ++vacuous_count;

                vacuous += std::format(
                        "{}{} {}", vacuous.empty() ? "" : ", ", certified[index].window, certified[index].origin);

                if (class_text.find(certified_bytes_order[index].window) != std::string::npos)
                {
                    throw std::runtime_error{std::format("{}: a vacuous window occurs in the corpus", name)};
                }
            }
        }

        lines.push_back(std::format(
                "{} | occurring certificates: {} of {} certified pairs have a window occurring in some completely "
                "tokenizable class string, {} vacuous: {}",
                name, certified.size() - vacuous_count, certified.size(), vacuous_count,
                vacuous.empty() ? "none" : vacuous));

        certified_total += certified.size();

        vacuous_total += vacuous_count;

        // The gap corollary over the inventory: every class window expanded to its byte windows, since the walk
        // reads bytes; the conservative model must certify each, so where it refuses one the verdict is over what
        // it certifies and the line says so.
        {
            std::vector<std::string> storage;

            for (const auto& [window, origin] : certified)
            {
                std::vector<std::string> expansions{""};

                for (const auto letter : window)
                {
                    std::vector<std::string> longer;

                    for (const auto& prefix : expansions)
                    {
                        for (const auto member : members_of.at(letter))
                        {
                            longer.push_back(prefix + static_cast<char>(member));
                        }
                    }

                    expansions = std::move(longer);
                }

                for (auto& expansion : expansions)
                {
                    if (lexer.is_split_window(expansion) == origin)
                    {
                        storage.push_back(std::move(expansion));
                    }
                }
            }

            const Inventory index{certified_bytes_order};

            std::vector<std::pair<std::string_view, std::size_t>> inventory;

            for (const auto& window : storage)
            {
                for (const auto origin : index.origins_of.at(of_letters(to_letters(window))))
                {
                    inventory.emplace_back(window, origin);
                }
            }

            std::optional<std::size_t> span;

            if (!certified.empty())
            {
                span = lexer.anchor_free_span(inventory);
            }

            lines.push_back(std::format(
                    "{} | density verdict: {}", name,
                    certified.empty() ? "unbounded at this budget, no certified window to anchor on" :
                    span              ? std::to_string(*span) :
                                        "unbounded"));
        }

        // The anchors over the class string, the campaign's gap convention counting the runs to the two ends.
        const auto anchors{anchor_positions(certified_bytes_order, class_text)};

        const std::set<std::size_t> boundary_set(byte_starts->begin(), byte_starts->end());

        for (const auto anchor : anchors)
        {
            if (!boundary_set.contains(anchor))
            {
                throw std::runtime_error{std::format("{}: the anchor {} is off the boundary set", name, anchor)};
            }
        }

        std::vector<std::size_t> gaps;

        std::size_t previous{0};

        for (const auto anchor : anchors)
        {
            gaps.push_back(anchor - previous);

            previous = anchor;
        }

        gaps.push_back(class_text.size() - previous);

        const auto [count, per_kibibyte, library_gaps]{library_supply(certified_bytes_order, corpus, classes)};

        lines.push_back(std::format("{} | anchors per KiB: {:.1f}", name, per_kibibyte));
        lines.push_back(std::format("{} | anchor gap p50: {}", name, percentile(gaps, 0.5)));
        lines.push_back(std::format("{} | anchor gap p90: {}", name, percentile(gaps, 0.9)));
        lines.push_back(std::format("{} | anchor gap max: {}", name, std::ranges::max(gaps)));
        if (library_gaps)
        {
            lines.push_back(std::format(
                    "{} | anchor gaps by supply(): p50 {}, p90 {}, max {}, the runs to the ends excluded", name,
                    library_gaps->median, library_gaps->ninetieth, library_gaps->longest));
        }

        const auto snap_max{[&anchors, &class_text](const std::size_t workers) {
            return anchors.empty() ?
                           std::string{"no anchors"} :
                           std::to_string(std::ranges::max(snap_distances(anchors, class_text.size(), workers)));
        }};

        for (const auto workers : {8, 64})
        {
            lines.push_back(std::format("{} | snap max at {} workers: {}", name, workers, snap_max(workers)));
        }

        // The split theorem run whole: the corpus cut at every anchor, each chunk scanned on its own.
        std::vector<std::size_t> chunked;

        std::size_t begin{0};

        auto cuts{anchors};

        cuts.push_back(corpus.size());

        for (const auto end : cuts)
        {
            const auto starts{token_starts(lexer, std::string_view{corpus}.substr(begin, end - begin))};

            if (!starts)
            {
                throw std::runtime_error{std::format("{}: the chunk at {} does not tokenize", name, begin)};
            }

            for (const auto start : *starts)
            {
                chunked.push_back(begin + start);
            }

            begin = end;
        }

        if (chunked != *byte_starts)
        {
            throw std::runtime_error{std::format("{}: the chunked scans differ from the sequential scan", name)};
        }

        lines.push_back(std::format(
                "{} | split theorem: {} bytes cut at {} anchors, chunked scans byte-identical to the sequential "
                "{}-token scan",
                name, corpus.size(), anchors.size(), byte_starts->size()));

        table_rows.push_back(std::format(
                R"({} & {:.1f} & {} & {} & {} & {} \\)", name.substr(std::string_view{"campaign "}.size()),
                per_kibibyte, percentile(gaps, 0.5), percentile(gaps, 0.9), std::ranges::max(gaps), snap_max(8)));

        if (name == "campaign conventional")
        {
            for (const auto& [window, origin] : certified)
            {
                anchor_table.push_back(std::format("{} {}", window, origin));
            }
        }

        if (name == "campaign conventional" || name == "campaign split-friendly")
        {
            edit_rows.push_back(Measured_row{
                    .name = name,
                    .lexer = lexer,
                    .corpus = corpus,
                    .class_text = class_text,
                    .classify = classify,
                    .sigma = sigma,
                    .certified = certified_bytes_order});
        }

        note(std::format("{}: measured", name));
    }

    lines.push_back(std::format(
            "campaign occurrence | over the four rows: {} certified pairs, {} occurring and {} vacuous",
            certified_total, certified_total - vacuous_total, vacuous_total));

    lines.push_back(std::format(
            "campaign edit protocol | seed {}, rows {} and {}, {} cross-class and {} same-class substitutions per row, "
            "the generator reseeded per row",
            campaign_edit_seed, edit_rows.front().name.substr(std::string_view{"campaign "}.size()),
            edit_rows.back().name.substr(std::string_view{"campaign "}.size()), campaign_cross_class_edits,
            campaign_same_class_edits));

    for (const auto& row : edit_rows)
    {
        campaign_edit_lines(row, lines);

        note(std::format("{}: edits replayed", row.name));
    }

    // The differential auditor on the campaign's design change, and on a self-pair.
    {
        const auto& conventional{edit_rows.front().lexer};

        const auto& split_friendly{edit_rows.back().lexer};

        const auto [witness, exhaustive]{conventional.boundary_difference(split_friendly)};

        if (witness.empty())
        {
            throw std::runtime_error{"the design change must have a differential"};
        }

        std::string letters;

        for (const auto byte : witness)
        {
            letters.push_back(edit_rows.front().classify(static_cast<unsigned char>(byte)));
        }

        const auto shown{[](const std::vector<std::size_t>& starts) {
            std::string text{"["};

            for (std::size_t index{0}; index < starts.size(); ++index)
            {
                text += std::format("{}{}", index == 0 ? "" : ", ", starts[index]);
            }

            return text + "]";
        }};

        lines.push_back(std::format(
                "campaign differential | conventional against split-friendly: witness '{}' of length {}, boundaries {} "
                "against {}",
                letters, witness.size(), shown(*token_starts(conventional, witness)),
                shown(*token_starts(split_friendly, witness))));

        const auto [self_witness, self_exhaustive]{conventional.boundary_difference(conventional)};

        if (self_witness.empty() && self_exhaustive)
        {
            lines.emplace_back(
                    "campaign differential | conventional against itself: proved differential-free over every input");
        }
    }

    std::vector<std::string> table{
            R"(\begin{tabular}{@{}lrrrrr@{}})", R"(\toprule)",
            R"(Campaign row & anchors/KiB & gap p50 & gap p90 & gap max & snap max (8) \\)", R"(\midrule)"};

    table.insert(table.end(), table_rows.begin(), table_rows.end());

    table.insert(table.end(), {R"(\bottomrule)", R"(\end{tabular})"});

    return {Emission{.file = "campaign-splitting-stats.txt", .lines = std::move(lines)},
            Emission{.file = "campaign-anchor-table.txt", .lines = std::move(anchor_table)},
            Emission{.file = "campaign-supply-table.tex", .lines = std::move(table)}};
}

/**
 * @brief The wide cutoff sweep's decisions: over every token set of up to three tokens of up to three letters over
 *        the alphabet ab, every window of length two to four at every origin, decided by window_counterexample(); the
 *        triples, the refuted ones, and the refuted ones whose shortest counterexample attains the cutoff |W| + 2L - 2.
 * @return The emission, the parameter lines as the Python writes them and the three counts it decides.
 */
[[nodiscard]] Emission sweep_emission()
{
    const std::string_view letters{"ab"};

    std::vector<std::string> pool;

    for (std::size_t length{1}; length <= 3; ++length)
    {
        const auto count{static_cast<std::size_t>(1) << length};

        for (std::size_t code{0}; code < count; ++code)
        {
            std::string token;

            for (std::size_t at{0}; at < length; ++at)
            {
                token.push_back(letters[(code >> (length - 1 - at)) & 1U]);
            }

            pool.push_back(token);
        }
    }

    std::vector<std::string> windows;

    for (std::size_t length{2}; length <= 4; ++length)
    {
        const auto count{static_cast<std::size_t>(1) << length};

        for (std::size_t code{0}; code < count; ++code)
        {
            std::string window;

            for (std::size_t at{0}; at < length; ++at)
            {
                window.push_back(letters[(code >> (length - 1 - at)) & 1U]);
            }

            windows.push_back(window);
        }
    }

    std::vector<std::vector<std::string>> universes;

    for (std::size_t size{1}; size <= 3; ++size)
    {
        std::vector<char> take(pool.size(), 0);

        std::fill(take.begin(), take.begin() + static_cast<long>(size), 1);

        do
        {
            std::vector<std::string> tokens;

            for (std::size_t index{0}; index < pool.size(); ++index)
            {
                if (take[index] != 0)
                {
                    tokens.push_back(pool[index]);
                }
            }

            universes.push_back(std::move(tokens));
        } while (std::ranges::prev_permutation(take).found);
    }

    std::atomic<std::size_t> checked{0};

    std::atomic<std::size_t> refuted{0};

    std::atomic<std::size_t> tight{0};

    std::atomic<std::size_t> unsettled{0};

    std::atomic<std::size_t> next{0};

    const auto worker{[&] {
        for (auto index{next.fetch_add(1)}; index < universes.size(); index = next.fetch_add(1))
        {
            const auto& tokens{universes[index]};

            const auto lexer{compile(tokens)};

            const auto longest{std::ranges::max(tokens, {}, &std::string::size).size()};

            for (const auto& window : windows)
            {
                const auto cutoff{window.size() + (2 * longest) - 2};

                for (std::size_t origin{0}; origin < window.size(); ++origin)
                {
                    const auto [witness, exhaustive]{lexer.window_counterexample(window, origin)};

                    ++checked;

                    if (!exhaustive)
                    {
                        ++unsettled;
                    }
                    else if (!witness.empty())
                    {
                        ++refuted;

                        if (witness.size() == cutoff)
                        {
                            ++tight;
                        }
                    }
                }
            }
        }
    }};

    std::vector<std::thread> threads;

    for (unsigned thread{0}; thread < std::max(1U, std::thread::hardware_concurrency()); ++thread)
    {
        threads.emplace_back(worker);
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    std::vector<std::string> lines{
            "  alphabet ab, token length to 3, token-set size to 3, windows every",
            "  string of lengths 2 through 4, every origin, margin 3 above each cutoff",
            "  cutoff under test: N = |W| + 2L - 2, with L the longest token of the set",
            std::format("  triples checked: {}", checked.load()),
            std::format("  triples with a counterexample (refuted): {}", refuted.load()),
            std::format(
                    "  refuted triples whose shortest counterexample attains the cutoff (tight): {}", tight.load())};

    if (unsettled.load() > 0)
    {
        lines.push_back(std::format("  triples unsettled at the cap: {}", unsettled.load()));
    }

    return Emission{.file = "wide-cutoff-sweep.txt", .lines = std::move(lines)};
}

} // namespace

int main(const int argc, const char** argv)
{
    if (argc < 2 || (argc > 2 && argc < 5))
    {
        std::cerr << "usage: munch_paper4_recompute <output directory> [<twitter.json> <gpt2-merges.txt> "
                     "<campaign corpus directory>] [section...]\n";

        return EXIT_FAILURE;
    }

    try
    {
        const std::filesystem::path out{argv[1]};

        std::filesystem::create_directories(out);

        // The manifest is this run's, so it starts empty: a file an earlier run wrote stays on disk and is named
        // by no manifest, which is how the comparison tells the two apart.
        std::ofstream{out / "manifest.txt", std::ios::binary | std::ios::trunc};

        // The sections there are; a name that is none of them runs nothing, which is refused rather than run empty.
        static constexpr std::array<std::string_view, 6> known{"vocabularies", "budget",   "depth",
                                                               "frozen",       "campaign", "sweep"};

        std::set<std::string> sections;

        for (int index{5}; index < argc; ++index)
        {
            if (!std::ranges::contains(known, std::string_view{argv[index]}))
            {
                throw std::runtime_error{
                        std::string{"no section is named "} + argv[index] +
                        "; the sections are vocabularies, budget, depth, frozen, campaign and sweep"};
            }

            sections.emplace(argv[index]);
        }

        const auto wanted{[&sections](const std::string_view section) {
            return sections.empty() || sections.contains(std::string{section});
        }};

        if (argc == 2)
        {
            std::vector<std::string> lines;

            sync_line(utf8_shape(), "utf8-shape", lines);

            write(out, Emission{.file = "splitting-stats.txt", .lines = lines});

            write(out, sweep_emission());

            return EXIT_SUCCESS;
        }

        if (wanted("sweep"))
        {
            write(out, sweep_emission());
        }

        if (wanted("campaign"))
        {
            for (const auto& emission : campaign_emissions(argv[4]))
            {
                write(out, emission);
            }
        }

        const auto slices{wanted("vocabularies") || wanted("budget") || wanted("depth") || wanted("frozen")};

        if (!slices)
        {
            return EXIT_SUCCESS;
        }

        const auto corpus{read_file(argv[2])};

        if (corpus.size() < train_bytes + eval_bytes + calibration_bytes)
        {
            throw std::runtime_error{"the corpus is too short for the training, evaluation and calibration slices"};
        }

        const auto sample{std::string_view{corpus}.substr(train_bytes, eval_bytes)};

        const auto calibration{std::string_view{corpus}.substr(train_bytes + eval_bytes, calibration_bytes)};

        Merge_stats stats;

        const auto merges{train_bpe(std::string_view{corpus}.substr(0, train_bytes), merges_local, stats)};

        note(std::format("{} merges trained", merges.size()));

        std::vector<Vocabulary> vocabularies;

        vocabularies.push_back(
                measure_vocabulary("local-384", "local-384", local_tokens(merges, merges_local), sample));

        vocabularies.push_back(measure_vocabulary("gpt2-4k", "GPT2-prefix-4k", build_gpt2(read_file(argv[3])), sample));

        if (wanted("vocabularies"))
        {
            for (const auto& emission :
                 vocabulary_emissions(vocabularies, stats, local_tokens(merges, depths.front()), sample, calibration))
            {
                write(out, emission);
            }
        }

        if (wanted("budget"))
        {
            write(out, budget_emission(vocabularies, sample));
        }

        if (wanted("depth"))
        {
            write(out, depth_emission(merges, vocabularies.front(), sample));
        }

        if (wanted("frozen"))
        {
            for (const auto& emission : frozen_emissions(vocabularies, sample, calibration))
            {
                write(out, emission);
            }
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& failure)
    {
        std::cerr << "munch_paper4_recompute: " << failure.what() << '\n';

        return EXIT_FAILURE;
    }
}

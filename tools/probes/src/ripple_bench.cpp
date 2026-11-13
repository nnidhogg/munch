// Times verifier::ripple_radius() on prefixes of GPT-2's merge table, with the process's peak resident memory, and
// replays the witnesses of every finite answer on a reference encoder.
//
// The tables. For each prefix asked, the first n merges of the table read from gpt2-merges.txt, each part decoded from
// the file's byte-to-unicode spelling back to bytes, and the same prefix without its merges of equal parts and without
// every merge naming a part only a left-out merge makes. GPT-2's own prefixes have an infinite origin radius from 42
// merges on, the merge at rank 41 being an effective self-pair, which ripple_radius() finds taking the merges in rank
// order, before any graph. The prefixes without merges of equal parts have no effective self-pair, so their finite
// radius and mirrored constant are read off the overlap graph, and they are the tables that time it.
//
// What it checks. For every finite answer, each witness replayed by the encoder that applies the lowest-ranked merge
// present at its leftmost occurrence until none applies: the radius witness's origin ripple equal to the radius, and
// the mirrored witness's mirrored quantity equal to the mirrored constant. A witness not attained exits 1, as does a
// table that cannot be read, a nonempty line after the header that does not hold exactly one space or holds a part it
// cannot decode, a prefix that is not a positive whole number and a prefix longer than the table. A prefix too large
// for an unsigned long long and a table ripple_radius() refuses, among them one with an empty part or a part that is
// neither a byte nor an earlier merge's product, abort the process on the uncaught exception.
//
// What it measures. Per table: its merges, the verdict with the radius and the mirrored constant or the effective
// self-pair's rank, the time of the one call to ripple_radius(), and the process's peak resident memory after it. The
// peak only grows, so a collection runs one prefix per process. Every number printed is run-local: the output carries
// commit and dirty-state provenance, and a figure is quoted with the machine it ran on.
//
// Usage: munch_ripple_bench <gpt2-merges.txt> <prefix>...

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "munch/tools/benchmark/provenance.hpp"
#include "munch/tools/probes/files.hpp"
#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/ripple_radius.hpp"

namespace
{
using munch::verifier::Effective_self_pair;
using munch::verifier::Finite_ripple;
using munch::verifier::Merge;

/**
 * @brief The number of bytes, the tokens before any merge.
 */
constexpr std::size_t byte_count{256};

/**
 * @brief A table timed: its name and its merges in rank order.
 */
struct Named_table
{
    /**
     * @brief The name the table's line is headed with.
     */
    std::string name{};

    /**
     * @brief The merges in rank order.
     */
    std::vector<Merge> merges{};
};

/**
 * @brief The two measures of one prepended input: the origin ripple and the mirrored quantity.
 */
struct Ripple
{
    /**
     * @brief One past the last byte of the input whose covering token changes, zero when none does.
     */
    std::size_t origin{0};

    /**
     * @brief One past the start of the last token of the input's encoding whose span changes, zero when none does.
     */
    std::size_t mirrored{0};
};

/**
 * @brief Returns the byte each code point of GPT-2's byte-to-unicode map stands for: the printable bytes stand for
 *        themselves and the rest are numbered from U+0100 in byte order.
 * @return The bytes, by code point.
 */
std::map<std::uint32_t, char> byte_map()
{
    std::map<std::uint32_t, char> byte_of{};

    std::uint32_t extra{byte_count};

    for (std::uint32_t value{0}; value < byte_count; ++value)
    {
        const auto kept{(value >= '!' && value <= '~') || (value >= 0xA1 && value <= 0xAC) || value >= 0xAE};

        const auto point{kept ? value : extra++};

        byte_of[point] = static_cast<char>(value);
    }

    return byte_of;
}

/**
 * @brief Decodes a part of a merge from its byte-to-unicode spelling, UTF-8 of code points below U+0800, to bytes, a
 *        byte that leads no two-byte sequence read as a code point by itself.
 * @param byte_of The byte each code point stands for.
 * @param field The part as the file spells it.
 * @return The bytes, std::nullopt when a code point is outside the map.
 */
std::optional<std::string> decoded(const std::map<std::uint32_t, char>& byte_of, const std::string_view field)
{
    std::string bytes{};

    for (std::size_t at{0}; at < field.size();)
    {
        std::uint32_t point{static_cast<unsigned char>(field[at])};

        constexpr std::uint32_t two_byte_lead{0xC0};
        constexpr std::uint32_t lead_mask{0xE0};

        const auto paired{(point & lead_mask) == two_byte_lead && at + 1 < field.size()};

        if (paired)
        {
            const std::uint32_t tail{static_cast<unsigned char>(field[at + 1])};

            constexpr std::uint32_t lead_bits{0x1F};
            constexpr std::uint32_t tail_bits{0x3F};
            constexpr std::uint32_t tail_width{6};

            point = ((point & lead_bits) << tail_width) | (tail & tail_bits);
        }

        const auto found{byte_of.find(point)};

        if (found == byte_of.cend())
        {
            return std::nullopt;
        }

        const auto& [code, byte]{*found};

        bytes += byte;
        at += paired ? 2 : 1;
    }

    return bytes;
}

/**
 * @brief Reads a merge table in the format of gpt2-merges.txt: a header line, then one merge per line, its two parts
 *        separated by one space, each decoded to bytes, blank lines skipped.
 * @param text The file's text.
 * @return The merges in rank order, std::nullopt when a nonempty line does not hold exactly one space or holds a part
 *         it cannot decode; an empty part is read as the empty string, which ripple_radius() refuses.
 */
std::optional<std::vector<Merge>> merges_of(const std::string_view text)
{
    const auto byte_of{byte_map()};

    std::vector<Merge> merges{};

    for (const auto line_range : text | std::views::split('\n') | std::views::drop(1))
    {
        const std::string_view line{line_range.begin(), line_range.end()};

        if (line.empty())
        {
            continue;
        }

        const auto space{line.find(' ')};

        if (space == std::string_view::npos || line.find(' ', space + 1) != std::string_view::npos)
        {
            return std::nullopt;
        }

        const auto left{decoded(byte_of, line.substr(0, space))};
        const auto right{decoded(byte_of, line.substr(space + 1))};

        if (!left || !right)
        {
            return std::nullopt;
        }

        merges.push_back({.left = *left, .right = *right});
    }

    return merges;
}

/**
 * @brief Reads a prefix length from an argument that is wholly a positive whole number.
 * @param argument The argument.
 * @return The length, std::nullopt when the argument is not one.
 */
std::optional<std::size_t> prefix_of(const std::string_view argument)
{
    const auto is_digit{[](const char c) { return c >= '0' && c <= '9'; }};

    const auto digits{!argument.empty() && std::ranges::all_of(argument, is_digit)};

    if (!digits)
    {
        return std::nullopt;
    }

    const auto value{std::stoull(std::string{argument})};

    if (value == 0)
    {
        return std::nullopt;
    }

    return value;
}

/**
 * @brief Returns a table without its merges of equal parts and without every merge naming a part that only a left-out
 *        merge makes, so with no effective self-pair.
 * @param merges The merge table in rank order.
 * @return The merges kept, in rank order.
 */
std::vector<Merge> without_equal_parts(const std::vector<Merge>& merges)
{
    std::unordered_set<std::string> carried{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        carried.insert(std::string{static_cast<char>(byte)});
    }

    std::vector<Merge> kept{};

    for (const auto& [left, right] : merges)
    {
        const auto named{carried.contains(left) && carried.contains(right)};

        if (left == right || !named)
        {
            continue;
        }

        carried.insert(left + right);
        kept.push_back({.left = left, .right = right});
    }

    return kept;
}

/**
 * @brief Returns the peak resident memory of the process so far.
 * @return The peak, in mebibytes.
 */
double peak_mebibytes()
{
    rusage usage{};

    std::ignore = getrusage(RUSAGE_SELF, &usage);

    constexpr double kibibytes_per_mebibyte{1024.0};

    return static_cast<double>(usage.ru_maxrss) / kibibytes_per_mebibyte;
}

/**
 * @brief Returns the tokens canonical byte-pair encoding produces for a text: at every step the lowest-ranked merge
 *        present anywhere is applied at its leftmost occurrence.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @return The tokens.
 */
std::vector<std::string> reference_tokens(const std::vector<Merge>& merges, const std::string_view text)
{
    std::vector<std::string> tokens{};

    for (const auto byte : text)
    {
        tokens.push_back(std::string{byte});
    }

    for (auto merged{true}; merged;)
    {
        merged = false;

        for (const auto& [left, right] : merges)
        {
            const auto is_pair{[&](const std::string& first, const std::string& second) {
                return first == left && second == right;
            }};

            const auto at{std::ranges::adjacent_find(tokens, is_pair)};

            if (at == tokens.end())
            {
                continue;
            }

            *at = left + right;
            tokens.erase(at + 1);
            merged = true;

            break;
        }
    }

    return tokens;
}

/**
 * @brief Returns the start and the end of the token covering each byte of an encoding, the first token beginning at an
 *        index.
 * @param tokens The encoding.
 * @param first The index the first token begins at.
 * @return The covering spans, one per byte.
 */
std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>> covering_spans(
        const std::vector<std::string>& tokens, const std::ptrdiff_t first)
{
    std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>> spans{};

    auto begin{first};

    for (const auto& token : tokens)
    {
        const auto end{begin + static_cast<std::ptrdiff_t>(token.size())};

        spans.insert(spans.end(), token.size(), std::pair{begin, end});
        begin = end;
    }

    return spans;
}

/**
 * @brief Returns the origin ripple and the mirrored quantity of a symbol prepended to an input, by the reference
 *        encoder: the input's covering spans against those of the prepended input shifted left by the symbol's length.
 * @param merges The merge table in rank order.
 * @param prepended The prepended input.
 * @return The two measures.
 */
Ripple ripple_of(const std::vector<Merge>& merges, const munch::verifier::Prepended_input& prepended)
{
    const auto& [symbol, input]{prepended};

    const auto shift{static_cast<std::ptrdiff_t>(symbol.size())};

    const auto base_tokens{reference_tokens(merges, input)};

    const auto base{covering_spans(base_tokens, 0)};

    const auto whole_text{symbol + input};

    const auto whole_tokens{reference_tokens(merges, whole_text)};

    const auto whole{covering_spans(whole_tokens, -shift)};

    Ripple ripple{};

    for (std::size_t index{0}; index < input.size(); ++index)
    {
        const auto& [begin, end]{base[index]};

        if (base[index] == whole[symbol.size() + index])
        {
            continue;
        }

        ripple = {.origin = index + 1, .mirrored = static_cast<std::size_t>(begin) + 1};
    }

    return ripple;
}

/**
 * @brief Returns whether the witnesses of a finite answer attain its values under the reference encoder: the radius
 *        witness's origin ripple is the radius and the mirrored witness's mirrored quantity the mirrored constant, a
 *        witness absent exactly when its value is zero.
 * @param merges The merge table in rank order.
 * @param finite The finite answer.
 * @return True when both are attained.
 */
bool is_attained(const std::vector<Merge>& merges, const Finite_ripple& finite)
{
    const auto& [radius, radius_witness, mirrored, mirrored_witness]{finite};

    if (radius_witness.has_value() != (radius != 0) || mirrored_witness.has_value() != (mirrored != 0))
    {
        return false;
    }

    const auto radius_attained{!radius_witness || ripple_of(merges, *radius_witness).origin == radius};

    const auto mirrored_attained{!mirrored_witness || ripple_of(merges, *mirrored_witness).mirrored == mirrored};

    return radius_attained && mirrored_attained;
}

/**
 * @brief Decides one table, replays the witnesses of a finite answer, and prints its line.
 * @param table The table.
 * @return True when the table is decided and a finite answer's witnesses attain its values.
 */
bool time_table(const Named_table& table)
{
    const auto& [name, merges]{table};

    const auto started{std::chrono::steady_clock::now()};

    const auto ripple{munch::verifier::ripple_radius(merges)};

    const auto stopped{std::chrono::steady_clock::now()};

    const auto seconds{std::chrono::duration<double>(stopped - started).count()};

    const auto peak{peak_mebibytes()};

    if (const auto* const self_pair{std::get_if<Effective_self_pair>(&ripple)})
    {
        std::cout << std::format(
                "{}: {} merges, infinite at rank {}, {:.3f} s, peak {:.1f} MiB\n", name, merges.size(), self_pair->rank,
                seconds, peak);

        return true;
    }

    const auto& finite{std::get<Finite_ripple>(ripple)};

    if (!is_attained(merges, finite))
    {
        std::cerr << std::format("munch_ripple_bench: a witness of {} does not attain its value\n", name);

        return false;
    }

    std::cout << std::format(
            "{}: {} merges, radius {}, mirrored {}, {:.3f} s, peak {:.1f} MiB\n", name, merges.size(), finite.radius,
            finite.mirrored, seconds, peak);

    return true;
}

} // namespace

/**
 * @brief Times ripple_radius() on each prefix asked of a merge table and on the prefix without its merges of equal
 *        parts, printing a line per table after the provenance.
 * @param argc The argument count.
 * @param argv The table file, then the prefix lengths.
 * @return EXIT_SUCCESS, or EXIT_FAILURE when an argument or the table is refused or a witness is not attained.
 */
int main(const int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: munch_ripple_bench <gpt2-merges.txt> <prefix>...\n";

        return EXIT_FAILURE;
    }

    const std::string path{argv[1]};

    const auto text{munch::tools::probes::read_bytes(path)};

    if (!text)
    {
        std::cerr << std::format("munch_ripple_bench: cannot read the table {}\n", path);

        return EXIT_FAILURE;
    }

    const auto merges{merges_of(*text)};

    if (!merges)
    {
        std::cerr << std::format("munch_ripple_bench: {} holds a line that is not a merge\n", path);

        return EXIT_FAILURE;
    }

    const auto dirty_note{munch::tools::benchmark::dirty ? " (uncommitted changes present)" : ""};

    std::cout << "munch ripple bench\n";
    std::cout << std::format("  commit      {}{}\n", munch::tools::benchmark::commit, dirty_note);
    std::cout << std::format("  table       {} merges\n", merges->size());

    const std::span arguments{argv + 2, static_cast<std::size_t>(argc - 2)};

    for (const std::string_view argument : arguments)
    {
        const auto prefix{prefix_of(argument)};

        if (!prefix || *prefix > merges->size())
        {
            std::cerr << std::format("munch_ripple_bench: {} is not a prefix of the table\n", argument);

            return EXIT_FAILURE;
        }

        const std::vector first(merges->begin(), merges->begin() + static_cast<std::ptrdiff_t>(*prefix));

        const Named_table own{.name = std::format("first {}", *prefix), .merges = first};

        const Named_table finite{
                .name = std::format("first {} without equal parts", *prefix),
                .merges = without_equal_parts(first)};

        if (!time_table(own) || !time_table(finite))
        {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

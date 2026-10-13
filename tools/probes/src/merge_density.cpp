// Prints whether the certified windows of byte-pair tables are dense, by verifier::window_density(), beside the
// pinned-byte guarantee of a finite origin radius.
//
// The tables. The merge (a, b), whose right start radius is zero, so every window of width R + 1 is certified; the
// merges (a, b), (a, ab), under which no factor of a run of a's is certified at its last byte, and (a, b), (ab, c),
// (ab, abc), under which no factor of repeated ab is, both of finite radius; the chain (c, d), (b, c), (a, b), dense
// with a start radius of two; and GPT-2's first 7, 20, 41 and 64 merges, read from the token automaton fixture, the
// first three of radius 3 and dense, the last of infinite radius. Per table: the merges, the origin radius, the right
// start radius, the width R + Q_start + 1 at which every window has a cut with the bounds of the cuts' density bound,
// and the verdict on the windows of width at most R + 1 certified at their last byte, dense with the density bound or
// not dense with the loop every power of which avoids them and the pumping double when one shows it. A spelling is
// quoted, a byte outside printable ASCII written `\xHH` and the quote and the backslash escaped. Every line is
// "name: value" and stable. Given a directory, the probe writes the lines to merge_density.txt there in place of the
// standard output, and the test compares the file written into the build tree with data/merge_density.txt byte for
// byte.
//
// Usage: munch_merge_density [directory]

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/ripple_radius.hpp"
#include "munch/verifier/window_density.hpp"

namespace
{
using munch::verifier::Dense_windows;
using munch::verifier::Density_budget;
using munch::verifier::Effective_self_pair;
using munch::verifier::Merge;
using munch::verifier::Pinned_density;
using munch::verifier::Sparse_windows;

/**
 * @brief The fixture the GPT-2 table is read from, and the universe line naming it.
 */
constexpr std::string_view table_fixture{"/libs/verifier/tests/data/token_automaton_reference.txt"};

/**
 * @brief The prefix of the fixture line holding the 64 merges of GPT-2, each a left and a right part in hexadecimal.
 */
constexpr std::string_view table_line{"universe gpt2-64 tower "};

/**
 * @brief The prefixes of GPT-2's table printed, by their number of merges.
 */
constexpr std::array<std::size_t, 4> gpt2_prefixes{7, 20, 41, 64};

/**
 * @brief A table printed: its name and its merges in rank order.
 */
struct Named_table
{
    /**
     * @brief The name the table's lines are headed with.
     */
    std::string name{};

    /**
     * @brief The merges in rank order.
     */
    std::vector<Merge> merges{};
};

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
 * @brief Returns a spelling quoted: a printable ASCII byte as itself, the quote and the backslash escaped, and every
 *        other byte as `\xHH`.
 * @param spelling The spelling.
 * @return The spelling, quoted.
 */
std::string quoted_spelling(const std::string_view spelling)
{
    constexpr unsigned char first_printable{0x20};
    constexpr unsigned char last_printable{0x7E};

    std::string out{'"'};

    for (const auto byte : spelling)
    {
        const auto value{static_cast<unsigned char>(byte)};

        const auto printable{value >= first_printable && value <= last_printable};

        if (byte == '"' || byte == '\\')
        {
            out += std::format(R"(\{})", byte);
        }
        else if (printable)
        {
            out += byte;
        }
        else
        {
            out += std::format(R"(\x{:02X})", value);
        }
    }

    out += '"';

    return out;
}

/**
 * @brief Returns the tables printed, the small ones named by their merges and GPT-2's prefixes by their size.
 * @param gpt2 The 64 merges of GPT-2 in rank order.
 * @return The tables, in the order printed.
 */
std::vector<Named_table> tables_of(const std::vector<Merge>& gpt2)
{
    std::vector<Named_table> tables{
            {.name = "(a, b)", .merges = {{.left = "a", .right = "b"}}},
            {.name = "(a, b), (a, ab)", .merges = {{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}}},
            {.name = "(a, b), (ab, c), (ab, abc)",
             .merges = {{.left = "a", .right = "b"}, {.left = "ab", .right = "c"}, {.left = "ab", .right = "abc"}}},
            {.name = "(c, d), (b, c), (a, b)",
             .merges = {{.left = "c", .right = "d"}, {.left = "b", .right = "c"}, {.left = "a", .right = "b"}}}};

    for (const auto count : gpt2_prefixes)
    {
        const auto end{gpt2.begin() + static_cast<std::ptrdiff_t>(count)};

        std::vector prefix(gpt2.begin(), end);

        tables.push_back({.name = std::format("GPT-2's first {} merges", count), .merges = std::move(prefix)});
    }

    return tables;
}

/**
 * @brief Prints the pinned-byte guarantee of a finite origin radius: the radii and the width at which every window has
 *        a cut, with the bounds of the cuts' density bound.
 * @param out The stream printed to.
 * @param pinned The pinned-byte guarantee.
 */
void print_pinned(std::ostream& out, const Pinned_density& pinned)
{
    const auto& [radius, start_radius, width]{pinned};

    out << std::format("origin radius: {}", radius) << '\n';

    out << std::format("right start radius: {}", start_radius) << '\n';

    out << std::format(
                   "cuts: dense, every window of width {} having one, density bound from {} to {}", width, radius + 1,
                   width)
        << '\n';
}

/**
 * @brief Prints the decision on one table, headed by its name and its number of merges.
 * @param out The stream printed to.
 * @param table The table.
 */
void print_density(std::ostream& out, const Named_table& table)
{
    const auto& [name, merges]{table};

    out << std::format("table: {}", name) << '\n';

    out << std::format("merges: {}", merges.size()) << '\n';

    const auto density{munch::verifier::window_density(merges)};

    if (std::holds_alternative<Effective_self_pair>(density))
    {
        const auto [rank]{std::get<Effective_self_pair>(density)};

        out << std::format("origin radius: infinite, the effective self-pair at rank {}", rank) << '\n';

        out << "cuts: not dense at any width" << '\n';

        out << "last byte: not dense at any width" << '\n';

        return;
    }

    if (std::holds_alternative<Density_budget>(density))
    {
        const auto& [pinned]{std::get<Density_budget>(density)};

        print_pinned(out, pinned);

        out << "last byte: undecided, the budget of avoiding strings spent" << '\n';

        return;
    }

    if (std::holds_alternative<Dense_windows>(density))
    {
        const auto& [pinned, bound]{std::get<Dense_windows>(density)};

        print_pinned(out, pinned);

        out << std::format("last byte: dense at width at most {}, density bound {}", pinned.radius + 1, bound) << '\n';

        return;
    }

    const auto& [pinned, loop, pumping_double]{std::get<Sparse_windows>(density)};

    print_pinned(out, pinned);

    out << std::format(
                   "last byte: not dense at any width, every power of {} avoiding every certified window",
                   quoted_spelling(loop))
        << '\n';

    if (pumping_double)
    {
        const auto& [token, symbol]{*pumping_double};

        out << std::format("pumping double: {} with {}", quoted_spelling(token), quoted_spelling(symbol)) << '\n';
    }
}

} // namespace

/**
 * @brief Prints the density decision of every table, a blank line between tables, to the standard output or to
 *        merge_density.txt in a directory.
 * @param argc The argument count.
 * @param argv The directory the file is written to, the standard output when none is given.
 * @return EXIT_SUCCESS, or EXIT_FAILURE when the GPT-2 table cannot be read or the file cannot be written.
 */
int main(const int argc, char** argv)
{
    const auto gpt2{gpt2_merges()};

    if (!gpt2)
    {
        std::cerr << std::format(
                             "munch_merge_density: cannot read the GPT-2 table from {}{}", SOURCE_DIR, table_fixture)
                  << '\n';

        return EXIT_FAILURE;
    }

    const auto tables{tables_of(*gpt2)};

    std::ostringstream out{};

    for (std::size_t at{0}; at < tables.size(); ++at)
    {
        if (at != 0)
        {
            out << '\n';
        }

        print_density(out, tables[at]);
    }

    if (argc < 2)
    {
        std::cout << out.str();

        return EXIT_SUCCESS;
    }

    const auto path{std::filesystem::path{argv[1]} / "merge_density.txt"};

    std::ofstream file{path, std::ios::binary};

    file << out.str();

    if (!file)
    {
        std::cerr << std::format("munch_merge_density: cannot write {}", path.string()) << '\n';

        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "munch/verifier/window_density.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/ripple_radius.hpp"
#include "munch/verifier/verifier_decisions.hpp"

using namespace munch::verifier;

namespace
{
/**
 * @brief The fixture the GPT-2 table is read from.
 */
constexpr std::string_view gpt2_fixture{"/libs/verifier/tests/data/token_automaton_reference.txt"};

/**
 * @brief The prefix of the fixture line holding the 64 merges of GPT-2, each a left and a right part in hex.
 */
constexpr std::string_view gpt2_line{"universe gpt2-64 tower "};

/**
 * @brief The longest string the brute force enumerates, past which an avoiding string counts as not dense.
 */
constexpr std::size_t longest_enumerated{12};

/**
 * @brief What the reference fixture holds, counted as it is checked.
 */
struct Fixture_counts
{
    /**
     * @brief The universe lines.
     */
    std::size_t universes{0};

    /**
     * @brief The infinite lines.
     */
    std::size_t infinite{0};

    /**
     * @brief The dense lines.
     */
    std::size_t dense{0};

    /**
     * @brief The sparse lines.
     */
    std::size_t sparse{0};

    /**
     * @brief The double lines.
     */
    std::size_t doubles{0};

    /**
     * @brief The dense lines with a right start radius of zero.
     */
    std::size_t zero_start{0};
};

/**
 * @brief The reference fixture under check: the current universe's table and line, the pumping double its decision
 *        named and whether a double line has matched it, and the counts so far.
 */
struct Fixture_check
{
    /**
     * @brief The current universe's merge table in rank order.
     */
    std::vector<Merge> merges{};

    /**
     * @brief The current universe's line, for the failure messages.
     */
    std::string universe{};

    /**
     * @brief The pumping double the current universe's decision named, none when it named none.
     */
    std::optional<Pumping_double> pumping_double{};

    /**
     * @brief Whether a double line has matched the named pumping double.
     */
    bool matched{false};

    /**
     * @brief The lines checked so far, by kind.
     */
    Fixture_counts counts{};
};

/**
 * @brief Returns the token sequence canonical byte-pair encoding produces for a text: at every step the lowest-rank
 *        merge present anywhere is applied at its leftmost occurrence.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @return The tokens.
 */
std::vector<std::string> reference_tokens(const std::vector<Merge>& merges, const std::string_view text)
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
 * @brief Returns the start of the token covering a byte of a text under the reference encoder.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @param byte The byte's index.
 * @return The index at which the covering token begins.
 */
std::size_t start_of(const std::vector<Merge>& merges, const std::string_view text, const std::size_t byte)
{
    std::size_t begin{0};

    for (const auto& token : reference_tokens(merges, text))
    {
        if (byte < begin + token.size())
        {
            return begin;
        }

        begin += token.size();
    }

    return begin;
}

/**
 * @brief Returns the bytes a table's merges name, ascending, each once.
 * @param merges The merge table in rank order.
 * @return The bytes, as one-byte spellings.
 */
std::vector<std::string> bytes_named(const std::vector<Merge>& merges)
{
    std::set<unsigned char> named{};

    for (const auto& [left, right] : merges)
    {
        named.insert(left.begin(), left.end());
        named.insert(right.begin(), right.end());
    }

    std::vector<std::string> bytes{};

    for (const auto byte : named)
    {
        const std::string spelling{static_cast<char>(byte)};

        bytes.push_back(spelling);
    }

    return bytes;
}

/**
 * @brief Returns the longest token a table can produce: its longest product, or one byte with no merge.
 * @param merges The merge table in rank order.
 * @return The longest token's length.
 */
std::size_t longest_token(const std::vector<Merge>& merges)
{
    std::size_t longest{1};

    for (const auto& [left, right] : merges)
    {
        longest = std::max(longest, left.size() + right.size());
    }

    return longest;
}

/**
 * @brief Returns every string over some bytes shorter than a length, the empty string first.
 * @param bytes The bytes, as one-byte spellings.
 * @param length The bound on the strings' length.
 * @return The strings, shortest first.
 */
std::vector<std::string> strings_below(const std::vector<std::string>& bytes, const std::size_t length)
{
    std::vector<std::string> strings{std::string{}};

    std::vector<std::string> layer{std::string{}};

    for (std::size_t size{1}; size < length; ++size)
    {
        std::vector<std::string> longer{};

        for (const auto& text : layer)
        {
            for (const auto& byte : bytes)
            {
                longer.push_back(text + byte);
            }
        }

        strings.insert(strings.end(), longer.begin(), longer.end());

        layer = std::move(longer);
    }

    return strings;
}

/**
 * @brief The brute force of a table's window certificates: a window is certified when the token covering its last byte
 *        begins at one offset of it in every context of fewer than L bytes on either side, L the longest token, which
 *        is exact since a refutation overhangs the window by fewer than L bytes on either side.
 */
class Brute_force
{
public:
    /**
     * @brief Takes the table and enumerates the contexts over the bytes it names.
     * @param merges The merge table in rank order.
     */
    explicit Brute_force(std::vector<Merge> merges)
        : merges_{std::move(merges)}
        , bytes_{bytes_named(merges_)}
        , contexts_{strings_below(bytes_, longest_token(merges_))}
    {}

    /**
     * @brief Returns the bytes the table names.
     * @return The bytes, as one-byte spellings, ascending.
     */
    [[nodiscard]] const std::vector<std::string>& bytes() const noexcept { return bytes_; }

    /**
     * @brief Returns whether a window is certified at some origin.
     * @param window The window, nonempty.
     * @return True when the covering token of its last byte begins at one offset of it in every context.
     */
    [[nodiscard]] bool is_certified(const std::string& window)
    {
        if (const auto found{certified_.find(window)}; found != certified_.end())
        {
            const auto& [known, certified]{*found};

            return certified;
        }

        std::set<std::ptrdiff_t> offsets{};

        for (const auto& before : contexts_)
        {
            for (const auto& after : contexts_)
            {
                const auto text{before + window + after};

                const auto start{start_of(merges_, text, before.size() + window.size() - 1)};

                offsets.insert(static_cast<std::ptrdiff_t>(start) - static_cast<std::ptrdiff_t>(before.size()));
            }
        }

        const auto certified{offsets.size() == 1 && *offsets.begin() >= 0};

        certified_.emplace(window, certified);

        return certified;
    }

private:
    /**
     * @brief The merge table in rank order.
     */
    std::vector<Merge> merges_{};

    /**
     * @brief The bytes the table names, as one-byte spellings.
     */
    std::vector<std::string> bytes_{};

    /**
     * @brief Every string over those bytes shorter than the longest token.
     */
    std::vector<std::string> contexts_{};

    /**
     * @brief Each window decided, with whether it is certified.
     */
    std::map<std::string, bool> certified_{};
};

/**
 * @brief Returns the density bound of the certified windows of width at most R + 1 by enumeration, the strings over the
 *        named bytes that avoid them extended a byte at a time, or std::nullopt when avoiding strings of the longest
 *        length enumerated remain.
 * @param brute The brute force of the table.
 * @param radius The origin radius R.
 * @return The density bound, or std::nullopt.
 */
std::optional<std::size_t> enumerated_bound(Brute_force& brute, const std::size_t radius)
{
    const auto avoids{[&](const std::string& text) {
        const auto widest{std::min(radius + 1, text.size())};

        for (std::size_t width{1}; width <= widest; ++width)
        {
            if (brute.is_certified(text.substr(text.size() - width)))
            {
                return false;
            }
        }

        return true;
    }};

    std::vector<std::string> layer{std::string{}};

    for (std::size_t length{1}; length <= longest_enumerated; ++length)
    {
        std::vector<std::string> longer{};

        for (const auto& text : layer)
        {
            for (const auto& byte : brute.bytes())
            {
                if (avoids(text + byte))
                {
                    longer.push_back(text + byte);
                }
            }
        }

        if (longer.empty())
        {
            return length;
        }

        layer = std::move(longer);
    }

    return std::nullopt;
}

/**
 * @brief Returns a power of a loop at least a length past a width, so that it holds every factor of the loop's infinite
 *        power that is at most that width.
 * @param loop The loop, nonempty.
 * @param width The width.
 * @return The power.
 */
std::string power_of(const std::string& loop, const std::size_t width)
{
    std::string text{loop};

    while (text.size() < 2 * width + loop.size())
    {
        text += loop;
    }

    return text;
}

/**
 * @brief Checks that no factor of width at most R + 1 of a power of a loop is certified, each window decided by
 *        miscovering() on the table's merge tower at the start its last byte has in its own encoding.
 * @param merges The merge table in rank order.
 * @param radius The origin radius R.
 * @param loop The loop, nonempty.
 * @param context The fixture line or test checked.
 */
void expect_avoiding(
        const std::vector<Merge>& merges, const std::size_t radius, const std::string& loop, const std::string& context)
{
    ASSERT_FALSE(loop.empty()) << context;

    const auto tower{merge_tower(merges)};

    const auto text{power_of(loop, radius + 1)};

    std::set<std::string> windows{};

    for (std::size_t width{1}; width <= radius + 1; ++width)
    {
        for (std::size_t at{0}; at + width <= text.size(); ++at)
        {
            windows.insert(text.substr(at, width));
        }
    }

    for (const auto& window : windows)
    {
        const auto origin{start_of(merges, window, window.size() - 1)};

        EXPECT_TRUE(miscovering(tower, window, origin)) << context << " certifies " << window;
    }
}

/**
 * @brief Checks a pumping double by the reference encoder: its token encodes as itself and the token twice as two
 *        copies of it, the token twice and the byte begin with another token, and every power of the token avoids every
 *        certified window.
 * @param merges The merge table in rank order.
 * @param radius The origin radius R.
 * @param pumping_double The pumping double.
 * @param context The fixture line or test checked.
 */
void expect_pumping(
        const std::vector<Merge>& merges, const std::size_t radius, const Pumping_double& pumping_double,
        const std::string& context)
{
    const auto& [token, symbol]{pumping_double};

    const std::vector alone{token};
    const std::vector twice{token, token};

    EXPECT_EQ(reference_tokens(merges, token), alone) << context;
    EXPECT_EQ(reference_tokens(merges, token + token), twice) << context;
    EXPECT_NE(reference_tokens(merges, token + token + symbol).front(), token) << context;

    expect_avoiding(merges, radius, token, context);
}

/**
 * @brief Checks the pinned-byte guarantee against ripple_radius() and the reference fields: the radius, the start
 *        radius and the width R + Q_start + 1.
 * @param merges The merge table in rank order.
 * @param pinned The pinned-byte guarantee.
 * @param radius The reference origin radius R.
 * @param start_radius The reference right start radius.
 * @param context The fixture line or test checked.
 */
void expect_pinned(
        const std::vector<Merge>& merges, const Pinned_density& pinned, const std::size_t radius,
        const std::size_t start_radius, const std::string& context)
{
    const auto ripple{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(ripple)) << context;

    EXPECT_EQ(std::get<Finite_ripple>(ripple).radius, radius) << context;
    EXPECT_EQ(pinned.radius, radius) << context;
    EXPECT_EQ(pinned.start_radius, start_radius) << context;
    EXPECT_EQ(pinned.width, radius + start_radius + 1) << context;
}

/**
 * @brief Returns the message window_density() refuses a table with, or the empty text when it takes the table.
 * @param merges The merge table in rank order.
 * @return The refusal.
 */
std::string refusal_of(const std::vector<Merge>& merges)
{
    try
    {
        std::ignore = window_density(merges);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the bytes a lowercase hex field spells, two digits per byte.
 * @param field The hex digits.
 * @return The bytes.
 */
std::string bytes_of(const std::string_view field)
{
    std::string bytes{};

    for (std::size_t at{0}; at + 1 < field.size(); at += 2)
    {
        unsigned value{};

        std::ignore = std::from_chars(field.data() + at, field.data() + at + 2, value, 16);

        bytes += static_cast<char>(value);
    }

    return bytes;
}

/**
 * @brief Reads the 64 merges of GPT-2 from the token automaton fixture, each part written in hex.
 * @return The merges in rank order, empty when the fixture or its line is not there.
 */
std::vector<Merge> gpt2_merges()
{
    std::ifstream file{std::string{SOURCE_DIR} + std::string{gpt2_fixture}};

    std::vector<Merge> merges{};

    for (std::string line{}; std::getline(file, line);)
    {
        if (!line.starts_with(gpt2_line))
        {
            continue;
        }

        std::istringstream fields{line.substr(gpt2_line.size())};

        for (std::string merge{}; fields >> merge;)
        {
            const std::string_view field{merge};

            const auto comma{field.find(',')};

            const auto left{bytes_of(field.substr(0, comma))};

            const auto right{bytes_of(field.substr(comma + 1))};

            merges.push_back({.left = left, .right = right});
        }
    }

    return merges;
}

/**
 * @brief Checks that a pumping double the last universe's decision named was matched by a double line.
 * @param check The fixture under check.
 */
void expect_double_matched(const Fixture_check& check)
{
    EXPECT_TRUE(!check.pumping_double || check.matched) << check.universe << " names a double the fixture lacks";
}

/**
 * @brief Starts a universe line's universe: reads its merges, each a left and a right part joined by a comma.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_universe(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    expect_double_matched(check);

    std::vector<Merge> merges{};

    for (std::string merge{}; fields >> merge;)
    {
        const auto comma{merge.find(',')};

        ASSERT_NE(comma, std::string::npos) << context;

        merges.push_back({.left = merge.substr(0, comma), .right = merge.substr(comma + 1)});
    }

    ++check.counts.universes;
    check.universe = context;
    check.merges = std::move(merges);
    check.pumping_double = std::nullopt;
    check.matched = false;
}

/**
 * @brief Checks an infinite line: the decision returns the effective self-pair at the reference rank.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_infinite(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t rank{};

    fields >> rank;

    const auto found{window_density(check.merges)};

    ++check.counts.infinite;

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found)) << context;

    EXPECT_EQ(std::get<Effective_self_pair>(found).rank, rank) << context;
}

/**
 * @brief Checks a dense line: the decision returns dense windows with the reference bound and the pinned-byte
 *        guarantee, the bound at least R + 1 and equal to it when the start radius is zero.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_dense(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t radius{};
    std::size_t start_radius{};
    std::size_t bound{};

    fields >> radius >> start_radius >> bound;

    const auto found{window_density(check.merges)};

    ++check.counts.dense;

    if (start_radius == 0)
    {
        ++check.counts.zero_start;

        EXPECT_EQ(bound, radius + 1) << context;
    }

    ASSERT_TRUE(std::holds_alternative<Dense_windows>(found)) << context;

    const auto& [pinned, found_bound]{std::get<Dense_windows>(found)};

    EXPECT_EQ(found_bound, bound) << context;
    EXPECT_GE(found_bound, radius + 1) << context;

    expect_pinned(check.merges, pinned, radius, start_radius, context);
}

/**
 * @brief Checks a sparse line: the decision returns sparse windows with the reference loop and the pinned-byte
 *        guarantee, the loop's powers avoiding every certified window, and keeps the pumping double it names for the
 *        double line.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_sparse(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t radius{};
    std::size_t start_radius{};
    std::string loop{};

    fields >> radius >> start_radius >> loop;

    const auto found{window_density(check.merges)};

    ++check.counts.sparse;

    ASSERT_TRUE(std::holds_alternative<Sparse_windows>(found)) << context;

    const auto& [pinned, found_loop, pumping_double]{std::get<Sparse_windows>(found)};

    EXPECT_EQ(found_loop, loop) << context;

    expect_pinned(check.merges, pinned, radius, start_radius, context);

    expect_avoiding(check.merges, radius, found_loop, context);

    check.pumping_double = pumping_double;

    if (pumping_double)
    {
        expect_pumping(check.merges, radius, *pumping_double, context);
    }
}

/**
 * @brief Checks a double line: the decision of the universe named the reference pumping double.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_double(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::string token{};
    std::string symbol{};

    fields >> token >> symbol;

    ++check.counts.doubles;

    ASSERT_TRUE(check.pumping_double) << context;

    EXPECT_EQ(check.pumping_double->token, token) << context;
    EXPECT_EQ(check.pumping_double->symbol, symbol) << context;

    check.matched = true;
}

/**
 * @brief Checks one fixture line against the decision, a comment or blank line checking nothing.
 * @param check The fixture under check.
 * @param line The line.
 */
void expect_fixture_line(Fixture_check& check, const std::string& line)
{
    std::istringstream fields{line};

    std::string kind{};

    if (!(fields >> kind) || kind.starts_with('#'))
    {
        return;
    }

    const auto context{std::format("{} | {}", check.universe, line)};

    if (kind == "universe")
    {
        start_universe(check, fields, line);
    }
    else if (kind == "infinite")
    {
        expect_infinite(check, fields, context);
    }
    else if (kind == "dense")
    {
        expect_dense(check, fields, context);
    }
    else if (kind == "sparse")
    {
        expect_sparse(check, fields, context);
    }
    else if (kind == "double")
    {
        expect_double(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

/**
 * @brief Adds every admissible table that extends a table by merges over its words, up to a number of merges, each part
 *        a word and no product a word already.
 * @param tables The tables, each extension appended.
 * @param merges The table extended.
 * @param words The bytes and the table's products.
 * @param count The most merges a table holds.
 */
void grow(
        std::vector<std::vector<Merge>>& tables, const std::vector<Merge>& merges,
        const std::vector<std::string>& words, const std::size_t count)
{
    if (merges.size() == count)
    {
        return;
    }

    for (const auto& left : words)
    {
        for (const auto& right : words)
        {
            const auto product{left + right};

            if (std::ranges::find(words, product) != words.end())
            {
                continue;
            }

            auto longer{merges};

            longer.push_back({.left = left, .right = right});

            auto more_words{words};

            more_words.push_back(product);

            tables.push_back(longer);

            grow(tables, longer, more_words, count);
        }
    }
}

/**
 * @brief Returns every admissible table of one to three merges over a and b, each part a byte or an earlier product.
 * @return The tables.
 */
std::vector<std::vector<Merge>> tiny_tables()
{
    constexpr std::size_t most_merges{3};

    std::vector<std::vector<Merge>> tables{};

    grow(tables, {}, {"a", "b"}, most_merges);

    return tables;
}

} // namespace

TEST(Window_density_test, The_empty_table_is_dense_with_bound_one_and_every_window_of_width_one_pinned)
{
    const auto found{window_density({})};

    ASSERT_TRUE(std::holds_alternative<Dense_windows>(found));

    const auto& [pinned, bound]{std::get<Dense_windows>(found)};

    EXPECT_EQ(bound, 1U);
    EXPECT_EQ(pinned.radius, 0U);
    EXPECT_EQ(pinned.start_radius, 0U);
    EXPECT_EQ(pinned.width, 1U);
}

TEST(Window_density_test, One_merge_has_every_window_of_width_two_certified_and_the_bound_at_the_radius_plus_one)
{
    // With Q_start = 0 every window of width R + 1 is certified, so the bound is R + 1.
    const std::vector<Merge> merges{{.left = "a", .right = "b"}};

    const auto found{window_density(merges)};

    ASSERT_TRUE(std::holds_alternative<Dense_windows>(found));

    const auto& [pinned, bound]{std::get<Dense_windows>(found)};

    EXPECT_EQ(bound, 2U);

    expect_pinned(merges, pinned, 1, 0, "(a, b)");
}

TEST(Window_density_test, Runs_of_a_avoid_every_certified_window_under_a_b_and_a_ab_and_the_double_of_a_shows_it)
{
    // Over (a, b), (a, ab) a run of a's is single bytes and ends in aab before a b, so no factor of it is certified.
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}};

    const auto found{window_density(merges)};

    ASSERT_TRUE(std::holds_alternative<Sparse_windows>(found));

    const auto& [pinned, loop, pumping_double]{std::get<Sparse_windows>(found)};

    EXPECT_EQ(loop, "a");

    ASSERT_TRUE(pumping_double);

    EXPECT_EQ(pumping_double->token, "a");
    EXPECT_EQ(pumping_double->symbol, "b");

    expect_pinned(merges, pinned, 2, 1, "(a, b), (a, ab)");

    expect_avoiding(merges, 2, loop, "(a, b), (a, ab)");

    expect_pumping(merges, 2, *pumping_double, "(a, b), (a, ab)");
}

TEST(Window_density_test, Repeated_ab_avoids_every_certified_window_under_the_three_merges_ending_in_ab_abc)
{
    const std::vector<Merge> merges{
            {.left = "a", .right = "b"},
            {.left = "ab", .right = "c"},
            {.left = "ab", .right = "abc"}};

    const auto found{window_density(merges)};

    ASSERT_TRUE(std::holds_alternative<Sparse_windows>(found));

    const auto& [pinned, loop, pumping_double]{std::get<Sparse_windows>(found)};

    EXPECT_EQ(loop, "ab");

    expect_pinned(merges, pinned, 4, 2, "(a, b), (ab, c), (ab, abc)");

    expect_avoiding(merges, 4, loop, "(a, b), (ab, c), (ab, abc)");

    if (pumping_double)
    {
        expect_pumping(merges, 4, *pumping_double, "(a, b), (ab, c), (ab, abc)");
    }
}

TEST(Window_density_test, The_chain_of_three_merges_is_dense_although_its_start_radius_is_two)
{
    const std::vector<Merge> merges{
            {.left = "c", .right = "d"},
            {.left = "b", .right = "c"},
            {.left = "a", .right = "b"}};

    const auto found{window_density(merges)};

    ASSERT_TRUE(std::holds_alternative<Dense_windows>(found));

    const auto& [pinned, bound]{std::get<Dense_windows>(found)};

    EXPECT_EQ(bound, 3U);

    expect_pinned(merges, pinned, 1, 2, "the chain of three merges");
}

TEST(Window_density_test, An_effective_self_pair_makes_the_windows_sparse_at_its_own_rank)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}};

    const auto found{window_density(merges)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));

    EXPECT_EQ(std::get<Effective_self_pair>(found).rank, 1U);
}

TEST(Window_density_test, A_budget_of_zero_reports_the_budget_spent_with_the_pinned_byte_guarantee)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}};

    const auto found{window_density(merges, 0)};

    ASSERT_TRUE(std::holds_alternative<Density_budget>(found));

    expect_pinned(merges, std::get<Density_budget>(found).pinned, 2, 1, "(a, b), (a, ab) with no budget");
}

TEST(Window_density_test, A_table_of_zero_start_radius_is_dense_at_the_radius_plus_one_even_with_a_budget_of_zero)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}};

    const auto found{window_density(merges, 0)};

    ASSERT_TRUE(std::holds_alternative<Dense_windows>(found));

    const auto& [pinned, bound]{std::get<Dense_windows>(found)};

    EXPECT_EQ(bound, 2U);

    expect_pinned(merges, pinned, 1, 0, "(a, b) with no budget");
}

TEST(Window_density_test, The_prefixes_of_GPT_2_of_finite_radius_are_dense_with_the_bounds_of_the_density_note)
{
    const auto merges{gpt2_merges()};

    ASSERT_EQ(merges.size(), 64U);

    // The prefixes of 7, 20 and 41 merges have R = 3, Q_start 1, 1 and 2, and N - 1 = 3, 3 and 4.
    const std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> prefixes{{7, 1, 4}, {20, 1, 4}, {41, 2, 5}};

    for (const auto& [count, start_radius, bound] : prefixes)
    {
        const std::vector prefix(merges.begin(), merges.begin() + static_cast<std::ptrdiff_t>(count));

        const auto found{window_density(prefix)};

        const auto context{std::format("GPT-2's first {} merges", count)};

        ASSERT_TRUE(std::holds_alternative<Dense_windows>(found)) << context;

        const auto& [pinned, found_bound]{std::get<Dense_windows>(found)};

        EXPECT_EQ(found_bound, bound) << context;

        expect_pinned(prefix, pinned, 3, start_radius, context);

        // The avoiding strings number at most a hundred.
        EXPECT_TRUE(std::holds_alternative<Dense_windows>(window_density(prefix, 100))) << context;
    }

    EXPECT_TRUE(std::holds_alternative<Effective_self_pair>(window_density(merges)));
}

TEST(Window_density_test, Every_table_of_one_to_three_merges_over_a_and_b_has_the_verdict_of_the_brute_force)
{
    std::size_t dense{0};
    std::size_t sparse{0};

    for (const auto& merges : tiny_tables())
    {
        const auto found{window_density(merges)};

        std::string context{};

        for (const auto& [left, right] : merges)
        {
            context += std::format("({}, {}) ", left, right);
        }

        if (std::holds_alternative<Effective_self_pair>(found))
        {
            EXPECT_TRUE(std::holds_alternative<Effective_self_pair>(ripple_radius(merges))) << context;

            continue;
        }

        const auto radius{std::get<Finite_ripple>(ripple_radius(merges)).radius};

        Brute_force brute{merges};

        const auto bound{enumerated_bound(brute, radius)};

        if (bound)
        {
            ASSERT_TRUE(std::holds_alternative<Dense_windows>(found)) << context;

            EXPECT_EQ(std::get<Dense_windows>(found).bound, *bound) << context;

            ++dense;

            continue;
        }

        ASSERT_TRUE(std::holds_alternative<Sparse_windows>(found)) << context;

        const auto& loop{std::get<Sparse_windows>(found).loop};

        const auto text{power_of(loop, radius + 1)};

        for (std::size_t width{1}; width <= radius + 1; ++width)
        {
            for (std::size_t at{0}; at + width <= text.size(); ++at)
            {
                EXPECT_FALSE(brute.is_certified(text.substr(at, width))) << context << " at " << at;
            }
        }

        ++sparse;
    }

    EXPECT_EQ(dense, 70U);
    EXPECT_EQ(sparse, 44U);
}

TEST(Window_density_test, Refuses_a_merge_naming_a_part_the_vocabulary_does_not_carry)
{
    const auto refusal{refusal_of({{.left = "a", .right = "b"}, {.left = "a", .right = "bc"}})};

    EXPECT_TRUE(refusal.contains(R"(window_density: the merge ("a", "bc") at rank 1 names "bc")")) << refusal;
}

TEST(Window_density_test, Refuses_a_merge_whose_product_an_earlier_merge_already_carries)
{
    const auto refusal{refusal_of(
            {{.left = "a", .right = "b"},
             {.left = "a", .right = "a"},
             {.left = "aa", .right = "b"},
             {.left = "a", .right = "ab"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "ab") at rank 3 recreates the token "aab")")) << refusal;
}

TEST(Window_density_test, The_decision_gives_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/density_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    expect_double_matched(check);

    EXPECT_EQ(check.counts.universes, 2221U);
    EXPECT_EQ(check.counts.infinite, 1758U);
    EXPECT_EQ(check.counts.dense, 243U);
    EXPECT_EQ(check.counts.sparse, 220U);
    EXPECT_EQ(check.counts.doubles, 170U);
    EXPECT_EQ(check.counts.zero_start, 30U);
}

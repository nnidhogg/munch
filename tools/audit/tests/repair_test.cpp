#include "munch/tools/audit/repair.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/token_set.hpp"

using namespace munch::tools::audit;
using munch::regex::parse;

namespace
{
/**
 * @brief The pieces a drawn pattern is built from: single bytes, sets of two and four, and a set of every byte but two,
 *        so that the drawn sets hold byte classes of one, two and many bytes.
 */
constexpr std::array atoms{"a", "b", "c", "[ab]", "[cd]", "[a-d]", "[^ab]", R"(\n)"};

/**
 * @brief The operators a drawn piece may carry, none among them.
 */
constexpr std::array suffixes{"", "", "*", "+", "?"};

/**
 * @brief Draws a number below a bound from a sequence.
 * @param sequence The sequence, advanced by the draw.
 * @param below The bound.
 * @return The number.
 */
std::size_t draw(std::mt19937& sequence, const std::size_t below)
{
    std::uniform_int_distribution<std::size_t> distribution{0, below - 1};

    return distribution(sequence);
}

/**
 * @brief Draws a pattern: one to three pieces, each an atom with or without an operator, or a choice of two atoms.
 * @param sequence The sequence the pattern is drawn from.
 * @return The pattern, as regex::parse() reads it.
 */
std::string random_pattern(std::mt19937& sequence)
{
    std::string pattern{};

    for (auto pieces{draw(sequence, 3) + 1}; pieces > 0; --pieces)
    {
        const std::string_view atom{atoms[draw(sequence, atoms.size())]};

        const std::string_view suffix{suffixes[draw(sequence, suffixes.size())]};

        if (draw(sequence, 4) == 0)
        {
            const std::string_view other{atoms[draw(sequence, atoms.size())]};

            pattern += std::format("({}|{}){}", atom, other, suffix);

            continue;
        }

        pattern += std::format("{}{}", atom, suffix);
    }

    return pattern;
}

/**
 * @brief Draws a token set: one to three rules, each a drawn pattern under its index as id, a priority below three and
 *        discarded or kept.
 * @param sequence The sequence the set is drawn from.
 * @return The set.
 */
Token_set random_set(std::mt19937& sequence)
{
    Token_set set{};

    for (std::size_t id{0}, rules{draw(sequence, 3) + 1}; id < rules; ++id)
    {
        const auto pattern{random_pattern(sequence)};

        const auto priority{draw(sequence, 3)};

        const auto discarded{draw(sequence, 2) == 0};

        set.rules.push_back({.regex = parse(pattern), .id = id, .priority = priority, .discarded = discarded});
    }

    return set;
}

/**
 * @brief Returns what giving one byte a token of its own does to a set, tried on its own: the set compiled with the
 *        byte alone added under the smallest id no rule carries at a priority past every rule's, and the byte and the
 *        other bytes certified with it read off it, in the sense the token's kind asks for.
 * @param set The set.
 * @param byte The byte.
 * @param discarded Whether the token is discarded.
 * @param before The bytes that certified in that sense before the addition, ascending.
 * @param found The repairs of that kind, which the byte's is appended to when the byte certifies.
 */
void try_byte(
        const Token_set& set, const unsigned char byte, const bool discarded, const std::vector<unsigned char>& before,
        std::vector<Repair>& found)
{
    auto id{0UZ};

    while (std::ranges::contains(set.rules, id, &Token_rule::id))
    {
        ++id;
    }

    const auto priority{std::ranges::max(set.rules, {}, &Token_rule::priority).priority + 1};

    const std::string spelled{static_cast<char>(byte)};

    auto edited{set};

    edited.rules.push_back(
            {.regex = munch::regex::text(spelled), .id = id, .priority = priority, .discarded = discarded});

    const auto lexer{compile(edited)};

    const auto certifies{[&](const std::size_t value) {
        const auto symbol{static_cast<char>(value)};

        return discarded ? lexer.is_split_point_ignoring(symbol) : lexer.is_split_point(symbol);
    }};

    if (!certifies(byte))
    {
        return;
    }

    std::vector<unsigned char> gained{};

    for (std::size_t value{0}; value < byte_values; ++value)
    {
        const auto other{static_cast<unsigned char>(value)};

        if (other != byte && !std::ranges::binary_search(before, other) && certifies(value))
        {
            gained.push_back(other);
        }
    }

    found.push_back(Repair{.byte = byte, .gained = std::move(gained)});
}

/**
 * @brief Returns the repairs of a set with every byte tried on its own, as a visible token and, unless it already
 *        certifies once the discarded tokens are deleted, as a discarded one: 512 compilations at most.
 * @param set The set.
 * @param report The set's report.
 * @return The repairs, each list ascending.
 */
Repairs every_byte_tried(const Token_set& set, const Report& report)
{
    Repairs found{};

    auto& [visible, discarded]{found};

    for (std::size_t value{0}; value < byte_values; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        try_byte(set, byte, false, report.exact, visible);

        if (!std::ranges::binary_search(report.modulo, byte))
        {
            try_byte(set, byte, true, report.modulo, discarded);
        }
    }

    return found;
}

/**
 * @brief Expects the repairs repairs() decides over a set to be the ones every byte tried on its own gives.
 * @param set The set.
 * @param report The set's report.
 * @param label What a failure names the set by.
 */
void expect_tried_alike(const Token_set& set, const Report& report, const std::string_view label)
{
    const auto decided{repairs(set, report)};

    const auto tried{every_byte_tried(set, report)};

    const auto& [visible, discarded]{decided};

    const auto& [tried_visible, tried_discarded]{tried};

    ASSERT_EQ(visible.size(), tried_visible.size()) << label;
    ASSERT_EQ(discarded.size(), tried_discarded.size()) << label;

    for (std::size_t row{0}; row < visible.size(); ++row)
    {
        EXPECT_EQ(visible[row].byte, tried_visible[row].byte) << label;
        EXPECT_EQ(visible[row].gained, tried_visible[row].gained) << label;
    }

    for (std::size_t row{0}; row < discarded.size(); ++row)
    {
        EXPECT_EQ(discarded[row].byte, tried_discarded[row].byte) << label;
        EXPECT_EQ(discarded[row].gained, tried_discarded[row].gained) << label;
    }
}

} // namespace

TEST(Repair_test, Each_byte_class_tried_through_its_lowest_byte_repairs_as_every_byte_tried_on_its_own_does)
{
    // Sets of one to three rules over a, b, c, d, the newline and the bytes but a and b, each kept or discarded, those
    // that certify no byte among the draws kept until 40 are held. Over each, repairs() and the brute force that
    // compiles the set once per byte and kind agree on every row and its other bytes, and the rows are tallied so that
    // each kind is seen certifying and some class of more than one byte is seen standing behind its lowest.
    std::mt19937 sequence{0x5EEDU};

    std::size_t held{0};

    std::size_t visible_rows{0};

    std::size_t discarded_rows{0};

    std::size_t spread_classes{0};

    const auto spread{[](const std::vector<unsigned char>& members) { return members.size() > 1; }};

    for (std::size_t drawn{0}; held < 40; ++drawn)
    {
        const auto set{random_set(sequence)};

        const auto report{audit(set, 1)};

        if (!report.exact.empty())
        {
            continue;
        }

        ++held;

        const auto label{std::format("set {} drawn", drawn)};

        expect_tried_alike(set, report, label);

        const auto [visible, discarded]{repairs(set, report)};

        visible_rows += visible.size();

        discarded_rows += discarded.size();

        spread_classes += std::ranges::count_if(report.classes, spread);
    }

    EXPECT_GT(visible_rows, 0U);
    EXPECT_GT(discarded_rows, 0U);
    EXPECT_GT(spread_classes, 0U);
}

TEST(Repair_test, A_discarded_token_certifies_the_bytes_that_only_its_own_byte_kept_from_certifying)
{
    // b alone is no token and begins b^n c, n of one or more, and b^n, n of two or more, all discarded, and c is
    // discarded alone: c fails only after b, which accepts nothing, so b's discarded token certifies b and c with it,
    // as the brute force says.
    const Token_set set{
            .rules = {
                    {.regex = parse("b(b*c|b+)"), .id = 0, .priority = 0, .discarded = true},
                    {.regex = parse("c"), .id = 1, .priority = 0, .discarded = true}}};

    const auto report{audit(set, 1)};

    const auto [visible, discarded]{repairs(set, report)};

    const auto b_row{std::ranges::find(discarded, 'b', &Repair::byte)};

    EXPECT_TRUE(report.exact.empty());
    ASSERT_NE(b_row, discarded.end());
    EXPECT_EQ(b_row->gained, std::vector<unsigned char>{'c'});

    expect_tried_alike(set, report, "b(b*c|b+) and c");
}

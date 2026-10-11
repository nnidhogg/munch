#include "munch/verifier/ripple_radius.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "munch/verifier/merge_tower.hpp"

using namespace munch::verifier;

namespace
{
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
     * @brief The refused lines.
     */
    std::size_t refusals{0};

    /**
     * @brief The finite lines.
     */
    std::size_t finite{0};

    /**
     * @brief The infinite lines.
     */
    std::size_t infinite{0};
};

/**
 * @brief The reference fixture under check: the current universe's table and line, and the counts so far.
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
     * @brief The lines checked so far, by kind.
     */
    Fixture_counts counts{};
};

/**
 * @brief The span of a token covering a byte, as the byte indices of its first byte and of the byte after its last.
 */
struct Span
{
    /**
     * @brief Equal when both ends are.
     */
    auto operator<=>(const Span&) const = default;

    /**
     * @brief The index of the token's first byte, negative for a token reaching into a prepended string.
     */
    std::ptrdiff_t begin{};

    /**
     * @brief The index after the token's last byte.
     */
    std::ptrdiff_t end{};
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
 * @brief Returns the span of the token covering each byte of an encoding, the first token beginning at an index.
 * @param tokens The encoding.
 * @param first The index the first token begins at.
 * @return The covering spans, one per byte.
 */
std::vector<Span> covering_spans(const std::vector<std::string>& tokens, const std::ptrdiff_t first)
{
    std::vector<Span> spans{};

    auto begin{first};

    for (const auto& token : tokens)
    {
        const auto end{begin + static_cast<std::ptrdiff_t>(token.size())};

        spans.insert(spans.end(), token.size(), Span{.begin = begin, .end = end});
        begin = end;
    }

    return spans;
}

/**
 * @brief Returns the origin ripple and the mirrored quantity of a string prepended to an input, by the reference
 *        encoder: the input's covering spans against those of the prepended input shifted left by the string's length.
 * @param merges The merge table in rank order.
 * @param prepended The prepended string.
 * @param input The input.
 * @return The two measures.
 */
Ripple ripple_of(const std::vector<Merge>& merges, const std::string_view prepended, const std::string_view input)
{
    const auto shift{static_cast<std::ptrdiff_t>(prepended.size())};

    const auto base{covering_spans(reference_tokens(merges, input), 0)};

    const auto whole{covering_spans(reference_tokens(merges, std::string{prepended} + std::string{input}), -shift)};

    Ripple ripple{};

    for (std::size_t index{0}; index < input.size(); ++index)
    {
        const auto& [begin, end]{base[index]};

        if (base[index] == whole[prepended.size() + index])
        {
            continue;
        }

        ripple = {.origin = index + 1, .mirrored = static_cast<std::size_t>(begin) + 1};
    }

    return ripple;
}

/**
 * @brief Checks that a prepended input attains a radius and one attains a mirrored constant under the reference
 *        encoder, each witness present exactly when its value is not zero.
 * @param merges The merge table in rank order.
 * @param finite The radius and the mirrored constant with their witnesses.
 * @param context The fixture line or test checked.
 */
void expect_attained(const std::vector<Merge>& merges, const Finite_ripple& finite, const std::string& context)
{
    const auto& [radius, radius_witness, mirrored, mirrored_witness]{finite};

    ASSERT_EQ(radius_witness.has_value(), radius != 0) << context;
    ASSERT_EQ(mirrored_witness.has_value(), mirrored != 0) << context;

    if (radius_witness)
    {
        const auto& [symbol, input]{*radius_witness};

        const auto [radius_reached, radius_mirror]{ripple_of(merges, symbol, input)};

        EXPECT_EQ(radius_reached, radius) << context << " on " << input;
    }

    if (mirrored_witness)
    {
        const auto& [symbol, input]{*mirrored_witness};

        const auto [mirror_origin, mirror_reached]{ripple_of(merges, symbol, input)};

        EXPECT_EQ(mirror_reached, mirrored) << context << " on " << input;
    }
}

/**
 * @brief Checks that a merge is an effective self-pair whose ripple the reference encoder makes unbounded: its parts
 *        equal, the merges before it encoding the two copies as two tokens, and prepending the part to an even power
 *        of it changing the covering token of every byte.
 * @param merges The merge table in rank order.
 * @param rank The merge's rank.
 * @param context The fixture line or test checked.
 */
void expect_effective_self_pair(const std::vector<Merge>& merges, const std::size_t rank, const std::string& context)
{
    ASSERT_LT(rank, merges.size()) << context;

    const auto& [left, right]{merges[rank]};

    ASSERT_EQ(left, right) << context;

    const std::vector<Merge> before{merges.begin(), merges.begin() + static_cast<std::ptrdiff_t>(rank)};

    EXPECT_EQ(reference_tokens(before, left + right), (std::vector{left, right})) << context;

    std::string input{};

    for (std::size_t half{1}; half <= 3; ++half)
    {
        input += left + left;

        EXPECT_EQ(ripple_of(merges, left, input).origin, input.size()) << context << " on " << input;
    }
}

/**
 * @brief Returns the message ripple_radius() refuses a table with, or the empty text when it takes the table.
 * @param merges The merge table in rank order.
 * @return The refusal.
 */
std::string refusal_of(const std::vector<Merge>& merges)
{
    try
    {
        std::ignore = ripple_radius(merges);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the text of a fixture field, "-" standing for the empty text.
 * @param field The field.
 * @return The text.
 */
std::string text_of(const std::string& field)
{
    return field == "-" ? std::string{} : field;
}

/**
 * @brief Starts a universe line's universe: reads its merges, each a left and a right part joined by a comma.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_universe(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
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
}

/**
 * @brief Checks a refused line: the table refused with a message naming the merge's rank and the reason.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_refusal(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t rank{};
    std::string reason{};

    fields >> rank >> reason;

    const auto refusal{refusal_of(check.merges)};

    ++check.counts.refusals;

    EXPECT_TRUE(refusal.contains(std::format(" at rank {} {} ", rank, reason))) << context << ": " << refusal;
}

/**
 * @brief Checks a finite line: the radius and the mirrored constant equal to the reference's, and both the decision's
 *        witnesses and the reference's attaining them.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_finite(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t radius{};
    std::size_t mirrored{};
    std::string radius_symbol{};
    std::string radius_input{};
    std::string mirrored_symbol{};
    std::string mirrored_input{};

    fields >> radius >> mirrored >> radius_symbol >> radius_input >> mirrored_symbol >> mirrored_input;

    const auto found{ripple_radius(check.merges)};

    ++check.counts.finite;

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found)) << context;

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, radius) << context;
    EXPECT_EQ(finite.mirrored, mirrored) << context;

    expect_attained(check.merges, finite, context);

    const auto witness_of{[](const std::size_t value, const std::string& symbol, const std::string& input) {
        return value == 0 ? std::nullopt : std::optional{Prepended_input{.symbol = symbol, .input = text_of(input)}};
    }};

    const auto radius_witness{witness_of(radius, radius_symbol, radius_input)};

    const auto mirrored_witness{witness_of(mirrored, mirrored_symbol, mirrored_input)};

    const Finite_ripple reference{
            .radius = radius,
            .radius_witness = radius_witness,
            .mirrored = mirrored,
            .mirrored_witness = mirrored_witness};

    expect_attained(check.merges, reference, context);
}

/**
 * @brief Checks an infinite line: the first effective self-pair's rank equal to the reference's and replayed, and the
 *        reference's certificate pumped, the ripple growing by the loop's length with every repetition.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_infinite(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t rank{};
    std::string byte{};
    std::string stem{};
    std::string loop{};
    std::string exit{};
    std::string tail{};

    fields >> rank >> byte >> stem >> loop >> exit >> tail;

    const auto found{ripple_radius(check.merges)};

    ++check.counts.infinite;

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found)) << context;
    EXPECT_EQ(std::get<Effective_self_pair>(found).rank, rank) << context;

    expect_effective_self_pair(check.merges, rank, context);

    std::string pumped{text_of(stem)};
    std::size_t previous{ripple_of(check.merges, byte, pumped + text_of(exit) + text_of(tail)).origin};

    for (std::size_t repetitions{1}; repetitions <= 3; ++repetitions)
    {
        pumped += text_of(loop);

        const auto ripple{ripple_of(check.merges, byte, pumped + text_of(exit) + text_of(tail)).origin};

        EXPECT_EQ(ripple, previous + text_of(loop).size()) << context << " pumped " << repetitions << " times";

        previous = ripple;
    }
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
    else if (kind == "refused")
    {
        expect_refusal(check, fields, context);
    }
    else if (kind == "finite")
    {
        expect_finite(check, fields, context);
    }
    else if (kind == "infinite")
    {
        expect_infinite(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

} // namespace

TEST(Ripple_radius_test, One_merge_over_a_is_infinite_at_its_own_rank)
{
    const std::vector<Merge> merges{{.left = "a", .right = "a"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));
    EXPECT_EQ(std::get<Effective_self_pair>(found).rank, 0U);

    expect_effective_self_pair(merges, 0, "one merge over a");
}

TEST(Ripple_radius_test, A_proper_ordered_table_moves_every_covering_token_of_b_followed_by_ab_repeated)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));
    EXPECT_EQ(std::get<Effective_self_pair>(found).rank, 1U);

    std::string input{"b"};

    for (std::size_t repetitions{1}; repetitions <= 6; ++repetitions)
    {
        input += "ab";

        EXPECT_EQ(ripple_of(merges, "a", input).origin, input.size()) << input;
    }
}

TEST(Ripple_radius_test, A_self_pair_whose_copies_never_meet_at_its_stage_leaves_the_radius_finite)
{
    const std::vector<Merge> merges{
            {.left = "a", .right = "b"},
            {.left = "b", .right = "a"},
            {.left = "ba", .right = "ba"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, 2U);

    // The merges before the self-pair never leave two copies of ba adjacent: baba encodes as b, ab, a.
    const std::vector<Merge> before{merges[0], merges[1]};

    EXPECT_EQ(reference_tokens(before, "baba"), (std::vector<std::string>{"b", "ab", "a"}));

    expect_attained(merges, finite, "ab, ba, ba+ba");
}

TEST(Ripple_radius_test, The_mirrored_constant_lies_below_the_radius_when_the_last_changed_token_begins_early)
{
    const std::vector<Merge> merges{{.left = "b", .right = "c"}, {.left = "a", .right = "bc"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, 2U);
    EXPECT_EQ(finite.mirrored, 1U);
    EXPECT_EQ(ripple_of(merges, "a", "bc").origin, 2U);
    EXPECT_EQ(ripple_of(merges, "a", "bc").mirrored, 1U);

    expect_attained(merges, finite, "bc, a+bc");
}

TEST(Ripple_radius_test, The_empty_table_has_radius_zero_and_no_witness)
{
    const auto found{ripple_radius({})};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& [radius, radius_witness, mirrored, mirrored_witness]{std::get<Finite_ripple>(found)};

    EXPECT_EQ(radius, 0U);
    EXPECT_EQ(mirrored, 0U);
    EXPECT_FALSE(radius_witness);
    EXPECT_FALSE(mirrored_witness);
}

TEST(Ripple_radius_test, The_Fibonacci_tables_reach_the_largest_finite_radius_of_their_merge_count)
{
    // Each merge joins the two latest products, a then b and then each word the one before it followed by itself.
    std::vector<std::string> words{"a", "b"};
    std::vector<Merge> merges{};
    std::vector<std::size_t> fibonacci{1, 1};

    for (std::size_t count{1}; count <= 8; ++count)
    {
        const auto& left{words[words.size() - 2]};
        const auto& right{words.back()};

        merges.push_back({.left = left, .right = right});
        words.push_back(left + right);

        while (fibonacci.size() < count + 3)
        {
            fibonacci.push_back(fibonacci.back() + fibonacci[fibonacci.size() - 2]);
        }

        const auto found{ripple_radius(merges)};

        ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found)) << count;

        const auto& finite{std::get<Finite_ripple>(found)};

        EXPECT_EQ(finite.radius, fibonacci[count + 2] - 2) << count;

        expect_attained(merges, finite, std::format("the Fibonacci table of {} merges", count));
    }
}

TEST(Ripple_radius_test, Refuses_a_merge_naming_a_part_the_vocabulary_does_not_carry)
{
    const auto refusal{refusal_of({{.left = "a", .right = "b"}, {.left = "a", .right = "bc"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "bc") at rank 1 names "bc")")) << refusal;
}

TEST(Ripple_radius_test, Refuses_a_merge_whose_product_an_earlier_merge_already_carries)
{
    const auto refusal{refusal_of(
            {{.left = "a", .right = "b"},
             {.left = "a", .right = "a"},
             {.left = "aa", .right = "b"},
             {.left = "a", .right = "ab"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "ab") at rank 3 recreates the token "aab")")) << refusal;
}

TEST(Ripple_radius_test, Refuses_a_table_outside_the_ordering_condition_at_the_merge_recreating_an_earlier_part)
{
    // The lowest merge creates aab, a part of a higher one, which only a product spelled twice can do.
    const auto refusal{refusal_of(
            {{.left = "a", .right = "a"},
             {.left = "a", .right = "b"},
             {.left = "a", .right = "ab"},
             {.left = "aab", .right = "aa"},
             {.left = "aa", .right = "b"}})};

    EXPECT_TRUE(refusal.contains(R"(("aa", "b") at rank 4 recreates the token "aab")")) << refusal;
}

TEST(Ripple_radius_test, A_refusal_quotes_a_spelling_holding_a_NUL_and_still_names_the_rank)
{
    const std::string nul(1, '\0');

    const auto refusal{
            refusal_of({{.left = nul, .right = "a"}, {.left = "\"", .right = "\\"}, {.left = nul, .right = "a"}})};

    EXPECT_TRUE(refusal.contains(R"(("\x00", "a") at rank 2 recreates the token "\x00a")")) << refusal;

    const auto missing{refusal_of({{.left = "\xFF", .right = "\"\\"}})};

    EXPECT_TRUE(missing.contains(R"(("\xFF", "\"\\") at rank 0 names "\"\\")")) << missing;
}

TEST(Ripple_radius_test, The_decision_gives_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/ripple_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 810U);
    EXPECT_EQ(check.counts.refusals, 80U);
    EXPECT_EQ(check.counts.finite, 209U);
    EXPECT_EQ(check.counts.infinite, 521U);
}

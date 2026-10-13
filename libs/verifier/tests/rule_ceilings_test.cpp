#include "munch/verifier/rule_ceilings.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
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
     * @brief The ceilings lines.
     */
    std::size_t ceilings{0};

    /**
     * @brief The ceilings lines of a table of finite origin radius.
     */
    std::size_t finite{0};
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
 * @brief The two right ripples of one appended input: the span ripple and the start ripple.
 */
struct Right_ripple
{
    /**
     * @brief The largest distance from the input's end to the start of a token of its encoding that the encoding with
     *        the byte appended does not hold, zero when none.
     */
    std::size_t span{0};

    /**
     * @brief The largest distance from the input's end to a byte whose covering token begins elsewhere once the byte
     *        is appended, zero when none.
     */
    std::size_t start{0};
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
 * @brief Returns the span of every token of an encoding, as the index of its first byte and the index after its last.
 * @param tokens The encoding.
 * @return The spans, in order.
 */
std::vector<std::pair<std::size_t, std::size_t>> spans_of(const std::vector<std::string>& tokens)
{
    std::vector<std::pair<std::size_t, std::size_t>> spans{};

    std::size_t begin{0};

    for (const auto& token : tokens)
    {
        spans.emplace_back(begin, begin + token.size());
        begin += token.size();
    }

    return spans;
}

/**
 * @brief Returns the start of the token covering each byte of an encoding.
 * @param tokens The encoding.
 * @return The starts, one per byte.
 */
std::vector<std::size_t> starts_of(const std::vector<std::string>& tokens)
{
    std::vector<std::size_t> starts{};

    for (const auto& [begin, end] : spans_of(tokens))
    {
        starts.insert(starts.end(), end - begin, begin);
    }

    return starts;
}

/**
 * @brief Returns the right span ripple and the right start ripple of a byte appended to an input, by the reference
 *        encoder: the input's tokens and covering starts against those of the input with the byte appended.
 * @param merges The merge table in rank order.
 * @param input The input.
 * @param symbol The appended byte.
 * @return The two ripples.
 */
Right_ripple ripple_of(const std::vector<Merge>& merges, const std::string_view input, const std::string_view symbol)
{
    const auto base_tokens{reference_tokens(merges, input)};

    const auto appended_tokens{reference_tokens(merges, std::string{input} + std::string{symbol})};

    const auto base_spans{spans_of(base_tokens)};
    const auto appended_spans{spans_of(appended_tokens)};

    Right_ripple ripple{};

    for (const auto& span : base_spans)
    {
        const auto& [begin, end]{span};

        if (std::ranges::find(appended_spans, span) == appended_spans.end())
        {
            ripple.span = std::max(ripple.span, input.size() - begin);
        }
    }

    const auto base_starts{starts_of(base_tokens)};
    const auto appended_starts{starts_of(appended_tokens)};

    for (std::size_t index{0}; index < input.size(); ++index)
    {
        if (base_starts[index] != appended_starts[index])
        {
            ripple.start = std::max(ripple.start, input.size() - index);
        }
    }

    return ripple;
}

/**
 * @brief Checks that the two right radii are attained by their witnesses under the reference encoder, each witness
 *        present exactly when its radius is not zero.
 * @param merges The merge table in rank order.
 * @param ceilings The ceilings with the two radii and their witnesses.
 * @param context The fixture line or test checked.
 */
void expect_attained(const std::vector<Merge>& merges, const Rule_ceilings& ceilings, const std::string& context)
{
    const auto& [start_ceiling, radius_ceiling, loose_ceiling, span_radius, span_witness, start_radius, start_witness]{
            ceilings};

    ASSERT_EQ(span_witness.has_value(), span_radius != 0) << context;
    ASSERT_EQ(start_witness.has_value(), start_radius != 0) << context;

    if (span_witness)
    {
        const auto& [input, symbol]{*span_witness};

        const auto [span, start]{ripple_of(merges, input, symbol)};

        EXPECT_EQ(span, span_radius) << context << " on " << input << " with " << symbol;
    }

    if (start_witness)
    {
        const auto& [input, symbol]{*start_witness};

        const auto [span, start]{ripple_of(merges, input, symbol)};

        EXPECT_EQ(start, start_radius) << context << " on " << input << " with " << symbol;
    }
}

/**
 * @brief Checks the ceilings against ripple_radius(): both radius ceilings present exactly when the origin radius is
 *        finite, and then each at least the radius.
 * @param merges The merge table in rank order.
 * @param ceilings The ceilings.
 * @param context The fixture line or test checked.
 */
void expect_radius_below(const std::vector<Merge>& merges, const Rule_ceilings& ceilings, const std::string& context)
{
    const auto ripple{ripple_radius(merges)};

    const auto finite{std::holds_alternative<Finite_ripple>(ripple)};

    ASSERT_EQ(ceilings.radius_ceiling.has_value(), finite) << context;
    ASSERT_EQ(ceilings.loose_ceiling.has_value(), finite) << context;

    if (!finite)
    {
        return;
    }

    const auto radius{std::get<Finite_ripple>(ripple).radius};

    EXPECT_LE(radius, *ceilings.radius_ceiling) << context;
    EXPECT_LE(radius, *ceilings.loose_ceiling) << context;
}

/**
 * @brief Returns the message rule_ceilings() refuses a table with, or the empty text when it takes the table.
 * @param merges The merge table in rank order.
 * @return The refusal.
 */
std::string refusal_of(const std::vector<Merge>& merges)
{
    try
    {
        std::ignore = rule_ceilings(merges);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the value of a fixture field, "-" standing for none.
 * @param field The field.
 * @return The value.
 */
std::optional<std::size_t> value_of(const std::string& field)
{
    return field == "-" ? std::nullopt : std::optional{std::stoull(field)};
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
 * @brief Checks a ceilings line: the three ceilings and the two radii equal to the reference's, the witnesses attaining
 *        the radii, and the origin radius within the ceilings.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_ceilings(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t start_ceiling{};
    std::string radius_ceiling{};
    std::string loose_ceiling{};
    std::size_t span_radius{};
    std::size_t start_radius{};

    fields >> start_ceiling >> radius_ceiling >> loose_ceiling >> span_radius >> start_radius;

    const auto found{rule_ceilings(check.merges)};

    ++check.counts.ceilings;

    if (found.radius_ceiling)
    {
        ++check.counts.finite;
    }

    EXPECT_EQ(found.start_ceiling, start_ceiling) << context;
    EXPECT_EQ(found.radius_ceiling, value_of(radius_ceiling)) << context;
    EXPECT_EQ(found.loose_ceiling, value_of(loose_ceiling)) << context;
    EXPECT_EQ(found.span_radius, span_radius) << context;
    EXPECT_EQ(found.start_radius, start_radius) << context;

    expect_attained(check.merges, found, context);

    expect_radius_below(check.merges, found, context);
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
    else if (kind == "ceilings")
    {
        expect_ceilings(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

} // namespace

TEST(Rule_ceilings_test,
     The_empty_table_has_no_witness_its_right_radii_and_start_and_radius_ceilings_at_zero_and_its_loose_one_at_one)
{
    const auto found{rule_ceilings({})};

    const auto& [start_ceiling, radius_ceiling, loose_ceiling, span_radius, span_witness, start_radius, start_witness]{
            found};

    EXPECT_EQ(start_ceiling, 0U);
    EXPECT_EQ(radius_ceiling, std::optional<std::size_t>{0});
    EXPECT_EQ(loose_ceiling, std::optional<std::size_t>{1});
    EXPECT_EQ(span_radius, 0U);
    EXPECT_EQ(start_radius, 0U);
    EXPECT_FALSE(span_witness);
    EXPECT_FALSE(start_witness);
}

TEST(Rule_ceilings_test, The_chain_tables_attain_the_start_ceiling_with_tokens_of_two_bytes)
{
    // The chain of m merges (a_(m-1), a_m), ..., (a_0, a_1) over distinct bytes has Q_span = m and Q_start = m - 1.
    const std::string bytes{"abcdefghij"};

    for (std::size_t count{1}; count + 1 <= bytes.size(); ++count)
    {
        std::vector<Merge> merges{};

        for (auto at{count}; at > 0; --at)
        {
            merges.push_back({.left = bytes.substr(at - 1, 1), .right = bytes.substr(at, 1)});
        }

        const auto found{rule_ceilings(merges)};

        const auto context{std::format("the chain of {} merges", count)};

        EXPECT_EQ(found.span_radius, count) << context;
        EXPECT_EQ(found.start_radius, count - 1) << context;
        EXPECT_EQ(found.start_ceiling, count - 1) << context;

        expect_attained(merges, found, context);

        expect_radius_below(merges, found, context);
    }
}

TEST(Rule_ceilings_test, A_table_whose_runs_of_a_certify_no_cut_has_both_right_radii_and_a_radius_of_two)
{
    // Over (a, b) and (a, ab) a run of a's is a token a alone and ends in aab before a b.
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}};

    const auto found{rule_ceilings(merges)};

    EXPECT_EQ(found.span_radius, 2U);
    EXPECT_EQ(found.start_radius, 1U);
    EXPECT_EQ(found.start_ceiling, 1U);
    EXPECT_EQ(found.radius_ceiling, std::optional<std::size_t>{3});

    expect_attained(merges, found, "(a, b), (a, ab)");

    expect_radius_below(merges, found, "(a, b), (a, ab)");
}

TEST(Rule_ceilings_test, An_effective_self_pair_leaves_the_right_radii_finite_and_withdraws_the_radius_ceilings)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}};

    const auto found{rule_ceilings(merges)};

    EXPECT_FALSE(found.radius_ceiling);
    EXPECT_FALSE(found.loose_ceiling);
    EXPECT_EQ(found.start_ceiling, 2U);

    expect_attained(merges, found, "(a, b), (ab, ab)");

    expect_radius_below(merges, found, "(a, b), (ab, ab)");
}

TEST(Rule_ceilings_test, Refuses_a_merge_naming_a_part_the_vocabulary_does_not_carry)
{
    const auto refusal{refusal_of({{.left = "a", .right = "b"}, {.left = "a", .right = "bc"}})};

    EXPECT_TRUE(refusal.contains(R"(rule_ceilings: the merge ("a", "bc") at rank 1 names "bc")")) << refusal;
}

TEST(Rule_ceilings_test, Refuses_a_merge_whose_product_an_earlier_merge_already_carries)
{
    const auto refusal{refusal_of(
            {{.left = "a", .right = "b"},
             {.left = "a", .right = "a"},
             {.left = "aa", .right = "b"},
             {.left = "a", .right = "ab"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "ab") at rank 3 recreates the token "aab")")) << refusal;
}

TEST(Rule_ceilings_test, The_ceilings_and_radii_give_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/ceilings_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 2221U);
    EXPECT_EQ(check.counts.ceilings, 2221U);
    EXPECT_EQ(check.counts.finite, 463U);
}

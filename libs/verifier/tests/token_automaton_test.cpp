#include "munch/verifier/token_automaton.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

using namespace munch::verifier;

namespace
{
/**
 * @brief A boundary marking, one bit per byte saying whether a boundary follows it.
 */
using Marking_t = std::vector<bool>;

/**
 * @brief The rank of each merge by its two parts.
 */
using Ranks_t = std::map<std::pair<std::string, std::string>, std::size_t>;

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
     * @brief The universe lines whose pairs are also decided on the merge tower.
     */
    std::size_t towers{0};

    /**
     * @brief The certify lines.
     */
    std::size_t certifications{0};

    /**
     * @brief The certify lines whose answer is a miscovering.
     */
    std::size_t miscoverings{0};

    /**
     * @brief The certify lines also decided on the merge tower.
     */
    std::size_t tower_certifications{0};
};

/**
 * @brief A universe of the fixture: its merge table, the rank of each merge, the longest token, the token automaton of
 *        the table, and its merge tower where the fixture asks for it.
 */
struct Universe
{
    /**
     * @brief The merge table in rank order.
     */
    std::vector<Merge> merges{};

    /**
     * @brief The rank of each merge by its two parts.
     */
    Ranks_t ranks{};

    /**
     * @brief The length of the longest token, a byte or a product.
     */
    std::size_t longest{1};

    /**
     * @brief The token automaton of the table, built once for every pair of the universe.
     */
    std::optional<Token_automaton> automaton{};

    /**
     * @brief The merge tower of the table, std::nullopt when the fixture decides the universe without it.
     */
    std::optional<Verifier> tower{};
};

/**
 * @brief The reference fixture under check: the current universe, its line, and the counts so far.
 */
struct Fixture_check
{
    /**
     * @brief The current universe.
     */
    Universe universe{};

    /**
     * @brief The current universe's line, its merges left out, for the failure messages.
     */
    std::string context{};

    /**
     * @brief The lines checked so far, by kind.
     */
    Fixture_counts counts{};
};

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
 * @brief Returns the ranks of a merge table, each merge by its two parts.
 * @param merges The merge table in rank order.
 * @return The ranks.
 */
Ranks_t ranks_of(const std::vector<Merge>& merges)
{
    Ranks_t ranks{};

    for (std::size_t rank{0}; rank < merges.size(); ++rank)
    {
        const auto& [left, right]{merges[rank]};

        ranks.try_emplace({left, right}, rank);
    }

    return ranks;
}

/**
 * @brief Returns the token sequence canonical byte-pair encoding produces for a text: at every step the lowest-rank
 *        merge present anywhere is applied at its leftmost occurrence.
 * @param ranks The rank of each merge by its two parts.
 * @param text The text.
 * @return The tokens.
 */
std::vector<std::string> reference_tokens(const Ranks_t& ranks, const std::string_view text)
{
    std::vector<std::string> sequence{};

    for (const auto byte : text)
    {
        sequence.push_back(std::string{byte});
    }

    for (;;)
    {
        std::optional<std::size_t> lowest{};
        std::size_t at{0};

        for (std::size_t index{0}; index + 1 < sequence.size(); ++index)
        {
            const auto found{ranks.find({sequence[index], sequence[index + 1]})};

            if (found == ranks.cend())
            {
                continue;
            }

            const auto& [parts, rank]{*found};

            if (lowest && rank >= *lowest)
            {
                continue;
            }

            lowest = rank;
            at = index;
        }

        if (!lowest)
        {
            return sequence;
        }

        sequence[at] += sequence[at + 1];
        const auto next{sequence.begin() + static_cast<std::ptrdiff_t>(at) + 1};

        sequence.erase(next);
    }
}

/**
 * @brief Returns the boundary marking of the reference encoder's segmentation of a text, a boundary after every token
 *        but the last.
 * @param ranks The rank of each merge by its two parts.
 * @param text The text.
 * @return The marking.
 */
Marking_t reference_marking(const Ranks_t& ranks, const std::string_view text)
{
    Marking_t marking(text.size(), false);

    std::size_t end{0};

    for (const auto& token : reference_tokens(ranks, text))
    {
        end += token.size();

        if (end < text.size())
        {
            marking[end - 1] = true;
        }
    }

    return marking;
}

/**
 * @brief Returns whether the verifier accepts a marked string.
 * @param verifier The verifier.
 * @param marked The marked string.
 * @return True when it is accepted.
 */
bool is_accepted(const Verifier& verifier, const Marked_string& marked)
{
    const auto& [bytes, boundaries]{marked};

    auto state{std::optional{verifier.start()}};

    for (std::size_t index{0}; index < bytes.size() && state; ++index)
    {
        const Marked symbol{.byte = static_cast<unsigned char>(bytes[index]), .boundary_after = boundaries[index]};

        state = verifier.step(*state, symbol);
    }

    return state && verifier.accepts(*state);
}

/**
 * @brief Returns the start of the token containing a byte of a marked string.
 * @param marked The marked string.
 * @param index The byte.
 * @return The index of the token's first byte.
 */
std::size_t covering_boundary(const Marked_string& marked, std::size_t index)
{
    while (index > 0 && !marked.boundaries[index - 1])
    {
        --index;
    }

    return index;
}

/**
 * @brief Returns whether some occurrence of a window in a marked string has a covering boundary other than its origin.
 * @param marked The marked string.
 * @param window The window.
 * @param origin The origin.
 * @return True when an occurrence is mis-covered.
 */
bool has_miscovered_occurrence(const Marked_string& marked, const std::string_view window, const std::size_t origin)
{
    for (auto at{marked.bytes.find(window)}; at != std::string::npos; at = marked.bytes.find(window, at + 1))
    {
        const auto last{at + window.size() - 1};

        if (covering_boundary(marked, last) != at + origin)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks that under the reference encoder's marking of a text some occurrence of the window is mis-covered.
 * @param universe The universe.
 * @param bytes The text.
 * @param window The window.
 * @param origin The origin.
 * @param context The fixture line checked.
 */
void expect_miscovered(
        const Universe& universe, const std::string& bytes, const std::string_view window, const std::size_t origin,
        const std::string& context)
{
    const Marked_string marked{.bytes = bytes, .boundaries = reference_marking(universe.ranks, bytes)};

    EXPECT_TRUE(has_miscovered_occurrence(marked, window, origin)) << context << " on " << bytes;
}

/**
 * @brief Returns the message token_automaton() refuses a table with or miscovering() a pair with, or the empty text
 *        when they take them.
 * @param merges The merge table in rank order.
 * @param window The window.
 * @param origin The origin.
 * @return The refusal.
 */
std::string refusal_of(
        const std::vector<Merge>& merges, const std::string_view window = "a", const std::size_t origin = 0)
{
    try
    {
        const auto automaton{token_automaton(merges)};

        std::ignore = miscovering(automaton, window, origin);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Starts a universe line's universe: reads its name, whether its pairs are also decided on the merge tower, and
 *        its merges, each a left and a right part in hex joined by a comma, and builds the token automaton and the
 *        tower when asked.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 */
void start_universe(Fixture_check& check, std::istringstream& fields)
{
    std::string name{};
    std::string decided{};

    fields >> name >> decided;

    check.context = std::format("universe {} {}", name, decided);

    std::vector<Merge> merges{};

    std::size_t longest{1};

    for (std::string merge{}; fields >> merge;)
    {
        const auto comma{merge.find(',')};

        ASSERT_NE(comma, std::string::npos) << check.context;

        const std::string_view field{merge};

        const auto left{bytes_of(field.substr(0, comma))};

        const auto right{bytes_of(field.substr(comma + 1))};

        const auto length{left.size() + right.size()};

        longest = std::max(longest, length);

        merges.push_back({.left = left, .right = right});
    }

    auto automaton{token_automaton(merges)};

    auto tower{decided == "tower" ? std::optional{merge_tower(merges)} : std::nullopt};

    ++check.counts.universes;

    if (tower)
    {
        ++check.counts.towers;
    }

    auto ranks{ranks_of(merges)};

    check.universe = {
            .merges = std::move(merges),
            .ranks = std::move(ranks),
            .longest = longest,
            .automaton = std::move(automaton),
            .tower = std::move(tower)};
}

/**
 * @brief Checks a certify line: the certificate's answer, the same on the merge tower where the universe has one, and
 *        for a miscovering its segmentation the encoder's, its occurrence mis-covered, its length within the bound, and
 *        both it and the reference's text mis-covered under the encoder's marking.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_certification(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::string window_field{};
    std::size_t origin{};
    std::string verdict{};
    std::string text{};

    fields >> window_field >> origin >> verdict >> text;

    const auto window{bytes_of(window_field)};

    const auto& universe{check.universe};

    const auto found{miscovering(*universe.automaton, window, origin)};

    ++check.counts.certifications;

    if (universe.tower)
    {
        const auto on_tower{miscovering(*universe.tower, window, origin)};

        ++check.counts.tower_certifications;

        EXPECT_EQ(found.has_value(), on_tower.has_value()) << context;
    }

    if (verdict == "certified")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ++check.counts.miscoverings;

    ASSERT_TRUE(found) << context;

    const auto& [segmentation, occurrence]{*found};

    const auto& [bytes, boundaries]{segmentation};

    const auto bound{2 * universe.longest + window.size() - 2};

    EXPECT_EQ(boundaries, reference_marking(universe.ranks, bytes)) << context;
    EXPECT_LE(bytes.size(), bound) << context;
    ASSERT_LE(occurrence + window.size(), bytes.size()) << context;

    const auto spelled{std::string_view{bytes}.substr(occurrence, window.size())};

    const auto last{occurrence + window.size() - 1};

    EXPECT_EQ(spelled, window) << context;
    EXPECT_NE(covering_boundary(segmentation, last), occurrence + origin) << context;

    if (universe.tower)
    {
        EXPECT_TRUE(is_accepted(*universe.tower, segmentation)) << context;
    }

    expect_miscovered(universe, bytes, window, origin, context);
    expect_miscovered(universe, bytes_of(text), window, origin, context);
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

    if (kind == "universe")
    {
        start_universe(check, fields);
    }
    else if (kind == "certify")
    {
        const auto context{std::format("{} | {}", check.context, line)};

        expect_certification(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

} // namespace

TEST(Token_automaton_test, Every_ab_lies_in_a_token_beginning_at_its_a_and_the_token_ab_miscovers_origin_one)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto automaton{token_automaton(merges)};

    EXPECT_FALSE(miscovering(automaton, "ab", 0));

    const auto found{miscovering(automaton, "ab", 1)};

    ASSERT_TRUE(found);

    const auto& [segmentation, occurrence]{*found};

    const auto& [bytes, boundaries]{segmentation};

    EXPECT_EQ(bytes, "ab");
    EXPECT_EQ(boundaries, Marking_t({false, false}));
    EXPECT_EQ(occurrence, 0U);
}

TEST(Token_automaton_test, The_empty_table_cuts_every_pair_of_bytes_between_them)
{
    const std::vector<Merge> merges{};

    const auto automaton{token_automaton(merges)};

    EXPECT_FALSE(miscovering(automaton, "ab", 1));
    EXPECT_FALSE(miscovering(automaton, "x", 0));

    const auto found{miscovering(automaton, "ab", 0)};

    ASSERT_TRUE(found);

    const auto& [segmentation, occurrence]{*found};

    const auto& [bytes, boundaries]{segmentation};

    EXPECT_EQ(bytes, "ab");
    EXPECT_EQ(boundaries, Marking_t({true, false}));
    EXPECT_EQ(occurrence, 0U);
}

TEST(Token_automaton_test, Refuses_an_empty_window_and_an_origin_outside_it)
{
    EXPECT_EQ(refusal_of({}, "", 0), "miscovering: the window is empty or its origin lies outside it");
    EXPECT_EQ(refusal_of({}, "ab", 2), "miscovering: the window is empty or its origin lies outside it");
}

TEST(Token_automaton_test, Refuses_a_merge_naming_a_part_the_vocabulary_does_not_carry)
{
    const auto refusal{refusal_of({{.left = "a", .right = "b"}, {.left = "a", .right = "bc"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "bc") at rank 1 names "bc")")) << refusal;
}

TEST(Token_automaton_test, Refuses_a_merge_whose_product_an_earlier_merge_already_carries)
{
    const auto refusal{refusal_of(
            {{.left = "a", .right = "b"},
             {.left = "a", .right = "a"},
             {.left = "aa", .right = "b"},
             {.left = "a", .right = "ab"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "ab") at rank 3 recreates the token "aab")")) << refusal;
}

TEST(Token_automaton_test, Refuses_a_table_whose_later_merge_recreates_a_token_an_earlier_one_consumed)
{
    const auto refusal{refusal_of(
            {{.left = "c", .right = "c"},
             {.left = "c", .right = "cc"},
             {.left = "ccc", .right = "a"},
             {.left = "cc", .right = "c"}})};

    EXPECT_TRUE(refusal.contains(R"(("cc", "c") at rank 3 recreates the token "ccc")")) << refusal;
}

TEST(Token_automaton_test, A_refusal_quotes_a_spelling_holding_a_NUL_and_still_names_the_rank)
{
    const std::string nul{'\0'};

    const auto refusal{
            refusal_of({{.left = nul, .right = "a"}, {.left = R"(")", .right = R"(\)"}, {.left = nul, .right = "a"}})};

    EXPECT_TRUE(refusal.contains(R"(("\x00", "a") at rank 2 recreates the token "\x00a")")) << refusal;

    const auto missing{refusal_of({{.left = "\xFF", .right = R"("\)"}})};

    EXPECT_TRUE(missing.contains(R"(("\xFF", "\"\\") at rank 0 names "\"\\")")) << missing;
}

TEST(Token_automaton_test,
     The_decision_gives_the_reference_answers_on_every_universe_of_the_fixture_and_agrees_with_the_tower)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/token_automaton_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 410U);
    EXPECT_EQ(check.counts.towers, 47U);
    EXPECT_EQ(check.counts.certifications, 40594U);
    EXPECT_EQ(check.counts.miscoverings, 24024U);
    EXPECT_EQ(check.counts.tower_certifications, 3720U);
}

TEST(Token_automaton_test, Has_the_base_state_and_one_state_per_merge_whose_product_a_transition_reads)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto automaton{token_automaton(merges)};

    const auto& tokens{automaton.tokens()};

    EXPECT_EQ(token_automaton({}).state_count(), 1U);
    EXPECT_EQ(automaton.state_count(), 3U);
    EXPECT_EQ(tokens[*automaton.token_of("a")].target, 1U);
    EXPECT_EQ(tokens[*automaton.token_of("ab")].target, 2U);
    EXPECT_EQ(tokens[*automaton.token_of("aba")].target, 1U);
}

TEST(Token_automaton_test, The_tokens_a_state_allows_are_every_token_is_allowed_admits_there_in_ascending_order)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto automaton{token_automaton(merges)};

    const auto after_a{automaton.allowed_tokens(1)};

    EXPECT_EQ(after_a.size(), 257U);
    EXPECT_FALSE(std::ranges::contains(after_a, *automaton.token_of("b")));
    EXPECT_EQ(token_automaton({}).allowed_tokens(0).size(), 256U);

    const auto& tokens{automaton.tokens()};

    for (Token_automaton::State_t state{0}; state < automaton.state_count(); ++state)
    {
        std::vector<Token_automaton::Token_t> admitted{};

        for (Token_automaton::Token_t token{0}; token < tokens.size(); ++token)
        {
            if (automaton.is_allowed(state, token))
            {
                admitted.push_back(token);
            }
        }

        EXPECT_EQ(automaton.allowed_tokens(state), admitted) << "state " << state;
    }

    for (const auto& [spelling, left, target] : tokens)
    {
        EXPECT_LT(target.value_or(0), automaton.state_count()) << spelling;
    }
}

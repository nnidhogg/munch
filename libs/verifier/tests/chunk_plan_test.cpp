#include "munch/verifier/chunk_plan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "munch/verifier/merge_tower.hpp"
#include "munch/verifier/piece_set.hpp"
#include "munch/verifier/ripple_radius.hpp"
#include "munch/verifier/rule_ceilings.hpp"
#include "munch/verifier/token_automaton.hpp"

using namespace munch::verifier;

namespace
{
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
 * @brief Returns the boundaries of an encoding, the index after each token.
 * @param tokens The encoding.
 * @return The boundaries, ascending.
 */
std::vector<std::size_t> boundaries_of(const std::vector<std::string>& tokens)
{
    std::vector<std::size_t> boundaries{};

    std::size_t end{0};

    for (const auto& token : tokens)
    {
        end += token.size();
        boundaries.push_back(end);
    }

    return boundaries;
}

/**
 * @brief Returns the cut a chunk's evidence places, the input's size for the last chunk.
 * @param evidence The evidence.
 * @param size The input's size.
 * @return The cut.
 */
std::size_t cut_of(const Cut_evidence& evidence, const std::size_t size)
{
    if (std::holds_alternative<Certified_window>(evidence))
    {
        const auto& [begin, width, origin]{std::get<Certified_window>(evidence)};

        return begin + origin;
    }

    if (std::holds_alternative<Uniform_window>(evidence))
    {
        const auto& [begin, width, byte, start]{std::get<Uniform_window>(evidence)};

        return begin + start;
    }

    if (std::holds_alternative<Supplied_cut>(evidence))
    {
        return std::get<Supplied_cut>(evidence).position;
    }

    return size;
}

/**
 * @brief Checks a plan of a text under a table: the chunks cover the text in order, each cut is the one its evidence
 *        places, a boundary of the text's encoding, and every window within the budget and inside the text, and each
 *        worker's encoding of the bytes it reads, kept below its cut, is the text's encoding of its chunk.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @param plan The plan.
 * @param budget The widest window the plan may certify.
 * @param context The test checked.
 */
void expect_true_cuts(
        const std::vector<Merge>& merges, const std::string& text, const Chunk_plan& plan, const std::size_t budget,
        const std::string& context)
{
    const auto& chunks{plan.chunks};

    ASSERT_FALSE(chunks.empty()) << context;
    EXPECT_EQ(chunks.front().begin, 0U) << context;
    EXPECT_EQ(chunks.back().owned_end, text.size()) << context;
    EXPECT_TRUE(std::holds_alternative<Input_end>(chunks.back().evidence)) << context;
    EXPECT_LE(plan.windows_decided, plan.windows_asked) << context;

    const auto whole{reference_tokens(merges, text)};

    const auto boundaries{boundaries_of(whole)};

    std::vector<std::string> stitched{};

    for (std::size_t index{0}; index < chunks.size(); ++index)
    {
        const auto& [begin, owned_end, read_end, evidence]{chunks[index]};

        const auto chunk_context{
                std::format("{} chunk {} [{}, {}) read to {}", context, index, begin, owned_end, read_end)};

        if (index > 0)
        {
            EXPECT_EQ(begin, chunks[index - 1].owned_end) << chunk_context;
        }

        EXPECT_LT(begin, std::max(owned_end, begin + 1)) << chunk_context;
        EXPECT_LE(owned_end, read_end) << chunk_context;
        EXPECT_LE(read_end, text.size()) << chunk_context;
        EXPECT_EQ(cut_of(evidence, text.size()), owned_end) << chunk_context;

        if (std::holds_alternative<Certified_window>(evidence))
        {
            const auto& [window_begin, width, origin]{std::get<Certified_window>(evidence)};

            EXPECT_LE(width, budget) << chunk_context;
            EXPECT_LE(window_begin + width, text.size()) << chunk_context;
        }

        if (owned_end < text.size())
        {
            EXPECT_TRUE(std::ranges::binary_search(boundaries, owned_end)) << chunk_context;
        }

        const auto read{text.substr(begin, read_end - begin)};

        auto spent{begin};

        for (const auto& token : reference_tokens(merges, read))
        {
            if (spent >= owned_end)
            {
                break;
            }

            stitched.push_back(token);
            spent += token.size();
        }
    }

    EXPECT_EQ(stitched, whole) << context;
}

/**
 * @brief Draws a text over {a, b, c} from a sequence.
 * @param sequence The sequence, advanced by the draw.
 * @param length The text's length.
 * @return The text.
 */
std::string random_text(std::mt19937& sequence, const std::size_t length)
{
    std::uniform_int_distribution<std::size_t> distribution{0, 2};

    std::string text{};

    for (std::size_t index{0}; index < length; ++index)
    {
        text.push_back(static_cast<char>('a' + distribution(sequence)));
    }

    return text;
}

/**
 * @brief Returns the uniform radii of a table, its origin radius and right start radius, std::nullopt when the radius
 *        is infinite.
 * @param merges The merge table in rank order.
 * @return The radii.
 */
std::optional<Uniform_radii> radii_of(const std::vector<Merge>& merges)
{
    const auto ripple{ripple_radius(merges)};

    if (!std::holds_alternative<Finite_ripple>(ripple))
    {
        return std::nullopt;
    }

    const auto radius{std::get<Finite_ripple>(ripple).radius};

    const auto start_radius{rule_ceilings(merges).start_radius};

    return Uniform_radii{.radius = radius, .start_radius = start_radius};
}

/**
 * @brief The small tables the plans are drawn over, finite and infinite in origin radius.
 * @return The tables.
 */
std::vector<std::vector<Merge>> small_tables()
{
    return {{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}},
            {{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}},
            {{.left = "b", .right = "c"}, {.left = "a", .right = "bc"}, {.left = "abc", .right = "a"}},
            {{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}},
            {{.left = "c", .right = "b"}, {.left = "b", .right = "a"}, {.left = "a", .right = "a"}}};
}

/**
 * @brief Returns the segmentation a forward Viterbi pass picks for a text in exact arithmetic: at each position, over
 *        the pieces in order, a path replaces the best one ending there only on a strictly greater score, a token's
 *        score summed along the path.
 * @param pieces The pieces.
 * @param scores The score of each piece, by its place among the pieces.
 * @param text The text.
 * @return The tokens, or std::nullopt when the pieces cannot spell the text.
 */
std::optional<std::vector<std::string>> viterbi(
        const std::vector<std::string>& pieces, const std::vector<long>& scores, const std::string_view text)
{
    std::vector<std::optional<long>> best(text.size() + 1);
    std::vector<std::size_t> last(text.size() + 1, 0);

    best[0] = 0;

    for (std::size_t end{1}; end <= text.size(); ++end)
    {
        for (std::size_t piece{0}; piece < pieces.size(); ++piece)
        {
            const auto& spelling{pieces[piece]};

            const auto fits{spelling.size() <= end && text.substr(end - spelling.size()).starts_with(spelling)};

            if (!fits || !best[end - spelling.size()])
            {
                continue;
            }

            const auto score{*best[end - spelling.size()] + scores[piece]};

            if (!best[end] || score > *best[end])
            {
                best[end] = score;
                last[end] = piece;
            }
        }
    }

    if (!best[text.size()])
    {
        return std::nullopt;
    }

    std::vector<std::string> tokens{};

    for (auto end{text.size()}; end > 0; end -= tokens.back().size())
    {
        tokens.push_back(pieces[last[end]]);
    }

    std::ranges::reverse(tokens);

    return tokens;
}

/**
 * @brief Checks a plan of a text over a piece set: the chunks cover the text in order, each cut a straddle-free
 *        position of the text read to and no further, its evidence naming it, and each chunk segmented alone by the
 *        exact Viterbi pass gives the tokens the pass gives the whole text there.
 * @param pieces The pieces.
 * @param scores The score of each piece, by its place among the pieces.
 * @param text The text, which the pieces spell.
 * @param plan The plan.
 * @param context The test checked.
 */
void expect_optimal_chunks(
        const std::vector<std::string>& pieces, const std::vector<long>& scores, const std::string& text,
        const Chunk_plan& plan, const std::string& context)
{
    const auto& chunks{plan.chunks};

    ASSERT_FALSE(chunks.empty()) << context;
    EXPECT_EQ(chunks.front().begin, 0U) << context;
    EXPECT_EQ(chunks.back().owned_end, text.size()) << context;
    EXPECT_TRUE(std::holds_alternative<Input_end>(chunks.back().evidence)) << context;
    EXPECT_EQ(plan.windows_asked, 0U) << context;

    const auto cuts{piece_set(pieces).straddle_free(text)};

    std::vector<std::string> stitched{};

    for (std::size_t index{0}; index < chunks.size(); ++index)
    {
        const auto& [begin, owned_end, read_end, evidence]{chunks[index]};

        const auto chunk_context{std::format("{} chunk {} [{}, {})", context, index, begin, owned_end)};

        if (index > 0)
        {
            EXPECT_EQ(begin, chunks[index - 1].owned_end) << chunk_context;
        }

        EXPECT_EQ(read_end, owned_end) << chunk_context;

        if (index + 1 < chunks.size())
        {
            ASSERT_TRUE(std::holds_alternative<Straddle_free_cut>(evidence)) << chunk_context;
            EXPECT_EQ(std::get<Straddle_free_cut>(evidence).position, owned_end) << chunk_context;
            EXPECT_TRUE(std::ranges::binary_search(cuts, owned_end)) << chunk_context;
        }

        const auto alone{viterbi(pieces, scores, text.substr(begin, owned_end - begin))};

        ASSERT_TRUE(alone) << chunk_context;

        stitched.insert(stitched.end(), alone->begin(), alone->end());
    }

    EXPECT_EQ(stitched, viterbi(pieces, scores, text)) << context;
}

} // namespace

TEST(Chunk_plan_test, An_empty_input_and_a_request_for_no_chunks_give_one_chunk_ending_at_the_input_end)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}};

    const auto automaton{token_automaton(merges)};

    const auto empty{chunk_plan(automaton, "", {.chunks = 4, .budget = 2, .supplied = {}}, std::nullopt)};

    ASSERT_EQ(empty.chunks.size(), 1U);
    EXPECT_EQ(empty.chunks.front().owned_end, 0U);

    const auto none{chunk_plan(automaton, "abab", {.chunks = 0, .budget = 2, .supplied = {}}, std::nullopt)};

    ASSERT_EQ(none.chunks.size(), 1U);
    EXPECT_EQ(none.chunks.front().owned_end, 4U);
    EXPECT_EQ(none.windows_asked, 0U);
}

TEST(Chunk_plan_test, Every_cut_on_the_token_automaton_is_a_boundary_and_every_worker_encodes_its_chunk_alone)
{
    std::mt19937 sequence{0x5EEDU};

    for (const auto& merges : small_tables())
    {
        const auto automaton{token_automaton(merges)};

        const auto radii{radii_of(merges)};

        for (std::size_t trial{0}; trial < 12; ++trial)
        {
            const auto text{random_text(sequence, 40)};

            for (std::size_t budget{0}; budget <= 4; ++budget)
            {
                for (const auto chunks : {std::size_t{3}, text.size()})
                {
                    const Plan_request request{.chunks = chunks, .budget = budget, .supplied = {}};

                    const auto context{std::format("{} budget {} chunks {}", text, budget, chunks)};

                    expect_true_cuts(merges, text, chunk_plan(automaton, text, request, std::nullopt), budget, context);

                    expect_true_cuts(merges, text, chunk_plan(automaton, text, request, radii), budget, context);
                }
            }
        }
    }
}

TEST(Chunk_plan_test, Every_cut_on_the_merge_tower_is_a_boundary_with_its_window_inside_its_chunk)
{
    std::mt19937 sequence{0x5EEDU};

    for (const auto& merges : small_tables())
    {
        const auto tower{merge_tower(merges)};

        for (std::size_t trial{0}; trial < 4; ++trial)
        {
            const auto text{random_text(sequence, 32)};

            for (std::size_t budget{1}; budget <= 3; ++budget)
            {
                const Plan_request request{.chunks = text.size(), .budget = budget, .supplied = {}};

                const auto plan{chunk_plan(tower, text, request)};

                const auto context{std::format("{} budget {}", text, budget)};

                expect_true_cuts(merges, text, plan, budget, context);

                for (const auto& [begin, owned_end, read_end, evidence] : plan.chunks)
                {
                    if (std::holds_alternative<Certified_window>(evidence))
                    {
                        EXPECT_GE(std::get<Certified_window>(evidence).begin, begin) << context;
                    }
                }
            }
        }
    }
}

TEST(Chunk_plan_test, The_two_routes_cut_alike_where_the_tower_takes_every_window_the_automaton_takes)
{
    // Over (a, b) and (ab, a) every certified cut of the tower is independent, so the routes differ only by the
    // spacing.
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto tower{merge_tower(merges)};
    const auto automaton{token_automaton(merges)};

    const std::string text{"abaabbabaababbaaab"};

    const Plan_request request{.chunks = 3, .budget = 3, .supplied = {}};

    const auto on_tower{chunk_plan(tower, text, request)};
    const auto on_automaton{chunk_plan(automaton, text, request, std::nullopt)};

    ASSERT_EQ(on_tower.chunks.size(), on_automaton.chunks.size());

    for (std::size_t index{0}; index < on_tower.chunks.size(); ++index)
    {
        EXPECT_EQ(on_tower.chunks[index].owned_end, on_automaton.chunks[index].owned_end) << index;
    }
}

TEST(Chunk_plan_test, Runs_of_a_are_cut_by_the_uniform_window_alone_under_a_table_certifying_none_of_their_windows)
{
    // Over (a, b) and (a, ab) no window of a's is certified at its last byte, while every window of four bytes pins its
    // byte 2, the origin radius being 2 and the right start radius 1.
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "a", .right = "ab"}};

    const auto automaton{token_automaton(merges)};

    const std::string text(64, 'a');

    const Plan_request request{.chunks = 8, .budget = 6, .supplied = {}};

    const auto without{chunk_plan(automaton, text, request, std::nullopt)};

    EXPECT_EQ(without.chunks.size(), 1U);

    const auto radii{radii_of(merges)};

    ASSERT_TRUE(radii);
    EXPECT_EQ(radii->radius, 2U);
    EXPECT_EQ(radii->start_radius, 1U);

    const auto with{chunk_plan(automaton, text, request, radii)};

    EXPECT_EQ(with.chunks.size(), 8U);

    for (std::size_t index{0}; index + 1 < with.chunks.size(); ++index)
    {
        const auto& [begin, owned_end, read_end, evidence]{with.chunks[index]};

        ASSERT_TRUE(std::holds_alternative<Uniform_window>(evidence)) << index;

        const auto& [window_begin, width, byte, start]{std::get<Uniform_window>(evidence)};

        EXPECT_EQ(width, 4U) << index;
        EXPECT_EQ(byte, 2U) << index;
        EXPECT_EQ(owned_end, (index + 1) * 8) << index;
    }

    expect_true_cuts(merges, text, with, request.budget, "a^64 with the radii");
}

TEST(Chunk_plan_test, A_supplied_cut_is_taken_at_its_byte_with_its_context_read_past_it)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}};

    const auto automaton{token_automaton(merges)};

    const std::vector<Supplied_cut> supplied{
            {.position = 3, .context = 2},
            {.position = 5, .context = 1},
            {.position = 9, .context = 5}};

    const Plan_request request{.chunks = 3, .budget = 0, .supplied = supplied};

    const auto plan{chunk_plan(automaton, "aaaaaaaaaaaa", request, std::nullopt)};

    ASSERT_EQ(plan.chunks.size(), 3U);

    const auto& [first_begin, first_end, first_read, first_evidence]{plan.chunks[0]};

    // The targets are 4 and 8, so the cuts at 5 and 9 are taken, the second reading to the input's end.
    EXPECT_EQ(first_end, 5U);
    EXPECT_EQ(first_read, 6U);
    EXPECT_TRUE(std::holds_alternative<Supplied_cut>(first_evidence));

    const auto& [second_begin, second_end, second_read, second_evidence]{plan.chunks[1]};

    EXPECT_EQ(second_end, 9U);
    EXPECT_EQ(second_read, 12U);
    EXPECT_EQ(plan.windows_asked, 0U);
}

TEST(Chunk_plan_test, A_repeated_window_is_decided_once_and_a_cut_before_the_floor_is_not_widened)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto automaton{token_automaton(merges)};

    std::string text{};

    for (std::size_t repetitions{0}; repetitions < 32; ++repetitions)
    {
        text += "abb";
    }

    const auto plan{chunk_plan(automaton, text, {.chunks = text.size(), .budget = 4, .supplied = {}}, std::nullopt)};

    EXPECT_LT(plan.windows_decided, 13U);
    EXPECT_GT(plan.windows_asked, 3 * plan.windows_decided);

    expect_true_cuts(merges, text, plan, 4, "(abb)^32");
}

TEST(Chunk_plan_test, Refuses_a_token_automaton_over_the_characters_of_an_alphabet)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}};

    const auto automaton{token_automaton(merges, "ab")};

    const Plan_request request{.chunks = 4, .budget = 3, .supplied = {}};

    std::string refusal{};

    try
    {
        std::ignore = chunk_plan(automaton, "abababababababab", request, std::nullopt);
    }
    catch (const std::invalid_argument& error)
    {
        refusal = error.what();
    }

    EXPECT_TRUE(refusal.contains("chunk_plan: the token automaton reads 2 symbols")) << refusal;
}

TEST(Chunk_plan_test, Each_chunk_of_a_piece_set_segmented_alone_by_exact_Viterbi_gives_the_whole_input_its_optimum)
{
    std::mt19937 sequence{0x5EEDU};

    std::uniform_int_distribution<std::size_t> count{2, 7};
    std::uniform_int_distribution<std::size_t> length{1, 4};
    std::uniform_int_distribution<long> score{-6, -1};

    for (std::size_t trial{0}; trial < 200; ++trial)
    {
        std::vector<std::string> pieces{"a", "b", "c"};

        const auto wanted{count(sequence)};

        for (std::size_t index{0}; index < wanted; ++index)
        {
            auto piece{random_text(sequence, length(sequence))};

            if (!std::ranges::contains(pieces, piece))
            {
                pieces.push_back(std::move(piece));
            }
        }

        std::ranges::shuffle(pieces, sequence);

        // Scores from a narrow range tie often, and the pass breaks every tie on the whole input as on its chunks.
        std::vector<long> scores{};

        for (std::size_t piece{0}; piece < pieces.size(); ++piece)
        {
            scores.push_back(score(sequence));
        }

        const auto set{piece_set(pieces)};

        const auto text{random_text(sequence, 60)};

        for (const auto chunks : {std::size_t{3}, std::size_t{8}, text.size()})
        {
            const Plan_request request{.chunks = chunks, .budget = 0, .supplied = {}};

            const auto context{std::format("trial {} {} chunks {}", trial, text, chunks)};

            expect_optimal_chunks(pieces, scores, text, chunk_plan(set, text, request), context);
        }
    }
}

TEST(Chunk_plan_test, A_piece_set_cuts_only_where_no_piece_straddles_and_scans_a_stretch_without_a_cut_whole)
{
    const std::vector<std::string> pieces{"a", "aa", "b"};

    const auto set{piece_set(pieces)};

    // Every interior position of a run of a's lies inside an occurrence of aa, so the run holds no cut.
    const std::string run(16, 'a');

    const auto whole{chunk_plan(set, run, {.chunks = 4, .budget = 4, .supplied = {}})};

    ASSERT_EQ(whole.chunks.size(), 1U);
    EXPECT_EQ(whole.chunks.front().owned_end, 16U);

    // The budget is not read: the cuts are the two sides of the b.
    const auto plan{chunk_plan(set, "aaaaabaaaa", {.chunks = 4, .budget = 4, .supplied = {}})};

    ASSERT_EQ(plan.chunks.size(), 3U);
    EXPECT_EQ(plan.chunks[0].owned_end, 5U);
    EXPECT_EQ(plan.chunks[1].owned_end, 6U);
    EXPECT_EQ(plan.chunks[2].owned_end, 10U);
    EXPECT_TRUE(std::holds_alternative<Straddle_free_cut>(plan.chunks[1].evidence));
}

TEST(Chunk_plan_test, A_supplied_cut_over_a_piece_set_comes_first_at_its_position_with_its_context_read_past_it)
{
    const std::vector<std::string> pieces{"a", "aa", "b"};

    const auto set{piece_set(pieces)};

    const std::vector<Supplied_cut> supplied{
            {.position = 2, .context = 1},
            {.position = 6, .context = 0},
            {.position = 8, .context = 3}};

    const auto plan{chunk_plan(set, "aaaaabaaaa", {.chunks = 4, .budget = 0, .supplied = supplied})};

    ASSERT_EQ(plan.chunks.size(), 4U);

    // The targets are 2, 5 and 7: the supplied cut at 2, the straddle-free 5 before the supplied 6, and the supplied 8.
    const auto& [first_begin, first_end, first_read, first_evidence]{plan.chunks[0]};

    EXPECT_EQ(first_end, 2U);
    EXPECT_EQ(first_read, 3U);
    EXPECT_TRUE(std::holds_alternative<Supplied_cut>(first_evidence));

    const auto& [second_begin, second_end, second_read, second_evidence]{plan.chunks[1]};

    EXPECT_EQ(second_end, 5U);
    EXPECT_EQ(second_read, 5U);
    EXPECT_TRUE(std::holds_alternative<Straddle_free_cut>(second_evidence));

    const auto& [third_begin, third_end, third_read, third_evidence]{plan.chunks[2]};

    EXPECT_EQ(third_end, 8U);
    EXPECT_EQ(third_read, 10U);
    EXPECT_TRUE(std::holds_alternative<Supplied_cut>(third_evidence));

    // At a position both give, the supplied cut is taken.
    const std::vector<Supplied_cut> at_five{{.position = 5, .context = 2}};

    const auto tied{chunk_plan(set, "aaaaabaaaa", {.chunks = 2, .budget = 0, .supplied = at_five})};

    ASSERT_EQ(tied.chunks.size(), 2U);
    EXPECT_EQ(tied.chunks[0].owned_end, 5U);
    EXPECT_EQ(tied.chunks[0].read_end, 7U);
    EXPECT_TRUE(std::holds_alternative<Supplied_cut>(tied.chunks[0].evidence));
}

#include "munch/verifier/piece_set.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "munch/verifier/verifier_decisions.hpp"

using namespace munch::verifier;

namespace
{
/**
 * @brief The interior boundaries of one segmentation, ascending.
 */
using Boundaries_t = std::vector<std::size_t>;

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
     * @brief The cuts lines.
     */
    std::size_t texts{0};

    /**
     * @brief The window lines.
     */
    std::size_t windows{0};

    /**
     * @brief The window lines whose cut is refused.
     */
    std::size_t refused{0};
};

/**
 * @brief Returns the bytes a lowercase hex field spells, two digits per byte, "-" spelling the empty text.
 * @param field The hex digits.
 * @return The bytes.
 */
std::string bytes_of(const std::string_view field)
{
    std::string bytes{};

    if (field == "-")
    {
        return bytes;
    }

    for (std::size_t at{0}; at + 1 < field.size(); at += 2)
    {
        unsigned value{};

        std::ignore = std::from_chars(field.data() + at, field.data() + at + 2, value, 16);

        bytes += static_cast<char>(value);
    }

    return bytes;
}

/**
 * @brief Returns every segmentation of a text into pieces, each as its interior boundaries, by extending every
 *        segmentation of each prefix with every piece that follows it.
 * @param pieces The pieces.
 * @param text The text.
 * @return The segmentations, none when the pieces cannot spell the text.
 */
std::vector<Boundaries_t> segmentations(const std::vector<std::string>& pieces, const std::string_view text)
{
    std::vector<std::vector<Boundaries_t>> ending(text.size() + 1);

    ending[0].emplace_back();

    for (std::size_t at{0}; at < text.size(); ++at)
    {
        for (const auto& piece : pieces)
        {
            if (!text.substr(at).starts_with(piece))
            {
                continue;
            }

            for (auto boundaries : ending[at])
            {
                if (at > 0)
                {
                    boundaries.push_back(at);
                }

                ending[at + piece.size()].push_back(std::move(boundaries));
            }
        }
    }

    return ending[text.size()];
}

/**
 * @brief Returns the interior positions of a text that no occurrence of a piece straddles, by searching every piece at
 *        every start.
 * @param pieces The pieces.
 * @param text The text.
 * @return The positions, ascending.
 */
std::vector<std::size_t> unstraddled(const std::vector<std::string>& pieces, const std::string_view text)
{
    std::vector<bool> straddled(text.size() + 1, false);

    for (const auto& piece : pieces)
    {
        for (auto at{text.find(piece)}; at != std::string_view::npos; at = text.find(piece, at + 1))
        {
            for (auto position{at + 1}; position < at + piece.size(); ++position)
            {
                straddled[position] = true;
            }
        }
    }

    std::vector<std::size_t> positions{};

    for (std::size_t position{1}; position < text.size(); ++position)
    {
        if (!straddled[position])
        {
            positions.push_back(position);
        }
    }

    return positions;
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
 * @brief Draws a set of up to seven distinct pieces of one to four letters over an alphabet, with every letter added
 *        as a piece of its own when asked.
 * @param sequence The sequence, advanced by the draw.
 * @param alphabet The letters.
 * @param with_letters Whether every letter is a piece.
 * @return The pieces, in drawn order.
 */
std::vector<std::string> random_pieces(std::mt19937& sequence, const std::string_view alphabet, const bool with_letters)
{
    std::uniform_int_distribution<std::size_t> count{2, 7};
    std::uniform_int_distribution<std::size_t> length{1, 4};
    std::uniform_int_distribution<std::size_t> letter{0, alphabet.size() - 1};

    std::set<std::string> drawn{};

    const auto wanted{count(sequence)};

    for (std::size_t index{0}; index < wanted; ++index)
    {
        std::string piece{};

        const auto bytes{length(sequence)};

        for (std::size_t at{0}; at < bytes; ++at)
        {
            piece += alphabet[letter(sequence)];
        }

        drawn.insert(piece);
    }

    if (with_letters)
    {
        for (const auto byte : alphabet)
        {
            drawn.insert(std::string{byte});
        }
    }

    std::vector<std::string> pieces{drawn.begin(), drawn.end()};

    std::ranges::shuffle(pieces, sequence);

    return pieces;
}

/**
 * @brief Draws a text over an alphabet.
 * @param sequence The sequence, advanced by the draw.
 * @param alphabet The letters.
 * @param length The text's length.
 * @return The text.
 */
std::string random_text(std::mt19937& sequence, const std::string_view alphabet, const std::size_t length)
{
    std::uniform_int_distribution<std::size_t> letter{0, alphabet.size() - 1};

    std::string text{};

    for (std::size_t index{0}; index < length; ++index)
    {
        text += alphabet[letter(sequence)];
    }

    return text;
}

/**
 * @brief Returns every string over an alphabet of at most a length, the empty one first.
 * @param alphabet The letters.
 * @param longest The longest length.
 * @return The strings, shorter ones first.
 */
std::vector<std::string> strings_over(const std::string_view alphabet, const std::size_t longest)
{
    std::vector<std::string> strings{""};

    for (std::size_t from{0}; from < strings.size(); ++from)
    {
        if (strings[from].size() == longest)
        {
            continue;
        }

        for (const auto byte : alphabet)
        {
            strings.push_back(strings[from] + byte);
        }
    }

    return strings;
}

/**
 * @brief Returns the message piece_set() or piece_cut() refuses with, or the empty text when it answers.
 * @param pieces The pieces.
 * @param window The window, or std::nullopt to build the set alone.
 * @param offset The offset of the cut in the window.
 * @return The refusal.
 */
std::string refusal_of(
        const std::vector<std::string>& pieces, const std::optional<std::string_view> window = std::nullopt,
        const std::size_t offset = 0)
{
    try
    {
        const auto set{piece_set(pieces)};

        if (window)
        {
            std::ignore = piece_cut(set, *window, offset);
        }
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Checks a cuts line: the straddle-free positions of the text are the fixture's.
 * @param set The universe's piece set.
 * @param fields The line past its kind.
 * @param context The fixture line checked.
 */
void expect_cuts(const Piece_set& set, std::istringstream& fields, const std::string& context)
{
    std::string text_field{};

    fields >> text_field;

    std::vector<std::size_t> expected{};

    for (std::size_t position{}; fields >> position;)
    {
        expected.push_back(position);
    }

    EXPECT_EQ(set.straddle_free(bytes_of(text_field)), expected) << context;
}

/**
 * @brief Returns the tokens of a marked string, each run of bytes up to a boundary or the end.
 * @param segmentation The marked string.
 * @return The tokens, in order.
 */
std::vector<std::string> tokens_of(const Marked_string& segmentation)
{
    const auto& [bytes, boundaries]{segmentation};

    std::vector<std::string> tokens{""};

    for (std::size_t at{0}; at < bytes.size(); ++at)
    {
        tokens.back() += bytes[at];

        if (boundaries[at] && at + 1 < bytes.size())
        {
            tokens.emplace_back();
        }
    }

    return tokens;
}

/**
 * @brief Checks that a refutation of a cut replays: its string holds the window at its occurrence, its tokens are
 *        pieces of the set spelling the string, and no boundary of them lies at the cut.
 * @param set The piece set.
 * @param window The window.
 * @param offset The cut.
 * @param refutation The refutation piece_cut() gave.
 * @param context The check's context.
 */
void expect_replays(
        const Piece_set& set, const std::string_view window, const std::size_t offset, const Straddling& refutation,
        const std::string& context)
{
    const auto& [segmentation, occurrence]{refutation};
    const auto& [bytes, boundaries]{segmentation};

    ASSERT_EQ(boundaries.size(), bytes.size()) << context;
    ASSERT_LE(occurrence + window.size(), bytes.size()) << context;
    EXPECT_EQ(std::string_view{bytes}.substr(occurrence, window.size()), window) << context;
    EXPECT_FALSE(boundaries.back()) << context;
    EXPECT_FALSE(boundaries[occurrence + offset - 1]) << context;

    for (const auto& token : tokens_of(segmentation))
    {
        EXPECT_TRUE(std::ranges::contains(set.pieces(), token)) << context;
    }
}

/**
 * @brief Checks a window line: piece_cut() gives the fixture's verdict, a certified cut is straddle-free in the window
 *        itself, and a refused one's refutation is the fixture's witness, its text, the window's start in it and its
 *        tokens, and replays.
 * @param set The universe's piece set.
 * @param fields The line past its kind.
 * @param context The fixture line checked.
 */
void expect_window(const Piece_set& set, std::istringstream& fields, const std::string& context)
{
    std::string window_field{};
    std::size_t offset{};
    std::string verdict{};

    fields >> window_field >> offset >> verdict;

    const auto window{bytes_of(window_field)};

    const auto refuted{piece_cut(set, window, offset)};

    ASSERT_EQ(refuted.has_value(), verdict == "refused") << context;

    if (!refuted)
    {
        EXPECT_TRUE(std::ranges::binary_search(set.straddle_free(window), offset)) << context;

        return;
    }

    std::string text_field{};
    std::size_t at{};
    std::string tokens_field{};

    fields >> text_field >> at >> tokens_field;

    std::vector<std::string> tokens{};

    for (const auto token_field : std::views::split(tokens_field, ','))
    {
        tokens.push_back(bytes_of(std::string_view{token_field}));
    }

    const auto& [segmentation, occurrence]{*refuted};

    EXPECT_EQ(segmentation.bytes, bytes_of(text_field)) << context;
    EXPECT_EQ(occurrence, at) << context;
    EXPECT_EQ(tokens_of(segmentation), tokens) << context;

    expect_replays(set, window, offset, *refuted, context);
}

} // namespace

TEST(Piece_set_test, The_set_and_the_decision_give_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/piece_reference.txt"};

    ASSERT_TRUE(file.is_open());

    std::optional<Piece_set> set{};

    Fixture_counts counts{};

    for (std::string line{}; std::getline(file, line);)
    {
        std::istringstream fields{line};

        std::string kind{};

        if (!(fields >> kind) || kind.starts_with('#'))
        {
            continue;
        }

        if (kind == "universe")
        {
            std::string name{};

            fields >> name;

            std::vector<std::string> pieces{};

            for (std::string piece{}; fields >> piece;)
            {
                pieces.push_back(bytes_of(piece));
            }

            set = piece_set(pieces);

            ++counts.universes;

            continue;
        }

        ASSERT_TRUE(set) << line;

        if (kind == "cuts")
        {
            expect_cuts(*set, fields, line);

            ++counts.texts;

            continue;
        }

        const auto refused{line.contains(" refused ")};

        expect_window(*set, fields, line);

        ++counts.windows;
        counts.refused += refused ? 1 : 0;
    }

    EXPECT_EQ(counts.universes, 62U);
    EXPECT_EQ(counts.texts, 558U);
    EXPECT_EQ(counts.windows, 6104U);
    EXPECT_EQ(counts.refused, 4916U);
}

TEST(Piece_set_test, Every_straddle_free_position_is_a_boundary_of_every_segmentation_and_of_the_best_one)
{
    std::mt19937 sequence{0x5EEDU};

    std::uniform_int_distribution<long> score{-12, -1};

    std::size_t segmented{0};

    for (std::size_t trial{0}; trial < 300; ++trial)
    {
        const auto alphabet{trial % 2 == 0 ? std::string_view{"ab"} : std::string_view{"abc"}};

        const auto pieces{random_pieces(sequence, alphabet, trial % 3 != 0)};

        const auto set{piece_set(pieces)};

        std::vector<long> scores{};

        for (std::size_t piece{0}; piece < pieces.size(); ++piece)
        {
            scores.push_back(score(sequence));
        }

        for (std::size_t length{0}; length <= 14; ++length)
        {
            const auto text{random_text(sequence, alphabet, length)};

            const auto cuts{set.straddle_free(text)};

            const auto context{std::format("trial {} text {}", trial, text)};

            ASSERT_EQ(cuts, unstraddled(pieces, text)) << context;

            for (const auto& boundaries : segmentations(pieces, text))
            {
                EXPECT_TRUE(std::ranges::includes(boundaries, cuts)) << context;

                ++segmented;
            }

            const auto best{viterbi(pieces, scores, text)};

            if (!best)
            {
                continue;
            }

            Boundaries_t ends{};

            std::size_t end{0};

            for (const auto& token : *best | std::views::take(best->size() - 1))
            {
                end += token.size();
                ends.push_back(end);
            }

            EXPECT_TRUE(std::ranges::includes(ends, cuts)) << context;
        }
    }

    EXPECT_GT(segmented, 10000U);
}

TEST(Piece_set_test, A_window_cut_is_certified_exactly_when_every_extension_by_up_to_L_minus_one_bytes_keeps_it)
{
    std::mt19937 sequence{0xC07U};

    for (std::size_t trial{0}; trial < 40; ++trial)
    {
        const auto alphabet{trial % 2 == 0 ? std::string_view{"ab"} : std::string_view{"abc"}};

        const auto pieces{random_pieces(sequence, alphabet, true)};

        const auto set{piece_set(pieces)};

        const auto sides{strings_over(alphabet, set.longest() - 1)};

        for (const auto& window : strings_over(alphabet, 4))
        {
            for (std::size_t offset{1}; offset < window.size(); ++offset)
            {
                const auto keeps{[&](const std::string& left, const std::string& right) {
                    const auto text{left + window + right};

                    const auto cut{left.size() + offset};

                    return std::ranges::all_of(segmentations(pieces, text), [cut](const Boundaries_t& boundaries) {
                        return std::ranges::binary_search(boundaries, cut);
                    });
                }};

                const auto every{std::ranges::all_of(sides, [&](const std::string& left) {
                    return std::ranges::all_of(sides, [&](const std::string& right) { return keeps(left, right); });
                })};

                const auto refuted{piece_cut(set, window, offset)};

                const auto context{std::format("trial {} {} {}", trial, window, offset)};

                EXPECT_EQ(!refuted, every) << context;

                if (refuted)
                {
                    expect_replays(set, window, offset, *refuted, context);
                }
            }
        }
    }
}

TEST(Piece_set_test, At_width_two_the_cut_is_the_pair_table_and_a_pair_no_piece_holds_certifies_any_window)
{
    const std::vector<std::string> pieces{"\xE2\x96\x81", "\xE2\x96\x81the", "the", "th", "he", "t", "h", "e"};

    const auto set{piece_set(pieces)};

    EXPECT_EQ(set.longest(), 6U);
    EXPECT_TRUE(set.has_pair('t', 'h'));
    EXPECT_FALSE(set.has_pair('e', 't'));
    EXPECT_TRUE(piece_cut(set, "th", 1));
    EXPECT_FALSE(piece_cut(set, "et", 1));
    EXPECT_FALSE(piece_cut(set, "theth", 3));

    // U+2581 straddles its own inner bytes, and no piece holds e followed by its first byte.
    EXPECT_TRUE(piece_cut(set, "\xE2\x96\x81", 1));
    EXPECT_FALSE(piece_cut(set, "e\xE2\x96\x81", 1));
    EXPECT_TRUE(piece_cut(set, "\xE2\x96\x81t", 3));

    EXPECT_EQ(set.straddle_free("the\xE2\x96\x81the"), (std::vector<std::size_t>{3}));
    EXPECT_EQ(set.straddle_free("eth"), (std::vector<std::size_t>{1}));
}

TEST(Piece_set_test, A_wide_window_is_decided_by_the_pieces_inside_it_and_a_narrow_one_by_the_pieces_hanging_out)
{
    const std::vector<std::string> pieces{"a", "b", "abba"};

    const auto set{piece_set(pieces)};

    // With L - 1 = 3 bytes on each side only a piece inside the window can straddle the cut.
    EXPECT_FALSE(piece_cut(set, "aabbbb", 3));
    EXPECT_TRUE(piece_cut(set, "aabbab", 3));

    // bb at its middle could be the middle of abba and ab its start, while abba agrees with neither bbb nor abbb there.
    EXPECT_TRUE(piece_cut(set, "bb", 1));
    EXPECT_TRUE(piece_cut(set, "ab", 1));
    EXPECT_FALSE(piece_cut(set, "bbb", 1));
    EXPECT_FALSE(piece_cut(set, "abbb", 2));
}

TEST(Piece_set_test, Refuses_an_empty_piece_a_piece_held_twice_an_empty_window_and_an_offset_outside_it)
{
    EXPECT_EQ(refusal_of({"a", ""}), "piece_set: the piece at index 1 is empty");
    EXPECT_EQ(refusal_of({"ab", "b", std::string{"a\0b", 3}, "ab"}), R"(piece_set: the pieces carry "ab" twice)");
    EXPECT_EQ(refusal_of({"\x01\"", "\x01\""}), R"(piece_set: the pieces carry "\x01\"" twice)");

    const std::string outside{"piece_cut: the window is empty or its offset does not lie between two of its bytes"};

    EXPECT_EQ(refusal_of({"a"}, "", 0), outside);
    EXPECT_EQ(refusal_of({"a"}, "ab", 0), outside);
    EXPECT_EQ(refusal_of({"a"}, "ab", 2), outside);
    EXPECT_TRUE(refusal_of({"a"}, "ab", 1).empty());
    EXPECT_TRUE(refusal_of({}, "ab", 1).empty());
}

#include "munch/verifier/ripple_radius.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
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
 * @brief How a test reads a text: as its bytes, the symbols of the byte alphabet, or as its UTF-8 characters.
 */
enum class Reading : std::uint8_t
{
    /**
     * @brief Each byte a symbol.
     */
    bytes,

    /**
     * @brief Each UTF-8 character a symbol.
     */
    characters
};

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

    /**
     * @brief The characters lines.
     */
    std::size_t in_characters{0};
};

/**
 * @brief The reference fixture under check: how it reads its texts, the current universe's table, alphabet and line,
 *        and the counts so far.
 */
struct Fixture_check
{
    /**
     * @brief How the fixture's texts are read, by bytes or by characters.
     */
    Reading reading{};

    /**
     * @brief The current universe's merge table in rank order.
     */
    std::vector<Merge> merges{};

    /**
     * @brief The current universe's alphabet, UTF-8, in a fixture read by characters.
     */
    std::string alphabet{};

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
 * @brief Returns the bytes of a text, each a symbol spelled by itself.
 * @param text The text.
 * @return The bytes, in order.
 */
std::vector<std::string> bytes_of(const std::string_view text)
{
    std::vector<std::string> bytes{};

    for (const auto byte : text)
    {
        bytes.push_back(std::string{byte});
    }

    return bytes;
}

/**
 * @brief Returns the characters of a UTF-8 text, each spelled by its bytes: the text split before every byte that is
 *        not a continuation byte.
 * @param text The text.
 * @return The characters, in order.
 */
std::vector<std::string> characters_of(const std::string_view text)
{
    std::vector<std::string> characters{};

    for (const auto byte : text)
    {
        constexpr unsigned char continuation_mask{0xC0};
        constexpr unsigned char continuation_bits{0x80};

        const auto value{static_cast<unsigned char>(byte)};

        const auto continues{(value & continuation_mask) == continuation_bits};

        if (continues && !characters.empty())
        {
            characters.back() += byte;

            continue;
        }

        characters.push_back(std::string{byte});
    }

    return characters;
}

/**
 * @brief Returns the symbols of a text as a reading takes them, its bytes or its characters.
 * @param reading How the text is read.
 * @param text The text.
 * @return The symbols, in order.
 */
std::vector<std::string> symbols_of(const Reading reading, const std::string_view text)
{
    return reading == Reading::characters ? characters_of(text) : bytes_of(text);
}

/**
 * @brief Returns the token sequence canonical byte-pair encoding produces for a text given as its symbols: at every
 *        step the lowest-rank merge present anywhere is applied at its leftmost occurrence.
 * @param merges The merge table in rank order.
 * @param sequence The text's symbols, its bytes or its characters.
 * @return The tokens.
 */
std::vector<std::string> reference_tokens(const std::vector<Merge>& merges, std::vector<std::string> sequence)
{
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
 * @brief Returns the origin ripple and the mirrored quantity of a string prepended to an input, in bytes, by the
 *        reference encoder over the symbols a reading takes: the input's covering spans against those of the prepended
 *        input shifted left by the string's length.
 * @param merges The merge table in rank order.
 * @param reading How the texts are read, by bytes or by characters.
 * @param prepended The prepended string.
 * @param input The input.
 * @return The two measures.
 */
Ripple ripple_of(
        const std::vector<Merge>& merges, const Reading reading, const std::string_view prepended,
        const std::string_view input)
{
    const auto shift{static_cast<std::ptrdiff_t>(prepended.size())};

    const auto base_tokens{reference_tokens(merges, symbols_of(reading, input))};

    const auto base{covering_spans(base_tokens, 0)};

    const auto whole_text{std::string{prepended} + std::string{input}};

    const auto whole_tokens{reference_tokens(merges, symbols_of(reading, whole_text))};

    const auto whole{covering_spans(whole_tokens, -shift)};

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
 * @param reading How the texts are read, by bytes or by characters.
 * @param finite The radius and the mirrored constant with their witnesses.
 * @param context The fixture line or test checked.
 */
void expect_attained(
        const std::vector<Merge>& merges, const Reading reading, const Finite_ripple& finite,
        const std::string& context)
{
    const auto& [radius, radius_witness, mirrored, mirrored_witness]{finite};

    ASSERT_EQ(radius_witness.has_value(), radius != 0) << context;
    ASSERT_EQ(mirrored_witness.has_value(), mirrored != 0) << context;

    if (radius_witness)
    {
        const auto& [symbol, input]{*radius_witness};

        const auto [radius_reached, radius_mirror]{ripple_of(merges, reading, symbol, input)};

        EXPECT_EQ(radius_reached, radius) << context << " on " << input;
    }

    if (mirrored_witness)
    {
        const auto& [symbol, input]{*mirrored_witness};

        const auto [mirror_origin, mirror_reached]{ripple_of(merges, reading, symbol, input)};

        EXPECT_EQ(mirror_reached, mirrored) << context << " on " << input;
    }
}

/**
 * @brief Checks that a merge is an effective self-pair whose ripple the reference encoder makes unbounded: its parts
 *        equal, the merges before it encoding the two copies as two tokens, and prepending the part to an even power of
 *        it changing the covering token of every byte.
 * @param merges The merge table in rank order.
 * @param reading How the texts are read, by bytes or by characters.
 * @param rank The merge's rank.
 * @param context The fixture line or test checked.
 */
void expect_effective_self_pair(
        const std::vector<Merge>& merges, const Reading reading, const std::size_t rank, const std::string& context)
{
    ASSERT_LT(rank, merges.size()) << context;

    const auto& [left, right]{merges[rank]};

    ASSERT_EQ(left, right) << context;

    const std::vector<Merge> before{merges.begin(), merges.begin() + static_cast<std::ptrdiff_t>(rank)};

    const auto encoding{reference_tokens(before, symbols_of(reading, left + right))};

    EXPECT_EQ(encoding, (std::vector{left, right})) << context;

    std::string input{};

    for (std::size_t half{1}; half <= 3; ++half)
    {
        input += left + left;

        const auto [origin, mirrored]{ripple_of(merges, reading, left, input)};

        EXPECT_EQ(origin, input.size()) << context << " on " << input;
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
 * @brief Returns the message ripple_radius() over an alphabet of characters refuses a table with, or the empty text
 *        when it takes the table.
 * @param merges The merge table in rank order.
 * @param alphabet The alphabet, UTF-8.
 * @return The refusal.
 */
std::string refusal_over(const std::vector<Merge>& merges, const std::string_view alphabet)
{
    try
    {
        std::ignore = ripple_radius(merges, alphabet);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the decision on the current universe of a fixture: over its alphabet in a fixture read by characters,
 *        and over the bytes otherwise.
 * @param check The fixture under check.
 * @return The decision.
 */
Ripple_radius decided(const Fixture_check& check)
{
    if (check.reading == Reading::characters)
    {
        return ripple_radius(check.merges, check.alphabet);
    }

    return ripple_radius(check.merges);
}

/**
 * @brief Returns a decision written out whole, the verdict with every value and every witness, so that two decisions
 *        compare to the byte.
 * @param ripple The decision.
 * @return The text.
 */
std::string written(const Ripple_radius& ripple)
{
    if (std::holds_alternative<Effective_self_pair>(ripple))
    {
        const auto [rank]{std::get<Effective_self_pair>(ripple)};

        return std::format("infinite {}", rank);
    }

    const auto& [radius, radius_witness, mirrored, mirrored_witness]{std::get<Finite_ripple>(ripple)};

    const auto witness_text{[](const std::optional<Prepended_input>& witness) {
        return witness ? std::format("{} {}", witness->symbol, witness->input) : std::string{"none"};
    }};

    const auto radius_text{witness_text(radius_witness)};

    const auto mirrored_text{witness_text(mirrored_witness)};

    return std::format("finite {} {} {} {}", radius, mirrored, radius_text, mirrored_text);
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
 * @brief Starts a universe line's universe: reads its alphabet in a fixture read by characters, and its merges, each a
 *        left and a right part joined by a comma.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_universe(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    if (check.reading == Reading::characters)
    {
        fields >> check.alphabet;
    }

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

    const auto over_characters{check.reading == Reading::characters};

    const auto refusal{over_characters ? refusal_over(check.merges, check.alphabet) : refusal_of(check.merges)};

    ++check.counts.refusals;

    const auto expected{std::format(" at rank {} {} ", rank, reason)};

    EXPECT_TRUE(refusal.contains(expected)) << context << ": " << refusal;
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

    const auto found{decided(check)};

    ++check.counts.finite;

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found)) << context;

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, radius) << context;
    EXPECT_EQ(finite.mirrored, mirrored) << context;

    expect_attained(check.merges, check.reading, finite, context);

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

    expect_attained(check.merges, check.reading, reference, context);
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
    std::string symbol{};
    std::string stem{};
    std::string loop{};
    std::string exit{};
    std::string tail{};

    fields >> rank >> symbol >> stem >> loop >> exit >> tail;

    const auto found{decided(check)};

    ++check.counts.infinite;

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found)) << context;
    const auto [found_rank]{std::get<Effective_self_pair>(found)};

    EXPECT_EQ(found_rank, rank) << context;

    expect_effective_self_pair(check.merges, check.reading, rank, context);

    const auto ending{text_of(exit) + text_of(tail)};

    std::string pumped{text_of(stem)};

    const auto [first_origin, first_mirrored]{ripple_of(check.merges, check.reading, symbol, pumped + ending)};

    std::size_t previous{first_origin};

    for (std::size_t repetitions{1}; repetitions <= 3; ++repetitions)
    {
        pumped += text_of(loop);

        const auto input{pumped + ending};

        const auto [ripple, mirrored]{ripple_of(check.merges, check.reading, symbol, input)};

        EXPECT_EQ(ripple, previous + text_of(loop).size()) << context << " pumped " << repetitions << " times";

        previous = ripple;
    }
}

/**
 * @brief Checks a characters line: the decision's radius in bytes between the radius r counted in characters times
 *        the shortest and times the longest of the characters the table names, and over characters of one length k the
 *        radius k r and a mirrored constant m > 0 counted in characters k (m - 1) + 1 in bytes.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_characters(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t radius{};
    std::size_t mirrored{};

    fields >> radius >> mirrored;

    const auto found{decided(check)};

    ++check.counts.in_characters;

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found)) << context;

    const auto& finite{std::get<Finite_ripple>(found)};

    std::vector<std::size_t> lengths{};

    for (const auto& [left, right] : check.merges)
    {
        const auto characters{characters_of(left + right)};

        for (const auto& character : characters)
        {
            lengths.push_back(character.size());
        }
    }

    const auto [shortest, longest]{std::ranges::minmax(lengths)};

    EXPECT_LE(shortest * radius, finite.radius) << context;
    EXPECT_LE(finite.radius, longest * radius) << context;

    if (shortest != longest)
    {
        return;
    }

    const auto shifted{mirrored == 0 ? 0 : shortest * (mirrored - 1) + 1};

    EXPECT_EQ(finite.radius, shortest * radius) << context;
    EXPECT_EQ(finite.mirrored, shifted) << context;
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
    else if (kind == "characters")
    {
        expect_characters(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

/**
 * @brief The alphabet of the SentencePiece-style character table: U+2581 for the space, as SentencePiece writes it, and
 *        characters of one to four UTF-8 bytes.
 */
constexpr std::string_view sentencepiece_alphabet{"▁thecaéß中文😀"};

/**
 * @brief Returns the merges of the SentencePiece-style character table, in rank order: words begun by U+2581, and
 *        merges joining characters of every UTF-8 length.
 * @return The merge table.
 */
std::vector<Merge> sentencepiece_merges()
{
    return {{.left = "▁", .right = "t"},    {.left = "h", .right = "e"},   {.left = "▁t", .right = "he"},
            {.left = "c", .right = "a"},    {.left = "é", .right = "t"},   {.left = "中", .right = "文"},
            {.left = "▁", .right = "中文"}, {.left = "😀", .right = "😀"}, {.left = "ß", .right = "e"},
            {.left = "ca", .right = "ét"}};
}

/**
 * @brief Returns the ASCII alphabet, the characters U+0000 to U+007F in order, as UTF-8 text.
 * @return The alphabet.
 */
std::string ascii_alphabet()
{
    constexpr std::size_t ascii_count{0x80};

    std::string alphabet{};

    for (std::size_t byte{0}; byte < ascii_count; ++byte)
    {
        alphabet += static_cast<char>(byte);
    }

    return alphabet;
}

/**
 * @brief Returns the CJK ideograph an offset past U+4E00, spelled in UTF-8 as its three bytes.
 * @param offset The offset, below 0x1000.
 * @return The character.
 */
std::string ideograph(const std::size_t offset)
{
    const auto code_point{0x4E00U + offset};

    const auto first{static_cast<char>(0xE0U | (code_point >> 12U))};
    const auto second{static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU))};
    const auto third{static_cast<char>(0x80U | (code_point & 0x3FU))};

    return {first, second, third};
}

/**
 * @brief Returns the Fibonacci table of a number of merges over two symbols: each merge joins the two latest words, the
 *        first symbol then the second and then each word the one before it followed by itself.
 * @param first The first symbol.
 * @param second The second symbol.
 * @param count The number of merges.
 * @return The merge table in rank order.
 */
std::vector<Merge> fibonacci_merges(const std::string& first, const std::string& second, const std::size_t count)
{
    std::vector words{first, second};
    std::vector<Merge> merges{};

    for (std::size_t rank{0}; rank < count; ++rank)
    {
        const auto& left{words[words.size() - 2]};
        const auto& right{words.back()};

        merges.push_back({.left = left, .right = right});
        words.push_back(left + right);
    }

    return merges;
}

} // namespace

TEST(Ripple_radius_test, One_merge_over_a_is_infinite_at_its_own_rank)
{
    const std::vector<Merge> merges{{.left = "a", .right = "a"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));
    const auto [rank]{std::get<Effective_self_pair>(found)};

    EXPECT_EQ(rank, 0U);

    expect_effective_self_pair(merges, Reading::bytes, 0, "one merge over a");
}

TEST(Ripple_radius_test, A_proper_ordered_table_moves_every_covering_token_of_b_followed_by_ab_repeated)
{
    const std::vector<Merge> merges{{.left = "a", .right = "b"}, {.left = "ab", .right = "ab"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));
    const auto [rank]{std::get<Effective_self_pair>(found)};

    EXPECT_EQ(rank, 1U);

    std::string input{"b"};

    for (std::size_t repetitions{1}; repetitions <= 6; ++repetitions)
    {
        input += "ab";

        const auto [origin, mirrored]{ripple_of(merges, Reading::bytes, "a", input)};

        EXPECT_EQ(origin, input.size()) << input;
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

    EXPECT_EQ(reference_tokens(before, bytes_of("baba")), (std::vector<std::string>{"b", "ab", "a"}));

    expect_attained(merges, Reading::bytes, finite, "ab, ba, ba+ba");
}

TEST(Ripple_radius_test, The_mirrored_constant_lies_below_the_radius_when_the_last_changed_token_begins_early)
{
    const std::vector<Merge> merges{{.left = "b", .right = "c"}, {.left = "a", .right = "bc"}};

    const auto found{ripple_radius(merges)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, 2U);
    EXPECT_EQ(finite.mirrored, 1U);

    const auto [origin, mirror]{ripple_of(merges, Reading::bytes, "a", "bc")};

    EXPECT_EQ(origin, 2U);
    EXPECT_EQ(mirror, 1U);

    expect_attained(merges, Reading::bytes, finite, "bc, a+bc");
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
    std::vector<std::size_t> fibonacci{1, 1};

    for (std::size_t count{1}; count <= 8; ++count)
    {
        const auto merges{fibonacci_merges("a", "b", count)};

        while (fibonacci.size() < count + 3)
        {
            const auto next{fibonacci.back() + fibonacci[fibonacci.size() - 2]};

            fibonacci.push_back(next);
        }

        const auto found{ripple_radius(merges)};

        ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found)) << count;

        const auto& finite{std::get<Finite_ripple>(found)};

        EXPECT_EQ(finite.radius, fibonacci[count + 2] - 2) << count;

        const auto context{std::format("the Fibonacci table of {} merges", count)};

        expect_attained(merges, Reading::bytes, finite, context);
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
    const std::string nul{'\0'};

    const auto refusal{
            refusal_of({{.left = nul, .right = "a"}, {.left = R"(")", .right = R"(\)"}, {.left = nul, .right = "a"}})};

    EXPECT_TRUE(refusal.contains(R"(("\x00", "a") at rank 2 recreates the token "\x00a")")) << refusal;

    const auto missing{refusal_of({{.left = "\xFF", .right = R"("\)"}})};

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

TEST(Ripple_radius_test, Over_characters_the_radius_counts_the_bytes_of_every_character_whose_token_changes)
{
    const std::vector<Merge> merges{{.left = "é", .right = "t"}, {.left = "▁", .right = "ét"}};

    const auto found{ripple_radius(merges, "▁ét")};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& finite{std::get<Finite_ripple>(found)};

    // Prepending the three bytes of ▁ to ét, two characters and three bytes, makes the one token ▁ét.
    EXPECT_EQ(finite.radius, 3U);
    EXPECT_EQ(finite.mirrored, 1U);

    ASSERT_TRUE(finite.radius_witness);
    EXPECT_EQ(finite.radius_witness->symbol, "▁");

    expect_attained(merges, Reading::characters, finite, "é+t, ▁+ét");
}

TEST(Ripple_radius_test, On_a_sentencepiece_style_table_different_inputs_attain_the_radius_in_bytes_and_in_characters)
{
    auto merges{sentencepiece_merges()};

    const auto is_self_pair{[](const Merge& merge) { return merge.left == merge.right; }};

    std::erase_if(merges, is_self_pair);

    const auto found{ripple_radius(merges, sentencepiece_alphabet)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& finite{std::get<Finite_ripple>(found)};

    EXPECT_EQ(finite.radius, 6U);

    expect_attained(merges, Reading::characters, finite, "the SentencePiece-style table without its self-pair");

    // ▁ moves the six bytes of 中文, two characters, and the three bytes of the, three characters.
    const auto [ideographs, ideographs_mirrored]{ripple_of(merges, Reading::characters, "▁", "中文")};
    const auto [letters, letters_mirrored]{ripple_of(merges, Reading::characters, "▁", "the")};

    EXPECT_EQ(ideographs, 6U);
    EXPECT_EQ(letters, 3U);
}

TEST(Ripple_radius_test, The_sentencepiece_style_table_is_infinite_at_its_merge_of_two_copies_of_one_character)
{
    const auto merges{sentencepiece_merges()};

    const auto found{ripple_radius(merges, sentencepiece_alphabet)};

    ASSERT_TRUE(std::holds_alternative<Effective_self_pair>(found));
    const auto [rank]{std::get<Effective_self_pair>(found)};

    EXPECT_EQ(rank, 7U);

    expect_effective_self_pair(merges, Reading::characters, rank, "the SentencePiece-style table");
}

TEST(Ripple_radius_test, An_alphabet_of_more_characters_than_a_byte_holds_gives_three_times_the_radius_of_two_letters)
{
    constexpr std::size_t character_count{300};
    constexpr std::size_t merge_count{8};

    std::string alphabet{};

    for (std::size_t offset{0}; offset < character_count; ++offset)
    {
        alphabet += ideograph(offset);
    }

    const auto letters{fibonacci_merges("a", "b", merge_count)};

    const auto merges{fibonacci_merges(ideograph(0), ideograph(character_count - 1), merge_count)};

    const auto over_letters{ripple_radius(letters)};

    const auto found{ripple_radius(merges, alphabet)};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(over_letters));
    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& letter_ripple{std::get<Finite_ripple>(over_letters)};
    const auto& finite{std::get<Finite_ripple>(found)};

    // Every character three bytes long, the radius is three times the radius in characters, which the letters give.
    EXPECT_EQ(finite.radius, 3 * letter_ripple.radius);
    EXPECT_EQ(finite.mirrored, 3 * (letter_ripple.mirrored - 1) + 1);

    expect_attained(merges, Reading::characters, finite, "the Fibonacci table over two ideographs");
}

TEST(Ripple_radius_test, The_empty_table_over_the_empty_alphabet_has_radius_zero_and_no_witness)
{
    const auto found{ripple_radius({}, "")};

    ASSERT_TRUE(std::holds_alternative<Finite_ripple>(found));

    const auto& [radius, radius_witness, mirrored, mirrored_witness]{std::get<Finite_ripple>(found)};

    EXPECT_EQ(radius, 0U);
    EXPECT_EQ(mirrored, 0U);
    EXPECT_FALSE(radius_witness);
    EXPECT_FALSE(mirrored_witness);
}

TEST(Ripple_radius_test, Refuses_an_alphabet_holding_a_character_twice_and_a_merge_naming_a_character_outside_it)
{
    const auto twice{refusal_over({}, "éb\xC3\xA9")};

    EXPECT_TRUE(twice.contains(R"(the alphabet carries "\xC3\xA9" twice)")) << twice;

    const auto outside{refusal_over({{.left = "é", .right = "t"}, {.left = "ét", .right = "ü"}}, "ét")};

    EXPECT_TRUE(outside.contains(R"(("\xC3\xA9t", "\xC3\xBC") at rank 1 names "\xC3\xBC")")) << outside;
    EXPECT_TRUE(refusal_over({{.left = "é", .right = "t"}}, "ét").empty());
}

TEST(Ripple_radius_test, The_decision_over_characters_gives_the_reference_answers_on_every_universe_of_its_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/ripple_character_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{.reading = Reading::characters};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 5540U);
    EXPECT_EQ(check.counts.refusals, 1096U);
    EXPECT_EQ(check.counts.finite, 927U);
    EXPECT_EQ(check.counts.infinite, 3517U);
    EXPECT_EQ(check.counts.in_characters, 927U);
}

TEST(Ripple_radius_test, Over_the_characters_of_ascii_every_universe_of_the_fixture_is_decided_as_over_the_bytes)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/ripple_reference.txt"};

    ASSERT_TRUE(file.is_open());

    const auto alphabet{ascii_alphabet()};

    Fixture_check check{};

    std::size_t refused{0};

    for (std::string line{}; std::getline(file, line);)
    {
        std::istringstream fields{line};

        std::string kind{};

        if (!(fields >> kind) || kind != "universe")
        {
            continue;
        }

        start_universe(check, fields, line);

        const auto refusal{refusal_of(check.merges)};

        EXPECT_EQ(refusal_over(check.merges, alphabet), refusal) << line;

        if (!refusal.empty())
        {
            ++refused;

            continue;
        }

        const auto over_bytes{ripple_radius(check.merges)};

        const auto over_characters{ripple_radius(check.merges, alphabet)};

        EXPECT_EQ(written(over_characters), written(over_bytes)) << line;
    }

    EXPECT_EQ(check.counts.universes, 810U);
    EXPECT_EQ(refused, 80U);
}

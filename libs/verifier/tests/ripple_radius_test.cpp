#include "munch/verifier/ripple_radius.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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
 * @brief Where one copy of the spelling automaton is in the oracle's product: the token it has guessed, by its place in
 *        the canonical vocabulary, and how many of its bytes it has read, or the start, no token guessed.
 */
struct Copy_position
{
    /**
     * @brief Ordered member by member, so pairs key the map of interned ones.
     */
    auto operator<=>(const Copy_position&) const = default;

    /**
     * @brief The guessed token, none at the start.
     */
    std::optional<std::size_t> token{};

    /**
     * @brief The bytes of the token read, from one to its length.
     */
    std::size_t read{};
};

/**
 * @brief A node of the oracle's product: the copy reading the input alone and the copy that read the prepended symbol
 *        first.
 */
struct Copy_pair
{
    /**
     * @brief Ordered member by member, so pairs key the map of interned ones.
     */
    auto operator<=>(const Copy_pair&) const = default;

    /**
     * @brief The copy reading the input alone.
     */
    Copy_position base{};

    /**
     * @brief The copy that read the prepended symbol as the first symbol of its first token.
     */
    Copy_position prepended{};
};

/**
 * @brief A step of the oracle's product: the node it reaches, the byte read, the byte's offset in the base copy's
 *        token, and whether the two copies' tokens covering the byte differ.
 */
struct Product_step
{
    /**
     * @brief The node reached.
     */
    std::size_t target{};

    /**
     * @brief The byte read.
     */
    char byte{};

    /**
     * @brief The byte's offset in the base copy's token.
     */
    std::size_t offset{};

    /**
     * @brief Whether the two copies' tokens covering the byte differ in offset or length.
     */
    bool marked{};
};

/**
 * @brief A step of the oracle's product named by the node it leaves and its index among that node's steps.
 */
struct Product_arrival
{
    /**
     * @brief The node the step leaves.
     */
    std::size_t node{};

    /**
     * @brief The step's index among the node's steps.
     */
    std::size_t step{};
};

/**
 * @brief The overlap graph as Theorem 3 of the report defines it, the full product: every pair reached from the sources
 *        expanded, a pair ending both copies at a token end included, over the canonical tokens the reference encoder
 *        gives.
 */
struct Full_product
{
    /**
     * @brief The canonical tokens: the symbols the table names in alphabet order, then the products whose spelling the
     *        reference encoder encodes as themselves, in rank order.
     */
    std::vector<std::string> tokens{};

    /**
     * @brief The pair each node reads as, by node.
     */
    std::vector<Copy_pair> pairs{};

    /**
     * @brief The steps of each node, by node.
     */
    std::vector<std::vector<Product_step>> steps{};

    /**
     * @brief Whether each node ends both copies at a token end, by node.
     */
    std::vector<bool> accepting{};
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

    const auto pair_text{left + right};
    const auto encoding{reference_tokens(before, symbols_of(reading, pair_text))};

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
 * @brief Checks a characters line: the decision's radius in bytes between the radius r counted in characters times the
 *        shortest and times the longest of the characters the table names, and over characters of one length k the
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

/**
 * @brief Returns the symbols of an alphabet as a reading takes them: the 256 bytes in order over the bytes, and the
 *        alphabet's characters in order over the characters.
 * @param reading How the texts are read.
 * @param alphabet The alphabet, UTF-8, read over the characters only.
 * @return The symbols, each spelled by its bytes.
 */
std::vector<std::string> alphabet_symbols(const Reading reading, const std::string_view alphabet)
{
    if (reading == Reading::characters)
    {
        return characters_of(alphabet);
    }

    constexpr std::size_t byte_count{256};

    std::vector<std::string> symbols{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        symbols.push_back(std::string{static_cast<char>(byte)});
    }

    return symbols;
}

/**
 * @brief Returns the canonical tokens of a table: the symbols its merges name, in alphabet order, then each product the
 *        reference encoder encodes as itself, in rank order.
 * @param merges The merge table in rank order.
 * @param reading How the texts are read.
 * @param symbols The alphabet's symbols in order.
 * @return The tokens.
 */
std::vector<std::string> canonical_tokens(
        const std::vector<Merge>& merges, const Reading reading, const std::vector<std::string>& symbols)
{
    std::vector<std::string> named{};

    for (const auto& [left, right] : merges)
    {
        const auto parts{symbols_of(reading, left + right)};

        named.insert(named.end(), parts.begin(), parts.end());
    }

    std::vector<std::string> tokens{};

    for (const auto& symbol : symbols)
    {
        if (std::ranges::contains(named, symbol))
        {
            tokens.push_back(symbol);
        }
    }

    for (const auto& [left, right] : merges)
    {
        const auto product{left + right};

        const auto encoding{reference_tokens(merges, symbols_of(reading, product))};

        if (encoding == std::vector{product})
        {
            tokens.push_back(product);
        }
    }

    return tokens;
}

/**
 * @brief Builds the full product of the overlap graph: from every source, the start paired with a canonical token
 *        beginning with a named symbol of which the second copy has read that symbol, every pair reached is expanded by
 *        every byte, a copy inside its token reading the token's next byte and a copy at a token end beginning every
 *        canonical token the reference encoder lets follow the one it ended.
 * @param merges The merge table in rank order.
 * @param reading How the texts are read.
 * @param symbols The alphabet's symbols in order.
 * @return The product.
 */
Full_product full_product(
        const std::vector<Merge>& merges, const Reading reading, const std::vector<std::string>& symbols)
{
    Full_product product{
            .tokens = canonical_tokens(merges, reading, symbols),
            .pairs = {},
            .steps = {},
            .accepting = {}};

    const auto& tokens{product.tokens};

    std::map<Copy_pair, std::size_t> interned{};

    std::map<std::pair<std::size_t, std::size_t>, bool> adjacencies{};

    const auto at_end{[&tokens](const Copy_position& position) {
        return !position.token || position.read == tokens[*position.token].size();
    }};

    const auto intern{[&](const Copy_pair& pair) {
        const auto [entry, inserted]{interned.try_emplace(pair, product.pairs.size())};

        const auto& [key, node]{*entry};

        if (inserted)
        {
            product.pairs.push_back(pair);
            product.steps.emplace_back();
            product.accepting.push_back(at_end(pair.base) && at_end(pair.prepended));
        }

        return node;
    }};

    const auto adjacent{[&](const std::size_t first, const std::size_t second) {
        const auto [entry, inserted]{adjacencies.try_emplace({first, second}, false)};

        auto& [key, is_adjacent]{*entry};

        if (inserted)
        {
            const auto pair_text{tokens[first] + tokens[second]};

            const auto encoding{reference_tokens(merges, symbols_of(reading, pair_text))};

            is_adjacent = encoding == std::vector{tokens[first], tokens[second]};
        }

        return is_adjacent;
    }};

    // Each reading is the position after the byte, the byte's offset in its token and the token's length.
    const auto readings{[&](const Copy_position& position, const char byte) {
        std::vector<std::tuple<Copy_position, std::size_t, std::size_t>> found{};

        const auto& [token, read]{position};

        if (!at_end(position))
        {
            const auto& spelling{tokens[*token]};

            if (spelling[read] == byte)
            {
                found.emplace_back(Copy_position{.token = token, .read = read + 1}, read, spelling.size());
            }

            return found;
        }

        for (std::size_t next{0}; next < tokens.size(); ++next)
        {
            const auto& spelling{tokens[next]};

            if (spelling.front() != byte || (token && !adjacent(*token, next)))
            {
                continue;
            }

            found.emplace_back(Copy_position{.token = next, .read = 1}, 0, spelling.size());
        }

        return found;
    }};

    for (const auto& symbol : symbols)
    {
        if (!std::ranges::contains(tokens, symbol))
        {
            continue;
        }

        for (std::size_t token{0}; token < tokens.size(); ++token)
        {
            if (tokens[token].starts_with(symbol))
            {
                std::ignore = intern({.base = {}, .prepended = {.token = token, .read = symbol.size()}});
            }
        }
    }

    constexpr std::size_t byte_count{256};

    for (std::size_t node{0}; node < product.pairs.size(); ++node)
    {
        const auto [base, prepended]{product.pairs[node]};

        for (std::size_t value{0}; value < byte_count; ++value)
        {
            const auto byte{static_cast<char>(value)};

            for (const auto& [base_next, base_offset, base_length] : readings(base, byte))
            {
                for (const auto& [prepended_next, prepended_offset, prepended_length] : readings(prepended, byte))
                {
                    const auto target{intern({.base = base_next, .prepended = prepended_next})};

                    const auto marked{base_offset != prepended_offset || base_length != prepended_length};

                    product.steps[node].push_back(
                            {.target = target, .byte = byte, .offset = base_offset, .marked = marked});
                }
            }
        }
    }

    return product;
}

/**
 * @brief Returns the nodes of the product from which a path leads to the given ones, the given ones included.
 * @param product The product.
 * @param given Whether each node is given, by node.
 * @return Whether each node leads to a given one, by node.
 */
std::vector<bool> reaching(const Full_product& product, std::vector<bool> given)
{
    for (auto grown{true}; grown;)
    {
        grown = false;

        for (std::size_t node{0}; node < product.steps.size(); ++node)
        {
            const auto leads{[&given](const Product_step& step) { return given[step.target]; }};

            if (given[node] || !std::ranges::any_of(product.steps[node], leads))
            {
                continue;
            }

            given[node] = true;
            grown = true;
        }
    }

    return given;
}

/**
 * @brief Returns the bytes of a shortest path in the product from a node to a node ending both copies, the steps tried
 *        breadth first in their order.
 * @param product The product.
 * @param from The node, which leads to a node ending both copies.
 * @return The bytes read on the path.
 */
std::string product_completion(const Full_product& product, const std::size_t from)
{
    std::vector<std::optional<Product_arrival>> arrivals(product.steps.size());
    std::vector<bool> seen(product.steps.size(), false);
    std::vector<std::size_t> frontier{from};

    seen[from] = true;

    auto end{from};

    for (std::size_t head{0}; !product.accepting[end]; ++head)
    {
        const auto node{frontier[head]};

        const auto& steps{product.steps[node]};

        for (std::size_t index{0}; index < steps.size() && !product.accepting[end]; ++index)
        {
            const auto target{steps[index].target};

            if (seen[target])
            {
                continue;
            }

            seen[target] = true;
            arrivals[target] = Product_arrival{.node = node, .step = index};
            frontier.push_back(target);
            end = target;
        }
    }

    std::string bytes{};

    for (auto node{end}; node != from;)
    {
        const auto [previous, index]{*arrivals[node]};

        bytes += product.steps[previous][index].byte;
        node = previous;
    }

    std::ranges::reverse(bytes);

    return bytes;
}

/**
 * @brief Returns what the full product decides: none when its marked part, the nodes leading to a marked step into a
 *        node leading to one ending both copies, holds a cycle, and otherwise the radius and the mirrored constant read
 *        off the longest paths from the sources inside it, each with the prepended input the first step attaining it in
 *        node order gives, along the longest path found first and a shortest completion.
 * @param product The product.
 * @return The finite ripple, none when the radius is infinite.
 */
std::optional<Finite_ripple> product_decision(const Full_product& product)
{
    const auto node_count{product.steps.size()};

    const auto live{reaching(product, product.accepting)};

    std::vector<bool> marked(node_count, false);

    for (std::size_t node{0}; node < node_count; ++node)
    {
        const auto into_live{[&live](const Product_step& step) { return step.marked && live[step.target]; }};

        marked[node] = std::ranges::any_of(product.steps[node], into_live);
    }

    const auto marked_part{reaching(product, marked)};

    std::vector<std::size_t> incoming(node_count, 0);

    for (std::size_t node{0}; node < node_count; ++node)
    {
        for (const auto& [target, byte, offset, step_marked] : product.steps[node])
        {
            if (marked_part[node] && marked_part[target])
            {
                ++incoming[target];
            }
        }
    }

    std::vector<std::size_t> ready{};

    for (std::size_t node{0}; node < node_count; ++node)
    {
        if (marked_part[node] && !product.pairs[node].base.token)
        {
            ready.push_back(node);
        }
    }

    std::vector<std::size_t> depths(node_count, 0);
    std::vector<std::optional<Product_arrival>> arrivals(node_count);

    for (std::size_t head{0}; head < ready.size(); ++head)
    {
        const auto node{ready[head]};

        const auto& steps{product.steps[node]};

        for (std::size_t index{0}; index < steps.size(); ++index)
        {
            const auto target{steps[index].target};

            if (!marked_part[target])
            {
                continue;
            }

            if (depths[node] + 1 > depths[target])
            {
                depths[target] = depths[node] + 1;
                arrivals[target] = Product_arrival{.node = node, .step = index};
            }

            --incoming[target];

            if (incoming[target] == 0)
            {
                ready.push_back(target);
            }
        }
    }

    if (ready.size() != static_cast<std::size_t>(std::ranges::count(marked_part, true)))
    {
        return std::nullopt;
    }

    std::optional<Product_arrival> radius_step{};
    std::optional<Product_arrival> mirrored_step{};

    Finite_ripple finite{};

    for (std::size_t node{0}; node < node_count; ++node)
    {
        const auto& steps{product.steps[node]};

        for (std::size_t index{0}; index < steps.size() && marked_part[node]; ++index)
        {
            const auto& [target, byte, offset, step_marked]{steps[index]};

            if (!step_marked || !live[target])
            {
                continue;
            }

            const auto reach{depths[node] + 1};

            if (reach > finite.radius)
            {
                finite.radius = reach;
                radius_step = Product_arrival{.node = node, .step = index};
            }

            if (reach - offset > finite.mirrored)
            {
                finite.mirrored = reach - offset;
                mirrored_step = Product_arrival{.node = node, .step = index};
            }
        }
    }

    const auto witness{[&](const std::optional<Product_arrival>& attaining) -> std::optional<Prepended_input> {
        if (!attaining)
        {
            return std::nullopt;
        }

        const auto& [node, index]{*attaining};

        const auto& [target, byte, offset, step_marked]{product.steps[node][index]};

        std::string bytes{byte};

        auto source{node};

        for (auto arrival{arrivals[source]}; arrival; arrival = arrivals[source])
        {
            const auto [previous, previous_step]{*arrival};

            bytes += product.steps[previous][previous_step].byte;
            source = previous;
        }

        std::ranges::reverse(bytes);

        bytes += product_completion(product, target);

        const auto& [token, read]{product.pairs[source].prepended};

        return Prepended_input{.symbol = product.tokens[*token].substr(0, read), .input = bytes};
    }};

    finite.radius_witness = witness(radius_step);
    finite.mirrored_witness = witness(mirrored_step);

    return finite;
}

/**
 * @brief Checks a table's decision against the full product: an infinite radius exactly when the product's marked part
 *        holds a cycle, with the first effective self-pair replayed and pumped, and otherwise every value and witness
 *        equal to the product's to the byte, both witnesses replayed against the reference encoder.
 * @param merges The merge table in rank order, admissible.
 * @param reading How the texts are read.
 * @param alphabet The alphabet, UTF-8, read over the characters only.
 * @param context The table checked, for the failure messages.
 * @return True when the product finds the radius finite.
 */
bool expect_decided_as_the_full_product(
        const std::vector<Merge>& merges, const Reading reading, const std::string_view alphabet,
        const std::string& context)
{
    const auto product{full_product(merges, reading, alphabet_symbols(reading, alphabet))};

    const auto oracle{product_decision(product)};

    const auto found{reading == Reading::characters ? ripple_radius(merges, alphabet) : ripple_radius(merges)};

    if (!oracle)
    {
        const auto* const self_pair{std::get_if<Effective_self_pair>(&found)};

        EXPECT_TRUE(self_pair) << context << ": " << written(found);

        if (self_pair)
        {
            expect_effective_self_pair(merges, reading, self_pair->rank, context);
        }

        return false;
    }

    EXPECT_EQ(written(found), written(*oracle)) << context;

    if (const auto* const finite{std::get_if<Finite_ripple>(&found)})
    {
        expect_attained(merges, reading, *finite, context);
    }

    return true;
}

/**
 * @brief Returns a table written out as its merges, for the failure messages.
 * @param merges The merge table in rank order.
 * @return The text.
 */
std::string table_text(const std::vector<Merge>& merges)
{
    std::string text{};

    for (const auto& [left, right] : merges)
    {
        text += std::format("({}, {}) ", left, right);
    }

    return text;
}

/**
 * @brief Returns every admissible table of one to a number of merges over symbols: each merge's parts tokens the
 *        symbols or the merges before it carry, and its product one they do not.
 * @param symbols The symbols.
 * @param depth The largest number of merges.
 * @return The tables.
 */
std::vector<std::vector<Merge>> admissible_tables(const std::vector<std::string>& symbols, const std::size_t depth)
{
    std::vector<std::vector<Merge>> tables{};

    std::vector<std::vector<Merge>> layer{{}};

    for (std::size_t count{1}; count <= depth; ++count)
    {
        std::vector<std::vector<Merge>> next{};

        for (const auto& table : layer)
        {
            auto vocabulary{symbols};

            for (const auto& [left, right] : table)
            {
                vocabulary.push_back(left + right);
            }

            for (const auto& left : vocabulary)
            {
                for (const auto& right : vocabulary)
                {
                    if (std::ranges::contains(vocabulary, left + right))
                    {
                        continue;
                    }

                    auto longer{table};

                    longer.push_back({.left = left, .right = right});
                    next.push_back(std::move(longer));
                }
            }
        }

        tables.insert(tables.end(), next.begin(), next.end());
        layer = std::move(next);
    }

    return tables;
}

/**
 * @brief Draws an admissible table of a number of merges over symbols with no merge of equal parts, so of finite
 *        radius: each merge's parts drawn from the tokens the symbols and the merges before it carry, again while the
 *        parts are equal or their product is carried.
 * @param sequence The random sequence.
 * @param symbols The symbols.
 * @param count The number of merges.
 * @return The table.
 */
std::vector<Merge> random_table(
        std::mt19937& sequence, const std::vector<std::string>& symbols, const std::size_t count)
{
    auto vocabulary{symbols};

    std::vector<Merge> merges{};

    while (merges.size() < count)
    {
        std::uniform_int_distribution<std::size_t> draw{0, vocabulary.size() - 1};

        const auto left{vocabulary[draw(sequence)]};
        const auto right{vocabulary[draw(sequence)]};

        if (left == right || std::ranges::contains(vocabulary, left + right))
        {
            continue;
        }

        merges.push_back({.left = left, .right = right});
        vocabulary.push_back(left + right);
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

TEST(Ripple_radius_test,
     Every_table_of_up_to_four_merges_over_two_bytes_or_three_over_three_is_decided_as_the_full_product)
{
    std::size_t tables{0};
    std::size_t finite{0};

    const std::vector<std::pair<std::vector<std::string>, std::size_t>> universes{
            {{"a", "b"}, 4},
            {{"a", "b", "c"}, 3}};

    for (const auto& [symbols, depth] : universes)
    {
        for (const auto& merges : admissible_tables(symbols, depth))
        {
            const auto context{table_text(merges)};

            ++tables;

            if (expect_decided_as_the_full_product(merges, Reading::bytes, {}, context))
            {
                ++finite;
            }
        }
    }

    EXPECT_EQ(tables, 13303U);
    EXPECT_EQ(finite, 3158U);
}

TEST(Ripple_radius_test, Random_tables_over_bytes_and_over_characters_are_decided_as_the_full_product)
{
    std::mt19937 sequence{0x5EEDU};

    std::uniform_int_distribution<std::size_t> count{5, 12};

    const std::vector<std::string> bytes{"a", "b", "c", "d"};

    constexpr std::string_view alphabet{"aé中😀"};

    const auto characters{characters_of(alphabet)};

    std::size_t finite{0};

    constexpr std::size_t draws{300};

    for (std::size_t drawn{0}; drawn < draws; ++drawn)
    {
        const auto over_bytes{random_table(sequence, bytes, count(sequence))};

        const auto over_characters{random_table(sequence, characters, count(sequence))};

        const auto byte_context{std::format("draw {} over bytes: {}", drawn, table_text(over_bytes))};

        const auto character_context{std::format("draw {} over characters: {}", drawn, table_text(over_characters))};

        if (expect_decided_as_the_full_product(over_bytes, Reading::bytes, {}, byte_context))
        {
            ++finite;
        }

        if (expect_decided_as_the_full_product(over_characters, Reading::characters, alphabet, character_context))
        {
            ++finite;
        }
    }

    EXPECT_EQ(finite, 2 * draws);
}

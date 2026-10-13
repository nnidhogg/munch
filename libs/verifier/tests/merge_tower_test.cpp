#include "munch/verifier/merge_tower.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

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
 * @brief What the reference fixture holds, counted as it is checked.
 */
struct Fixture_counts
{
    /**
     * @brief The universe lines.
     */
    std::size_t universes{0};

    /**
     * @brief The states lines.
     */
    std::size_t state_counts{0};

    /**
     * @brief The string lines.
     */
    std::size_t strings{0};

    /**
     * @brief The certify lines.
     */
    std::size_t certifications{0};

    /**
     * @brief The certify lines whose answer is a miscovering.
     */
    std::size_t miscoverings{0};

    /**
     * @brief The gap lines.
     */
    std::size_t gaps{0};

    /**
     * @brief The realizable lines.
     */
    std::size_t realizabilities{0};

    /**
     * @brief The pair lines.
     */
    std::size_t pairs{0};
};

/**
 * @brief A universe of the fixture: its merge table and the merge tower of the table.
 */
struct Universe
{
    /**
     * @brief The merge table in rank order.
     */
    std::vector<Merge> merges;

    /**
     * @brief The merge tower of the table.
     */
    Verifier tower;
};

/**
 * @brief The reference fixture under check: the universes so far, the current one last, and the counts so far.
 */
struct Fixture_check
{
    /**
     * @brief The universes, by the index the pair lines name them by, the current one last.
     */
    std::vector<Universe> universes{};

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
 * @brief Returns the boundary marking of a segmentation, a boundary after the last byte of every token but the last.
 * @param tokens The tokens.
 * @return The marking, one bit per byte.
 */
Marking_t token_marking(const std::vector<std::string>& tokens)
{
    Marking_t marking{};

    for (const auto& token : tokens)
    {
        marking.resize(marking.size() + token.size(), false);
        marking.back() = true;
    }

    if (!marking.empty())
    {
        marking.back() = false;
    }

    return marking;
}

/**
 * @brief Returns the boundary marking of the reference encoder's segmentation of a text over the bytes, a boundary
 *        after every token but the last.
 * @param merges The merge table in rank order.
 * @param text The text.
 * @return The marking.
 */
Marking_t reference_marking(const std::vector<Merge>& merges, const std::string_view text)
{
    const auto tokens{reference_tokens(merges, bytes_of(text))};

    return token_marking(tokens);
}

/**
 * @brief Returns the boundary marking of the reference encoder's segmentation of a UTF-8 text over its characters, a
 *        boundary after the last byte of every token but the last.
 * @param merges The merge table in rank order, its parts spelled in UTF-8.
 * @param text The text.
 * @return The marking, one bit per byte.
 */
Marking_t character_marking(const std::vector<Merge>& merges, const std::string_view text)
{
    const auto tokens{reference_tokens(merges, characters_of(text))};

    return token_marking(tokens);
}

/**
 * @brief Returns whether the verifier accepts an input under a marking.
 * @param verifier The verifier.
 * @param input The input.
 * @param marking The marking, one bit per byte.
 * @return True when the marked string is accepted.
 */
bool is_accepted(const Verifier& verifier, const std::string_view input, const Marking_t& marking)
{
    auto state{std::optional{verifier.start()}};

    for (std::size_t index{0}; index < input.size() && state; ++index)
    {
        const Marked symbol{.byte = static_cast<unsigned char>(input[index]), .boundary_after = marking[index]};

        state = verifier.step(*state, symbol);
    }

    return state && verifier.accepts(*state);
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

    return bytes.size() == boundaries.size() && is_accepted(verifier, bytes, boundaries);
}

/**
 * @brief Returns the markings of an input the verifier accepts, found by enumerating every marking.
 * @param verifier The verifier.
 * @param input The input.
 * @return The accepted markings.
 */
std::set<Marking_t> accepted_markings(const Verifier& verifier, const std::string_view input)
{
    std::set<Marking_t> accepted{};

    for (std::size_t bits{0}; bits < (std::size_t{1} << input.size()); ++bits)
    {
        Marking_t marking(input.size(), false);

        for (std::size_t index{0}; index < input.size(); ++index)
        {
            marking[index] = ((bits >> index) & 1U) != 0;
        }

        if (is_accepted(verifier, input, marking))
        {
            accepted.insert(marking);
        }
    }

    return accepted;
}

/**
 * @brief Returns the marking of an input given as the positions a boundary follows.
 * @param input The input.
 * @param boundaries The byte indices a boundary follows.
 * @return The marking.
 */
Marking_t marking_of(const std::string_view input, const std::vector<std::size_t>& boundaries)
{
    Marking_t marking(input.size(), false);

    for (const auto index : boundaries)
    {
        marking[index] = true;
    }

    return marking;
}

/**
 * @brief Checks that the tower's accepted markings of a text are exactly the reference encoder's marking.
 * @param universe The universe.
 * @param text The text.
 * @param context The fixture line or test checked.
 */
void expect_reference_marking(const Universe& universe, const std::string_view text, const std::string& context)
{
    const auto expected{std::set<Marking_t>{reference_marking(universe.merges, text)}};

    EXPECT_EQ(accepted_markings(universe.tower, text), expected) << context << " on " << text;
}

/**
 * @brief Builds a universe: a merge table with the merge tower of it.
 * @param merges The merge table in rank order.
 * @return The universe.
 */
Universe universe_of(std::vector<Merge> merges)
{
    auto tower{merge_tower(merges)};

    return {.merges = std::move(merges), .tower = std::move(tower)};
}

/**
 * @brief Returns the message the merge tower refuses a table with, or the empty text when it takes the table.
 * @param merges The merge table in rank order.
 * @return The refusal.
 */
std::string refusal_of(const std::vector<Merge>& merges)
{
    try
    {
        std::ignore = merge_tower(merges);
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
 * @brief Returns the marking of a fixture bit field, "-" standing for no bits.
 * @param field The bits, a 1 where a boundary follows the byte.
 * @return The marking.
 */
Marking_t marking_of(const std::string& field)
{
    Marking_t marking{};

    for (const auto bit : text_of(field))
    {
        marking.push_back(bit == '1');
    }

    return marking;
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
        if (covering_boundary(marked, at + window.size() - 1) != at + origin)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks that the tower's one accepted marking of a text is the reference encoder's, and that under it some
 *        occurrence of the window is mis-covered.
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
    expect_reference_marking(universe, bytes, context);

    const Marked_string marked{.bytes = bytes, .boundaries = reference_marking(universe.merges, bytes)};

    EXPECT_TRUE(has_miscovered_occurrence(marked, window, origin)) << context << " on " << bytes;
}

/**
 * @brief Checks that two universes segment a text apart: each tower's one accepted marking is its reference encoder's,
 *        and the two markings differ.
 * @param a The first universe.
 * @param b The second universe.
 * @param bytes The text.
 * @param context The fixture line checked.
 */
void expect_diverging(const Universe& a, const Universe& b, const std::string& bytes, const std::string& context)
{
    expect_reference_marking(a, bytes, context);
    expect_reference_marking(b, bytes, context);

    EXPECT_NE(reference_marking(a.merges, bytes), reference_marking(b.merges, bytes)) << context << " on " << bytes;
}

/**
 * @brief Starts a universe line's universe: reads its merges, each a left and a right part joined by a comma, and
 *        builds the merge tower of the table.
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

    auto tower{merge_tower(merges)};

    ++check.counts.universes;
    check.universe = context;
    check.universes.push_back(Universe{.merges = std::move(merges), .tower = std::move(tower)});
}

/**
 * @brief Checks a states line: the tower's state count equal to the reference's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_state_count(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t expected{};

    fields >> expected;

    ++check.counts.state_counts;

    EXPECT_EQ(check.universes.back().tower.state_count(), expected) << context;
}

/**
 * @brief Checks a string line: the reference marking equal to the encoder's here, and the tower's accepted markings of
 *        the text exactly that one.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_string(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::string text{};
    std::string bits{};

    fields >> text >> bits;

    const auto& universe{check.universes.back()};

    const auto bytes{text_of(text)};

    ++check.counts.strings;

    EXPECT_EQ(reference_marking(universe.merges, bytes), marking_of(bits)) << context;

    expect_reference_marking(universe, bytes, context);
}

/**
 * @brief Checks a certify line: the certificate's answer, and for a miscovering its witness marked as the encoder marks
 *        it and as long as the reference's text, and both texts mis-covered under that marking.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_certification(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::string window{};
    std::size_t origin{};
    std::string verdict{};
    std::string text{};

    fields >> window >> origin >> verdict >> text;

    const auto& universe{check.universes.back()};

    const auto found{miscovering(universe.tower, window, origin)};

    ++check.counts.certifications;

    if (verdict == "certified")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ++check.counts.miscoverings;

    ASSERT_TRUE(found) << context;

    const auto& [segmentation, occurrence]{*found};

    const auto& [bytes, boundaries]{segmentation};

    EXPECT_TRUE(is_accepted(universe.tower, segmentation)) << context;
    EXPECT_EQ(boundaries, reference_marking(universe.merges, bytes)) << context;
    EXPECT_EQ(bytes.size(), text_of(text).size()) << context;
    ASSERT_LE(occurrence + window.size(), bytes.size()) << context;
    EXPECT_EQ(std::string_view{bytes}.substr(occurrence, window.size()), window) << context;
    EXPECT_NE(covering_boundary(segmentation, occurrence + window.size() - 1), occurrence + origin) << context;

    expect_miscovered(universe, bytes, window, origin, context);
    expect_miscovered(universe, text_of(text), window, origin, context);
}

/**
 * @brief Checks a gap line: the supremum equal to the reference's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_gap(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t expected{};

    fields >> expected;

    const auto gap{boundary_gap(check.universes.back().tower)};

    ++check.counts.gaps;

    ASSERT_TRUE(std::holds_alternative<std::size_t>(gap)) << context;
    EXPECT_EQ(std::get<std::size_t>(gap), expected) << context;
}

/**
 * @brief Checks a realizable line: the answer equal to the reference's, with a mask over every state when there is
 *        one.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_realizable(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    int expected{};

    fields >> expected;

    const auto& tower{check.universes.back().tower};

    const auto mask{realizable(tower)};

    ++check.counts.realizabilities;

    EXPECT_EQ(mask.has_value(), expected == 1) << context;

    if (mask)
    {
        EXPECT_EQ(mask->size(), tower.state_count()) << context;
    }
}

/**
 * @brief Checks a pair line: the divergence's half the boundary one as the reference's is, its witness marked as the
 *        first table's encoder marks it and as long as the reference's text, and both texts segmented apart.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_divergence(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t first{};
    std::size_t second{};
    std::string half{};
    std::string text{};

    fields >> first >> second >> half >> text;

    const auto& a{check.universes.at(first)};
    const auto& b{check.universes.at(second)};

    const auto found{divergence(a.tower, b.tower)};

    ++check.counts.pairs;

    if (half == "equivalent")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;
    ASSERT_EQ(half, "boundary") << context;

    const auto& [witness, found_half]{*found};

    const auto& [bytes, boundaries]{witness};

    EXPECT_EQ(found_half, Separation_half::boundary) << context;
    EXPECT_EQ(bytes.size(), text_of(text).size()) << context;
    EXPECT_EQ(boundaries, reference_marking(a.merges, bytes)) << context;

    expect_diverging(a, b, bytes, context);
    expect_diverging(a, b, text_of(text), context);
}

/**
 * @brief Checks one fixture line against the tower and the decisions, a comment or blank line checking nothing.
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
    else if (kind == "alphabet")
    {
        return;
    }
    else if (kind == "states")
    {
        expect_state_count(check, fields, context);
    }
    else if (kind == "string")
    {
        expect_string(check, fields, context);
    }
    else if (kind == "certify")
    {
        expect_certification(check, fields, context);
    }
    else if (kind == "gap")
    {
        expect_gap(check, fields, context);
    }
    else if (kind == "realizable")
    {
        expect_realizable(check, fields, context);
    }
    else if (kind == "pair")
    {
        expect_divergence(check, fields, context);
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
 * @brief Returns a verifier restricted to the bytes below a bound: its steps on every other byte removed, and the rest
 *        trimmed and numbered again by the verifier's constructor.
 * @param verifier The verifier.
 * @param bound The first byte removed.
 * @return The restricted verifier.
 */
Verifier restricted(const Verifier& verifier, const unsigned bound)
{
    Verifier::Transitions_t kept{};

    for (const auto& [key, to] : verifier.transitions())
    {
        const auto& [from, symbol]{key};

        if (symbol.byte < bound)
        {
            kept.emplace(key, to);
        }
    }

    return {verifier.start(), std::move(kept), verifier.accept_states()};
}

/**
 * @brief Returns the number of markings of an input the verifier accepts, counted over its runs on both bits of every
 *        byte rather than by enumerating the markings.
 * @param verifier The verifier.
 * @param input The input.
 * @return The number of accepted markings.
 */
std::size_t accepted_count(const Verifier& verifier, const std::string_view input)
{
    std::map<Verifier::State_t, std::size_t> runs{{verifier.start(), 1}};

    for (const auto byte : input)
    {
        std::map<Verifier::State_t, std::size_t> next{};

        for (const auto& [state, count] : runs)
        {
            for (const auto boundary_after : {false, true})
            {
                const auto to{verifier.step(
                        state, {.byte = static_cast<unsigned char>(byte), .boundary_after = boundary_after})};

                if (to)
                {
                    next[*to] += count;
                }
            }
        }

        runs = std::move(next);
    }

    std::size_t accepted{0};

    for (const auto& [state, count] : runs)
    {
        if (verifier.accepts(state))
        {
            accepted += count;
        }
    }

    return accepted;
}

/**
 * @brief Returns every string of up to a number of characters drawn from a list, shortest first and in list order
 *        within a length.
 * @param characters The characters.
 * @param longest The most characters a string holds.
 * @return The strings, the empty one first.
 */
std::vector<std::string> strings_over(const std::vector<std::string>& characters, const std::size_t longest)
{
    std::vector<std::string> strings{""};

    for (std::size_t begin{0}, length{0}; length < longest; ++length)
    {
        const auto end{strings.size()};

        for (auto index{begin}; index < end; ++index)
        {
            for (const auto& character : characters)
            {
                strings.push_back(strings[index] + character);
            }
        }

        begin = end;
    }

    return strings;
}

/**
 * @brief Returns the message the merge tower over an alphabet of characters refuses a table with, or the empty text
 *        when it takes the table.
 * @param merges The merge table in rank order.
 * @param alphabet The alphabet, UTF-8.
 * @return The refusal.
 */
std::string refusal_over(const std::vector<Merge>& merges, const std::string_view alphabet)
{
    try
    {
        std::ignore = merge_tower(merges, alphabet);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

} // namespace

TEST(Merge_tower_test, One_merge_over_a_segments_aaa_as_aa_then_a)
{
    const auto universe{universe_of({{.left = "a", .right = "a"}})};

    EXPECT_EQ(accepted_markings(universe.tower, "aaa"), std::set<Marking_t>{marking_of("aaa", {1})});
    EXPECT_EQ(accepted_markings(universe.tower, "aaaa"), std::set<Marking_t>{marking_of("aaaa", {1})});
    EXPECT_EQ(accepted_markings(universe.tower, "a"), std::set<Marking_t>{marking_of("a", {})});
    EXPECT_TRUE(universe.tower.accepts(universe.tower.start()));

    for (const auto text : {"", "aa", "aaaaa", "aaaaaaaa", "ab", "ba", "aaba", "aabaaa"})
    {
        expect_reference_marking(universe, text, "one merge over a");
    }
}

TEST(Merge_tower_test, A_later_merge_may_name_an_earlier_product)
{
    const auto universe{
            universe_of({{.left = "a", .right = "b"}, {.left = "ab", .right = "a"}, {.left = "aba", .right = "b"}})};

    EXPECT_EQ(accepted_markings(universe.tower, "abaab"), std::set<Marking_t>{marking_of("abaab", {2})});
    EXPECT_EQ(accepted_markings(universe.tower, "abab"), std::set<Marking_t>{marking_of("abab", {1})});
    EXPECT_EQ(accepted_markings(universe.tower, "ababb"), std::set<Marking_t>{marking_of("ababb", {1, 3})});

    for (const auto text : {"aba", "abb", "baba", "abaaba", "ababa", "abababab", "abaabab"})
    {
        expect_reference_marking(universe, text, "ab, ab+a, aba+b");
    }
}

TEST(Merge_tower_test, The_empty_table_segments_byte_by_byte)
{
    const auto universe{universe_of({})};

    EXPECT_EQ(accepted_markings(universe.tower, "xyz"), std::set<Marking_t>{marking_of("xyz", {0, 1})});
    EXPECT_EQ(accepted_markings(universe.tower, ""), std::set<Marking_t>{Marking_t{}});
    EXPECT_EQ(universe.tower.state_count(), 3U);
}

TEST(Merge_tower_test, Refuses_a_merge_naming_a_part_the_vocabulary_does_not_carry)
{
    const auto refusal{refusal_of({{.left = "a", .right = "b"}, {.left = "a", .right = "bc"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "bc") at rank 1 names "bc")")) << refusal;
}

TEST(Merge_tower_test, Refuses_a_merge_whose_product_an_earlier_merge_already_carries)
{
    const auto refusal{refusal_of(
            {{.left = "a", .right = "b"},
             {.left = "a", .right = "a"},
             {.left = "aa", .right = "b"},
             {.left = "a", .right = "ab"}})};

    EXPECT_TRUE(refusal.contains(R"(("a", "ab") at rank 3 recreates the token "aab")")) << refusal;
}

TEST(Merge_tower_test, Refuses_a_table_whose_later_merge_recreates_a_token_an_earlier_one_consumed)
{
    const auto refusal{refusal_of(
            {{.left = "c", .right = "c"},
             {.left = "c", .right = "cc"},
             {.left = "ccc", .right = "a"},
             {.left = "cc", .right = "c"}})};

    EXPECT_TRUE(refusal.contains(R"(("cc", "c") at rank 3 recreates the token "ccc")")) << refusal;
}

TEST(Merge_tower_test, A_refusal_quotes_a_spelling_holding_a_NUL_and_still_names_the_rank)
{
    // A raw NUL in the message would end it there for every reader of what(), so a byte outside printable ASCII is
    // written as an escape, and the quote and the backslash are escaped beside it.
    const std::string nul(1, '\0');

    const auto refusal{
            refusal_of({{.left = nul, .right = "a"}, {.left = "\"", .right = "\\"}, {.left = nul, .right = "a"}})};

    EXPECT_TRUE(refusal.contains(R"(("\x00", "a") at rank 2 recreates the token "\x00a")")) << refusal;

    const auto missing{refusal_of({{.left = "\xFF", .right = "\"\\"}})};

    EXPECT_TRUE(missing.contains(R"(("\xFF", "\"\\") at rank 0 names "\"\\")")) << missing;
}

TEST(Merge_tower_test, The_tower_and_the_decisions_give_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/bpe_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 14U);
    EXPECT_EQ(check.counts.state_counts, 14U);
    EXPECT_EQ(check.counts.strings, 10177U);
    EXPECT_EQ(check.counts.certifications, 1008U);
    EXPECT_EQ(check.counts.miscoverings, 718U);
    EXPECT_EQ(check.counts.gaps, 14U);
    EXPECT_EQ(check.counts.realizabilities, 14U);
    EXPECT_EQ(check.counts.pairs, 105U);
}

TEST(Merge_tower_test,
     Over_the_characters_of_ascii_the_tower_is_the_byte_tower_restricted_to_them_on_every_fixture_table)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/bpe_reference.txt"};

    ASSERT_TRUE(file.is_open());

    constexpr unsigned first_beyond_ascii{0x80};

    const auto alphabet{ascii_alphabet()};

    std::size_t tables{0};

    for (std::string line{}; std::getline(file, line);)
    {
        std::istringstream fields{line};

        std::string kind{};

        if (!(fields >> kind) || kind != "universe")
        {
            continue;
        }

        std::vector<Merge> merges{};

        for (std::string merge{}; fields >> merge;)
        {
            const auto comma{merge.find(',')};

            merges.push_back({.left = merge.substr(0, comma), .right = merge.substr(comma + 1)});
        }

        const auto bytes{restricted(merge_tower(merges), first_beyond_ascii)};

        const auto characters{merge_tower(merges, alphabet)};

        ++tables;

        EXPECT_EQ(characters.state_count(), bytes.state_count()) << line;
        EXPECT_EQ(characters.transitions(), bytes.transitions()) << line;
        EXPECT_EQ(characters.accept_states(), bytes.accept_states()) << line;
    }

    EXPECT_EQ(tables, 14U);
}

TEST(Merge_tower_test, A_sentencepiece_style_character_table_marks_every_short_string_as_the_character_encoder_does)
{
    const auto merges{sentencepiece_merges()};

    const auto tower{merge_tower(merges, sentencepiece_alphabet)};

    const auto texts{strings_over(characters_of(sentencepiece_alphabet), 4)};

    for (const auto& text : texts)
    {
        EXPECT_TRUE(is_accepted(tower, text, character_marking(merges, text))) << text;
        EXPECT_EQ(accepted_count(tower, text), 1U) << text;
    }

    EXPECT_EQ(texts.size(), 16105U);
    EXPECT_EQ(accepted_markings(tower, "▁the"), std::set<Marking_t>{marking_of("▁the", {})});
    EXPECT_EQ(accepted_markings(tower, "▁中文"), std::set<Marking_t>{marking_of("▁中文", {})});
    EXPECT_EQ(accepted_markings(tower, "caét"), std::set<Marking_t>{marking_of("caét", {})});
    EXPECT_EQ(accepted_markings(tower, "😀😀😀"), std::set<Marking_t>{marking_of("😀😀😀", {7})});
}

TEST(Merge_tower_test, The_character_tower_takes_no_boundary_inside_a_character_and_no_character_it_does_not_hold_whole)
{
    const auto tower{merge_tower(sentencepiece_merges(), sentencepiece_alphabet)};

    EXPECT_EQ(accepted_markings(tower, "é"), std::set<Marking_t>{marking_of("é", {})});
    EXPECT_EQ(accepted_markings(tower, "ét"), std::set<Marking_t>{marking_of("ét", {})});
    EXPECT_EQ(accepted_markings(tower, "éé"), std::set<Marking_t>{marking_of("éé", {1})});
    EXPECT_EQ(accepted_count(tower, "\xE2\x96"), 0U);
    EXPECT_EQ(accepted_count(tower, "t\xE2\x96"), 0U);
    EXPECT_EQ(accepted_count(tower, "\xA9"), 0U);
    EXPECT_EQ(accepted_count(tower, "x"), 0U);
    EXPECT_EQ(accepted_count(tower, "\xC3\xA8"), 0U);
}

TEST(Merge_tower_test, Every_window_of_up_to_two_characters_is_decided_on_the_character_tower_as_the_encoder_marks)
{
    const auto merges{sentencepiece_merges()};

    const auto tower{merge_tower(merges, sentencepiece_alphabet)};

    const auto characters{characters_of(sentencepiece_alphabet)};

    std::vector<Marked_string> marked{};

    for (const auto& text : strings_over(characters, 4))
    {
        marked.push_back({.bytes = text, .boundaries = character_marking(merges, text)});
    }

    const auto miscovered_anywhere{[&](const std::string_view window, const std::size_t origin) {
        return std::ranges::any_of(
                marked, [&](const Marked_string& text) { return has_miscovered_occurrence(text, window, origin); });
    }};

    std::size_t certified{0};
    std::size_t refused{0};

    for (const auto& window : strings_over(characters, 2) | std::views::drop(1))
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            const auto found{miscovering(tower, window, origin)};

            if (!found)
            {
                ++certified;

                EXPECT_FALSE(miscovered_anywhere(window, origin)) << window << " at " << origin;

                continue;
            }

            ++refused;

            const auto& [segmentation, occurrence]{*found};

            EXPECT_TRUE(is_accepted(tower, segmentation)) << window << " at " << origin;
            EXPECT_EQ(segmentation.boundaries, character_marking(merges, segmentation.bytes)) << window;
            EXPECT_TRUE(has_miscovered_occurrence(segmentation, window, origin)) << window << " at " << origin;
        }
    }

    EXPECT_EQ(certified, 117U);
    EXPECT_EQ(refused, 389U);
}

TEST(Merge_tower_test, An_alphabet_of_more_characters_than_a_byte_holds_builds_one_tower_over_all_of_them)
{
    constexpr std::size_t character_count{300};
    constexpr std::size_t stride{30};

    std::string alphabet{};

    for (std::size_t offset{0}; offset < character_count; ++offset)
    {
        alphabet += ideograph(offset);
    }

    std::vector<Merge> merges{};

    for (std::size_t offset{0}; offset < character_count; offset += stride)
    {
        merges.push_back({.left = ideograph(offset), .right = ideograph(offset + 1)});
    }

    merges.push_back({.left = ideograph(0) + ideograph(1), .right = ideograph(character_count - 1)});

    const auto tower{merge_tower(merges, alphabet)};

    const auto last{ideograph(character_count - 1)};

    for (const auto& text :
         {ideograph(0) + ideograph(1) + last, ideograph(270) + ideograph(271) + last, ideograph(298) + last + last})
    {
        EXPECT_TRUE(is_accepted(tower, text, character_marking(merges, text))) << text;
        EXPECT_EQ(accepted_count(tower, text), 1U) << text;
    }

    EXPECT_EQ(character_marking(merges, ideograph(0) + ideograph(1) + last), Marking_t(9, false));
    EXPECT_EQ(accepted_count(tower, ideograph(character_count)), 0U);
}

TEST(Merge_tower_test, Refuses_an_alphabet_holding_a_character_twice)
{
    const auto refusal{refusal_over({}, "aéba\xC3\xA9")};

    EXPECT_TRUE(refusal.contains(R"(the alphabet carries "a" twice)")) << refusal;

    const auto again{refusal_over({}, "éb\xC3\xA9")};

    EXPECT_TRUE(again.contains(R"(the alphabet carries "\xC3\xA9" twice)")) << again;
}

TEST(Merge_tower_test, Refuses_a_merge_naming_a_character_outside_the_alphabet)
{
    const auto refusal{refusal_over({{.left = "é", .right = "t"}, {.left = "ét", .right = "ü"}}, "ét")};

    EXPECT_TRUE(refusal.contains(R"(("\xC3\xA9t", "\xC3\xBC") at rank 1 names "\xC3\xBC")")) << refusal;
    EXPECT_TRUE(refusal_over({{.left = "é", .right = "t"}}, "ét").empty());
    EXPECT_FALSE(refusal_over({{.left = "é", .right = "t"}}, "et").empty());
}

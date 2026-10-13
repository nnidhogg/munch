#include "munch/verifier/token_automaton.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
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
 * @brief The canonical pairs of an adjacency relation, each by its left and its right token.
 */
using Pairs_t = std::set<std::pair<std::string, std::string>>;

/**
 * @brief The score of each piece a key rule may form, the highest merged first.
 */
using Scores_t = std::map<std::string, int>;

/**
 * @brief Which of two adjacent pairs of one score a key rule merges first.
 */
enum class Tie
{
    /**
     * @brief The leftmost, as SentencePiece's loop merges.
     */
    leftmost,

    /**
     * @brief The rightmost.
     */
    rightmost
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
 * @brief What the adjacency fixture holds, counted as it is checked.
 */
struct Relation_counts
{
    /**
     * @brief The universe lines.
     */
    std::size_t universes{0};

    /**
     * @brief The universes with an admissible table, whose token automaton is decided beside the relation's.
     */
    std::size_t tables{0};

    /**
     * @brief The certify lines.
     */
    std::size_t certifications{0};

    /**
     * @brief The certify lines whose answer is a miscovering.
     */
    std::size_t miscoverings{0};

    /**
     * @brief The diverge lines.
     */
    std::size_t divergences{0};

    /**
     * @brief The diverge lines whose answer is that the two encoders are equal.
     */
    std::size_t equal{0};
};

/**
 * @brief A universe of the adjacency fixture: its alphabet, its canonical tokens and pairs, its longest token, its
 *        admissible table where it has one, and the token automata of the relation and of the table.
 */
struct Relation_universe
{
    /**
     * @brief The alphabet, UTF-8 text whose characters are the symbols.
     */
    std::string alphabet{};

    /**
     * @brief The canonical tokens in order, the alphabet's symbols first.
     */
    std::vector<std::string> tokens{};

    /**
     * @brief The canonical pairs.
     */
    std::vector<Adjacent> pairs{};

    /**
     * @brief The length in bytes of the longest canonical token.
     */
    std::size_t longest{1};

    /**
     * @brief The universe's admissible merge table in rank order, std::nullopt when it has none.
     */
    std::optional<std::vector<Merge>> table{};

    /**
     * @brief The token automaton of the relation, built at the universe's first certify line.
     */
    std::optional<Token_automaton> relation{};

    /**
     * @brief The token automaton of the table over the alphabet, built with the relation's where there is a table.
     */
    std::optional<Token_automaton> compiled{};
};

/**
 * @brief The adjacency fixture under check: its universes by name, the current one's name, and the counts so far.
 */
struct Relation_check
{
    /**
     * @brief The universes read so far, by name.
     */
    std::map<std::string, Relation_universe> universes{};

    /**
     * @brief The current universe's name.
     */
    std::string current{};

    /**
     * @brief The lines checked so far, by kind.
     */
    Relation_counts counts{};
};

/**
 * @brief A chain of canonical tokens spelling a prefix of a text: the bytes it spells, its last token, and the
 *        boundary marking of the bytes so far, a boundary after every token.
 */
struct Chain_prefix
{
    /**
     * @brief The number of bytes spelled.
     */
    std::size_t end{};

    /**
     * @brief The last token, empty before the first.
     */
    std::string last{};

    /**
     * @brief The marking of the bytes spelled, a boundary after the last byte of every token.
     */
    Marking_t marking{};
};

/**
 * @brief A key rule over {a, b} with its canonical tokens and pairs, read off its own runs by their definition, and
 *        the token automaton of the relation.
 */
struct Key_rule
{
    /**
     * @brief The score of each piece the rule may form.
     */
    Scores_t scores{};

    /**
     * @brief The occurrence of a tie merged first.
     */
    Tie tie{};

    /**
     * @brief The canonical tokens, the symbols first and then the pieces that segment as themselves, shortest first.
     */
    std::vector<std::string> tokens{};

    /**
     * @brief The canonical pairs.
     */
    std::vector<Adjacent> pairs{};

    /**
     * @brief The length of the longest canonical token.
     */
    std::size_t longest{1};

    /**
     * @brief The token automaton of the relation.
     */
    std::optional<Token_automaton> automaton{};
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
 * @brief Returns the bytes of a text, each a symbol spelled by itself.
 * @param text The text.
 * @return The bytes, in order.
 */
std::vector<std::string> byte_symbols(const std::string_view text)
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
 * @param ranks The rank of each merge by its two parts.
 * @param sequence The text's symbols, its bytes or its characters.
 * @return The tokens.
 */
std::vector<std::string> reference_tokens(const Ranks_t& ranks, std::vector<std::string> sequence)
{
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
 * @param ranks The rank of each merge by its two parts.
 * @param text The text.
 * @return The marking.
 */
Marking_t reference_marking(const Ranks_t& ranks, const std::string_view text)
{
    const auto tokens{reference_tokens(ranks, byte_symbols(text))};

    return token_marking(tokens);
}

/**
 * @brief Returns the boundary marking of the reference encoder's segmentation of a UTF-8 text over its characters, a
 *        boundary after the last byte of every token but the last.
 * @param ranks The rank of each merge by its two parts, spelled in UTF-8.
 * @param text The text.
 * @return The marking, one bit per byte.
 */
Marking_t character_marking(const Ranks_t& ranks, const std::string_view text)
{
    const auto tokens{reference_tokens(ranks, characters_of(text))};

    return token_marking(tokens);
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
 * @brief Returns whether every byte of a text lies below 0x80, so that the text is ASCII.
 * @param text The text.
 * @return True when the text is ASCII.
 */
bool is_ascii(const std::string_view text)
{
    const auto below{[](const char byte) { return static_cast<unsigned char>(byte) < 0x80U; }};

    return std::ranges::all_of(text, below);
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
 * @brief Returns the merges of a universe line after its name and its decided field, each a left and a right part in
 *        hex joined by a comma.
 * @param fields The line after its first three words.
 * @return The merge table in rank order.
 */
std::vector<Merge> merges_of(std::istringstream& fields)
{
    std::vector<Merge> merges{};

    for (std::string merge{}; fields >> merge;)
    {
        const std::string_view field{merge};

        const auto comma{field.find(',')};

        const auto left{bytes_of(field.substr(0, comma))};

        const auto right{bytes_of(field.substr(comma + 1))};

        merges.push_back({.left = left, .right = right});
    }

    return merges;
}

/**
 * @brief Checks that two decisions of one window certificate are the same, to the byte: both certify, or both return
 *        one segmentation with one occurrence.
 * @param found The decision on one automaton.
 * @param expected The decision on the other.
 * @param context The fixture line or test checked.
 */
void expect_same(
        const std::optional<Miscovering>& found, const std::optional<Miscovering>& expected, const std::string& context)
{
    ASSERT_EQ(found.has_value(), expected.has_value()) << context;

    if (!found)
    {
        return;
    }

    EXPECT_EQ(found->segmentation.bytes, expected->segmentation.bytes) << context;
    EXPECT_EQ(found->segmentation.boundaries, expected->segmentation.boundaries) << context;
    EXPECT_EQ(found->occurrence, expected->occurrence) << context;
}

/**
 * @brief Returns the message token_automaton() over an alphabet of characters refuses a table with, or the empty text
 *        when it takes the table.
 * @param merges The merge table in rank order.
 * @param alphabet The alphabet, UTF-8.
 * @return The refusal.
 */
std::string refusal_over(const std::vector<Merge>& merges, const std::string_view alphabet)
{
    try
    {
        std::ignore = token_automaton(merges, alphabet);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the message token_automaton() over an adjacency relation refuses it with, or the empty text when it
 *        takes the relation.
 * @param tokens The canonical tokens.
 * @param pairs The canonical pairs.
 * @return The refusal.
 */
std::string relation_refusal(const std::vector<std::string>& tokens, const std::vector<Adjacent>& pairs)
{
    try
    {
        std::ignore = token_automaton(tokens, pairs);
    }
    catch (const std::invalid_argument& refusal)
    {
        return refusal.what();
    }

    return {};
}

/**
 * @brief Returns the pairs of an adjacency relation as a set, each by its two tokens.
 * @param pairs The pairs.
 * @return The set.
 */
Pairs_t pair_set(const std::vector<Adjacent>& pairs)
{
    Pairs_t set{};

    for (const auto& [left, right] : pairs)
    {
        set.emplace(left, right);
    }

    return set;
}

/**
 * @brief Returns the boundary marking of every chain of canonical tokens with canonical adjacent pairs that spells a
 *        text, the encoder's segmentation of it by the canonical adjacency theorem when there is exactly one.
 * @param tokens The canonical tokens.
 * @param pairs The canonical pairs.
 * @param text The text, nonempty.
 * @return The markings, one bit per byte, in no particular order.
 */
std::vector<Marking_t> chain_markings(
        const std::vector<std::string>& tokens, const Pairs_t& pairs, const std::string_view text)
{
    const std::set<std::string_view> canonical{tokens.cbegin(), tokens.cend()};

    std::vector<Chain_prefix> open{{}};
    std::vector<Marking_t> markings{};

    while (!open.empty())
    {
        auto [end, last, marking]{std::move(open.back())};

        open.pop_back();

        if (end == text.size())
        {
            marking.back() = false;
            markings.push_back(std::move(marking));

            continue;
        }

        for (auto next{end + 1}; next <= text.size(); ++next)
        {
            const auto token{text.substr(end, next - end)};

            const auto follows{last.empty() || pairs.contains({last, std::string{token}})};

            if (!canonical.contains(token) || !follows)
            {
                continue;
            }

            auto extended{marking};

            extended.resize(next, false);
            extended.back() = true;

            open.push_back({.end = next, .last = std::string{token}, .marking = std::move(extended)});
        }
    }

    return markings;
}

/**
 * @brief Returns the canonical tokens of a token automaton, those a transition reads, in number order.
 * @param automaton The token automaton.
 * @return The spellings.
 */
std::vector<std::string> canonical_spellings(const Token_automaton& automaton)
{
    std::vector<std::string> spellings{};

    for (const auto& [spelling, left, target] : automaton.tokens())
    {
        if (target)
        {
            spellings.push_back(spelling);
        }
    }

    return spellings;
}

/**
 * @brief Returns the canonical pairs of a token automaton, each token a transition reads followed by each its state
 *        may read.
 * @param automaton The token automaton.
 * @return The pairs, each by its two tokens.
 */
Pairs_t canonical_pairs(const Token_automaton& automaton)
{
    const auto& tokens{automaton.tokens()};

    Pairs_t pairs{};

    for (const auto& [spelling, left, target] : tokens)
    {
        if (!target)
        {
            continue;
        }

        for (const auto right : automaton.allowed_tokens(*target))
        {
            pairs.emplace(spelling, tokens[right].spelling);
        }
    }

    return pairs;
}

/**
 * @brief Starts a universe line's universe of the adjacency fixture: reads its name, its rule and its alphabet in hex.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 */
void start_relation_universe(Relation_check& check, std::istringstream& fields)
{
    std::string name{};
    std::string rule{};
    std::string alphabet{};

    fields >> name >> rule >> alphabet;

    check.current = name;
    check.universes[name] = {.alphabet = bytes_of(alphabet)};

    ++check.counts.universes;
}

/**
 * @brief Reads a tokens line into the current universe, each token in hex, and its longest token.
 * @param universe The current universe.
 * @param fields The line after its first word.
 */
void read_tokens(Relation_universe& universe, std::istringstream& fields)
{
    for (std::string field{}; fields >> field;)
    {
        auto token{bytes_of(field)};

        universe.longest = std::max(universe.longest, token.size());
        universe.tokens.push_back(std::move(token));
    }
}

/**
 * @brief Reads an adjacent line into the current universe: its left token and every right token that follows it.
 * @param universe The current universe.
 * @param fields The line after its first word.
 */
void read_adjacent(Relation_universe& universe, std::istringstream& fields)
{
    std::string left{};

    fields >> left;

    for (std::string right{}; fields >> right;)
    {
        universe.pairs.push_back({.left = bytes_of(left), .right = bytes_of(right)});
    }
}

/**
 * @brief Builds the current universe's automata unless built: the relation's, and the table's where there is one,
 *        checked to hold the universe's canonical tokens in order and its canonical pairs and to diverge from the
 *        relation's in neither order.
 * @param check The fixture under check.
 * @return The current universe.
 */
Relation_universe& built_universe(Relation_check& check)
{
    auto& universe{check.universes.at(check.current)};

    if (universe.relation)
    {
        return universe;
    }

    universe.relation = token_automaton(universe.tokens, universe.pairs);

    if (!universe.table)
    {
        return universe;
    }

    ++check.counts.tables;

    universe.compiled = token_automaton(*universe.table, universe.alphabet);

    EXPECT_EQ(canonical_spellings(*universe.compiled), universe.tokens) << check.current;
    EXPECT_EQ(canonical_pairs(*universe.compiled), pair_set(universe.pairs)) << check.current;
    EXPECT_FALSE(encoder_divergence(*universe.compiled, *universe.relation)) << check.current;
    EXPECT_FALSE(encoder_divergence(*universe.relation, *universe.compiled)) << check.current;

    return universe;
}

/**
 * @brief Checks that a marked string is the one chain of a universe's relation spelling its bytes, so the encoder's
 *        segmentation of them.
 * @param universe The universe.
 * @param marked The marked string.
 * @param context The fixture line checked.
 */
void expect_only_chain(const Relation_universe& universe, const Marked_string& marked, const std::string& context)
{
    const auto markings{chain_markings(universe.tokens, pair_set(universe.pairs), marked.bytes)};

    ASSERT_EQ(markings.size(), 1U) << context << " on " << marked.bytes;
    EXPECT_EQ(markings.front(), marked.boundaries) << context << " on " << marked.bytes;
}

/**
 * @brief Checks a miscovering on a universe of the adjacency fixture: its length within the bound, its occurrence
 *        spelling the window and mis-covered, and its segmentation the relation's one chain spelling its bytes.
 * @param universe The universe.
 * @param found The miscovering.
 * @param window The window.
 * @param origin The origin.
 * @param context The fixture line checked.
 */
void expect_relation_miscovering(
        const Relation_universe& universe, const Miscovering& found, const std::string_view window,
        const std::size_t origin, const std::string& context)
{
    const auto& [segmentation, occurrence]{found};

    const auto bound{2 * universe.longest + window.size() - 2};

    const auto last{occurrence + window.size() - 1};

    EXPECT_LE(segmentation.bytes.size(), bound) << context;
    ASSERT_LE(occurrence + window.size(), segmentation.bytes.size()) << context;
    EXPECT_EQ(std::string_view{segmentation.bytes}.substr(occurrence, window.size()), window) << context;
    EXPECT_NE(covering_boundary(segmentation, last), occurrence + origin) << context;

    expect_only_chain(universe, segmentation, context);
}

/**
 * @brief Checks a certify line of the adjacency fixture: the certificate's answer on the relation's automaton and on
 *        the table's where there is one, each miscovering within the bound and the relation's one chain, and the
 *        reference's text mis-covered under its one chain.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_relation_certification(Relation_check& check, std::istringstream& fields, const std::string& context)
{
    std::string window_field{};
    std::size_t origin{};
    std::string verdict{};
    std::string text{};

    fields >> window_field >> origin >> verdict >> text;

    const auto window{bytes_of(window_field)};

    const auto& universe{built_universe(check)};

    const auto found{miscovering(*universe.relation, window, origin)};

    const auto on_table{universe.compiled ? miscovering(*universe.compiled, window, origin) : found};

    ++check.counts.certifications;

    EXPECT_EQ(on_table.has_value(), found.has_value()) << context;

    if (verdict == "certified")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ++check.counts.miscoverings;

    ASSERT_TRUE(found) << context;
    ASSERT_TRUE(on_table) << context;

    expect_relation_miscovering(universe, *found, window, origin, context);
    expect_relation_miscovering(universe, *on_table, window, origin, context);

    const auto bytes{bytes_of(text)};

    const auto markings{chain_markings(universe.tokens, pair_set(universe.pairs), bytes)};

    ASSERT_EQ(markings.size(), 1U) << context;

    const Marked_string marked{.bytes = bytes, .boundaries = markings.front()};

    EXPECT_TRUE(has_miscovered_occurrence(marked, window, origin)) << context;
}

/**
 * @brief Checks a diverge line of the adjacency fixture: encoder_divergence() on the two universes' automata, each the
 *        table's where there is one, gives the reference's answer in both orders, and a witness is at most 2L - 1
 *        bytes, no shorter than the shortest string separating the two where the reference searched, and the one
 *        chain of each relation spelling it marks it differently.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_relation_divergence(Relation_check& check, std::istringstream& fields, const std::string& context)
{
    std::string first_name{};
    std::string second_name{};
    std::string answer{};
    std::string shortest{};

    fields >> first_name >> second_name >> answer >> shortest;

    const auto& first{check.universes.at(first_name)};
    const auto& second{check.universes.at(second_name)};

    const auto& first_automaton{first.compiled ? *first.compiled : *first.relation};
    const auto& second_automaton{second.compiled ? *second.compiled : *second.relation};

    const auto found{encoder_divergence(first_automaton, second_automaton)};

    const auto reversed{encoder_divergence(second_automaton, first_automaton)};

    ++check.counts.divergences;

    EXPECT_EQ(found.has_value(), reversed.has_value()) << context;

    if (answer == "equal")
    {
        ++check.counts.equal;

        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    const auto longest{std::max(first.longest, second.longest)};

    EXPECT_EQ(*found, bytes_of(answer)) << context;
    EXPECT_LE(found->size(), 2 * longest - 1) << context;

    if (shortest != "-")
    {
        std::size_t length{};

        std::istringstream{shortest} >> length;

        EXPECT_GE(found->size(), length) << context;
    }

    const auto first_markings{chain_markings(first.tokens, pair_set(first.pairs), *found)};
    const auto second_markings{chain_markings(second.tokens, pair_set(second.pairs), *found)};

    ASSERT_EQ(first_markings.size(), 1U) << context;
    ASSERT_EQ(second_markings.size(), 1U) << context;
    EXPECT_NE(first_markings.front(), second_markings.front()) << context;
}

/**
 * @brief Checks one line of the adjacency fixture, a comment or blank line checking nothing.
 * @param check The fixture under check.
 * @param line The line.
 */
void expect_relation_line(Relation_check& check, const std::string& line)
{
    std::istringstream fields{line};

    std::string kind{};

    if (!(fields >> kind) || kind.starts_with('#'))
    {
        return;
    }

    const auto context{std::format("{} | {}", check.current, line.substr(0, 200))};

    if (kind == "universe")
    {
        start_relation_universe(check, fields);
    }
    else if (kind == "table")
    {
        check.universes.at(check.current).table = merges_of(fields);
    }
    else if (kind == "tokens")
    {
        read_tokens(check.universes.at(check.current), fields);
    }
    else if (kind == "adjacent")
    {
        read_adjacent(check.universes.at(check.current), fields);
    }
    else if (kind == "certify")
    {
        expect_relation_certification(check, fields, context);
    }
    else if (kind == "diverge")
    {
        expect_relation_divergence(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown fixture line: " << line;
    }
}

/**
 * @brief Returns the canonical tokens and pairs of a merge table over the bytes by the reference encoder: every byte,
 *        then each product in rank order that the encoder segments as itself, and every pair of them it segments into
 *        the two, a pair with a byte no merge spells always among them, since no merge reaches that byte.
 * @param merges The merge table in rank order.
 * @return The tokens and the pairs.
 */
std::pair<std::vector<std::string>, std::vector<Adjacent>> reference_relation(const std::vector<Merge>& merges)
{
    constexpr std::size_t byte_count{256};

    const auto ranks{ranks_of(merges)};

    std::vector<std::string> tokens{};

    for (std::size_t byte{0}; byte < byte_count; ++byte)
    {
        tokens.push_back(std::string{static_cast<char>(byte)});
    }

    std::set<char> spelled{};

    for (const auto& [left, right] : merges)
    {
        spelled.insert(left.cbegin(), left.cend());
        spelled.insert(right.cbegin(), right.cend());

        const auto product{left + right};

        const auto own{reference_tokens(ranks, byte_symbols(product))};

        if (own == std::vector{product})
        {
            tokens.push_back(product);
        }
    }

    const auto reaches{[&](const std::string& token) { return token.size() > 1 || spelled.contains(token.front()); }};

    std::vector<Adjacent> pairs{};

    for (const auto& left : tokens)
    {
        for (const auto& right : tokens)
        {
            if (!reaches(left) || !reaches(right))
            {
                pairs.push_back({.left = left, .right = right});

                continue;
            }

            const auto segmented{reference_tokens(ranks, byte_symbols(left + right))};

            if (segmented == std::vector{left, right})
            {
                pairs.push_back({.left = left, .right = right});
            }
        }
    }

    return {tokens, pairs};
}

/**
 * @brief Returns the tokens a key rule segments a text into: at every step, of the adjacent pairs whose concatenation
 *        is a scored piece, the one of the highest score is merged, the leftmost or the rightmost of a tie.
 * @param scores The score of each piece.
 * @param tie The occurrence of a tie merged first.
 * @param text The text, each byte a symbol.
 * @return The tokens.
 */
std::vector<std::string> key_rule_tokens(const Scores_t& scores, const Tie tie, const std::string_view text)
{
    auto sequence{byte_symbols(text)};

    for (;;)
    {
        std::optional<int> best{};
        std::size_t at{0};

        for (std::size_t index{0}; index + 1 < sequence.size(); ++index)
        {
            const auto found{scores.find(sequence[index] + sequence[index + 1])};

            if (found == scores.cend())
            {
                continue;
            }

            const auto& [piece, score]{*found};

            const auto later_tie{best && score == *best && tie == Tie::rightmost};

            if (best && score <= *best && !later_tie)
            {
                continue;
            }

            best = score;
            at = index;
        }

        if (!best)
        {
            return sequence;
        }

        sequence[at] += sequence[at + 1];
        const auto next{sequence.begin() + static_cast<std::ptrdiff_t>(at) + 1};

        sequence.erase(next);
    }
}

/**
 * @brief Returns a key rule over {a, b} with its relation by definition: a token is canonical when the rule segments
 *        its spelling as itself, and a pair of canonical tokens when the rule segments their concatenation into the
 *        two.
 * @param scores The score of each piece.
 * @param tie The occurrence of a tie merged first.
 * @return The rule.
 */
Key_rule key_rule(const Scores_t& scores, const Tie tie)
{
    Key_rule rule{.scores = scores, .tie = tie, .tokens = {"a", "b"}};

    std::vector<std::string> pieces{};

    for (const auto& [piece, score] : scores)
    {
        pieces.push_back(piece);
    }

    const auto shorter{[](const std::string& left, const std::string& right) { return left.size() < right.size(); }};

    std::ranges::stable_sort(pieces, shorter);

    for (const auto& piece : pieces)
    {
        if (key_rule_tokens(scores, tie, piece) == std::vector{piece})
        {
            rule.tokens.push_back(piece);
            rule.longest = std::max(rule.longest, piece.size());
        }
    }

    for (const auto& left : rule.tokens)
    {
        for (const auto& right : rule.tokens)
        {
            if (key_rule_tokens(scores, tie, left + right) == std::vector{left, right})
            {
                rule.pairs.push_back({.left = left, .right = right});
            }
        }
    }

    rule.automaton = token_automaton(rule.tokens, rule.pairs);

    return rule;
}

/**
 * @brief Draws a key rule over {a, b}: two to six distinct pieces of two or three symbols, each scored 0, 1 or 2 so
 *        that ties are common, and a random tie direction.
 * @param sequence The random sequence.
 * @return The rule's scores and its tie direction.
 */
std::pair<Scores_t, Tie> random_key_rule(std::mt19937& sequence)
{
    auto pool{strings_over({"a", "b"}, 3)};

    pool.erase(pool.begin(), pool.begin() + 3);

    std::uniform_int_distribution<std::size_t> count{2, 6};
    std::uniform_int_distribution<std::size_t> place{0, pool.size() - 1};
    std::uniform_int_distribution<int> score{0, 2};
    std::bernoulli_distribution rightmost{0.5};

    Scores_t scores{};

    const auto wanted{count(sequence)};

    while (scores.size() < wanted)
    {
        const auto& piece{pool[place(sequence)]};

        scores.try_emplace(piece, score(sequence));
    }

    const auto tie{rightmost(sequence) ? Tie::rightmost : Tie::leftmost};

    return {scores, tie};
}

/**
 * @brief Returns the length of the shortest string over {a, b} of at most a number of bytes that two key rules segment
 *        apart, by brute force.
 * @param first The first rule.
 * @param second The second rule.
 * @param longest The most bytes searched.
 * @return The length, or std::nullopt when the two segment every such string alike.
 */
std::optional<std::size_t> shortest_separation(const Key_rule& first, const Key_rule& second, const std::size_t longest)
{
    for (const auto& text : strings_over({"a", "b"}, longest) | std::views::drop(1))
    {
        const auto by_first{key_rule_tokens(first.scores, first.tie, text)};
        const auto by_second{key_rule_tokens(second.scores, second.tie, text)};

        if (by_first != by_second)
        {
            return text.size();
        }
    }

    return std::nullopt;
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

TEST(Token_automaton_test,
     Over_the_characters_of_ascii_every_ascii_window_of_the_fixture_is_decided_as_on_the_byte_automaton)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/token_automaton_reference.txt"};

    ASSERT_TRUE(file.is_open());

    const auto alphabet{ascii_alphabet()};

    std::optional<Token_automaton> bytes{};
    std::optional<Token_automaton> characters{};

    std::size_t tables{0};
    std::size_t decisions{0};

    for (std::string line{}; std::getline(file, line);)
    {
        std::istringstream fields{line};

        std::string kind{};
        std::string first{};
        std::string second{};

        if (!(fields >> kind) || kind.starts_with('#'))
        {
            continue;
        }

        fields >> first >> second;

        if (kind == "universe")
        {
            const auto merges{merges_of(fields)};

            const auto spells_ascii{[](const Merge& merge) { return is_ascii(merge.left) && is_ascii(merge.right); }};

            const auto ascii{std::ranges::all_of(merges, spells_ascii)};

            bytes = ascii ? std::optional{token_automaton(merges)} : std::nullopt;
            characters = ascii ? std::optional{token_automaton(merges, alphabet)} : std::nullopt;
            tables += ascii ? 1 : 0;

            continue;
        }

        const auto window{bytes_of(first)};

        if (!characters || !is_ascii(window))
        {
            continue;
        }

        std::size_t origin{};

        std::istringstream{second} >> origin;

        ++decisions;

        expect_same(miscovering(*characters, window, origin), miscovering(*bytes, window, origin), line);
    }

    EXPECT_EQ(tables, 408U);
    EXPECT_EQ(decisions, 31360U);
}

TEST(Token_automaton_test,
     The_characters_of_the_alphabet_are_the_first_tokens_in_its_order_and_the_products_follow_them)
{
    const auto automaton{token_automaton(sentencepiece_merges(), sentencepiece_alphabet)};

    const auto& tokens{automaton.tokens()};

    ASSERT_EQ(tokens.size(), 21U);
    EXPECT_EQ(tokens[0].spelling, "▁");
    EXPECT_EQ(tokens[6].spelling, "é");
    EXPECT_EQ(tokens[10].spelling, "😀");
    EXPECT_EQ(automaton.token_of("▁t"), 11U);
    EXPECT_EQ(automaton.token_of("caét"), 20U);
    EXPECT_EQ(tokens[20].left, automaton.token_of("ca"));
    EXPECT_FALSE(automaton.token_of("x"));
    EXPECT_FALSE(automaton.token_of("\xE2\x96"));
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

    const auto bytes{token_automaton(merges)};

    const auto after_a{bytes.allowed_tokens(1)};

    EXPECT_EQ(after_a.size(), 257U);
    EXPECT_FALSE(std::ranges::contains(after_a, *bytes.token_of("b")));
    EXPECT_EQ(token_automaton({}).allowed_tokens(0).size(), 256U);

    const auto characters{token_automaton(sentencepiece_merges(), sentencepiece_alphabet)};

    for (const auto& automaton : {bytes, characters})
    {
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
}

TEST(Token_automaton_test,
     On_a_sentencepiece_style_table_every_window_of_up_to_three_characters_is_decided_as_on_the_tower)
{
    const auto merges{sentencepiece_merges()};

    const auto ranks{ranks_of(merges)};

    const auto automaton{token_automaton(merges, sentencepiece_alphabet)};

    const auto tower{merge_tower(merges, sentencepiece_alphabet)};

    const auto longest{std::string_view{"▁中文"}.size()};

    std::size_t certified{0};
    std::size_t refused{0};

    for (const auto& window : strings_over(characters_of(sentencepiece_alphabet), 3) | std::views::drop(1))
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            const auto context{std::format("{} at {}", window, origin)};

            const auto found{miscovering(automaton, window, origin)};

            const auto on_tower{miscovering(tower, window, origin)};

            EXPECT_EQ(found.has_value(), on_tower.has_value()) << context;

            if (!found)
            {
                ++certified;

                continue;
            }

            ++refused;

            const auto& [segmentation, occurrence]{*found};

            const auto& [bytes, boundaries]{segmentation};

            EXPECT_EQ(boundaries, character_marking(ranks, bytes)) << context;
            EXPECT_LE(bytes.size(), 2 * longest + window.size() - 2) << context;
            EXPECT_EQ(std::string_view{bytes}.substr(occurrence, window.size()), window) << context;
            EXPECT_NE(covering_boundary(segmentation, occurrence + window.size() - 1), occurrence + origin) << context;
            EXPECT_TRUE(is_accepted(tower, segmentation)) << context;
        }
    }

    EXPECT_EQ(certified, 1432U);
    EXPECT_EQ(refused, 7060U);
}

TEST(Token_automaton_test, An_alphabet_of_more_characters_than_a_byte_holds_is_decided_as_on_its_tower)
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

    const auto automaton{token_automaton(merges, alphabet)};

    const auto tower{merge_tower(merges, alphabet)};

    const std::vector characters{ideograph(0),   ideograph(1),   ideograph(2),
                                 ideograph(150), ideograph(151), ideograph(character_count - 1)};

    for (const auto& window : strings_over(characters, 2) | std::views::drop(1))
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            const auto found{miscovering(automaton, window, origin)};

            const auto on_tower{miscovering(tower, window, origin)};

            EXPECT_EQ(found.has_value(), on_tower.has_value()) << window << " at " << origin;
        }
    }

    EXPECT_EQ(automaton.tokens().size(), character_count + merges.size());
    EXPECT_FALSE(miscovering(automaton, ideograph(150), 0));
    EXPECT_TRUE(miscovering(automaton, ideograph(1), 0));
    EXPECT_TRUE(miscovering(automaton, ideograph(character_count - 1), 0));
}

TEST(Token_automaton_test, Refuses_an_alphabet_holding_a_character_twice_and_a_merge_naming_a_character_outside_it)
{
    const auto twice{refusal_over({}, "éb\xC3\xA9")};

    EXPECT_TRUE(twice.contains(R"(the alphabet carries "\xC3\xA9" twice)")) << twice;

    const auto outside{refusal_over({{.left = "é", .right = "t"}, {.left = "ét", .right = "ü"}}, "ét")};

    EXPECT_TRUE(outside.contains(R"(("\xC3\xA9t", "\xC3\xBC") at rank 1 names "\xC3\xBC")")) << outside;
    EXPECT_TRUE(refusal_over({{.left = "é", .right = "t"}}, "ét").empty());
}

TEST(Token_automaton_test, Refuses_a_relation_holding_a_token_twice_and_a_pair_naming_a_token_it_does_not_hold)
{
    const auto twice{relation_refusal({"a", "b", "a"}, {})};

    EXPECT_TRUE(twice.contains(R"(the tokens carry "a" twice)")) << twice;

    const auto missing{relation_refusal({"a", "b"}, {{.left = "a", .right = "b"}, {.left = "b", .right = "\xFF"}})};

    EXPECT_TRUE(missing.contains(R"(the pair ("b", "\xFF") names "\xFF", which the tokens do not carry)")) << missing;
    EXPECT_TRUE(relation_refusal({"a", "b"}, {{.left = "a", .right = "b"}}).empty());
}

TEST(Token_automaton_test, The_relation_of_the_ab_ba_tie_group_certifies_from_its_pairs_and_diverges_from_each_table)
{
    const std::vector<std::string> tokens{"a", "b", "ab", "ba"};

    const std::vector<Adjacent> pairs{
            {.left = "a", .right = "a"},   {.left = "a", .right = "ab"},  {.left = "b", .right = "b"},
            {.left = "b", .right = "ba"},  {.left = "ab", .right = "a"},  {.left = "ab", .right = "b"},
            {.left = "ab", .right = "ab"}, {.left = "ab", .right = "ba"}, {.left = "ba", .right = "a"},
            {.left = "ba", .right = "b"},  {.left = "ba", .right = "ab"}, {.left = "ba", .right = "ba"}};

    const auto relation{token_automaton(tokens, pairs)};

    const auto after_a{*relation.tokens()[*relation.token_of("a")].target};

    EXPECT_EQ(relation.state_count(), 5U);
    EXPECT_EQ(relation.allowed_tokens(0).size(), 4U);
    EXPECT_EQ(relation.allowed_tokens(after_a), (std::vector<Token_automaton::Token_t>{0, 2}));

    EXPECT_FALSE(miscovering(relation, "aa", 1));
    EXPECT_FALSE(miscovering(relation, "aab", 1));

    const auto found{miscovering(relation, "ab", 1)};

    ASSERT_TRUE(found);

    const auto& [segmentation, occurrence]{*found};

    EXPECT_EQ(segmentation.bytes, "ab");
    EXPECT_EQ(segmentation.boundaries, Marking_t({false, false}));

    const std::vector<Merge> ab_first{{.left = "a", .right = "b"}, {.left = "b", .right = "a"}};
    const std::vector<Merge> ba_first{{.left = "b", .right = "a"}, {.left = "a", .right = "b"}};

    EXPECT_EQ(encoder_divergence(relation, token_automaton(ab_first, "ab")), "bab");
    EXPECT_EQ(encoder_divergence(relation, token_automaton(ba_first, "ab")), "aba");
    EXPECT_FALSE(encoder_divergence(relation, relation));
    EXPECT_EQ(encoder_divergence(relation, token_automaton(ab_first)), std::string{'\0'});
}

TEST(Token_automaton_test,
     The_relation_gives_the_reference_answers_on_every_universe_of_the_adjacency_fixture_and_agrees_with_the_tables)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/adjacency_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Relation_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_relation_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 199U);
    EXPECT_EQ(check.counts.tables, 51U);
    EXPECT_EQ(check.counts.certifications, 10598U);
    EXPECT_EQ(check.counts.miscoverings, 7674U);
    EXPECT_EQ(check.counts.divergences, 114U);
    EXPECT_EQ(check.counts.equal, 72U);
}

TEST(Token_automaton_test, The_relation_of_every_small_table_of_the_fixture_decides_each_of_its_pairs_as_the_table_does)
{
    // The relation is read off the reference encoder, |V|^2 encodings, so the two GPT-2 prefixes of 1,000 and 5,000
    // merges are left out.
    constexpr std::size_t most_merges{64};

    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/token_automaton_reference.txt"};

    ASSERT_TRUE(file.is_open());

    std::optional<Ranks_t> ranks{};
    std::optional<Token_automaton> table{};
    std::optional<Token_automaton> relation{};

    std::size_t tables{0};
    std::size_t decisions{0};

    for (std::string line{}; std::getline(file, line);)
    {
        std::istringstream fields{line};

        std::string kind{};
        std::string first{};
        std::string second{};

        if (!(fields >> kind) || kind.starts_with('#'))
        {
            continue;
        }

        fields >> first >> second;

        if (kind == "universe")
        {
            const auto merges{merges_of(fields)};

            const auto small{merges.size() <= most_merges};

            ranks = small ? std::optional{ranks_of(merges)} : std::nullopt;

            if (!small)
            {
                continue;
            }

            const auto [tokens, pairs]{reference_relation(merges)};

            table = token_automaton(merges);
            relation = token_automaton(tokens, pairs);

            ++tables;

            EXPECT_FALSE(encoder_divergence(*table, *relation)) << line.substr(0, 200);
            EXPECT_FALSE(encoder_divergence(*relation, *table)) << line.substr(0, 200);

            continue;
        }

        if (!ranks)
        {
            continue;
        }

        const auto window{bytes_of(first)};

        std::size_t origin{};

        std::istringstream{second} >> origin;

        const auto found{miscovering(*relation, window, origin)};

        ++decisions;

        ASSERT_EQ(found.has_value(), miscovering(*table, window, origin).has_value()) << line;

        if (found)
        {
            EXPECT_EQ(found->segmentation.boundaries, reference_marking(*ranks, found->segmentation.bytes)) << line;
        }
    }

    EXPECT_EQ(tables, 408U);
    EXPECT_EQ(decisions, 32798U);
}

TEST(Token_automaton_test, Over_random_key_rules_with_ties_the_relation_decides_windows_and_divergence_as_brute_force)
{
    // Score sets over {a, b} with ties, each merged leftmost or rightmost first, checked on every window of up to three
    // bytes at every origin against every string of at most 2L + |window| - 1 bytes, and against the mirrored tie and
    // the rule drawn before it on every string of at most 2L + 2 bytes.
    constexpr std::size_t draws{300};

    std::mt19937 sequence{0x5EEDU};

    std::optional<Key_rule> previous{};

    std::size_t decisions{0};
    std::size_t refused{0};
    std::size_t compared{0};
    std::size_t separated{0};

    const auto windows{strings_over({"a", "b"}, 3)};

    const auto compare{[&](const Key_rule& rule, const Key_rule& other, const std::string& context) {
        const auto longest{std::max(rule.longest, other.longest)};

        const auto witness{encoder_divergence(*rule.automaton, *other.automaton)};

        const auto separation{shortest_separation(rule, other, 2 * longest + 2)};

        ++compared;

        ASSERT_EQ(witness.has_value(), separation.has_value()) << context;

        if (!witness)
        {
            return;
        }

        ++separated;

        const std::set<std::string> tokens{rule.tokens.cbegin(), rule.tokens.cend()};
        const std::set<std::string> other_tokens{other.tokens.cbegin(), other.tokens.cend()};

        EXPECT_LE(witness->size(), 2 * longest - 1) << context;
        EXPECT_NE(key_rule_tokens(rule.scores, rule.tie, *witness), key_rule_tokens(other.scores, other.tie, *witness))
                << context;
        EXPECT_GE(witness->size(), *separation) << context;

        if (tokens == other_tokens)
        {
            EXPECT_EQ(witness->size(), *separation) << context;
        }
    }};

    for (std::size_t drawn{0}; drawn < draws; ++drawn)
    {
        const auto [scores, tie]{random_key_rule(sequence)};

        const auto rule{key_rule(scores, tie)};

        const auto mirrored{key_rule(scores, tie == Tie::leftmost ? Tie::rightmost : Tie::leftmost)};

        const auto context{std::format("draw {}", drawn)};

        std::vector<Marked_string> texts{};

        for (const auto& text : strings_over({"a", "b"}, 2 * rule.longest + 2) | std::views::drop(1))
        {
            const auto tokens{key_rule_tokens(scores, tie, text)};

            texts.push_back({.bytes = text, .boundaries = token_marking(tokens)});
        }

        for (const auto& window : windows | std::views::drop(1))
        {
            for (std::size_t origin{0}; origin < window.size(); ++origin)
            {
                const auto bound{2 * rule.longest + window.size() - 1};

                const auto refutes{[&](const Marked_string& marked) {
                    return marked.bytes.size() <= bound && has_miscovered_occurrence(marked, window, origin);
                }};

                const auto by_brute_force{std::ranges::any_of(texts, refutes)};

                const auto found{miscovering(*rule.automaton, window, origin)};

                const auto line{std::format("{}, {} at {}", context, window, origin)};

                ++decisions;

                ASSERT_EQ(found.has_value(), by_brute_force) << line;

                if (!found)
                {
                    continue;
                }

                ++refused;

                const auto& [segmentation, occurrence]{*found};

                const auto& [bytes, boundaries]{segmentation};

                const auto last{occurrence + window.size() - 1};

                EXPECT_EQ(boundaries, token_marking(key_rule_tokens(scores, tie, bytes))) << line;
                EXPECT_LE(bytes.size(), bound - 1) << line;
                EXPECT_NE(covering_boundary(segmentation, last), occurrence + origin) << line;
            }
        }

        compare(rule, mirrored, context + " against its mirrored tie");

        if (previous)
        {
            compare(rule, *previous, context + " against the draw before it");
        }

        previous = rule;
    }

    EXPECT_EQ(decisions, 10200U);
    EXPECT_EQ(refused, 7721U);
    EXPECT_EQ(compared, 599U);
    EXPECT_EQ(separated, 477U);
}

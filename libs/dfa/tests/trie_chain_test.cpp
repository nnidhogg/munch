#include "munch/dfa/trie_chain.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "munch/dfa/verifier.hpp"
#include "munch/dfa/verifier_decisions.hpp"

using namespace munch::dfa;

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
     * @brief The string lines whose text the scanner does not consume whole.
     */
    std::size_t outsides{0};

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

    /**
     * @brief The pair lines whose witness falls in the domain half.
     */
    std::size_t domains{0};
};

/**
 * @brief A two-mode vocabulary: the initial tokens and the continuation tokens.
 */
struct Vocabulary
{
    /**
     * @brief The initial tokens.
     */
    std::vector<std::string> initial{};

    /**
     * @brief The continuation tokens.
     */
    std::vector<std::string> continuation{};
};

/**
 * @brief A universe of the fixture: its vocabulary and the trie chain of the vocabulary.
 */
struct Universe
{
    /**
     * @brief The vocabulary.
     */
    Vocabulary vocabulary;

    /**
     * @brief The trie chain of the vocabulary.
     */
    Verifier chain;
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
 * @brief Returns the length of the longest token of a set that is a prefix of a text.
 * @param tokens The tokens.
 * @param text The text.
 * @return The length, zero when no token is a prefix.
 */
std::size_t longest_prefix(const std::vector<std::string>& tokens, const std::string_view text)
{
    std::size_t best{0};

    for (const auto& token : tokens)
    {
        if (token.size() > best && text.starts_with(token))
        {
            best = token.size();
        }
    }

    return best;
}

/**
 * @brief Returns the boundary marking of the reference scanner's segmentation of a text, longest match among the
 *        initial tokens first and among the continuation tokens from then on, a boundary after every token but the
 *        last.
 * @param vocabulary The vocabulary.
 * @param text The text.
 * @return The marking, or std::nullopt when no token of the mode matches before the text ends.
 */
std::optional<Marking_t> reference_marking(const Vocabulary& vocabulary, const std::string_view text)
{
    Marking_t marking(text.size(), false);

    auto first{true};

    for (std::size_t position{0}; position < text.size(); first = false)
    {
        const auto& tokens{first ? vocabulary.initial : vocabulary.continuation};

        const auto length{longest_prefix(tokens, text.substr(position))};

        if (length == 0)
        {
            return std::nullopt;
        }

        position += length;

        if (position < text.size())
        {
            marking[position - 1] = true;
        }
    }

    return marking;
}

/**
 * @brief Returns the markings the reference scanner gives a text: its segmentation, or none.
 * @param vocabulary The vocabulary.
 * @param text The text.
 * @return The set of the reference marking, empty when the scanner does not consume the text whole.
 */
std::set<Marking_t> reference_markings(const Vocabulary& vocabulary, const std::string_view text)
{
    std::set<Marking_t> markings{};

    const auto marking{reference_marking(vocabulary, text)};

    if (marking)
    {
        markings.insert(*marking);
    }

    return markings;
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
 * @brief Checks that the chain's accepted markings of a text are exactly the reference scanner's.
 * @param universe The universe.
 * @param text The text.
 * @param context The fixture line or test checked.
 */
void expect_reference_marking(const Universe& universe, const std::string_view text, const std::string& context)
{
    EXPECT_EQ(accepted_markings(universe.chain, text), reference_markings(universe.vocabulary, text))
            << context << " on " << text;
}

/**
 * @brief Builds a universe: a vocabulary with the trie chain of it.
 * @param vocabulary The vocabulary.
 * @return The universe.
 */
Universe universe_of(Vocabulary vocabulary)
{
    auto chain{trie_chain(vocabulary.initial, vocabulary.continuation)};

    return {.vocabulary = std::move(vocabulary), .chain = std::move(chain)};
}

/**
 * @brief Returns the message the trie chain refuses a vocabulary with, or the empty text when it takes the
 *        vocabulary.
 * @param vocabulary The vocabulary.
 * @return The refusal.
 */
std::string refusal_of(const Vocabulary& vocabulary)
{
    try
    {
        std::ignore = trie_chain(vocabulary.initial, vocabulary.continuation);
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
 * @brief Checks that the chain's one accepted marking of a text is the reference scanner's, and that under it some
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

    const auto marking{reference_marking(universe.vocabulary, bytes)};

    ASSERT_TRUE(marking) << context << " on " << bytes;

    const Marked_string marked{.bytes = bytes, .boundaries = *marking};

    EXPECT_TRUE(has_miscovered_occurrence(marked, window, origin)) << context << " on " << bytes;
}

/**
 * @brief Checks that two universes segment a text apart: each chain's one accepted marking is its reference
 *        scanner's, and the two markings differ.
 * @param a The first universe.
 * @param b The second universe.
 * @param bytes The text.
 * @param context The fixture line checked.
 */
void expect_diverging(const Universe& a, const Universe& b, const std::string& bytes, const std::string& context)
{
    expect_reference_marking(a, bytes, context);
    expect_reference_marking(b, bytes, context);

    const auto first{reference_marking(a.vocabulary, bytes)};
    const auto second{reference_marking(b.vocabulary, bytes)};

    EXPECT_TRUE(first && second) << context << " on " << bytes;
    EXPECT_NE(first, second) << context << " on " << bytes;
}

/**
 * @brief Checks that exactly one of two universes scans a text whole: each chain's accepted markings are its reference
 *        scanner's, and exactly one scanner has one.
 * @param a The first universe.
 * @param b The second universe.
 * @param bytes The text.
 * @param context The fixture line checked.
 */
void expect_one_domain(const Universe& a, const Universe& b, const std::string& bytes, const std::string& context)
{
    expect_reference_marking(a, bytes, context);
    expect_reference_marking(b, bytes, context);

    const auto first{reference_marking(a.vocabulary, bytes)};
    const auto second{reference_marking(b.vocabulary, bytes)};

    EXPECT_NE(first.has_value(), second.has_value()) << context << " on " << bytes;
}

/**
 * @brief Starts a universe line's universe: reads its initial tokens, a bar, and its continuation tokens, and builds
 *        the trie chain of the vocabulary.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_universe(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    Vocabulary vocabulary{};

    auto barred{false};

    for (std::string token{}; fields >> token;)
    {
        if (token == "|")
        {
            barred = true;

            continue;
        }

        auto& tokens{barred ? vocabulary.continuation : vocabulary.initial};

        tokens.push_back(token);
    }

    ASSERT_TRUE(barred) << context;

    ++check.counts.universes;
    check.universe = context;
    check.universes.push_back(universe_of(std::move(vocabulary)));
}

/**
 * @brief Checks a states line: the chain's state count equal to the reference's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_state_count(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t expected{};

    fields >> expected;

    ++check.counts.state_counts;

    EXPECT_EQ(check.universes.back().chain.state_count(), expected) << context;
}

/**
 * @brief Checks a string line: the reference marking equal to the scanner's here, "outside" standing for a text the
 *        scanner does not consume whole, and the chain's accepted markings of the text exactly that one or none.
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

    if (bits == "outside")
    {
        ++check.counts.outsides;

        EXPECT_FALSE(reference_marking(universe.vocabulary, bytes)) << context;
    }
    else
    {
        EXPECT_EQ(reference_marking(universe.vocabulary, bytes), marking_of(bits)) << context;
    }

    expect_reference_marking(universe, bytes, context);
}

/**
 * @brief Checks a certify line: the certificate's answer, and for a miscovering its witness marked as the scanner
 *        marks it and as long as the reference's text, and both texts mis-covered under that marking.
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

    const auto found{miscovering(universe.chain, window, origin)};

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

    EXPECT_TRUE(is_accepted(universe.chain, segmentation)) << context;
    EXPECT_EQ(boundaries, reference_marking(universe.vocabulary, bytes)) << context;
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

    const auto gap{boundary_gap(check.universes.back().chain)};

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

    const auto& chain{check.universes.back().chain};

    const auto mask{realizable(chain)};

    ++check.counts.realizabilities;

    EXPECT_EQ(mask.has_value(), expected == 1) << context;

    if (mask)
    {
        EXPECT_EQ(mask->size(), chain.state_count()) << context;
    }
}

/**
 * @brief Checks a pair line: the divergence's half the reference's, its witness as long as the reference's text, and
 *        both texts segmented apart by the two scanners for the boundary half, or scanned whole by exactly one of them
 *        for the domain half, the witness marked as that scanner marks it.
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

    const auto found{divergence(a.chain, b.chain)};

    ++check.counts.pairs;

    if (half == "equivalent")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    const auto& [found_half, witness]{*found};

    const auto& [bytes, boundaries]{witness};

    EXPECT_EQ(bytes.size(), text_of(text).size()) << context;

    if (half == "boundary")
    {
        EXPECT_EQ(found_half, Half::boundary) << context;
        EXPECT_EQ(boundaries, reference_marking(a.vocabulary, bytes)) << context;

        expect_diverging(a, b, bytes, context);
        expect_diverging(a, b, text_of(text), context);

        return;
    }

    ASSERT_EQ(half, "domain") << context;

    ++check.counts.domains;

    const auto& accepting{is_accepted(a.chain, witness) ? a : b};

    EXPECT_EQ(found_half, Half::domain) << context;
    EXPECT_EQ(boundaries, reference_marking(accepting.vocabulary, bytes)) << context;

    expect_one_domain(a, b, bytes, context);
    expect_one_domain(a, b, text_of(text), context);
}

/**
 * @brief Checks one fixture line against the chain and the decisions, a comment or blank line checking nothing.
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

} // namespace

TEST(Trie_chain_test, Initial_a_and_ab_with_continuation_b_segment_abb_as_ab_then_b)
{
    const auto universe{universe_of({.initial = {"a", "ab"}, .continuation = {"b"}})};

    EXPECT_EQ(accepted_markings(universe.chain, "abb"), std::set<Marking_t>{marking_of("abb", {1})});
    EXPECT_EQ(accepted_markings(universe.chain, "ab"), std::set<Marking_t>{marking_of("ab", {})});
    EXPECT_EQ(accepted_markings(universe.chain, "abbb"), std::set<Marking_t>{marking_of("abbb", {1, 2})});
    EXPECT_EQ(accepted_markings(universe.chain, "a"), std::set<Marking_t>{marking_of("a", {})});
    EXPECT_EQ(accepted_markings(universe.chain, ""), std::set<Marking_t>{Marking_t{}});
    EXPECT_TRUE(universe.chain.accepts(universe.chain.start()));

    for (const auto text : {"b", "ba", "aa", "aab", "abab", "abba", "bbb", "ababab"})
    {
        expect_reference_marking(universe, text, "a, ab | b");
    }
}

TEST(Trie_chain_test, A_run_that_dies_leaves_the_text_outside_the_domain)
{
    const auto universe{universe_of({.initial = {"a"}, .continuation = {"ba", "a"}})};

    EXPECT_EQ(accepted_markings(universe.chain, "ab"), std::set<Marking_t>{});
    EXPECT_EQ(accepted_markings(universe.chain, "abab"), std::set<Marking_t>{});
    EXPECT_EQ(accepted_markings(universe.chain, "aba"), std::set<Marking_t>{marking_of("aba", {0})});
    EXPECT_EQ(accepted_markings(universe.chain, "aa"), std::set<Marking_t>{marking_of("aa", {0})});
    EXPECT_EQ(accepted_markings(universe.chain, "abaa"), std::set<Marking_t>{marking_of("abaa", {0, 2})});

    for (const auto text : {"b", "abb", "aaba", "ababa", "abaaba"})
    {
        expect_reference_marking(universe, text, "a | ba, a");
    }
}

TEST(Trie_chain_test, A_token_in_both_sets_is_read_by_the_mode_of_its_segment)
{
    const auto universe{universe_of({.initial = {"a", "ab", "b"}, .continuation = {"a", "b"}})};

    EXPECT_EQ(accepted_markings(universe.chain, "abab"), std::set<Marking_t>{marking_of("abab", {1, 2})});
    EXPECT_EQ(accepted_markings(universe.chain, "bab"), std::set<Marking_t>{marking_of("bab", {0, 1})});

    for (const auto text : {"aab", "abb", "baba", "abba", "bbab"})
    {
        expect_reference_marking(universe, text, "a, ab, b | a, b");
    }
}

TEST(Trie_chain_test, An_empty_set_is_taken)
{
    const auto no_initial{universe_of({.initial = {}, .continuation = {"a"}})};

    EXPECT_EQ(no_initial.chain.state_count(), 1U);
    EXPECT_TRUE(no_initial.chain.accepts(no_initial.chain.start()));
    EXPECT_EQ(accepted_markings(no_initial.chain, "a"), std::set<Marking_t>{});

    const auto no_continuation{universe_of({.initial = {"a", "b"}, .continuation = {}})};

    EXPECT_EQ(no_continuation.chain.state_count(), 3U);
    EXPECT_EQ(accepted_markings(no_continuation.chain, "a"), std::set<Marking_t>{marking_of("a", {})});
    EXPECT_EQ(accepted_markings(no_continuation.chain, "ab"), std::set<Marking_t>{});
}

TEST(Trie_chain_test, Refuses_an_empty_token)
{
    const auto initial{refusal_of({.initial = {"a", ""}, .continuation = {"b"}})};

    EXPECT_TRUE(initial.contains("the initial token at index 1 is empty")) << initial;

    const auto continuation{refusal_of({.initial = {"a"}, .continuation = {""}})};

    EXPECT_TRUE(continuation.contains("the continuation token at index 0 is empty")) << continuation;
}

TEST(Trie_chain_test, The_chain_and_the_decisions_give_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/dfa/tests/data/wordpiece_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 16U);
    EXPECT_EQ(check.counts.state_counts, 16U);
    EXPECT_EQ(check.counts.strings, 13713U);
    EXPECT_EQ(check.counts.outsides, 12328U);
    EXPECT_EQ(check.counts.certifications, 1280U);
    EXPECT_EQ(check.counts.miscoverings, 412U);
    EXPECT_EQ(check.counts.gaps, 16U);
    EXPECT_EQ(check.counts.realizabilities, 16U);
    EXPECT_EQ(check.counts.pairs, 136U);
    EXPECT_EQ(check.counts.domains, 80U);
}

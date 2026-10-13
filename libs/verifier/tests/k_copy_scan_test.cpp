#include "munch/verifier/k_copy_scan.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/verifier/verifier.hpp"
#include "munch/verifier/verifier_decisions.hpp"

using namespace munch::dfa;
using namespace munch::verifier;

namespace
{
/**
 * @brief A boundary marking, one bit per byte saying whether a boundary follows it.
 */
using Marking_t = std::vector<bool>;

/**
 * @brief Per input of a domain, its one accepted marking.
 */
using Domain_t = std::map<std::string, Marking_t>;

/**
 * @brief A window and its origin, the window owned.
 */
using Pair_t = std::pair<std::string, std::size_t>;

/**
 * @brief Per anchor, its configurations as the least verifier state of each, and the remaining bytes of the windows
 *        anchoring it.
 */
using Anchors_t = std::map<std::size_t, std::pair<std::set<Verifier::State_t>, std::vector<std::string_view>>>;

/**
 * @brief The longest input the brute-force comparisons enumerate.
 */
constexpr std::size_t longest_input{6};

/**
 * @brief What the scan fixture holds, counted as it is checked.
 */
struct Scan_counts
{
    /**
     * @brief The universe lines.
     */
    std::size_t universes{0};

    /**
     * @brief The accepting lines, one per table universe.
     */
    std::size_t tables{0};

    /**
     * @brief The bound lines, one per rank bound of a universe.
     */
    std::size_t bounds{0};

    /**
     * @brief The fanouts lines that find no window.
     */
    std::size_t none{0};

    /**
     * @brief The pair lines.
     */
    std::size_t pairs{0};

    /**
     * @brief The scan lines.
     */
    std::size_t scans{0};

    /**
     * @brief The chunks the scan lines give.
     */
    std::size_t chunks{0};

    /**
     * @brief The scan lines with a chunk that resolves past its end.
     */
    std::size_t impure{0};

    /**
     * @brief The scan lines with a lookahead.
     */
    std::size_t lookahead{0};
};

/**
 * @brief The scan fixture under check: the current universe's verifier, the transitions of a table universe read so
 *        far, the current universe's line, the shortest windows the decision gives it, the pair whose scans follow, and
 *        the counts so far.
 */
struct Scan_check
{
    /**
     * @brief The verifier the lines are checked on, none while a table universe's lines are still being read.
     */
    std::optional<Verifier> current{};

    /**
     * @brief The transitions of the table universe read so far.
     */
    Verifier::Transitions_t transitions{};

    /**
     * @brief The current universe's line, for the failure messages.
     */
    std::string universe{};

    /**
     * @brief The shortest windows shortest_fanout_windows() gives the current universe.
     */
    std::vector<Fanout_window> windows{};

    /**
     * @brief The window of the pair whose scans follow.
     */
    std::string window{};

    /**
     * @brief The origin of the pair whose scans follow.
     */
    std::size_t origin{0};

    /**
     * @brief The rank of the pair whose scans follow.
     */
    std::size_t rank{0};

    /**
     * @brief The lines checked so far, by kind.
     */
    Scan_counts counts{};
};

/**
 * @brief What the brute-force comparison of scans covered: the scans, those of inputs outside the domain, the chunks
 *        by the copies their workers ran, and the chunks that resolved past their end.
 */
struct Brute_counts
{
    /**
     * @brief The scans compared.
     */
    std::size_t scans{0};

    /**
     * @brief The scans of inputs outside the domain.
     */
    std::size_t outside{0};

    /**
     * @brief Per number of copies, the chunks whose worker ran that many.
     */
    std::map<std::size_t, std::size_t> copies{};

    /**
     * @brief The chunks that resolved past their end.
     */
    std::size_t late{0};
};

/**
 * @brief Builds the trie DFA of a set of literal tokens, each token numbered by its position.
 * @param tokens The literal tokens, nonempty.
 * @return The DFA.
 */
Dfa literal_dfa(const std::vector<std::string>& tokens)
{
    Builder builder{};

    std::unordered_map<Dfa::State_t, std::unordered_map<char, Dfa::State_t>> children{};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        auto state{builder.init_state()};

        for (const auto byte : tokens[index])
        {
            const auto [entry, inserted]{children[state].try_emplace(byte, 0)};

            auto& [child_byte, child]{*entry};

            if (inserted)
            {
                child = builder.next_state();
                builder.add_transition(state, Label{byte}, child);
            }

            state = child;
        }

        builder.add_accept_state(state, Token{index});
    }

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the tokens a(a)*b and a, at priorities zero and one.
 * @return The DFA.
 */
Dfa a_plus_b_and_a_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q2, Label{'a'}, q2);
    builder.add_transition(q1, Label{'b'}, q3);
    builder.add_transition(q2, Label{'b'}, q3);
    builder.add_accept_state(q3, Token{0});
    builder.add_accept_state(q1, Token{1});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the tokens ab|ba and a, at priorities zero and one.
 * @return The DFA.
 */
Dfa ab_or_ba_and_a_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q0, Label{'b'}, q3);
    builder.add_transition(q3, Label{'a'}, q2);
    builder.add_accept_state(q2, Token{0});
    builder.add_accept_state(q1, Token{1});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the tokens a(a)* and b, at priorities zero and one.
 * @return The DFA.
 */
Dfa a_plus_and_b_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q1);
    builder.add_transition(q0, Label{'b'}, q2);
    builder.add_accept_state(q1, Token{0});
    builder.add_accept_state(q2, Token{1});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the tokens ab|a and b, at priorities zero and one.
 * @return The DFA.
 */
Dfa a_or_ab_and_b_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q2);
    builder.add_transition(q0, Label{'b'}, q3);
    builder.add_accept_state(q1, Token{0});
    builder.add_accept_state(q2, Token{0});
    builder.add_accept_state(q3, Token{1});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the tokens aa? and bb?, at priorities zero and one.
 * @return The DFA.
 */
Dfa a_a_opt_and_b_b_opt_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};
    const auto q2{builder.next_state()};
    const auto q3{builder.next_state()};
    const auto q4{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q2);
    builder.add_transition(q0, Label{'b'}, q3);
    builder.add_transition(q3, Label{'b'}, q4);
    builder.add_accept_state(q1, Token{0});
    builder.add_accept_state(q2, Token{0});
    builder.add_accept_state(q3, Token{1});
    builder.add_accept_state(q4, Token{1});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of a regular universe of the scan fixture, by its index there.
 * @param index The index, below five.
 * @return The DFA.
 */
Dfa regular_dfa(const std::size_t index)
{
    const std::array builders{
            a_plus_b_and_a_dfa, ab_or_ba_and_a_dfa, a_plus_and_b_dfa, a_or_ab_and_b_dfa, a_a_opt_and_b_b_opt_dfa};

    return builders.at(index)();
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
 * @brief Returns the marking of a fixture field, one bit per byte, "-" standing for the empty marking.
 * @param field The field.
 * @return The marking.
 */
Marking_t marking_of(const std::string& field)
{
    const auto bits{text_of(field)};

    const auto is_set{[](const char bit) { return bit == '1'; }};

    Marking_t marking(bits.size(), false);

    std::ranges::transform(bits, marking.begin(), is_set);

    return marking;
}

/**
 * @brief Returns the marked symbol of a fixture step field, a byte and a mark bit, a0 reading a unmarked.
 * @param field The field.
 * @return The symbol.
 */
Marked symbol_of(const std::string& field)
{
    return {.byte = static_cast<unsigned char>(field.at(0)), .boundary_after = field.at(1) == '1'};
}

/**
 * @brief Returns the chunk of a fixture scan field, BEGIN:COPIES:RESOLVED.
 * @param field The field.
 * @return The chunk's first byte, its copies and the cut at which it resolves, its end left zero.
 */
Scanned_chunk chunk_of(const std::string& field)
{
    std::istringstream parts{field};

    Scanned_chunk chunk{};

    char separator{};

    parts >> chunk.begin >> separator >> chunk.copies >> separator >> chunk.resolved;

    return chunk;
}

/**
 * @brief Starts a universe line's universe: the armed run of a literal token set or of a regular universe by index, or
 *        nothing yet for a table, whose steps and accepting line follow.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_verifier_universe(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    std::string kind{};

    fields >> kind;

    ++check.counts.universes;
    check.universe = context;
    check.current.reset();
    check.transitions.clear();

    if (kind == "table")
    {
        return;
    }

    if (kind == "regular")
    {
        std::size_t index{};

        fields >> index;

        check.current.emplace(armed_run(regular_dfa(index)));

        return;
    }

    std::vector<std::string> tokens{};

    for (std::string token{}; fields >> token;)
    {
        tokens.push_back(token);
    }

    check.current.emplace(armed_run(literal_dfa(tokens)));
}

/**
 * @brief Reads a step line into the table universe's transitions.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void read_step(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    Verifier::State_t from{};
    std::string symbol{};
    Verifier::State_t to{};

    fields >> from >> symbol >> to;

    const auto [entry, inserted]{check.transitions.try_emplace({from, symbol_of(symbol)}, to)};

    EXPECT_TRUE(inserted) << context;
}

/**
 * @brief Builds the table universe's verifier from its transitions and an accepting line, the start being state zero.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void build_table(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    Verifier::Accept_states_t accepting{};

    for (Verifier::State_t state{}; fields >> state;)
    {
        accepting.insert(state);
    }

    EXPECT_FALSE(check.current) << context;

    check.current.emplace(0, std::exchange(check.transitions, {}), std::move(accepting));

    ++check.counts.tables;
}

/**
 * @brief Checks a fanouts line: the decision gives no window for "none", or one window per bound from the least rank to
 *        the largest, the first of rank the least.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_fanouts(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    std::string least{};
    std::size_t largest{0};

    fields >> least >> largest;

    ASSERT_TRUE(check.current) << context;

    check.windows = shortest_fanout_windows(*check.current);

    if (least == "none")
    {
        ++check.counts.none;

        EXPECT_TRUE(check.windows.empty()) << context;

        return;
    }

    const auto least_rank{std::stoul(least)};

    ASSERT_EQ(check.windows.size(), largest - least_rank + 1) << context;
    EXPECT_EQ(check.windows.front().bound, least_rank) << context;
    EXPECT_EQ(check.windows.front().rank, least_rank) << context;
    EXPECT_EQ(check.windows.back().bound, largest) << context;
}

/**
 * @brief Checks a bound line: the decision's window at the bound is the reference's, with its origin and rank, and its
 *        rank is the pair's fan-out.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_bound(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t bound{0};
    std::string window{};
    std::size_t origin{0};
    std::size_t rank{0};

    fields >> bound >> window >> origin >> rank;

    ASSERT_TRUE(check.current) << context;
    ASSERT_FALSE(check.windows.empty()) << context;

    ++check.counts.bounds;

    const auto index{bound - check.windows.front().bound};

    ASSERT_LT(index, check.windows.size()) << context;

    const auto& found{check.windows[index]};

    EXPECT_EQ(found.bound, bound) << context;
    EXPECT_EQ(found.window, window) << context;
    EXPECT_EQ(found.origin, origin) << context;
    EXPECT_EQ(found.rank, rank) << context;
    EXPECT_EQ(fanout(*check.current, window, origin).rank, rank) << context;
}

/**
 * @brief Checks a pair line: the pair's fan-out and relative fan-out are the reference's ranks, and the relative
 *        classes partition the configurations; the pair is kept for the scan lines that follow.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_pair(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t relative{0};

    fields >> check.window >> check.origin >> check.rank >> relative;

    ASSERT_TRUE(check.current) << context;

    ++check.counts.pairs;

    const auto [rank, configurations]{fanout(*check.current, check.window, check.origin)};

    const auto [relative_rank, classes]{relative_fanout(*check.current, check.window, check.origin)};

    EXPECT_EQ(rank, check.rank) << context;
    EXPECT_EQ(relative_rank, relative) << context;
    EXPECT_EQ(classes.size(), relative_rank) << context;

    std::vector<Verifier::State_t> joined{};

    for (const auto& relative_class : classes)
    {
        joined.insert(joined.end(), relative_class.cbegin(), relative_class.cend());
    }

    std::ranges::sort(joined);

    EXPECT_EQ(joined, configurations) << context;
}

/**
 * @brief Checks a scan line: the k-copy scan of the input cut at every anchor of the pair, aiming for as many chunks as
 *        the input has bytes, gives the reference's segmentation and chunks, its first chunk with one copy and every
 *        other with the pair's rank.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_scan(Scan_check& check, std::istringstream& fields, const std::string& context)
{
    std::size_t lookahead{0};
    std::string input_field{};
    std::string marks_field{};

    fields >> lookahead >> input_field >> marks_field;

    std::vector<Scanned_chunk> expected{};

    for (std::string field{}; fields >> field;)
    {
        expected.push_back(chunk_of(field));
    }

    ASSERT_TRUE(check.current) << context;

    ++check.counts.scans;
    check.counts.chunks += expected.size();
    check.counts.lookahead += lookahead > 0 ? 1 : 0;

    const auto input{text_of(input_field)};

    const std::array inventory{Fanout_pair{.window = check.window, .origin = check.origin}};

    const Scan_request request{.chunks = input.size(), .inventory = inventory, .lookahead = lookahead};

    const auto found{k_copy_scan(*check.current, input, request)};

    ASSERT_TRUE(found) << context;

    const auto& [segmentation, chunks]{*found};

    EXPECT_EQ(segmentation.bytes, input) << context;
    EXPECT_EQ(segmentation.boundaries, marking_of(marks_field)) << context;
    ASSERT_EQ(chunks.size(), expected.size()) << context;

    for (std::size_t chunk{0}; chunk < chunks.size(); ++chunk)
    {
        const auto& [begin, end, copies, resolved]{chunks[chunk]};

        const auto next{chunk + 1 < chunks.size() ? expected[chunk + 1].begin : input.size()};

        const auto pair_copies{chunk == 0 ? std::size_t{1} : check.rank};

        EXPECT_EQ(begin, expected[chunk].begin) << context << " at chunk " << chunk;
        EXPECT_EQ(end, next) << context << " at chunk " << chunk;
        EXPECT_EQ(copies, expected[chunk].copies) << context << " at chunk " << chunk;
        EXPECT_EQ(copies, pair_copies) << context << " at chunk " << chunk;
        EXPECT_EQ(resolved, expected[chunk].resolved) << context << " at chunk " << chunk;
    }

    const auto late{[](const Scanned_chunk& chunk) { return chunk.resolved != chunk.end; }};

    check.counts.impure += std::ranges::any_of(chunks, late) ? 1 : 0;
}

/**
 * @brief Checks one scan fixture line against the scanner and its decisions, a comment or blank line checking nothing.
 * @param check The fixture under check.
 * @param line The line.
 */
void expect_scan_fixture_line(Scan_check& check, const std::string& line)
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
        start_verifier_universe(check, fields, line);
    }
    else if (kind == "step")
    {
        read_step(check, fields, context);
    }
    else if (kind == "accepting")
    {
        build_table(check, fields, context);
    }
    else if (kind == "fanouts")
    {
        expect_fanouts(check, fields, context);
    }
    else if (kind == "bound")
    {
        expect_bound(check, fields, context);
    }
    else if (kind == "pair")
    {
        expect_pair(check, fields, context);
    }
    else if (kind == "scan")
    {
        expect_scan(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown scan fixture line: " << line;
    }
}

/**
 * @brief Lists every string over an alphabet up to a length, the empty one first.
 * @param alphabet The bytes.
 * @param longest The longest length.
 * @return The strings.
 */
std::vector<std::string> strings_up_to(const std::string_view alphabet, const std::size_t longest)
{
    std::vector<std::string> strings{std::string{}};

    for (std::size_t begin{0}; begin < strings.size(); ++begin)
    {
        if (strings[begin].size() == longest)
        {
            continue;
        }

        for (const auto byte : alphabet)
        {
            strings.push_back(strings[begin] + byte);
        }
    }

    return strings;
}

/**
 * @brief Draws a number below a bound from a sequence.
 * @param sequence The sequence, advanced by the draw.
 * @param below The bound.
 * @return The number.
 */
std::size_t draw(std::mt19937& sequence, const std::size_t below)
{
    std::uniform_int_distribution<std::size_t> distribution{0, below - 1};

    return distribution(sequence);
}

/**
 * @brief Draws a random token DFA over {a, b}: two to four states, the initial one among them, each byte leading from
 *        each to a random one of them or nowhere, one of them but the initial accepting and each other but the initial
 *        accepting with probability one third.
 * @param sequence The sequence the DFA is drawn from.
 * @return The DFA.
 */
Dfa random_token_dfa(std::mt19937& sequence)
{
    Builder builder{};

    std::vector<Dfa::State_t> states{builder.init_state()};

    for (std::size_t added{draw(sequence, 3) + 1}; added > 0; --added)
    {
        states.push_back(builder.next_state());
    }

    for (const auto from : states)
    {
        for (const auto byte : std::string_view{"ab"})
        {
            // One target beyond the states is no transition at all.
            const auto target{draw(sequence, states.size() + 1)};

            if (target < states.size())
            {
                builder.add_transition(from, Label{byte}, states[target]);
            }
        }
    }

    const auto accepting{states[draw(sequence, states.size() - 1) + 1]};

    builder.add_accept_state(accepting, Token{0});

    for (std::size_t index{1}; index < states.size(); ++index)
    {
        if (draw(sequence, 3) == 0)
        {
            builder.add_accept_state(states[index], Token{0});
        }
    }

    return std::move(builder).build();
}

/**
 * @brief Draws a random literal token set over {a, b}: one to four distinct tokens of one to three bytes.
 * @param sequence The sequence the tokens are drawn from.
 * @return The tokens.
 */
std::vector<std::string> random_tokens(std::mt19937& sequence)
{
    const auto words{strings_up_to("ab", 3)};

    std::set<std::string> tokens{};

    for (std::size_t wanted{draw(sequence, 4) + 1}; tokens.size() < wanted;)
    {
        const auto index{draw(sequence, words.size() - 1) + 1};

        tokens.insert(words[index]);
    }

    return {tokens.cbegin(), tokens.cend()};
}

/**
 * @brief Returns the token lengths longest match takes over an input from its start, std::nullopt where no token
 *        matches.
 * @param tokens The literal tokens.
 * @param input The input.
 * @return The lengths in input order.
 */
std::optional<std::vector<std::size_t>> longest_match_lengths(
        const std::vector<std::string>& tokens, const std::string_view input)
{
    std::vector<std::size_t> lengths{};

    for (std::size_t begin{0}; begin < input.size();)
    {
        const auto rest{input.substr(begin)};

        const auto matches{[rest](const std::string& token) { return rest.starts_with(token); }};

        std::size_t longest{0};

        for (const auto& token : tokens | std::views::filter(matches))
        {
            longest = std::max(longest, token.size());
        }

        if (longest == 0)
        {
            return std::nullopt;
        }

        lengths.push_back(longest);
        begin += longest;
    }

    return lengths;
}

/**
 * @brief Returns the marking of token lengths: a boundary after every token but the last.
 * @param lengths The token lengths in input order.
 * @return The marking.
 */
Marking_t marking_of_lengths(const std::vector<std::size_t>& lengths)
{
    Marking_t marking{};

    for (const auto length : lengths)
    {
        marking.insert(marking.end(), length, false);
        marking.back() = true;
    }

    if (!marking.empty())
    {
        marking.back() = false;
    }

    return marking;
}

/**
 * @brief Returns the domain of maximal munch over literal tokens, read forward or backward, every input over {a, b} up
 *        to a length that it segments with its marking.
 * @param tokens The literal tokens.
 * @param backward Whether the scan reads from the input's end, taking the longest token that ends there.
 * @param longest The length.
 * @return The domain.
 */
Domain_t literal_domain(const std::vector<std::string>& tokens, const bool backward, const std::size_t longest)
{
    auto reversed{tokens};

    for (auto& token : reversed)
    {
        std::ranges::reverse(token);
    }

    Domain_t domain{};

    for (const auto& input : strings_up_to("ab", longest))
    {
        auto read{input};

        if (backward)
        {
            std::ranges::reverse(read);
        }

        auto lengths{longest_match_lengths(backward ? reversed : tokens, read)};

        if (!lengths)
        {
            continue;
        }

        if (backward)
        {
            std::ranges::reverse(*lengths);
        }

        domain.emplace(input, marking_of_lengths(*lengths));
    }

    return domain;
}

/**
 * @brief Builds the trie verifier of a finite domain: one state per marked prefix of an accepted marked string, the
 *        start the empty one, accepting at the marked strings themselves.
 * @param domain The domain, every marking ending without a boundary.
 * @return The verifier.
 */
Verifier trie_verifier(const Domain_t& domain)
{
    std::map<std::pair<Verifier::State_t, Marked>, Verifier::State_t> children{};

    Verifier::Transitions_t transitions{};

    Verifier::Accept_states_t accepting{};

    Verifier::State_t states{1};

    for (const auto& [input, marking] : domain)
    {
        Verifier::State_t state{0};

        for (std::size_t at{0}; at < input.size(); ++at)
        {
            const Marked symbol{.byte = static_cast<unsigned char>(input[at]), .boundary_after = marking[at]};

            const auto [entry, inserted]{children.try_emplace({state, symbol}, states)};

            const auto& [key, child]{*entry};

            if (inserted)
            {
                transitions.emplace(std::pair{state, symbol}, child);
                ++states;
            }

            state = child;
        }

        accepting.insert(state);
    }

    return Verifier{0, std::move(transitions), std::move(accepting)};
}

/**
 * @brief Returns the accepted markings of every input over {a, b} up to a length, found by walking the verifier's steps
 *        from the start, one per accepted input since the verifier is functional.
 * @param verifier The verifier, functional.
 * @param longest The length.
 * @return The domain within the length.
 */
Domain_t accepted_up_to(const Verifier& verifier, const std::size_t longest)
{
    Domain_t accepted{};

    std::vector<std::tuple<Verifier::State_t, std::string, Marking_t>> pending{{verifier.start(), "", {}}};

    while (!pending.empty())
    {
        const auto [state, bytes, marking]{pending.back()};

        pending.pop_back();

        if (verifier.accepts(state))
        {
            accepted.emplace(bytes, marking);
        }

        if (bytes.size() == longest)
        {
            continue;
        }

        for (const auto byte : std::string_view{"ab"})
        {
            for (const auto boundary_after : {false, true})
            {
                const auto to{verifier.step(
                        state, {.byte = static_cast<unsigned char>(byte), .boundary_after = boundary_after})};

                if (!to)
                {
                    continue;
                }

                auto next_marking{marking};

                next_marking.push_back(boundary_after);

                pending.emplace_back(*to, bytes + byte, std::move(next_marking));
            }
        }
    }

    return accepted;
}

/**
 * @brief Returns the states a byte string leads a verifier state to under any marks.
 * @param verifier The verifier.
 * @param state The state read from.
 * @param bytes The byte string.
 * @return The states reached, none when every marking is refused.
 */
std::set<Verifier::State_t> reached_from(
        const Verifier& verifier, const Verifier::State_t state, const std::string_view bytes)
{
    std::set states{state};

    for (const auto byte : bytes)
    {
        std::set<Verifier::State_t> next{};

        for (const auto from : states)
        {
            for (const auto boundary_after : {false, true})
            {
                const auto to{verifier.step(
                        from, {.byte = static_cast<unsigned char>(byte), .boundary_after = boundary_after})};

                if (to)
                {
                    next.insert(*to);
                }
            }
        }

        states = std::move(next);
    }

    return states;
}

/**
 * @brief Returns the anchors of an inventory in an input by the definition: the cuts after position zero the origins
 *        land on at the occurrences, each with the intersection of the configurations fanout() gives the pairs
 *        anchoring it, and with the remaining bytes of those pairs' windows.
 * @param verifier The verifier.
 * @param input The input.
 * @param inventory The pairs.
 * @return The anchors.
 */
Anchors_t anchors_by_definition(
        const Verifier& verifier, const std::string_view input, const std::span<const Fanout_pair> inventory)
{
    Anchors_t anchors{};

    for (const auto& [window, origin] : inventory)
    {
        const auto [rank, configurations]{fanout(verifier, window, origin)};

        const std::set carried(configurations.cbegin(), configurations.cend());

        for (std::size_t at{0}; at + window.size() <= input.size(); ++at)
        {
            if (!input.substr(at).starts_with(window) || at + origin == 0)
            {
                continue;
            }

            const auto [entry, inserted]{anchors.try_emplace(at + origin, carried, std::vector<std::string_view>{})};

            auto& [cut, anchor]{*entry};

            auto& [states, remainders]{anchor};

            std::set<Verifier::State_t> shared{};

            std::ranges::set_intersection(states, carried, std::inserter(shared, shared.end()));

            states = std::move(shared);
            remainders.push_back(window.substr(origin));
        }
    }

    return anchors;
}

/**
 * @brief Returns the chunk positions a scan aiming for a number of chunks cuts at, by the definition: from zero, each
 *        cut the first anchor at or after the later of its target, the input's size times its index over the chunks
 *        usable, and one past the cut before, and none once a target has no anchor after it.
 * @param anchors The anchors.
 * @param size The input's size.
 * @param chunks The chunks aimed for.
 * @return The positions from zero to the input's size.
 */
std::vector<std::size_t> cuts_by_definition(const Anchors_t& anchors, const std::size_t size, const std::size_t chunks)
{
    const auto usable{std::min(chunks, size)};

    std::vector<std::size_t> cuts{0};

    for (std::size_t index{1}; index < usable; ++index)
    {
        const auto target{size * index / usable};

        const auto floor{std::max(target, cuts.back() + 1)};

        const auto found{anchors.lower_bound(floor)};

        if (found == anchors.cend())
        {
            break;
        }

        cuts.push_back(found->first);
    }

    cuts.push_back(size);

    return cuts;
}

/**
 * @brief Returns, per cut of a scan after the start, the markings of the input up to the cut that the stitch keeps
 *        alive, by the definition.
 *
 * A marking of the input up to a cut is kept alive when the verifier reads it from the start and, at every cut up to
 * there, its state reads the remaining bytes of every window anchoring the cut and the lookahead past the cut, reaching
 * acceptance on the lookahead when it runs to the input's end, and at the input's end accepts.
 * @param verifier The verifier, trim.
 * @param input The input.
 * @param anchors The anchors.
 * @param cuts The cut positions, from zero to the input's size.
 * @param lookahead The bytes a worker reads past its chunk.
 * @return Per cut, the markings kept alive there, none at the start.
 */
std::vector<std::set<Marking_t>> alive_by_definition(
        const Verifier& verifier, const std::string_view input, const Anchors_t& anchors,
        const std::vector<std::size_t>& cuts, const std::size_t lookahead)
{
    const auto survives{[&](const Verifier::State_t state, const std::size_t level) {
        if (level + 1 == cuts.size())
        {
            return verifier.accepts(state);
        }

        const auto position{cuts[level]};

        const auto reads_remainder{
                [&](const std::string_view remainder) { return !reached_from(verifier, state, remainder).empty(); }};

        if (!std::ranges::all_of(anchors.at(position).second, reads_remainder))
        {
            return false;
        }

        const auto reached{reached_from(verifier, state, input.substr(position, lookahead))};

        if (position + lookahead < input.size())
        {
            return !reached.empty();
        }

        const auto accepting{[&verifier](const Verifier::State_t to) { return verifier.accepts(to); }};

        return std::ranges::any_of(reached, accepting);
    }};

    std::vector<std::set<Marking_t>> alive(cuts.size());

    std::vector<std::tuple<Verifier::State_t, Marking_t, std::size_t>> pending{{verifier.start(), {}, 1}};

    while (!pending.empty())
    {
        auto [state, marking, level]{pending.back()};

        pending.pop_back();

        if (marking.size() == cuts[level])
        {
            if (!survives(state, level))
            {
                continue;
            }

            alive[level].insert(marking);

            if (level + 1 == cuts.size())
            {
                continue;
            }

            ++level;
        }

        const auto byte{static_cast<unsigned char>(input[marking.size()])};

        for (const auto boundary_after : {false, true})
        {
            const auto to{verifier.step(state, {.byte = byte, .boundary_after = boundary_after})};

            if (!to)
            {
                continue;
            }

            auto next_marking{marking};

            next_marking.push_back(boundary_after);

            pending.emplace_back(*to, std::move(next_marking), level);
        }
    }

    return alive;
}

/**
 * @brief Checks a k-copy scan against the definitions: outside the domain no scan, inside it the domain's marking, the
 *        chunks at the cuts the definition places, one copy for the first and the anchor's configurations for each
 *        other, and each chunk resolved at the first cut from its end on at which every marking the stitch keeps alive
 *        there has one marking up to the chunk's end.
 * @param verifier The verifier, functional.
 * @param domain The domain, every input up to the inputs' length.
 * @param input The input.
 * @param request The request.
 * @param counts The comparisons so far, advanced.
 * @param context The test checked.
 */
void expect_scan_by_definition(
        const Verifier& verifier, const Domain_t& domain, const std::string& input, const Scan_request& request,
        Brute_counts& counts, const std::string& context)
{
    const auto found{k_copy_scan(verifier, input, request)};

    ++counts.scans;

    const auto in_domain{domain.find(input)};

    if (in_domain == domain.cend())
    {
        ++counts.outside;

        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    const auto& [segmentation, chunks]{*found};

    EXPECT_EQ(segmentation.bytes, input) << context;
    EXPECT_EQ(segmentation.boundaries, in_domain->second) << context;

    const auto anchors{anchors_by_definition(verifier, input, request.inventory)};

    const auto cuts{cuts_by_definition(anchors, input.size(), request.chunks)};

    ASSERT_EQ(chunks.size() + 1, cuts.size()) << context;

    const auto alive{alive_by_definition(verifier, input, anchors, cuts, request.lookahead)};

    for (std::size_t chunk{0}; chunk < chunks.size(); ++chunk)
    {
        const auto& [begin, end, copies, resolved]{chunks[chunk]};

        EXPECT_EQ(begin, cuts[chunk]) << context << " at chunk " << chunk;
        EXPECT_EQ(end, cuts[chunk + 1]) << context << " at chunk " << chunk;

        const auto expected_copies{chunk == 0 ? std::size_t{1} : anchors.at(begin).first.size()};

        EXPECT_EQ(copies, expected_copies) << context << " at chunk " << chunk;

        ++counts.copies[copies];

        const auto agree_up_to_end{[&](const std::size_t level) {
            std::set<Marking_t> prefixes{};

            for (const auto& marking : alive[level])
            {
                prefixes.emplace(marking.cbegin(), marking.cbegin() + static_cast<std::ptrdiff_t>(end));
            }

            return prefixes.size() == 1;
        }};

        const auto levels{std::views::iota(chunk + 1, cuts.size())};

        const auto resolving{std::ranges::find_if(levels, agree_up_to_end)};

        ASSERT_NE(resolving, levels.end()) << context << " at chunk " << chunk;
        EXPECT_EQ(resolved, cuts[*resolving]) << context << " at chunk " << chunk;

        counts.late += resolved != end ? 1 : 0;
    }
}

/**
 * @brief Returns the residual of a verifier state over a finite domain: every marked string, each byte followed by its
 *        mark, that the verifier accepts from the state.
 * @param verifier The verifier, of a finite domain.
 * @param state The state.
 * @return The residual.
 */
std::set<std::string> residual_of(const Verifier& verifier, const Verifier::State_t state)
{
    std::set<std::string> residual{};

    std::vector<std::pair<Verifier::State_t, std::string>> pending{{state, ""}};

    while (!pending.empty())
    {
        const auto [from, marked]{pending.back()};

        pending.pop_back();

        if (verifier.accepts(from))
        {
            residual.insert(marked);
        }

        for (const auto& [key, to] : verifier.transitions())
        {
            const auto& [source, symbol]{key};

            if (source != from)
            {
                continue;
            }

            const auto mark{symbol.boundary_after ? '1' : '0'};

            pending.emplace_back(to, marked + static_cast<char>(symbol.byte) + mark);
        }
    }

    return residual;
}

/**
 * @brief Returns the residuals at the cuts of a pair by the definition, over a finite domain: the residual of the state
 *        the input's marking reaches at the cut of every occurrence whose cut lies after position zero.
 * @param verifier The verifier, of the finite domain.
 * @param domain The domain.
 * @param window The window.
 * @param origin The origin.
 * @return The residuals.
 */
std::set<std::set<std::string>> residuals_at_cuts(
        const Verifier& verifier, const Domain_t& domain, const std::string_view window, const std::size_t origin)
{
    std::set<std::set<std::string>> residuals{};

    for (const auto& [input, marking] : domain)
    {
        std::vector states{verifier.start()};

        for (std::size_t at{0}; at < input.size(); ++at)
        {
            const Marked symbol{.byte = static_cast<unsigned char>(input[at]), .boundary_after = marking[at]};

            states.push_back(*verifier.step(states.back(), symbol));
        }

        for (std::size_t at{0}; at + window.size() <= input.size(); ++at)
        {
            if (std::string_view{input}.substr(at).starts_with(window) && at + origin > 0)
            {
                residuals.insert(residual_of(verifier, states[at + origin]));
            }
        }
    }

    return residuals;
}

/**
 * @brief Returns how a residual reads a byte string, by the definition: per marking of the string, the marked strings
 *        the residual holds after it, none when every one is refused.
 * @param residual The residual.
 * @param bytes The byte string.
 * @return Per marking in counting order, the residual after it.
 */
std::vector<std::set<std::string>> outcomes_of(const std::set<std::string>& residual, const std::string_view bytes)
{
    std::vector<std::set<std::string>> outcomes{};

    for (std::size_t marks{0}; marks < (std::size_t{1} << bytes.size()); ++marks)
    {
        std::string prefix{};

        for (std::size_t at{0}; at < bytes.size(); ++at)
        {
            prefix += bytes[at];
            prefix += (marks >> at & 1) == 1 ? '1' : '0';
        }

        std::set<std::string> after{};

        for (const auto& marked : residual)
        {
            if (marked.starts_with(prefix))
            {
                after.insert(marked.substr(prefix.size()));
            }
        }

        outcomes.push_back(std::move(after));
    }

    return outcomes;
}

/**
 * @brief Checks the decisions on a verifier of a finite domain against the definitions: every pair of a window of the
 *        domain's length at most has the rank and the relative classes the residuals at its cuts give, and the
 *        shortest windows of every bound are the least pairs of rank between one and the bound.
 * @param verifier The verifier, of a finite domain.
 * @param domain The domain.
 * @param context The test checked.
 */
void expect_decisions_by_definition(const Verifier& verifier, const Domain_t& domain, const std::string& context)
{
    std::map<std::size_t, Pair_t> least_of_rank{};

    for (const auto& window : strings_up_to("ab", longest_input) | std::views::drop(1))
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            const auto pair_context{std::format("{} on ({}, {})", context, window, origin)};

            const auto residuals{residuals_at_cuts(verifier, domain, window, origin)};

            const auto [rank, configurations]{fanout(verifier, window, origin)};

            EXPECT_EQ(rank, residuals.size()) << pair_context;

            if (!residuals.empty())
            {
                least_of_rank.try_emplace(residuals.size(), window, origin);
            }

            const auto [relative_rank, classes]{relative_fanout(verifier, window, origin)};

            const auto remainder{std::string_view{window}.substr(origin)};

            std::set<std::vector<std::set<std::string>>> outcomes{};

            for (const auto& residual : residuals)
            {
                outcomes.insert(outcomes_of(residual, remainder));
            }

            EXPECT_EQ(relative_rank, outcomes.size()) << pair_context;

            std::set<std::vector<std::set<std::string>>> class_outcomes{};

            for (const auto& relative_class : classes)
            {
                std::set<std::vector<std::set<std::string>>> within{};

                for (const auto configuration : relative_class)
                {
                    within.insert(outcomes_of(residual_of(verifier, configuration), remainder));
                }

                EXPECT_EQ(within.size(), 1U) << pair_context;

                class_outcomes.insert(within.cbegin(), within.cend());
            }

            EXPECT_EQ(class_outcomes, outcomes) << pair_context;
        }
    }

    const auto windows{shortest_fanout_windows(verifier)};

    if (least_of_rank.empty())
    {
        EXPECT_TRUE(windows.empty()) << context;

        return;
    }

    const auto least_rank{least_of_rank.cbegin()->first};

    const auto largest_rank{least_of_rank.crbegin()->first};

    ASSERT_EQ(windows.size(), largest_rank - least_rank + 1) << context;

    std::optional<std::tuple<std::size_t, std::string, std::size_t, std::size_t>> shortest{};

    for (auto bound{least_rank}; bound <= largest_rank; ++bound)
    {
        if (const auto found{least_of_rank.find(bound)}; found != least_of_rank.cend())
        {
            const auto& [window, origin]{found->second};

            const std::tuple candidate{window.size(), window, origin, bound};

            if (!shortest || candidate < *shortest)
            {
                shortest = candidate;
            }
        }

        const auto& [size, window, origin, rank]{*shortest};

        const auto& found{windows[bound - least_rank]};

        EXPECT_EQ(found.bound, bound) << context;
        EXPECT_EQ(found.window, window) << context << " at bound " << bound;
        EXPECT_EQ(found.origin, origin) << context << " at bound " << bound;
        EXPECT_EQ(found.rank, rank) << context << " at bound " << bound;
    }
}

/**
 * @brief Draws an inventory of one to three pairs over {a, b}, each window of one to three bytes and its origin drawn
 *        among its bytes.
 * @param sequence The sequence the pairs are drawn from.
 * @return The windows and their origins.
 */
std::vector<Pair_t> random_inventory(std::mt19937& sequence)
{
    const auto words{strings_up_to("ab", 3)};

    std::vector<Pair_t> pairs{};

    for (auto count{draw(sequence, 3) + 1}; count > 0; --count)
    {
        const auto& window{words[draw(sequence, words.size() - 1) + 1]};

        const auto origin{draw(sequence, window.size())};

        pairs.emplace_back(window, origin);
    }

    return pairs;
}

/**
 * @brief Returns the inventories a verifier's scans are compared on: a drawn inventory, and each shortest window of a
 *        bound alone.
 * @param verifier The verifier.
 * @param sequence The sequence the drawn inventory is drawn from.
 * @return The inventories.
 */
std::vector<std::vector<Pair_t>> inventories_of(const Verifier& verifier, std::mt19937& sequence)
{
    std::vector<std::vector<Pair_t>> inventories{random_inventory(sequence)};

    for (const auto& [bound, window, origin, rank] : shortest_fanout_windows(verifier))
    {
        inventories.push_back({{window, origin}});
    }

    return inventories;
}

/**
 * @brief Checks the k-copy scan of every input over {a, b} up to the inputs' length against the definitions, for every
 *        inventory given, aiming for one, two and as many chunks as the input has bytes, with no lookahead and two
 *        bytes of it.
 * @param verifier The verifier, functional.
 * @param inventories The inventories.
 * @param counts The comparisons so far, advanced.
 * @param context The test checked.
 */
void expect_scans_by_definition(
        const Verifier& verifier, const std::vector<std::vector<Pair_t>>& inventories, Brute_counts& counts,
        const std::string& context)
{
    const auto domain{accepted_up_to(verifier, longest_input)};

    for (const auto& pairs : inventories)
    {
        std::vector<Fanout_pair> inventory{};

        std::string named{};

        for (const auto& [window, origin] : pairs)
        {
            inventory.push_back({.window = window, .origin = origin});
            named += std::format(" ({}, {})", window, origin);
        }

        for (const auto& input : strings_up_to("ab", longest_input))
        {
            for (const auto chunks : {std::size_t{1}, std::size_t{2}, input.size()})
            {
                for (const auto lookahead : {std::size_t{0}, std::size_t{2}})
                {
                    const Scan_request request{.chunks = chunks, .inventory = inventory, .lookahead = lookahead};

                    const auto scan_context{std::format(
                            "{} with{} on {}, {} chunks, lookahead {}", context, named, input, chunks, lookahead)};

                    expect_scan_by_definition(verifier, domain, input, request, counts, scan_context);
                }
            }
        }
    }
}

/**
 * @brief Builds the verifier of backward maximal munch over a and aa: a run of a's is segmented into aa from its right
 *        end, a single a first when its length is odd.
 * @return The verifier.
 */
Verifier backward_a_aa()
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};

    // The start; a first a with aa to follow; an a closing a token; the first a of an aa after it; and the end.
    Verifier::Transitions_t transitions{{{0, a}, 1}, {{0, a_closing}, 2}, {{1, a}, 4}, {{1, a_closing}, 2},
                                        {{2, a}, 3}, {{3, a_closing}, 2}, {{3, a}, 4}};

    return Verifier{0, std::move(transitions), {0, 1, 4}};
}

} // namespace

TEST(K_copy_scan_test, The_scanner_and_its_decisions_give_the_reference_answers_on_every_universe_of_the_scan_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/verifier/tests/data/scan_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Scan_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_scan_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 4154U);
    EXPECT_EQ(check.counts.tables, 1660U);
    EXPECT_EQ(check.counts.bounds, 16461U);
    EXPECT_EQ(check.counts.pairs, 13198U);
    EXPECT_EQ(check.counts.scans, 13393U);
    EXPECT_EQ(check.counts.chunks, 48138U);
    EXPECT_EQ(check.counts.impure, 254U);
    EXPECT_EQ(check.counts.lookahead, 40U);
}

TEST(K_copy_scan_test, Maximal_munch_over_aa_and_a_cuts_before_every_a_with_two_copies_and_selects_purely)
{
    const auto verifier{armed_run(literal_dfa({"aa", "a"}))};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    const Scan_request request{.chunks = 5, .inventory = inventory, .lookahead = 0};

    const auto found{k_copy_scan(verifier, "aaaaa", request)};

    ASSERT_TRUE(found);

    const auto& [segmentation, chunks]{*found};

    EXPECT_EQ(segmentation.boundaries, (Marking_t{false, true, false, true, false}));
    ASSERT_EQ(chunks.size(), 5U);
    EXPECT_EQ(chunks.front().copies, 1U);

    for (std::size_t chunk{1}; chunk < chunks.size(); ++chunk)
    {
        EXPECT_EQ(chunks[chunk].begin, chunk);
        EXPECT_EQ(chunks[chunk].copies, 2U);
    }

    for (const auto& [begin, end, copies, resolved] : chunks)
    {
        EXPECT_EQ(resolved, end);
    }
}

TEST(K_copy_scan_test, Maximal_munch_over_a_and_aab_resolves_the_first_a_late_without_lookahead_and_at_once_with_two)
{
    const auto verifier{armed_run(literal_dfa({"a", "aab"}))};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    const auto scanned{[&](const std::size_t lookahead) {
        const Scan_request request{.chunks = 2, .inventory = inventory, .lookahead = lookahead};

        return k_copy_scan(verifier, "aa", request);
    }};

    const auto late{scanned(0)};

    ASSERT_TRUE(late);
    EXPECT_EQ(late->segmentation.boundaries, (Marking_t{true, false}));
    ASSERT_EQ(late->chunks.size(), 2U);
    EXPECT_EQ(late->chunks[0].resolved, 2U);

    const auto pure{scanned(2)};

    ASSERT_TRUE(pure);
    EXPECT_EQ(pure->segmentation.boundaries, (Marking_t{true, false}));
    ASSERT_EQ(pure->chunks.size(), 2U);
    EXPECT_EQ(pure->chunks[0].resolved, 1U);
}

TEST(K_copy_scan_test, Maximal_munch_over_a_with_a_lookahead_to_the_input_end_resolves_every_one_byte_chunk_at_its_end)
{
    const auto verifier{armed_run(literal_dfa({"a"}))};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    for (const std::size_t length : {8U, 32U, 128U})
    {
        const std::string input(length, 'a');

        const Scan_request request{.chunks = length, .inventory = inventory, .lookahead = length};

        const auto found{k_copy_scan(verifier, input, request)};

        ASSERT_TRUE(found) << input;

        const auto& [segmentation, chunks]{*found};

        Marking_t expected(length, true);

        expected.back() = false;

        EXPECT_EQ(segmentation.boundaries, expected) << input;
        ASSERT_EQ(chunks.size(), length) << input;

        for (const auto& [begin, end, copies, resolved] : chunks)
        {
            EXPECT_EQ(end, begin + 1) << input;
            EXPECT_EQ(copies, 1U) << input;
            EXPECT_EQ(resolved, end) << input;
        }
    }
}

TEST(K_copy_scan_test,
     Backward_maximal_munch_over_a_and_aa_without_lookahead_resolves_every_chunk_of_a_run_only_at_the_input_end)
{
    const auto verifier{backward_a_aa()};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    for (std::size_t length{2}; length <= 7; ++length)
    {
        const std::string input(length, 'a');

        const Scan_request request{.chunks = length, .inventory = inventory, .lookahead = 0};

        const auto found{k_copy_scan(verifier, input, request)};

        ASSERT_TRUE(found) << input;

        const auto& [segmentation, chunks]{*found};

        Marking_t expected(length, false);

        for (auto at{length % 2 == 1 ? std::size_t{0} : std::size_t{1}}; at + 1 < length; at += 2)
        {
            expected[at] = true;
        }

        EXPECT_EQ(segmentation.boundaries, expected) << input;
        EXPECT_EQ(chunks.size(), length) << input;

        for (const auto& [begin, end, copies, resolved] : chunks)
        {
            EXPECT_EQ(resolved, length) << input << " at " << begin;
        }
    }
}

TEST(K_copy_scan_test, A_scan_of_an_input_outside_the_domain_finds_no_chain)
{
    const auto verifier{armed_run(literal_dfa({"aa", "a"}))};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    const Scan_request request{.chunks = 3, .inventory = inventory, .lookahead = 0};

    EXPECT_FALSE(k_copy_scan(verifier, "aab", request));
    EXPECT_FALSE(k_copy_scan(verifier, "b", request));
}

TEST(K_copy_scan_test, An_empty_inventory_or_input_or_no_chunks_aimed_for_scans_one_chunk)
{
    const auto verifier{armed_run(literal_dfa({"aa", "a"}))};

    const auto expect_one_chunk{[&](const std::string_view input, const Scan_request& request) {
        const auto found{k_copy_scan(verifier, input, request)};

        ASSERT_TRUE(found) << input;

        const auto& [segmentation, chunks]{*found};

        ASSERT_EQ(chunks.size(), 1U) << input;
        EXPECT_EQ(chunks.front().begin, 0U) << input;
        EXPECT_EQ(chunks.front().end, input.size()) << input;
        EXPECT_EQ(chunks.front().copies, 1U) << input;
        EXPECT_EQ(chunks.front().resolved, input.size()) << input;
        EXPECT_EQ(segmentation.boundaries.size(), input.size()) << input;
    }};

    const std::array inventory{Fanout_pair{.window = "a", .origin = 0}};

    expect_one_chunk("aaa", {.chunks = 3, .inventory = {}, .lookahead = 0});
    expect_one_chunk("", {.chunks = 3, .inventory = inventory, .lookahead = 0});
    expect_one_chunk("aaa", {.chunks = 0, .inventory = inventory, .lookahead = 0});
}

TEST(K_copy_scan_test, The_scanner_and_relative_fanout_refuse_a_malformed_window)
{
    const auto verifier{armed_run(literal_dfa({"a", "ab"}))};

    const std::array empty{Fanout_pair{.window = "", .origin = 0}};
    const std::array outside{Fanout_pair{.window = "ab", .origin = 2}};

    const Scan_request empty_request{.chunks = 2, .inventory = empty, .lookahead = 0};
    const Scan_request outside_request{.chunks = 2, .inventory = outside, .lookahead = 0};

    EXPECT_THROW(std::ignore = k_copy_scan(verifier, "ab", empty_request), std::invalid_argument);
    EXPECT_THROW(std::ignore = k_copy_scan(verifier, "ab", outside_request), std::invalid_argument);
    EXPECT_THROW(std::ignore = relative_fanout(verifier, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = relative_fanout(verifier, "ab", 2), std::invalid_argument);
}

TEST(K_copy_scan_test, Maximal_munch_over_a_b_and_aa_has_rank_two_and_relative_rank_one_at_the_certified_cut_before_b)
{
    const auto verifier{armed_run(literal_dfa({"a", "b", "aa"}))};

    const auto [rank, configurations]{fanout(verifier, "b", 0)};

    const auto [relative_rank, classes]{relative_fanout(verifier, "b", 0)};

    EXPECT_EQ(rank, 2U);
    EXPECT_EQ(relative_rank, 1U);
    ASSERT_EQ(classes.size(), 1U);
    EXPECT_EQ(classes.front(), configurations);
}

TEST(K_copy_scan_test, Maximal_munch_over_a_and_aab_has_the_shortest_window_b_at_bound_one_and_a_at_bound_two)
{
    const auto verifier{armed_run(literal_dfa({"a", "aab"}))};

    const auto windows{shortest_fanout_windows(verifier)};

    ASSERT_EQ(windows.size(), 2U);

    const auto& [first_bound, first_window, first_origin, first_rank]{windows[0]};

    EXPECT_EQ(first_bound, 1U);
    EXPECT_EQ(first_window, "b");
    EXPECT_EQ(first_origin, 0U);
    EXPECT_EQ(first_rank, 1U);

    const auto& [second_bound, second_window, second_origin, second_rank]{windows[1]};

    EXPECT_EQ(second_bound, 2U);
    EXPECT_EQ(second_window, "a");
    EXPECT_EQ(second_origin, 0U);
    EXPECT_EQ(second_rank, 2U);
}

TEST(K_copy_scan_test, The_decisions_agree_with_the_definitions_over_finite_forward_and_backward_maximal_munch)
{
    std::mt19937 sequence{20261013};

    for (std::size_t drawn{0}; drawn < 12; ++drawn)
    {
        const auto tokens{random_tokens(sequence)};

        for (const auto backward : {false, true})
        {
            const auto domain{literal_domain(tokens, backward, longest_input)};

            const auto verifier{trie_verifier(domain)};

            auto named{std::format("{} maximal munch over", backward ? "backward" : "forward")};

            for (const auto& token : tokens)
            {
                named += " " + token;
            }

            expect_decisions_by_definition(verifier, domain, named);
        }
    }
}

TEST(K_copy_scan_test, The_scanner_agrees_with_the_definitions_over_random_verifiers_and_inventories)
{
    std::mt19937 sequence{20261013};

    Brute_counts counts{};

    for (std::size_t drawn{0}; drawn < 6; ++drawn)
    {
        const auto tokens{random_tokens(sequence)};

        for (const auto backward : {false, true})
        {
            const auto verifier{trie_verifier(literal_domain(tokens, backward, longest_input))};

            const auto inventories{inventories_of(verifier, sequence)};

            expect_scans_by_definition(verifier, inventories, counts, std::format("finite draw {}", drawn));
        }

        const auto armed{armed_run(random_token_dfa(sequence))};

        const auto inventories{inventories_of(armed, sequence)};

        expect_scans_by_definition(armed, inventories, counts, std::format("armed draw {}", drawn));
    }

    // Cuts of two and three configurations, inputs outside the domain and chunks resolving late are all reached.
    EXPECT_GT(counts.copies[2], 0U);
    EXPECT_GT(counts.copies[3], 0U);
    EXPECT_GT(counts.outside, 0U);
    EXPECT_GT(counts.late, 0U);
}

#include "munch/dfa/verifier.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "munch/dfa/builder.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/verifier_decisions.hpp"

using namespace munch::dfa;

namespace
{
/**
 * @brief A boundary marking, one bit per byte saying whether a boundary follows it.
 */
using Marking_t = std::vector<bool>;

/**
 * @brief The longest input the exhaustive comparisons enumerate.
 */
constexpr std::size_t longest_input{6};

/**
 * @brief A universe of the fixture: the DFA of its token set and the verifier of the DFA's armed run.
 */
struct Universe
{
    /**
     * @brief The DFA of the token set.
     */
    Dfa dfa;

    /**
     * @brief The verifier of the DFA's armed run.
     */
    Verifier verifier;
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
     * @brief The certify lines.
     */
    std::size_t certifications{0};

    /**
     * @brief The certify lines whose answer is a miscovering.
     */
    std::size_t miscoverings{0};

    /**
     * @brief The gap and lasso lines.
     */
    std::size_t gaps{0};

    /**
     * @brief The lasso lines.
     */
    std::size_t lassos{0};

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
 * @brief The reference fixture under check: the current universe, the literal universes by index, and the counts so
 *        far.
 */
struct Fixture_check
{
    /**
     * @brief The universe the lines are checked in.
     */
    std::optional<Universe> current{};

    /**
     * @brief The literal universes, by the index the pair lines name them by.
     */
    std::vector<Universe> literals{};

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
 * @brief What the cut fixture holds, counted as it is checked.
 */
struct Cut_counts
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
     * @brief The cut lines.
     */
    std::size_t cuts{0};

    /**
     * @brief The cut lines whose pair is not certified.
     */
    std::size_t uncertified{0};

    /**
     * @brief The cut lines whose answer is independence.
     */
    std::size_t independent{0};

    /**
     * @brief The cut lines whose answer is a dependence.
     */
    std::size_t dependent{0};

    /**
     * @brief The dependences whose prefix fails.
     */
    std::size_t prefix_failures{0};

    /**
     * @brief The dependences whose suffix fails.
     */
    std::size_t suffix_failures{0};
};

/**
 * @brief The cut fixture under check: the current universe's verifier, the transitions of a table universe read so
 *        far, the current universe's line, and the counts so far.
 */
struct Cut_check
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
     * @brief The lines checked so far, by kind.
     */
    Cut_counts counts{};
};

/**
 * @brief What the chunk fixture holds, counted as it is checked.
 */
struct Chunk_counts
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
     * @brief The inventory lines.
     */
    std::size_t inventories{0};

    /**
     * @brief The inventory lines whose answer is chunk independence.
     */
    std::size_t independent{0};

    /**
     * @brief The inventory lines whose answer is a chunk dependence.
     */
    std::size_t dependent{0};

    /**
     * @brief The inventories whose pairs are all independent by dependence(), each checked chunk-independent.
     */
    std::size_t cut_independent{0};

    /**
     * @brief The chunk-independent inventories with a pair that is not independent by dependence().
     */
    std::size_t converse{0};
};

/**
 * @brief The chunk fixture under check: the current universe's verifier, the transitions of a table universe read so
 *        far, the current universe's line, and the counts so far.
 */
struct Chunk_check
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
     * @brief The lines checked so far, by kind.
     */
    Chunk_counts counts{};
};

/**
 * @brief The sides of a cut that fail, the prefix then the suffix.
 */
using Failures_t = std::pair<bool, bool>;

/**
 * @brief The pairs of an inventory line, each a window and its origin, and the verdict that follows them.
 */
using Inventory_line_t = std::pair<std::vector<std::pair<std::string, std::size_t>>, std::string>;

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
 * @brief Builds the DFA of the token language (ab)*a.
 * @return The DFA.
 */
Dfa ab_star_a_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'b'}, q0);
    builder.add_accept_state(q1, Token{0});

    return std::move(builder).build();
}

/**
 * @brief Builds the DFA of the token language a(aa)*.
 * @return The DFA.
 */
Dfa a_odd_dfa()
{
    Builder builder{};

    const auto q0{builder.init_state()};
    const auto q1{builder.next_state()};

    builder.add_transition(q0, Label{'a'}, q1);
    builder.add_transition(q1, Label{'a'}, q0);
    builder.add_accept_state(q1, Token{0});

    return std::move(builder).build();
}

/**
 * @brief Returns the boundary projection of the maximal-munch scan of an input, by a reference scan over the DFA.
 * @param dfa The DFA of the token set.
 * @param input The input.
 * @return The marking, a boundary after every token but the last, or std::nullopt when the input is not completely
 *         tokenizable.
 */
std::optional<Marking_t> munch_marking(const Dfa& dfa, const std::string_view input)
{
    Marking_t marking(input.size(), false);

    std::size_t begin{0};

    while (begin < input.size())
    {
        std::optional<std::size_t> longest{};

        auto state{std::optional{dfa.init_state()}};

        for (auto end{begin}; end < input.size() && state; ++end)
        {
            state = dfa.advance(*state, input[end]);

            if (state && dfa.has_accept_token(*state))
            {
                longest = end + 1;
            }
        }

        if (!longest)
        {
            return std::nullopt;
        }

        begin = *longest;

        if (begin < input.size())
        {
            marking[begin - 1] = true;
        }
    }

    return marking;
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
 * @brief Checks that the verifier's accepted markings of every string over {a, b} up to the longest length are the
 *        maximal-munch marking when the string is completely tokenizable and none otherwise.
 * @param dfa The DFA of the token set.
 * @param verifier The verifier of the DFA.
 */
void expect_munch_markings(const Dfa& dfa, const Verifier& verifier)
{
    for (const auto& input : strings_up_to("ab", longest_input))
    {
        const auto expected{munch_marking(dfa, input)};

        const auto accepted{accepted_markings(verifier, input)};

        const auto wanted{expected ? std::set<Marking_t>{*expected} : std::set<Marking_t>{}};

        EXPECT_EQ(accepted, wanted) << "input " << input;
    }
}

/**
 * @brief Checks that every state is reachable from the start and can reach an accepting state, and that no transition
 *        enters the start.
 * @param verifier The verifier.
 */
void expect_trim(const Verifier& verifier)
{
    std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>> forward{};
    std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>> backward{};

    for (const auto& [key, to] : verifier.transitions())
    {
        const auto& [from, symbol]{key};

        EXPECT_LT(from, verifier.state_count());
        EXPECT_LT(to, verifier.state_count());
        EXPECT_NE(to, verifier.start());

        forward[from].push_back(to);
        backward[to].push_back(from);
    }

    const auto closure{[](const std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>>& edges,
                          std::vector<Verifier::State_t> pending) {
        std::unordered_set<Verifier::State_t> seen{pending.cbegin(), pending.cend()};

        while (!pending.empty())
        {
            const auto state{pending.back()};

            pending.pop_back();

            const auto found{edges.find(state)};

            if (found == edges.cend())
            {
                continue;
            }

            const auto& [from, successors]{*found};

            for (const auto next : successors)
            {
                const auto [position, inserted]{seen.insert(next)};

                if (inserted)
                {
                    pending.push_back(next);
                }
            }
        }

        return seen;
    }};

    const std::vector<Verifier::State_t> accepting{verifier.accept_states().cbegin(), verifier.accept_states().cend()};

    const auto reachable{closure(forward, {verifier.start()})};
    const auto coaccessible{closure(backward, accepting)};

    EXPECT_EQ(reachable.size(), verifier.state_count());
    EXPECT_EQ(coaccessible.size(), verifier.state_count());
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
 * @brief Builds the DFA of a regular universe of the reference fixture, by its index there.
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
 * @brief Returns the marked string of a fixture step list, each step a byte and a mark bit, "-" standing for no steps.
 * @param field The step list.
 * @return The marked string.
 */
Marked_string steps_of(const std::string& field)
{
    Marked_string steps{};

    const auto text{text_of(field)};

    for (std::size_t index{0}; index + 1 < text.size(); index += 2)
    {
        steps.bytes.push_back(text[index]);
        steps.boundaries.push_back(text[index + 1] == '1');
    }

    return steps;
}

/**
 * @brief Returns the concatenation of marked strings.
 * @param parts The marked strings, in order.
 * @return The concatenation.
 */
Marked_string concatenated(const std::vector<Marked_string>& parts)
{
    Marked_string whole{};

    for (const auto& [bytes, boundaries] : parts)
    {
        whole.bytes += bytes;
        whole.boundaries.insert(whole.boundaries.end(), boundaries.cbegin(), boundaries.cend());
    }

    return whole;
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
 * @brief Checks that stem, loop repeated zero, one and two times, and suffix are accepted, the loop nonempty and
 *        markless.
 * @param verifier The verifier.
 * @param lasso The lasso.
 * @param context The fixture line checked.
 */
void expect_lasso_replays(const Verifier& verifier, const Lasso& lasso, const std::string& context)
{
    const auto& [stem, loop, suffix]{lasso};

    const auto& [loop_bytes, loop_boundaries]{loop};

    EXPECT_FALSE(loop_bytes.empty()) << context;
    EXPECT_TRUE(std::ranges::none_of(loop_boundaries, std::identity{})) << context;

    for (const auto& pumped :
         {std::vector{stem, suffix}, std::vector{stem, loop, suffix}, std::vector{stem, loop, loop, suffix}})
    {
        EXPECT_TRUE(is_accepted(verifier, concatenated(pumped))) << context;
    }
}

/**
 * @brief Returns the states that can reach an accepting state.
 * @param verifier The verifier.
 * @return The completable states.
 */
std::unordered_set<Verifier::State_t> completable_states(const Verifier& verifier)
{
    std::unordered_set<Verifier::State_t> completable{verifier.accept_states()};

    for (auto grown{true}; grown;)
    {
        grown = false;

        for (const auto& [key, to] : verifier.transitions())
        {
            if (!completable.contains(to))
            {
                continue;
            }

            const auto& [from, symbol]{key};

            const auto [position, inserted]{completable.insert(from)};

            grown = grown || inserted;
        }
    }

    return completable;
}

/**
 * @brief Checks the mask's property and its exactness: every completable state's admitted steps are its transitions
 *        into completable states, sorted, and it accepts or has one; a state that cannot complete admits nothing.
 * @param verifier The verifier.
 * @param mask The mask.
 * @param context The fixture line checked.
 */
void expect_mask(const Verifier& verifier, const Mask& mask, const std::string& context)
{
    const auto completable{completable_states(verifier)};

    ASSERT_EQ(mask.size(), verifier.state_count()) << context;

    Mask expected(verifier.state_count());

    for (const auto& [key, to] : verifier.transitions())
    {
        const auto& [from, symbol]{key};

        if (completable.contains(from) && completable.contains(to))
        {
            expected[from].push_back(symbol);
        }
    }

    for (Verifier::State_t state{0}; state < mask.size(); ++state)
    {
        std::ranges::sort(expected[state]);

        EXPECT_EQ(mask[state], expected[state]) << context << " at state " << state;

        if (completable.contains(state))
        {
            EXPECT_TRUE(verifier.accepts(state) || !mask[state].empty()) << context;
        }
    }
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
 * @brief Checks that an input is in a universe's domain, the verifier's one accepted marking of it the maximal-munch
 *        scan's, and that under it some occurrence of the window is mis-covered.
 * @param universe The universe.
 * @param bytes The input.
 * @param window The window.
 * @param origin The origin.
 * @param context The fixture line checked.
 */
void expect_miscovered(
        const Universe& universe, const std::string& bytes, const std::string_view window, const std::size_t origin,
        const std::string& context)
{
    const auto marking{munch_marking(universe.dfa, bytes)};

    ASSERT_TRUE(marking) << context << " on " << bytes;
    EXPECT_EQ(accepted_markings(universe.verifier, bytes), std::set<Marking_t>{*marking}) << context << " on " << bytes;
    EXPECT_TRUE(has_miscovered_occurrence({.bytes = bytes, .boundaries = *marking}, window, origin))
            << context << " on " << bytes;
}

/**
 * @brief Checks that two universes diverge on an input in a half: each verifier accepts exactly the maximal-munch
 *        marking of its DFA when there is one, and both scans complete with markings apart for the boundary half, or
 *        exactly one completes for the domain half.
 * @param a The first universe.
 * @param b The second universe.
 * @param half The half.
 * @param bytes The input.
 * @param context The fixture line checked.
 */
void expect_diverging(
        const Universe& a, const Universe& b, const Half half, const std::string& bytes, const std::string& context)
{
    const auto munch_a{munch_marking(a.dfa, bytes)};
    const auto munch_b{munch_marking(b.dfa, bytes)};

    const auto wanted_a{munch_a ? std::set<Marking_t>{*munch_a} : std::set<Marking_t>{}};
    const auto wanted_b{munch_b ? std::set<Marking_t>{*munch_b} : std::set<Marking_t>{}};

    EXPECT_EQ(accepted_markings(a.verifier, bytes), wanted_a) << context << " on " << bytes;
    EXPECT_EQ(accepted_markings(b.verifier, bytes), wanted_b) << context << " on " << bytes;

    if (half == Half::boundary)
    {
        EXPECT_TRUE(munch_a && munch_b && *munch_a != *munch_b) << context << " on " << bytes;

        return;
    }

    EXPECT_NE(munch_a.has_value(), munch_b.has_value()) << context << " on " << bytes;
}

/**
 * @brief Checks a certify line: the certificate's answer, and for a miscovering its witness marked as the maximal-munch
 *        scan marks it and as long as the reference's text, and both texts mis-covered under that scan.
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

    const auto& universe{*check.current};

    const auto found{miscovering(universe.verifier, window, origin)};

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

    EXPECT_TRUE(is_accepted(universe.verifier, segmentation)) << context;
    EXPECT_EQ(std::optional{boundaries}, munch_marking(universe.dfa, bytes)) << context;
    EXPECT_EQ(bytes.size(), text_of(text).size()) << context;
    ASSERT_LE(occurrence + window.size(), bytes.size()) << context;
    EXPECT_EQ(std::string_view{bytes}.substr(occurrence, window.size()), window) << context;
    EXPECT_NE(covering_boundary(segmentation, occurrence + window.size() - 1), occurrence + origin) << context;

    expect_miscovered(universe, bytes, window, origin, context);
    expect_miscovered(universe, text_of(text), window, origin, context);
}

/**
 * @brief Checks a gap line or a lasso line: the supremum equal, or a lasso replaying, the reference's and the
 *        decision's.
 * @param check The fixture under check.
 * @param kind The line's first word.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_gap(Fixture_check& check, const std::string& kind, std::istringstream& fields, const std::string& context)
{
    const auto gap{boundary_gap(check.current->verifier)};

    ++check.counts.gaps;

    if (kind == "gap")
    {
        std::size_t expected{};

        fields >> expected;

        ASSERT_TRUE(std::holds_alternative<std::size_t>(gap)) << context;
        EXPECT_EQ(std::get<std::size_t>(gap), expected) << context;

        return;
    }

    ++check.counts.lassos;

    std::string stem{};
    std::string loop{};
    std::string suffix{};

    fields >> stem >> loop >> suffix;

    const Lasso expected_lasso{.stem = steps_of(stem), .loop = steps_of(loop), .suffix = steps_of(suffix)};

    expect_lasso_replays(check.current->verifier, expected_lasso, context);

    ASSERT_TRUE(std::holds_alternative<Lasso>(gap)) << context;

    expect_lasso_replays(check.current->verifier, std::get<Lasso>(gap), context);
}

/**
 * @brief Checks a realizable line: the answer equal, and the mask's property when there is one.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_realizable(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    int expected{};

    fields >> expected;

    const auto mask{realizable(check.current->verifier)};

    ++check.counts.realizabilities;

    EXPECT_EQ(mask.has_value(), expected == 1) << context;

    if (mask)
    {
        expect_mask(check.current->verifier, *mask, context);
    }
}

/**
 * @brief Checks a pair line: the divergence's half equal to the reference's, its witness marked as the accepting side's
 *        maximal-munch scan marks it and as long as the reference's text, and both texts diverging in the half.
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

    const auto& a{check.literals.at(first)};
    const auto& b{check.literals.at(second)};

    const auto found{divergence(a.verifier, b.verifier)};

    ++check.counts.pairs;

    if (half == "equivalent")
    {
        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    const auto& [found_half, witness]{*found};

    const auto& [bytes, boundaries]{witness};

    EXPECT_EQ(found_half, half == "boundary" ? Half::boundary : Half::domain) << context;
    EXPECT_EQ(bytes.size(), text_of(text).size()) << context;

    const auto a_marking{munch_marking(a.dfa, bytes)};

    const auto& accepting{found_half == Half::boundary || a_marking ? a : b};

    EXPECT_EQ(std::optional{boundaries}, munch_marking(accepting.dfa, bytes)) << context;

    expect_diverging(a, b, found_half, bytes, context);
    expect_diverging(a, b, found_half, text_of(text), context);
}

/**
 * @brief Starts a universe line's universe: builds its DFA, a literal token set or a regular universe by index, and
 *        takes its armed run, keeping the two side by side.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void start_universe(Fixture_check& check, std::istringstream& fields, const std::string& context)
{
    std::string kind{};

    fields >> kind;

    ++check.counts.universes;
    check.universe = context;

    if (kind == "regular")
    {
        std::size_t index{};

        fields >> index;

        auto dfa{regular_dfa(index)};
        auto verifier{armed_run(dfa)};

        check.current.emplace(Universe{.dfa = std::move(dfa), .verifier = std::move(verifier)});

        return;
    }

    std::vector<std::string> tokens{};

    for (std::string token{}; fields >> token;)
    {
        tokens.push_back(token);
    }

    auto dfa{literal_dfa(tokens)};
    auto verifier{armed_run(dfa)};

    check.current.emplace(Universe{.dfa = std::move(dfa), .verifier = std::move(verifier)});
    check.literals.push_back(*check.current);
}

/**
 * @brief Checks one fixture line against the decisions, a comment or blank line checking nothing.
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
    else if (kind == "certify")
    {
        expect_certification(check, fields, context);
    }
    else if (kind == "gap" || kind == "lasso")
    {
        expect_gap(check, kind, fields, context);
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
 * @brief Returns the marked symbol of a fixture step field, a byte and a mark bit, a0 reading a unmarked.
 * @param field The field.
 * @return The symbol.
 */
Marked symbol_of(const std::string& field)
{
    return {.byte = static_cast<unsigned char>(field.at(0)), .boundary_after = field.at(1) == '1'};
}

/**
 * @brief Returns the sides of a cut of a marked string that fail when each is replayed through the verifier: the
 *        prefix before the cut, its last mark cleared, and the suffix from it, a side failing when the verifier's
 *        accepted markings of its bytes are not exactly the whole string's marking restricted to it.
 * @param verifier The verifier.
 * @param marked The marked string.
 * @param cut The cut, at most the string's length.
 * @return Whether the prefix fails, and whether the suffix fails.
 */
Failures_t replayed_failures(const Verifier& verifier, const Marked_string& marked, const std::size_t cut)
{
    const auto& [bytes, boundaries]{marked};

    const auto length{static_cast<std::ptrdiff_t>(cut)};

    Marking_t prefix_marking{boundaries.cbegin(), boundaries.cbegin() + length};

    if (cut > 0)
    {
        prefix_marking.back() = false;
    }

    const Marking_t suffix_marking{boundaries.cbegin() + length, boundaries.cend()};

    const auto prefix{accepted_markings(verifier, std::string_view{bytes}.substr(0, cut))};
    const auto suffix{accepted_markings(verifier, std::string_view{bytes}.substr(cut))};

    return {prefix != std::set<Marking_t>{prefix_marking}, suffix != std::set<Marking_t>{suffix_marking}};
}

/**
 * @brief Checks a dependence: its marked string is the verifier's one accepted marking of its bytes, the window occurs
 *        at the occurrence, the cut is the occurrence plus the origin and a boundary of the marking, and replaying the
 *        prefix and the suffix through the verifier fails exactly the sides named, at least one.
 * @param verifier The verifier.
 * @param found The dependence.
 * @param window The window.
 * @param origin The origin.
 * @param context The fixture line checked.
 */
void expect_dependence(
        const Verifier& verifier, const Dependence& found, const std::string_view window, const std::size_t origin,
        const std::string& context)
{
    const auto& [segmentation, occurrence, cut, prefix_fails, suffix_fails]{found};

    const auto& [bytes, boundaries]{segmentation};

    EXPECT_EQ(accepted_markings(verifier, bytes), std::set<Marking_t>{boundaries}) << context << " on " << bytes;
    ASSERT_LE(occurrence + window.size(), bytes.size()) << context << " on " << bytes;
    EXPECT_EQ(std::string_view{bytes}.substr(occurrence, window.size()), window) << context << " on " << bytes;
    EXPECT_EQ(cut, occurrence + origin) << context << " on " << bytes;
    EXPECT_TRUE(cut == 0 || boundaries[cut - 1]) << context << " on " << bytes;
    EXPECT_TRUE(prefix_fails || suffix_fails) << context << " on " << bytes;
    EXPECT_EQ(replayed_failures(verifier, segmentation, cut), (Failures_t{prefix_fails, suffix_fails}))
            << context << " on " << bytes;
}

/**
 * @brief Checks a dependent cut line: the decision's witness as long as the reference's text, and both dependences,
 *        the reference's under the verifier's one marking of its text, checked.
 * @param check The fixture under check.
 * @param found The decision's dependence.
 * @param fields The line after its verdict.
 * @param window The window.
 * @param origin The origin.
 * @param context The line.
 */
void expect_dependent_line(
        Cut_check& check, const Dependence& found, std::istringstream& fields, const std::string_view window,
        const std::size_t origin, const std::string& context)
{
    std::string text{};
    std::size_t position{};
    std::size_t cut{};
    std::string side{};

    fields >> text >> position >> cut >> side;

    const auto& verifier{*check.current};

    ++check.counts.dependent;

    if (found.prefix_fails)
    {
        ++check.counts.prefix_failures;
    }

    if (found.suffix_fails)
    {
        ++check.counts.suffix_failures;
    }

    EXPECT_EQ(found.segmentation.bytes.size(), text.size()) << context;

    expect_dependence(verifier, found, window, origin, context);

    const auto markings{accepted_markings(verifier, text)};

    ASSERT_EQ(markings.size(), 1U) << context;

    const Dependence expected{
            .segmentation = {.bytes = text, .boundaries = *markings.cbegin()},
            .occurrence = position,
            .cut = cut,
            .prefix_fails = side != "suffix",
            .suffix_fails = side != "prefix"};

    expect_dependence(verifier, expected, window, origin, context);
}

/**
 * @brief Checks a cut line: an uncertified pair refused, an independent one answered so, and a dependent one checked
 *        with its witness.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_cut(Cut_check& check, std::istringstream& fields, const std::string& context)
{
    std::string window{};
    std::size_t origin{};
    std::string verdict{};

    fields >> window >> origin >> verdict;

    ASSERT_TRUE(check.current) << context;

    const auto& verifier{*check.current};

    ++check.counts.cuts;

    if (verdict == "uncertified")
    {
        ++check.counts.uncertified;

        EXPECT_THROW(std::ignore = dependence(verifier, window, origin), std::invalid_argument) << context;

        return;
    }

    const auto found{dependence(verifier, window, origin)};

    if (verdict == "independent")
    {
        ++check.counts.independent;

        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    expect_dependent_line(check, *found, fields, window, origin, context);
}

/**
 * @brief Starts a universe line's universe: the armed run of a literal token set or of a regular universe by index, or
 *        nothing yet for a table, whose steps and accepting line follow.
 * @tparam Check The fixture under check's type, the cut or the chunk fixture's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
template <typename Check>
void start_verifier_universe(Check& check, std::istringstream& fields, const std::string& context)
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
 * @tparam Check The fixture under check's type, the cut or the chunk fixture's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
template <typename Check>
void read_step(Check& check, std::istringstream& fields, const std::string& context)
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
 * @tparam Check The fixture under check's type, the cut or the chunk fixture's.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
template <typename Check>
void build_table(Check& check, std::istringstream& fields, const std::string& context)
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
 * @brief Checks one cut fixture line against the decision, a comment or blank line checking nothing.
 * @param check The fixture under check.
 * @param line The line.
 */
void expect_cut_fixture_line(Cut_check& check, const std::string& line)
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
    else if (kind == "cut")
    {
        expect_cut(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown cut fixture line: " << line;
    }
}

/**
 * @brief Returns the numbers of a comma-separated fixture field, at least one.
 * @param field The field.
 * @return The numbers, in order.
 */
std::vector<std::size_t> numbers_of(const std::string& field)
{
    std::vector<std::size_t> numbers{};

    for (const auto part : field | std::views::split(','))
    {
        const std::string number{part.begin(), part.end()};

        numbers.push_back(std::stoul(number));
    }

    return numbers;
}

/**
 * @brief Returns the chunks of a comma-separated fixture field of begin:end ranges, at least one.
 * @param field The field.
 * @return The chunks, in order.
 */
std::vector<Chunk> chunks_of(const std::string& field)
{
    std::vector<Chunk> chunks{};

    for (const auto part : field | std::views::split(','))
    {
        const std::string range{part.begin(), part.end()};

        const auto colon{range.find(':')};

        const auto begin{std::stoul(range.substr(0, colon))};

        const auto end{std::stoul(range.substr(colon + 1))};

        chunks.push_back({.begin = begin, .end = end});
    }

    return chunks;
}

/**
 * @brief Reads the pairs of an inventory line up to its verdict.
 * @param fields The line after its first word.
 * @return The windows with their origins, and the verdict.
 */
Inventory_line_t read_inventory(std::istringstream& fields)
{
    Inventory_line_t line{};

    auto& [pairs, verdict]{line};

    for (std::string word{}; fields >> word;)
    {
        if (word == "independent" || word == "dependent")
        {
            verdict = word;

            break;
        }

        std::size_t origin{};

        fields >> origin;

        pairs.emplace_back(word, origin);
    }

    return line;
}

/**
 * @brief Returns the anchors of an inventory in a text, the positions the windows' origins land on at every
 *        occurrence of every window, ascending and without repeats.
 * @param text The text.
 * @param inventory The pairs.
 * @return The anchors.
 */
std::vector<std::size_t> anchors_in(const std::string_view text, const std::span<const Certified_pair> inventory)
{
    std::set<std::size_t> anchors{};

    for (const auto& [window, origin] : inventory)
    {
        for (auto at{text.find(window)}; at != std::string_view::npos; at = text.find(window, at + 1))
        {
            anchors.insert(at + origin);
        }
    }

    return {anchors.cbegin(), anchors.cend()};
}

/**
 * @brief Returns the chunks between consecutive anchors of a marked string, the first from position zero and the last
 *        to the end, that fail when replayed through the verifier: a chunk fails when the verifier's accepted markings
 *        of its bytes are not exactly the whole string's marking restricted to it, its last mark cleared.
 * @param verifier The verifier.
 * @param marked The marked string.
 * @param anchors The anchors, ascending and at most the string's length.
 * @return The failing chunks, ascending.
 */
std::vector<Chunk> replayed_failing_chunks(
        const Verifier& verifier, const Marked_string& marked, const std::vector<std::size_t>& anchors)
{
    const auto& [bytes, boundaries]{marked};

    std::vector<std::size_t> ends{anchors};

    ends.push_back(bytes.size());

    std::vector<Chunk> failing{};

    std::size_t begin{0};

    for (const auto end : ends)
    {
        const auto first{boundaries.cbegin() + static_cast<std::ptrdiff_t>(begin)};

        const auto last{boundaries.cbegin() + static_cast<std::ptrdiff_t>(end)};

        Marking_t restricted{first, last};

        if (!restricted.empty())
        {
            restricted.back() = false;
        }

        const auto chunk{std::string_view{bytes}.substr(begin, end - begin)};

        if (accepted_markings(verifier, chunk) != std::set<Marking_t>{restricted})
        {
            failing.push_back({.begin = begin, .end = end});
        }

        begin = end;
    }

    return failing;
}

/**
 * @brief Checks a chunk dependence: its marked string is the verifier's one accepted marking of its bytes, its anchors
 *        are the inventory's in the text and each a boundary of the marking, and replaying every chunk between them
 *        through the verifier fails exactly the chunks named, at least one.
 * @param verifier The verifier.
 * @param found The chunk dependence.
 * @param inventory The pairs.
 * @param context The fixture line checked.
 */
void expect_chunk_dependence(
        const Verifier& verifier, const Chunk_dependence& found, const std::span<const Certified_pair> inventory,
        const std::string& context)
{
    const auto& [segmentation, anchors, failing]{found};

    const auto& [bytes, boundaries]{segmentation};

    EXPECT_EQ(accepted_markings(verifier, bytes), std::set<Marking_t>{boundaries}) << context << " on " << bytes;
    EXPECT_EQ(anchors, anchors_in(bytes, inventory)) << context << " on " << bytes;

    for (const auto anchor : anchors)
    {
        EXPECT_TRUE(anchor == 0 || boundaries[anchor - 1]) << context << " on " << bytes << " at " << anchor;
    }

    EXPECT_FALSE(failing.empty()) << context << " on " << bytes;
    EXPECT_EQ(failing, replayed_failing_chunks(verifier, segmentation, anchors)) << context << " on " << bytes;
}

/**
 * @brief Checks a dependent inventory line: the decision's witness as long as the reference's text, and both chunk
 *        dependences, the reference's under the verifier's one marking of its text, checked.
 * @param verifier The verifier.
 * @param found The decision's chunk dependence.
 * @param fields The line after its verdict.
 * @param inventory The pairs.
 * @param context The line.
 */
void expect_dependent_inventory_line(
        const Verifier& verifier, const Chunk_dependence& found, std::istringstream& fields,
        const std::span<const Certified_pair> inventory, const std::string& context)
{
    std::string text{};
    std::string anchors{};
    std::string chunks{};

    fields >> text >> anchors >> chunks;

    EXPECT_EQ(found.segmentation.bytes.size(), text.size()) << context;

    expect_chunk_dependence(verifier, found, inventory, context);

    const auto markings{accepted_markings(verifier, text)};

    ASSERT_EQ(markings.size(), 1U) << context;

    const Chunk_dependence expected{
            .segmentation = {.bytes = text, .boundaries = *markings.cbegin()},
            .anchors = numbers_of(anchors),
            .failing = chunks_of(chunks)};

    expect_chunk_dependence(verifier, expected, inventory, context);
}

/**
 * @brief Checks an inventory line: an independent inventory answered so, a dependent one checked with its witness, and
 *        an inventory whose pairs are all independent by dependence() answered independent, the relation of the two
 *        decisions checked on the data.
 * @param check The fixture under check.
 * @param fields The line after its first word.
 * @param context The line.
 */
void expect_inventory(Chunk_check& check, std::istringstream& fields, const std::string& context)
{
    const auto [pairs, verdict]{read_inventory(fields)};

    std::vector<Certified_pair> inventory{};

    for (const auto& [window, origin] : pairs)
    {
        inventory.push_back({.window = window, .origin = origin});
    }

    ASSERT_TRUE(check.current) << context;

    const auto& verifier{*check.current};

    ++check.counts.inventories;

    const auto found{chunk_dependence(verifier, inventory)};

    const auto independent_pair{
            [&verifier](const Certified_pair& pair) { return !dependence(verifier, pair.window, pair.origin); }};

    const auto every_pair_independent{std::ranges::all_of(inventory, independent_pair)};

    if (every_pair_independent)
    {
        ++check.counts.cut_independent;

        EXPECT_FALSE(found) << context << " with every pair independent";
    }

    if (verdict == "independent")
    {
        ++check.counts.independent;

        if (!every_pair_independent)
        {
            ++check.counts.converse;
        }

        EXPECT_FALSE(found) << context;

        return;
    }

    ASSERT_TRUE(found) << context;

    ++check.counts.dependent;

    expect_dependent_inventory_line(verifier, *found, fields, inventory, context);
}

/**
 * @brief Checks one chunk fixture line against the decision, a comment or blank line checking nothing.
 * @param check The fixture under check.
 * @param line The line.
 */
void expect_chunk_fixture_line(Chunk_check& check, const std::string& line)
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
    else if (kind == "inventory")
    {
        expect_inventory(check, fields, context);
    }
    else
    {
        ADD_FAILURE() << "unknown chunk fixture line: " << line;
    }
}

/**
 * @brief Builds the verifier of the two-mode scan over the initial tokens a and ab and the continuation token b: the
 *        maximal-munch scan whose fresh runs come from the continuation set once a token has closed, so that the
 *        domain is the empty input, a, ab and ab followed by any number of b, each b after ab a token of its own.
 * @return The verifier.
 */
Verifier two_mode_a_ab_then_b()
{
    constexpr Verifier::State_t start{0};
    constexpr Verifier::State_t a_read{1};
    constexpr Verifier::State_t ab_read{2};
    constexpr Verifier::State_t ab_closed{3};
    constexpr Verifier::State_t b_read{4};
    constexpr Verifier::State_t b_closed{5};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked b{.byte = 'b', .boundary_after = false};
    constexpr Marked b_closing{.byte = 'b', .boundary_after = true};

    return Verifier{
            start,
            {{{start, a}, a_read},
             {{a_read, b}, ab_read},
             {{a_read, b_closing}, ab_closed},
             {{ab_closed, b}, b_read},
             {{ab_closed, b_closing}, b_closed},
             {{b_closed, b}, b_read},
             {{b_closed, b_closing}, b_closed}},
            {start, a_read, ab_read, b_read}};
}

/**
 * @brief Builds the verifier of the two-mode scan over the initial tokens a, ab and b and the continuation tokens a and
 *        b: the domain is every string, segmented as the initial ab followed by single bytes when the string begins
 *        with ab and as single bytes otherwise, so that a single initial a is followed by a.
 * @return The verifier.
 */
Verifier two_mode_a_ab_b_then_a_b()
{
    constexpr Verifier::State_t start{0};
    constexpr Verifier::State_t a_read{1};
    constexpr Verifier::State_t a_closed{2};
    constexpr Verifier::State_t closed{3};
    constexpr Verifier::State_t last{4};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};
    constexpr Marked b{.byte = 'b', .boundary_after = false};
    constexpr Marked b_closing{.byte = 'b', .boundary_after = true};

    return Verifier{
            start,
            {{{start, a}, a_read},
             {{start, a_closing}, a_closed},
             {{start, b}, last},
             {{start, b_closing}, closed},
             {{a_read, b}, last},
             {{a_read, b_closing}, closed},
             {{a_closed, a}, last},
             {{a_closed, a_closing}, closed},
             {{closed, a}, last},
             {{closed, a_closing}, closed},
             {{closed, b}, last},
             {{closed, b_closing}, closed}},
            {start, a_read, last}};
}

/**
 * @brief Builds the verifier of the two-mode scan over the initial tokens a, aa and ba and the continuation token a:
 *        the domain is the empty input and the runs of a with or without a leading b, segmented as the initial aa or
 *        ba, or a alone, followed by single a's.
 * @return The verifier.
 */
Verifier two_mode_a_aa_ba_then_a()
{
    constexpr Verifier::State_t start{0};
    constexpr Verifier::State_t a_read{1};
    constexpr Verifier::State_t b_read{2};
    constexpr Verifier::State_t closed{3};
    constexpr Verifier::State_t last{4};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    return Verifier{
            start,
            {{{start, a}, a_read},
             {{start, b}, b_read},
             {{a_read, a}, last},
             {{a_read, a_closing}, closed},
             {{b_read, a}, last},
             {{b_read, a_closing}, closed},
             {{closed, a}, last},
             {{closed, a_closing}, closed}},
            {start, a_read, last}};
}

/**
 * @brief Builds the verifier of the threshold splice of the tokens a and b below length three and the tokens ab, a and
 *        b from it: the domain is every string, segmented byte by byte when shorter than three bytes and by maximal
 *        munch over ab, a and b otherwise, so that an a marked at length one or two is followed by a and an a read at
 *        length three or more is the last byte or the a of ab.
 * @return The verifier.
 */
Verifier splice_single_below_three_then_merged()
{
    constexpr Verifier::State_t start{0};
    constexpr Verifier::State_t a_read{1};
    constexpr Verifier::State_t a_closed{2};
    constexpr Verifier::State_t b_closed{3};
    constexpr Verifier::State_t second_a_read{4};
    constexpr Verifier::State_t pending{5};
    constexpr Verifier::State_t single_a{6};
    constexpr Verifier::State_t free{7};
    constexpr Verifier::State_t last{8};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};
    constexpr Marked b{.byte = 'b', .boundary_after = false};
    constexpr Marked b_closing{.byte = 'b', .boundary_after = true};

    return Verifier{
            start,
            {{{start, a}, a_read},
             {{start, a_closing}, a_closed},
             {{start, b}, last},
             {{start, b_closing}, b_closed},
             {{a_read, b_closing}, free},
             {{a_closed, a}, second_a_read},
             {{a_closed, a_closing}, single_a},
             {{a_closed, b}, last},
             {{b_closed, a}, second_a_read},
             {{b_closed, a_closing}, single_a},
             {{b_closed, b}, last},
             {{b_closed, b_closing}, free},
             {{second_a_read, b}, last},
             {{second_a_read, b_closing}, free},
             {{pending, b}, last},
             {{pending, b_closing}, free},
             {{single_a, a}, pending},
             {{single_a, a_closing}, single_a},
             {{free, a}, pending},
             {{free, a_closing}, single_a},
             {{free, b}, last},
             {{free, b_closing}, free}},
            {start, a_read, second_a_read, pending, last}};
}

} // namespace

TEST(Verifier_test, Literal_a_aab_accepts_exactly_the_munch_markings)
{
    const auto dfa{literal_dfa({"a", "aab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aaaa"), std::set<Marking_t>{marking_of("aaaa", {0, 1, 2})});
    EXPECT_EQ(accepted_markings(verifier, "aaab"), std::set<Marking_t>{marking_of("aaab", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Literal_a_ab_accepts_exactly_the_munch_markings)
{
    const auto dfa{literal_dfa({"a", "ab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aab"), std::set<Marking_t>{marking_of("aab", {0})});
    EXPECT_TRUE(accepted_markings(verifier, "b").empty());

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Literal_a_b_ab_refuses_the_marking_that_cuts_the_longer_token)
{
    const auto dfa{literal_dfa({"a", "b", "ab"})};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "ab"), std::set<Marking_t>{marking_of("ab", {})});
    EXPECT_EQ(accepted_markings(verifier, "ba"), std::set<Marking_t>{marking_of("ba", {0})});
    EXPECT_EQ(accepted_markings(verifier, "aab"), std::set<Marking_t>{marking_of("aab", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, Ab_star_a_restores_the_start_configuration_and_keeps_the_unconsumed_state_distinct)
{
    const auto dfa{ab_star_a_dfa()};

    const auto verifier{armed_run(dfa)};

    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    const auto after_a{verifier.step(verifier.start(), a)};

    ASSERT_TRUE(after_a);

    const auto after_ab{verifier.step(*after_a, b)};

    ASSERT_TRUE(after_ab);
    EXPECT_NE(*after_ab, verifier.start());
    EXPECT_TRUE(verifier.accepts(verifier.start()));
    EXPECT_FALSE(verifier.accepts(*after_ab));
    EXPECT_EQ(verifier.step(*after_ab, a), verifier.step(verifier.start(), a));

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, A_odd_accepts_one_marking_of_aa)
{
    const auto dfa{a_odd_dfa()};

    const auto verifier{armed_run(dfa)};

    EXPECT_EQ(accepted_markings(verifier, "aa"), std::set<Marking_t>{marking_of("aa", {0})});

    expect_munch_markings(dfa, verifier);
    expect_trim(verifier);
}

TEST(Verifier_test, The_start_accepts_the_empty_input)
{
    for (const auto& dfa : {literal_dfa({"a", "aab"}), literal_dfa({"a", "ab"}), ab_star_a_dfa(), a_odd_dfa()})
    {
        const auto verifier{armed_run(dfa)};

        EXPECT_EQ(verifier.start(), 0U);
        EXPECT_TRUE(verifier.accepts(verifier.start()));
        EXPECT_EQ(accepted_markings(verifier, ""), std::set<Marking_t>{Marking_t{}});
    }
}

TEST(Verifier_test, Constructor_redirects_a_transition_into_the_start_to_a_copy_of_it)
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};

    const Verifier verifier{5, {{{5, a}, 6}, {{6, a}, 5}}, {5, 6}};

    EXPECT_EQ(verifier.state_count(), 3U);
    EXPECT_EQ(verifier.step(0, a), std::optional<Verifier::State_t>{1});
    EXPECT_EQ(verifier.step(1, a), std::optional<Verifier::State_t>{2});
    EXPECT_EQ(verifier.step(2, a), std::optional<Verifier::State_t>{1});
    EXPECT_TRUE(verifier.accepts(2));

    for (const auto& to : verifier.transitions() | std::views::values)
    {
        EXPECT_NE(to, verifier.start());
    }
}

TEST(Verifier_test, Constructor_trims_a_caller_table_and_keeps_the_start)
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    const Verifier verifier{7, {{{7, a}, 3}, {{7, b}, 9}, {{11, a}, 3}}, {3, 11}};

    EXPECT_EQ(verifier.start(), 0U);
    EXPECT_EQ(verifier.state_count(), 2U);
    EXPECT_EQ(verifier.step(0, a), std::optional<Verifier::State_t>{1});
    EXPECT_EQ(verifier.step(0, b), std::nullopt);
    EXPECT_TRUE(verifier.accepts(1));
    EXPECT_EQ(verifier.accept_states(), Verifier::Accept_states_t{1});

    const Verifier accepting_start{4, {}, {4}};

    EXPECT_EQ(accepting_start.state_count(), 1U);
    EXPECT_TRUE(accepting_start.accepts(accepting_start.start()));

    const Verifier empty_domain{4, {{{4, a}, 5}}, {}};

    EXPECT_EQ(empty_domain.state_count(), 1U);
    EXPECT_TRUE(empty_domain.transitions().empty());
    EXPECT_FALSE(empty_domain.accepts(empty_domain.start()));
}

TEST(Verifier_test, Armed_run_refuses_an_accepting_initial_state)
{
    Builder builder{};

    builder.add_accept_state(builder.init_state(), Token{0});

    EXPECT_THROW(std::ignore = armed_run(builder.build()), std::invalid_argument);
}

TEST(Verifier_test, The_decisions_give_the_reference_answers_on_every_universe_of_the_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/dfa/tests/data/verifier_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Fixture_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 474U);
    EXPECT_EQ(check.literals.size(), 469U);
    EXPECT_EQ(check.counts.certifications, 16116U);
    EXPECT_EQ(check.counts.miscoverings, 10804U);
    EXPECT_EQ(check.counts.gaps, 474U);
    EXPECT_EQ(check.counts.lassos, 2U);
    EXPECT_EQ(check.counts.realizabilities, 474U);
    EXPECT_EQ(check.counts.pairs, 1818U);
}

TEST(Verifier_test, The_decisions_refuse_a_malformed_window_and_an_empty_domain)
{
    const auto verifier{armed_run(literal_dfa({"a", "ab"}))};

    EXPECT_THROW(std::ignore = miscovering(verifier, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = miscovering(verifier, "ab", 2), std::invalid_argument);
    EXPECT_THROW(std::ignore = dependence(verifier, "", 0), std::invalid_argument);
    EXPECT_THROW(std::ignore = dependence(verifier, "ab", 2), std::invalid_argument);

    constexpr Marked a{.byte = 'a', .boundary_after = false};

    const Verifier empty_domain{0, {{{0, a}, 1}}, {}};

    EXPECT_THROW(std::ignore = boundary_gap(empty_domain), std::invalid_argument);
    EXPECT_FALSE(realizable(empty_domain));
}

TEST(Verifier_test, Dependence_gives_the_reference_answers_on_every_universe_of_the_cut_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/dfa/tests/data/cut_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Cut_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_cut_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 489U);
    EXPECT_EQ(check.counts.tables, 15U);
    EXPECT_EQ(check.counts.cuts, 16626U);
    EXPECT_EQ(check.counts.uncertified, 11047U);
    EXPECT_EQ(check.counts.independent, 5517U);
    EXPECT_EQ(check.counts.dependent, 62U);
    EXPECT_EQ(check.counts.prefix_failures, 34U);
    EXPECT_EQ(check.counts.suffix_failures, 31U);
}

TEST(Verifier_test, Every_certified_cut_of_a_maximal_munch_scan_is_independent_and_an_uncertified_pair_is_refused)
{
    const auto verifier{armed_run(literal_dfa({"ab", "a", "b"}))};

    for (const auto& window : strings_up_to("ab", 3) | std::views::drop(1))
    {
        for (std::size_t origin{0}; origin < window.size(); ++origin)
        {
            if (miscovering(verifier, window, origin))
            {
                EXPECT_THROW(std::ignore = dependence(verifier, window, origin), std::invalid_argument)
                        << window << " " << origin;

                continue;
            }

            EXPECT_FALSE(dependence(verifier, window, origin)) << window << " " << origin;
        }
    }

    EXPECT_FALSE(miscovering(verifier, "ab", 0));
    EXPECT_TRUE(miscovering(verifier, "ab", 1));
}

TEST(Verifier_test, A_two_mode_scan_certifies_a_cut_whose_suffix_leaves_the_domain)
{
    const auto verifier{two_mode_a_ab_then_b()};

    const auto found{dependence(verifier, "bb", 1)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "abb");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("abb", {1}));
    EXPECT_EQ(found->occurrence, 1U);
    EXPECT_EQ(found->cut, 2U);
    EXPECT_FALSE(found->prefix_fails);
    EXPECT_TRUE(found->suffix_fails);

    expect_dependence(verifier, *found, "bb", 1, "bb 1");

    EXPECT_FALSE(dependence(verifier, "ab", 0));
    EXPECT_FALSE(dependence(verifier, "a", 0));
}

TEST(Verifier_test, The_cut_at_position_zero_fails_when_the_start_does_not_accept)
{
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    // The domain is ab alone, segmented a|b, so (a, 0) is certified and the cut at zero leaves an empty prefix outside
    // the domain.
    const Verifier verifier{0, {{{0, a_closing}, 1}, {{1, b}, 2}}, {2}};

    const auto found{dependence(verifier, "a", 0)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "ab");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("ab", {0}));
    EXPECT_EQ(found->occurrence, 0U);
    EXPECT_EQ(found->cut, 0U);
    EXPECT_TRUE(found->prefix_fails);
    EXPECT_FALSE(found->suffix_fails);

    expect_dependence(verifier, *found, "a", 0, "a 0");
}

TEST(Verifier_test, A_certified_cut_can_fail_on_both_sides)
{
    constexpr Marked a{.byte = 'a', .boundary_after = false};
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};

    // The domain is the empty input and aa segmented a|a: the cut at zero is independent, and the cut at one leaves a
    // on each side, outside the domain.
    const Verifier verifier{0, {{{0, a_closing}, 1}, {{1, a}, 2}}, {0, 2}};

    const auto found{dependence(verifier, "a", 0)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "aa");
    EXPECT_EQ(found->occurrence, 1U);
    EXPECT_EQ(found->cut, 1U);
    EXPECT_TRUE(found->prefix_fails);
    EXPECT_TRUE(found->suffix_fails);

    expect_dependence(verifier, *found, "a", 0, "a 0");
}

TEST(Verifier_test, Chunk_dependence_gives_the_reference_answers_on_every_universe_of_the_chunk_fixture)
{
    std::ifstream file{std::string{SOURCE_DIR} + "/libs/dfa/tests/data/chunk_reference.txt"};

    ASSERT_TRUE(file.is_open());

    Chunk_check check{};

    for (std::string line{}; std::getline(file, line);)
    {
        expect_chunk_fixture_line(check, line);
    }

    EXPECT_EQ(check.counts.universes, 494U);
    EXPECT_EQ(check.counts.tables, 20U);
    EXPECT_EQ(check.counts.inventories, 29085U);
    EXPECT_EQ(check.counts.independent, 27326U);
    EXPECT_EQ(check.counts.dependent, 1759U);
    EXPECT_EQ(check.counts.cut_independent, 27278U);
    EXPECT_EQ(check.counts.converse, 48U);
}

TEST(Verifier_test, Chunk_dependence_refuses_an_empty_inventory_and_an_uncertified_pair)
{
    const auto verifier{armed_run(literal_dfa({"ab", "a", "b"}))};

    const std::vector<Certified_pair> none{};
    const std::vector<Certified_pair> uncertified{{.window = "ab", .origin = 0}, {.window = "ab", .origin = 1}};
    const std::vector<Certified_pair> empty_window{{.window = "", .origin = 0}};
    const std::vector<Certified_pair> outside{{.window = "ab", .origin = 2}};
    const std::vector<Certified_pair> certified{{.window = "ab", .origin = 0}, {.window = "a", .origin = 0}};

    EXPECT_THROW(std::ignore = chunk_dependence(verifier, none), std::invalid_argument);
    EXPECT_THROW(std::ignore = chunk_dependence(verifier, uncertified), std::invalid_argument);
    EXPECT_THROW(std::ignore = chunk_dependence(verifier, empty_window), std::invalid_argument);
    EXPECT_THROW(std::ignore = chunk_dependence(verifier, outside), std::invalid_argument);
    EXPECT_FALSE(chunk_dependence(verifier, certified));
}

TEST(Verifier_test, Another_pair_anchors_the_suffix_that_fails_alone_so_the_inventory_is_chunk_independent)
{
    const auto verifier{two_mode_a_ab_b_then_a_b()};

    const std::vector<Certified_pair> alone{{.window = "aa", .origin = 1}};
    const std::vector<Certified_pair> with_aab{{.window = "aa", .origin = 1}, {.window = "aab", .origin = 2}};

    const auto cut{dependence(verifier, "aa", 1)};

    ASSERT_TRUE(cut);
    EXPECT_EQ(cut->segmentation.bytes, "aab");
    EXPECT_TRUE(cut->suffix_fails);

    const auto found{chunk_dependence(verifier, alone)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "aab");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("aab", {0, 1}));
    EXPECT_EQ(found->anchors, std::vector<std::size_t>{1});
    EXPECT_EQ(found->failing, (std::vector<Chunk>{{.begin = 1, .end = 3}}));

    expect_chunk_dependence(verifier, *found, alone, "aa 1");

    EXPECT_FALSE(chunk_dependence(verifier, with_aab));
}

TEST(Verifier_test, A_self_overlapping_pair_is_chunk_independent_alone_while_its_cut_is_not_independent)
{
    const auto verifier{two_mode_a_aa_ba_then_a()};

    const std::vector<Certified_pair> aaa{{.window = "aaa", .origin = 2}};
    const std::vector<Certified_pair> baa{{.window = "baa", .origin = 2}};

    const auto cut{dependence(verifier, "aaa", 2)};

    ASSERT_TRUE(cut);
    EXPECT_EQ(cut->segmentation.bytes, "aaaa");
    EXPECT_TRUE(cut->suffix_fails);

    EXPECT_FALSE(chunk_dependence(verifier, aaa));

    const auto found{chunk_dependence(verifier, baa)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "baaa");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("baaa", {1, 2}));
    EXPECT_EQ(found->anchors, std::vector<std::size_t>{2});
    EXPECT_EQ(found->failing, (std::vector<Chunk>{{.begin = 2, .end = 4}}));

    expect_chunk_dependence(verifier, *found, baa, "baa 2");
}

TEST(Verifier_test, A_threshold_splice_fails_a_middle_chunk_shorter_than_the_threshold)
{
    const auto verifier{splice_single_below_three_then_merged()};

    const std::vector<Certified_pair> at_zero{{.window = "a", .origin = 0}};
    const std::vector<Certified_pair> aa_bb{{.window = "aa", .origin = 1}, {.window = "bb", .origin = 1}};

    // The anchor at zero leaves an empty first chunk, and the middle chunk ab is segmented a|b alone.
    const auto found{chunk_dependence(verifier, at_zero)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "aba");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("aba", {1}));
    EXPECT_EQ(found->anchors, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(found->failing, (std::vector<Chunk>{{.begin = 0, .end = 2}}));

    expect_chunk_dependence(verifier, *found, at_zero, "a 0");

    // Between two chunks that hold, the middle chunk ab of a|ab|b fails; the shortest witness of the inventory is abb,
    // its first chunk failing.
    const Marked_string aabb{.bytes = "aabb", .boundaries = marking_of("aabb", {0, 2})};

    EXPECT_EQ(anchors_in(aabb.bytes, aa_bb), (std::vector<std::size_t>{1, 3}));
    EXPECT_EQ(replayed_failing_chunks(verifier, aabb, {1, 3}), (std::vector<Chunk>{{.begin = 1, .end = 3}}));

    const auto shortest{chunk_dependence(verifier, aa_bb)};

    ASSERT_TRUE(shortest);
    EXPECT_EQ(shortest->segmentation.bytes, "abb");
    EXPECT_EQ(shortest->anchors, std::vector<std::size_t>{2});
    EXPECT_EQ(shortest->failing, (std::vector<Chunk>{{.begin = 0, .end = 2}}));

    expect_chunk_dependence(verifier, *shortest, aa_bb, "aa 1 bb 1");
}

TEST(Verifier_test, The_anchor_at_position_zero_fails_when_the_start_does_not_accept)
{
    constexpr Marked a_closing{.byte = 'a', .boundary_after = true};
    constexpr Marked b{.byte = 'b', .boundary_after = false};

    // The domain is ab alone, segmented a|b, so (a, 0) anchors position zero and the empty first chunk lies outside the
    // domain.
    const Verifier verifier{0, {{{0, a_closing}, 1}, {{1, b}, 2}}, {2}};

    const std::vector<Certified_pair> at_zero{{.window = "a", .origin = 0}};

    const auto found{chunk_dependence(verifier, at_zero)};

    ASSERT_TRUE(found);
    EXPECT_EQ(found->segmentation.bytes, "ab");
    EXPECT_EQ(found->segmentation.boundaries, marking_of("ab", {0}));
    EXPECT_EQ(found->anchors, std::vector<std::size_t>{0});
    EXPECT_EQ(found->failing, (std::vector<Chunk>{{.begin = 0, .end = 0}}));

    expect_chunk_dependence(verifier, *found, at_zero, "a 0");
}

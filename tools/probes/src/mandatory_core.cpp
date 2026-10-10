// Decides the mandatory-core premise of the proof-directed planner: for a state q and a family K of byte strings, does
// every death word from q contain some member of K ending strictly before the killing byte?
//
// The planned prefilter narrows its candidate windows to core occurrences, and its exact-plan-equality argument rests
// on the premise above (the mandatory-death-core theorem). The premise is decided per grammar, not recognized by shape:
// there is a concrete automaton where a plausible component satisfies the first-exit intuition while an internal death
// bypasses the core entirely. The decision procedure: an Aho-Corasick matcher over K is producted with the live
// automaton, and the premise fails exactly when a pair of a live state and a match-free matcher state is reachable
// whose live state is not input-total, since any missing byte there ends a K-avoiding death word. The matcher reads
// only the live prefix; the killing byte is never fed to it, because a core completed on the killing byte is too late.
// Families are nonempty strings by contract; a state that can die with no core before the killing byte gets a refuted
// verdict with a reconstructed witness.
//
// What runs as a test. The shipped instances and the counterexamples, all pinned, and how a witness is rendered:
//   - a witness's byte that is neither printable nor a newline or a tab renders as `\x` and its two hexadecimal digits;
//   - the C-like cumulative row proves the family {*/} at its comment-interior state;
//   - a Python-like triple-quote row proves the family {three quotes} at its string-interior state;
//   - the RFC 8259 row refutes every family at its string-interior state with a one-byte witness, since a control byte
//     kills it immediately: JSON gets no filter and the planner's exhaustive walk stays;
//   - the token set {a, b} has no provable family anywhere although it certifies the window ab: the checker refuses,
//     and such grammars keep the exhaustive walk;
//   - a synthetic automaton refutes K = {c} with the witness ab, and a repaired variant of the same table proves it.
//
// The checker runs over any view with advance(state, byte), so hand-built tables and compiled automata run through the
// identical decision procedure.

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/study_rows.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Assertions;
using munch::tools::probes::Builder_dbg;
using munch::tools::probes::every_byte;
using munch::tools::probes::live_states;
using munch::tools::probes::published_cumulative_row;
using munch::tools::probes::States_t;
using namespace munch::regex;
using munch::dfa::Dfa;

/**
 * @brief Aho-Corasick matcher over a family of nonempty byte strings, reporting only whether a member has completed;
 *        goto and fail links are precomputed, and terminal reachability propagates over fails.
 */
class Matcher
{
public:
    /**
     * @brief Builds the trie of the family, its fail links breadth first, and each node's terminal flag propagated over
     *        its fail link.
     * @param family The family, nonempty strings.
     */
    explicit Matcher(const std::vector<std::string>& family);

    /**
     * @brief Matches one byte; std::nullopt once a family member has completed.
     * @param at The matcher node before the byte.
     * @param byte The byte.
     * @return The node after the byte, std::nullopt when a member completes on it.
     */
    [[nodiscard]] std::optional<std::size_t> step(std::size_t at, unsigned char byte) const;

private:
    /**
     * @brief A trie node with its goto links, its fail link and its terminal flag.
     */
    struct Node
    {
        /**
         * @brief The goto links by byte.
         */
        std::map<unsigned char, std::size_t> next{};

        /**
         * @brief The node of the longest proper suffix of this node's string that is in the trie, 0 for none.
         */
        std::size_t fail{0};

        /**
         * @brief Whether a family member ends here or at a node the fail links reach.
         */
        bool terminal{false};
    };

    /**
     * @brief Adds one family member to the trie, marking the node it ends at terminal.
     * @param word The member, nonempty.
     */
    void insert(const std::string& word);

    /**
     * @brief Links every node below the root to its fail node breadth first, propagating the terminal flag over each
     *        link.
     */
    void link_failures();

    /**
     * @brief Walks down the fail links from a node to the first one with a goto link on a byte, or to the root.
     * @param node The node the walk starts at.
     * @param byte The byte.
     * @return The first node on the walk with a goto link on the byte, 0 when none has one.
     */
    [[nodiscard]] std::size_t fall_back(std::size_t node, unsigned char byte) const;

    /**
     * @brief Follows a node's goto link on a byte.
     * @param node The node.
     * @param byte The byte.
     * @return The link's target, 0 when the node has no link on the byte.
     */
    [[nodiscard]] std::size_t goto_target(std::size_t node, unsigned char byte) const;

    /**
     * @brief The trie, the root at index 0.
     */
    std::vector<Node> nodes_;
};

Matcher::Matcher(const std::vector<std::string>& family)
{
    nodes_.push_back({});

    for (const auto& word : family)
    {
        insert(word);
    }

    link_failures();
}

void Matcher::insert(const std::string& word)
{
    std::size_t at{0};

    for (const char byte : word)
    {
        const auto key{static_cast<unsigned char>(byte)};

        if (!nodes_[at].next.contains(key))
        {
            nodes_.push_back({});

            nodes_[at].next.emplace(key, nodes_.size() - 1);
        }

        at = nodes_[at].next.at(key);
    }

    nodes_[at].terminal = true;
}

void Matcher::link_failures()
{
    std::deque<std::size_t> pending{};

    for (const auto& [key, child] : nodes_[0].next)
    {
        nodes_[child].fail = 0;

        pending.push_back(child);
    }

    while (!pending.empty())
    {
        const auto at{pending.front()};

        pending.pop_front();

        nodes_[at].terminal = nodes_[at].terminal || nodes_[nodes_[at].fail].terminal;

        for (const auto& [key, child] : nodes_[at].next)
        {
            const auto fall{fall_back(nodes_[at].fail, key)};

            const auto target{goto_target(fall, key)};

            nodes_[child].fail = target != child ? target : 0;

            pending.push_back(child);
        }
    }
}

std::size_t Matcher::fall_back(const std::size_t node, const unsigned char byte) const
{
    auto fall{node};

    while (fall != 0 && !nodes_[fall].next.contains(byte))
    {
        fall = nodes_[fall].fail;
    }

    return fall;
}

std::size_t Matcher::goto_target(const std::size_t node, const unsigned char byte) const
{
    const auto found{nodes_[node].next.find(byte)};

    if (found == nodes_[node].next.end())
    {
        return 0;
    }

    const auto& [symbol, target]{*found};

    return target;
}

std::optional<std::size_t> Matcher::step(const std::size_t at, const unsigned char byte) const
{
    const auto node{fall_back(at, byte)};

    const auto next{goto_target(node, byte)};

    if (nodes_[next].terminal)
    {
        return std::nullopt;
    }

    return next;
}

/**
 * @brief The checker's answer for one state and one family.
 */
struct Verdict
{
    /**
     * @brief Whether every death word from the state holds a member ending strictly before its killing byte.
     */
    bool proved{false};

    /**
     * @brief A death word no member precedes, empty when proved.
     */
    std::string witness{};
};

/**
 * @brief A pair of a live state and a matcher node, the decision procedure's search key.
 */
using Pair_t = std::pair<std::size_t, std::size_t>;

/**
 * @brief How the search first reached a pair: the pair it came from and the byte between them.
 */
struct Parent
{
    /**
     * @brief The pair the byte was read from.
     */
    Pair_t from{};

    /**
     * @brief The byte.
     */
    char byte{0};
};

/**
 * @brief The live subautomaton of a compiled DFA as a checker view, with a state finder for the tests.
 */
class Compiled
{
public:
    /**
     * @brief Keeps the automaton and its live states.
     * @param dfa The compiled automaton.
     */
    explicit Compiled(const Dfa& dfa);

    /**
     * @brief Advances the live subautomaton by one byte.
     * @param state A live state.
     * @param byte The byte.
     * @return The live state the byte leads to, std::nullopt when it leads to no state or to a dead one.
     */
    [[nodiscard]] std::optional<std::size_t> advance(std::size_t state, unsigned char byte) const;

    /**
     * @brief Returns the live state a completely tokenizable prefix leaves the automaton in, for locating interiors.
     * @param prefix The prefix, every byte of it leading to a state.
     * @return The state after the prefix.
     */
    [[nodiscard]] std::size_t after(std::string_view prefix) const;

private:
    /**
     * @brief The automaton, a copy of the one given.
     */
    Dfa dfa_;

    /**
     * @brief The automaton's trim states, the only ones advance() leads to.
     */
    States_t live_;
};

Compiled::Compiled(const Dfa& dfa) : dfa_{dfa}, live_{live_states(dfa_)}
{}

std::optional<std::size_t> Compiled::advance(const std::size_t state, const unsigned char byte) const
{
    const auto next{dfa_.advance(state, static_cast<char>(byte))};

    if (!next || !live_.contains(*next))
    {
        return std::nullopt;
    }

    return next;
}

std::size_t Compiled::after(const std::string_view prefix) const
{
    auto state{dfa_.init_state()};

    for (const char byte : prefix)
    {
        state = *dfa_.advance(state, byte);
    }

    return state;
}

/**
 * @brief The synthetic counterexample's table over the states q, s and t. Both S-states loop on every byte not named,
 *        so the only death in the whole table is s on b: exactly the counterexample's shape, an internal death
 *        bypassing every c-bearing first-exit word. The repaired table lets s survive b.
 */
struct Synthetic
{
    /**
     * @brief The state q, the initial one.
     */
    static constexpr std::size_t q{0};

    /**
     * @brief The state s, entered from q on a.
     */
    static constexpr std::size_t s{1};

    /**
     * @brief The state t, entered on c, which loops on every byte.
     */
    static constexpr std::size_t t{2};

    /**
     * @brief Advances the table by one byte.
     * @param state The state, below three.
     * @param byte The byte.
     * @return The state the byte leads to, std::nullopt when it kills the state.
     */
    [[nodiscard]] std::optional<std::size_t> advance(const std::size_t state, const unsigned char byte) const
    {
        if (state == q && byte == 'a')
        {
            return s;
        }

        if (state == q && byte == 'c')
        {
            return t;
        }

        if (state == q)
        {
            return q;
        }

        if (state == s)
        {
            if (byte == 'c')
            {
                return t;
            }

            if (byte == 'b' && !repaired)
            {
                return std::nullopt;
            }

            return s;
        }

        return t;
    }

    /**
     * @brief Whether s survives b.
     */
    bool repaired{false};
};

/**
 * @brief Decides the premise by its procedure: BFS over pairs of a live state and a match-free matcher state.
 *
 * The premise fails exactly when a reachable pair's live state is missing some byte, since appending that byte to the
 * pair's prefix is a death word no family member precedes; the witness is that word. Pairs whose matcher has completed
 * a member are satisfied for every continuation and are not expanded.
 *
 * @tparam View An automaton as the checker sees it: `advance(state, byte)` gives the live state a byte leads to, or
 *         std::nullopt where the byte kills the state.
 * @param view The automaton.
 * @param q The state decided.
 * @param family The family K, nonempty strings.
 * @return Proved, or refuted with the death word no member precedes.
 */
template <typename View>
Verdict check(const View& view, const std::size_t q, const std::vector<std::string>& family)
{
    const Matcher matcher{family};

    std::map<Pair_t, Parent> parent{};

    std::deque<Pair_t> pending{{q, 0}};

    std::set<Pair_t> seen{{q, 0}};

    const auto witness_of{[&parent, q](const Pair_t reached, const unsigned char byte) {
        std::string witness{static_cast<char>(byte)};

        for (auto at{reached}; at != Pair_t{q, 0};)
        {
            const auto& [from, by]{parent.at(at)};

            witness.insert(witness.begin(), by);

            at = from;
        }

        return witness;
    }};

    while (!pending.empty())
    {
        const auto [state, node]{pending.front()};

        pending.pop_front();

        for (const auto symbol : every_byte())
        {
            const auto byte{static_cast<unsigned char>(symbol)};

            const auto next{view.advance(state, byte)};

            // A killing byte with no completed member on the prefix refutes the family.
            if (!next)
            {
                return {.proved = false, .witness = witness_of({state, node}, byte)};
            }

            const auto stepped{matcher.step(node, byte)};

            // A member completed strictly before any later killing byte.
            if (!stepped)
            {
                continue;
            }

            const Pair_t reached{*next, *stepped};

            const auto [where, inserted]{seen.insert(reached)};

            if (!inserted)
            {
                continue;
            }

            parent.emplace(reached, Parent{.from = {state, node}, .byte = static_cast<char>(byte)});

            pending.push_back(reached);
        }
    }

    return {.proved = true, .witness = {}};
}

/**
 * @brief Renders a word for a verdict line: a newline as `\n`, a tab as `\t`, a printable byte as itself, and any other
 *        byte as `\x` and its two hexadecimal digits.
 * @param word The word.
 * @return The rendering.
 */
std::string printable(const std::string& word)
{
    std::string out{};

    for (const char byte : word)
    {
        switch (byte)
        {
        case '\n':
            out += R"(\n)";

            break;

        case '\t':
            out += R"(\t)";

            break;

        default:
            if (byte >= ' ' && byte <= '~')
            {
                out.push_back(byte);
            }
            else
            {
                out += std::format(R"(\x{:02x})", static_cast<unsigned char>(byte));
            }

            break;
        }
    }

    return out;
}

/**
 * @brief Renders a verdict for its line: `proved`, or `refuted, witness [<witness>]` with the witness rendered.
 * @param proved Whether the family is proved.
 * @param witness The refutation's witness, empty when proved.
 * @return The rendering.
 */
std::string verdict_text(const bool proved, const std::string& witness)
{
    if (proved)
    {
        return "proved";
    }

    return std::format("refuted, witness [{}]", printable(witness));
}

/**
 * @brief Asserts that a witness renders every byte so that the line names it: a byte that is neither printable nor a
 *        newline or a tab by its two hexadecimal digits.
 * @param assertions The probe's assertions.
 */
void witness_rendering(Assertions& assertions)
{
    assertions.expect(
            printable("a\x01\n\t\x7f\xff") == R"(a\x01\n\t\x7f\xff)", "a witness renders a byte it cannot name");
}

/**
 * @brief Decides the C-like cumulative row's comment interior: it must prove {*\/}, and an unrelated family must be
 *        refuted as a negative control; prints the verdict line.
 * @param assertions The probe's assertions.
 */
void comment_interior(Assertions& assertions)
{
    Builder_dbg builder{};

    published_cumulative_row(builder);

    const Compiled compiled{builder.dfa()};

    const auto interior{compiled.after("/*x")};

    const auto [proved, witness]{check(compiled, interior, {"*/"})};

    std::cout << std::format("C comment interior, K={{*/}}: {}\n", verdict_text(proved, witness));

    assertions.expect(proved, "the C row's comment interior does not prove {*/}");

    // Negative control: a family the interior can be killed around must be refuted.
    const auto [wrong_proved, wrong_witness]{check(compiled, interior, {"@@"})};

    assertions.expect(!wrong_proved, "the C row's comment interior proves an unrelated family");
}

/**
 * @brief Decides a Python-like triple-quote row's string interior, which must prove {three quotes}; prints the verdict
 *        line.
 * @param assertions The probe's assertions.
 */
void triple_quote_interior(Assertions& assertions)
{
    Builder_dbg builder{};

    const auto identifier{figures::identifier()};

    builder.add_token(identifier, Token::identifier, 2);

    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::whitespace, 2);

    const auto quote{'"'};

    const std::string one{quote};

    const auto two{one + one};

    const auto triple{two + one};

    const auto other{any_of(Set::all() - Set{quote})};

    const auto body{kleene(choice(other, concat(text(one), other), concat(text(two), other)))};

    builder.add_token(concat(text(triple), body, text(triple)), Token::string, 1);

    const Compiled compiled{builder.dfa()};

    const auto interior{compiled.after(R"("""x)")};

    const auto [proved, witness]{check(compiled, interior, {triple})};

    std::cout << std::format("Python triple interior, K={{\"\"\"}}: {}\n", verdict_text(proved, witness));

    assertions.expect(proved, "the triple-quote interior does not prove its delimiter family");
}

/**
 * @brief Decides the RFC 8259 row's string interior, which dies on a control byte with an empty prefix, so every family
 *        is refuted with a one-byte witness and the planner's exhaustive walk stays; prints the verdict line.
 * @param assertions The probe's assertions.
 */
void json_string_interior(Assertions& assertions)
{
    Builder_dbg builder{};

    figures::json(builder);

    const Compiled compiled{builder.dfa()};

    const auto interior{compiled.after(R"("x)")};

    const auto [proved, witness]{check(compiled, interior, {R"(",)"})};

    std::cout << std::format("JSON string interior, any K: {}\n", verdict_text(proved, witness));

    assertions.expect(!proved, "the JSON string interior proves a family although a control byte kills it");

    assertions.expect(witness.size() == 1, "the JSON refutation witness is not the immediate one-byte death");
}

/**
 * @brief Decides the accept state of {a, b}, which certifies the window ab yet proves no family anywhere: the family
 *        {a} must be refuted there. Prints the verdict line.
 * @param assertions The probe's assertions.
 */
void two_letter_accept(Assertions& assertions)
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::identifier, 2);

    builder.add_token(text("b"), Token::number, 2);

    const Compiled compiled{builder.dfa()};

    const auto accept{compiled.after("a")};

    const auto [proved, witness]{check(compiled, accept, {"a"})};

    assertions.expect(!proved, "the {a, b} accept state proves a family although it dies immediately");

    std::cout << std::format("{{a, b}} accept state, any K: {}\n", verdict_text(proved, witness));
}

/**
 * @brief Decides the synthetic counterexample: S = {q, s}, s dies on b, every first-exit word contains c, yet ab is a
 *        death word from q avoiding c. The checker must refute K = {c} with exactly that witness, and the repaired
 *        table, where s survives b, must prove it. Prints the verdict line.
 * @param assertions The probe's assertions.
 */
void synthetic_counterexample(Assertions& assertions)
{
    const Synthetic broken_table{.repaired = false};

    const auto [proved, witness]{check(broken_table, Synthetic::q, {"c"})};

    std::cout << std::format("synthetic counterexample, K={{c}}: {}\n", verdict_text(proved, witness));

    assertions.expect(!proved, "the counterexample table proves {c} although ab is a c-free death word");

    assertions.expect(witness == "ab", "the counterexample witness is not ab");

    const Synthetic repaired_table{.repaired = true};

    const auto [fixed_proved, fixed_witness]{check(repaired_table, Synthetic::q, {"c"})};

    // Repairing s removes the table's only death, so the premise holds vacuously: a state with no death words
    // constrains nothing, and the checker must say so rather than hunt for cores that need not exist.
    assertions.expect(fixed_proved, "a table with no death words must prove any family vacuously");
}

} // namespace

/**
 * @brief Checks the witness rendering, decides the five pinned cases and prints the verdict.
 * @return EXIT_SUCCESS when every assertion holds, EXIT_FAILURE otherwise.
 */
int main()
{
    Assertions assertions{};

    witness_rendering(assertions);

    comment_interior(assertions);

    triple_quote_interior(assertions);

    json_string_interior(assertions);

    two_letter_accept(assertions);

    synthetic_counterexample(assertions);

    std::cout << (assertions.has_failures() ? "assertion failures\n" : "all assertions hold\n");

    return assertions.has_failures() ? EXIT_FAILURE : EXIT_SUCCESS;
}

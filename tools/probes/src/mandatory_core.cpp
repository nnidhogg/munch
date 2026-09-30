// Decides the mandatory-core premise of the proof-directed planner: for a state q and a family K of byte
// strings, does every death word from q contain some member of K ending strictly before the killing byte?
//
// The planned prefilter narrows its candidate windows to core occurrences, and its exact-plan-equality argument
// rests on the premise above (the mandatory-death-core theorem). The premise is decided per grammar, not recognized
// by shape: there is a concrete automaton where a plausible component satisfies the first-exit intuition while an
// internal death bypasses the core entirely. The decision procedure: an Aho-Corasick matcher over K is producted with
// the live automaton, and the premise fails exactly when a pair of a live state and a match-free matcher
// state is reachable whose live state is not input-total, since any missing byte there ends a K-avoiding
// death word. The matcher reads only the live prefix; the killing byte is never fed to it, because a core
// completed on the killing byte is too late. Families are nonempty strings by contract; a state that can
// die with no core before the killing byte gets a refuted verdict with a reconstructed witness.
//
// What runs as a test. The shipped instances and the counterexamples, all pinned:
//   - the C-like cumulative row proves the family {*/} at its comment-interior state;
//   - a Python-like triple-quote row proves the family {three quotes} at its string-interior state;
//   - the RFC 8259 row refutes every family at its string-interior state with a one-byte witness, since a
//     control byte kills it immediately: JSON gets no filter and the planner's exhaustive walk stays;
//   - the token set {a, b} has no provable family anywhere although it certifies the window ab: the checker
//     refuses, and such grammars keep the exhaustive walk;
//   - a synthetic automaton refutes K = {c} with the witness ab, and a repaired variant of the same table proves
//     it.
//
// The checker runs over any view with advance(state, byte), so hand-built tables and compiled automata run
// through the identical decision procedure.

#include <cstddef>
#include <deque>
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
#include "munch/tools/probes/assertions.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/study_rows.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace
{
using figures::Token;
using munch::tools::probes::Assertions;
using munch::tools::probes::Builder_dbg;
using munch::tools::probes::live_states;
using munch::tools::probes::published_cumulative_row;
using namespace munch::regex;
using munch::dfa::Dfa;

/**
 * @brief Aho-Corasick matcher over a family of nonempty byte strings, reporting only whether a member has
 *        completed; goto and fail links are precomputed, and terminal reachability propagates over fails.
 */
class Matcher
{
public:
    /**
     * @brief Builds the trie of the family, its fail links breadth first, and each node's terminal flag propagated
     *        over its fail link.
     * @param family The family, nonempty strings.
     */
    explicit Matcher(const std::vector<std::string>& family)
    {
        nodes_.push_back({});

        for (const auto& word : family)
        {
            std::size_t at{0};

            for (const char byte : word)
            {
                const auto key{static_cast<unsigned char>(byte)};

                if (nodes_[at].next.contains(key))
                {
                    at = nodes_[at].next.at(key);
                }
                else
                {
                    nodes_.push_back({});

                    nodes_[at].next.emplace(key, nodes_.size() - 1);

                    at = nodes_[at].next.at(key);
                }
            }

            nodes_[at].terminal = true;
        }

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
                auto fall{nodes_[at].fail};

                while (fall != 0 && !nodes_[fall].next.contains(key))
                {
                    fall = nodes_[fall].fail;
                }

                nodes_[child].fail = nodes_[fall].next.contains(key) && nodes_[fall].next.at(key) != child ?
                                             nodes_[fall].next.at(key) :
                                             0;

                pending.push_back(child);
            }
        }
    }

    /**
     * @brief One byte of matching; std::nullopt once a family member has completed.
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
        std::map<unsigned char, std::size_t> next;

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
     * @brief The trie, the root at index 0.
     */
    std::vector<Node> nodes_;
};

/**
 * @brief The checker's answer for one state and one family.
 */
struct Verdict
{
    /**
     * @brief Whether every death word from the state holds a member ending strictly before its killing byte.
     */
    bool proved{};

    /**
     * @brief A death word no member precedes, empty when proved.
     */
    std::string witness;
};

/**
 * @brief The decision procedure: BFS over pairs of a live state and a match-free matcher state.
 *
 * The premise fails exactly when a reachable pair's live state is missing some byte, since appending that
 * byte to the pair's prefix is a death word no family member precedes; the witness is that word. Pairs
 * whose matcher has completed a member are satisfied for every continuation and are not expanded.
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

    std::map<std::pair<std::size_t, std::size_t>, std::pair<std::pair<std::size_t, std::size_t>, char>> parent{};

    std::deque<std::pair<std::size_t, std::size_t>> pending{{q, 0}};

    std::set<std::pair<std::size_t, std::size_t>> seen{{q, 0}};

    while (!pending.empty())
    {
        const auto [state, node]{pending.front()};

        pending.pop_front();

        for (int value{0}; value < 256; ++value)
        {
            const auto byte{static_cast<unsigned char>(value)};

            const auto next{view.advance(state, byte)};

            if (!next)
            {
                // A killing byte with no completed member on the prefix: reconstruct the witness.
                std::string witness{static_cast<char>(byte)};

                for (auto at{std::pair{state, node}}; at != std::pair{q, std::size_t{0}}; at = parent.at(at).first)
                {
                    witness.insert(witness.begin(), parent.at(at).second);
                }

                return {.proved = false, .witness = std::move(witness)};
            }

            const auto stepped{matcher.step(node, byte)};

            if (!stepped)
            {
                continue; // a member completed strictly before any later killing byte
            }

            if (const auto pair{std::pair{*next, *stepped}}; seen.insert(pair).second)
            {
                parent.emplace(pair, std::pair{std::pair{state, node}, static_cast<char>(byte)});

                pending.push_back(pair);
            }
        }
    }

    return {.proved = true, .witness = {}};
}

std::optional<std::size_t> Matcher::step(const std::size_t at, const unsigned char byte) const
{
    auto node{at};

    while (node != 0 && !nodes_[node].next.contains(byte))
    {
        node = nodes_[node].fail;
    }

    const auto next{nodes_[node].next.contains(byte) ? nodes_[node].next.at(byte) : 0};

    return nodes_[next].terminal ? std::nullopt : std::optional{next};
}

/**
 * @brief The live subautomaton of a compiled DFA as a checker view, with a state finder for the tests.
 */
struct Compiled
{
    /**
     * @brief Keeps the automaton and its live states.
     * @param dfa The compiled automaton.
     */
    explicit Compiled(const Dfa& dfa) : dfa_{dfa}, live_{live_states(dfa_)} {}

    /**
     * @brief One byte of the live subautomaton.
     * @param state A live state.
     * @param byte The byte.
     * @return The live state the byte leads to, std::nullopt when it leads to no state or to a dead one.
     */
    [[nodiscard]] std::optional<std::size_t> advance(const std::size_t state, const unsigned char byte) const
    {
        const auto next{dfa_.advance(state, static_cast<char>(byte))};

        return next && live_.contains(*next) ? next : std::nullopt;
    }

    /**
     * @brief The live state a completely tokenizable prefix leaves the automaton in, for locating interiors.
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
    std::set<std::size_t> live_;
};

/**
 * @brief Renders a word for a verdict line: a newline as `\n`, a tab as `\t`, a printable byte as a 0x01 byte
 *        followed by the byte itself, and any other byte as `\x`.
 * @param word The word.
 * @return The rendering.
 */
std::string printable(const std::string& word)
{
    std::string out{};

    for (const char byte : word)
    {
        out += byte == '\n'             ? std::string{"\\n"} :
               byte == '\t'             ? std::string{"\\t"} :
               byte >= 32 && byte < 127 ? std::string{1, byte} :
                                          std::string{"\\x"};
    }

    return out;
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

    const auto closer{check(compiled, interior, {"*/"})};

    std::cout << "C comment interior, K={*/}: "
              << (closer.proved ? "proved" : "refuted, witness [" + printable(closer.witness) + "]") << "\n";

    assertions.expect(closer.proved, "the C row's comment interior does not prove {*/}");

    // Negative control: a family the interior can be killed around must be refuted.
    const auto wrong{check(compiled, interior, {"@@"})};

    assertions.expect(!wrong.proved, "the C row's comment interior proves an unrelated family");
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
 * @brief Decides a Python-like triple-quote row's string interior, which must prove {three quotes}; prints the verdict
 *        line.
 * @param assertions The probe's assertions.
 */
void triple_quote_interior(Assertions& assertions)
{
    Builder_dbg builder{};

    builder.add_token(concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
    builder.add_token(plus(any_of(Set{' ', '\t', '\n', '\r'})), Token::Whitespace, 2);

    const auto quote{'"'};

    const std::string one{quote};

    const auto other{any_of(Set::all() - Set{quote})};

    builder.add_token(
            concat(text(one + one + one),
                   kleene(choice(other, concat(text(one), other), concat(text(one + one), other))),
                   text(one + one + one)),
            Token::String, 1);

    const Compiled compiled{builder.dfa()};

    const auto interior{compiled.after("\"\"\"x")};

    const auto triple{check(compiled, interior, {one + one + one})};

    std::cout << "Python triple interior, K={\"\"\"}: "
              << (triple.proved ? "proved" : "refuted, witness [" + printable(triple.witness) + "]") << "\n";

    assertions.expect(triple.proved, "the triple-quote interior does not prove its delimiter family");
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

    const auto interior{compiled.after("\"x")};

    const auto refuted{check(compiled, interior, {"\","})};

    std::cout << "JSON string interior, any K: "
              << (refuted.proved ? "proved" : "refuted, witness [" + printable(refuted.witness) + "]") << "\n";

    assertions.expect(!refuted.proved, "the JSON string interior proves a family although a control byte kills it");

    assertions.expect(refuted.witness.size() == 1, "the JSON refutation witness is not the immediate one-byte death");
}

/**
 * @brief Decides the accept state of {a, b}, which certifies the window ab yet proves no family anywhere: the family
 *        {a} must be refuted there. Prints the verdict line.
 * @param assertions The probe's assertions.
 */
void two_letter_accept(Assertions& assertions)
{
    Builder_dbg builder{};

    builder.add_token(text("a"), Token::Identifier, 2);
    builder.add_token(text("b"), Token::Number, 2);

    const Compiled compiled{builder.dfa()};

    const auto accept{compiled.after("a")};

    const auto refuted{check(compiled, accept, {"a"})};

    assertions.expect(!refuted.proved, "the {a, b} accept state proves a family although it dies immediately");

    std::cout << "{a, b} accept state, any K: refuted, witness [" << printable(refuted.witness) << "]\n";
}

/**
 * @brief The synthetic counterexample's table. States 0 = q, 1 = s, 2 = t. Both S-states loop on every byte not named,
 *        so the only death in the whole table is s on b: exactly the counterexample's shape, an internal death
 *        bypassing every c-bearing first-exit word. The repaired table lets s survive b.
 */
struct Synthetic
{
    /**
     * @brief Whether s survives b.
     */
    bool repaired{};

    /**
     * @brief One byte of the table.
     * @param state The state, below three.
     * @param byte The byte.
     * @return The state the byte leads to, std::nullopt when it kills the state.
     */
    [[nodiscard]] std::optional<std::size_t> advance(const std::size_t state, const unsigned char byte) const
    {
        if (state == 0)
        {
            return byte == 'a' ? 1 : byte == 'c' ? 2 : 0;
        }

        if (state == 1)
        {
            if (byte == 'c')
            {
                return 2;
            }

            if (byte == 'b' && !repaired)
            {
                return std::nullopt;
            }

            return 1;
        }

        return 2;
    }
};

/**
 * @brief Decides the synthetic counterexample: S = {q, s}, s dies on b, every first-exit word contains c, yet ab is a
 *        death word from q avoiding c. The checker must refute K = {c} with exactly that witness, and the repaired
 *        table, where s survives b, must prove it. Prints the verdict line.
 * @param assertions The probe's assertions.
 */
void synthetic_counterexample(Assertions& assertions)
{
    const auto broken{check(Synthetic{.repaired = false}, 0, {"c"})};

    std::cout << "synthetic counterexample, K={c}: "
              << (broken.proved ? "proved" : "refuted, witness [" + printable(broken.witness) + "]") << "\n";

    assertions.expect(!broken.proved, "the counterexample table proves {c} although ab is a c-free death word");

    assertions.expect(broken.witness == "ab", "the counterexample witness is not ab");

    const auto fixed{check(Synthetic{.repaired = true}, 0, {"c"})};

    // Repairing s removes the table's only death, so the premise holds vacuously: a state with no death words
    // constrains nothing, and the checker must say so rather than hunt for cores that need not exist.
    assertions.expect(fixed.proved, "a table with no death words must prove any family vacuously");
}
} // namespace

/**
 * @brief Decides the five pinned cases and prints the verdict.
 * @return 0 when every assertion holds, 1 otherwise.
 */
int main()
{
    Assertions assertions{};

    comment_interior(assertions);

    triple_quote_interior(assertions);

    json_string_interior(assertions);

    two_letter_accept(assertions);

    synthetic_counterexample(assertions);

    std::cout << (assertions.has_failures() ? "ASSERTION FAILURES\n" : "all assertions hold\n");

    return assertions.has_failures() ? 1 : 0;
}

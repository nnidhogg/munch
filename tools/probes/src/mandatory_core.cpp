// Decides the mandatory-core premise of the proof-directed planner: for a state q and a family K of byte
// strings, does EVERY death word from q contain some member of K ending strictly before the killing byte?
//
// WHY THIS EXISTS. The planned prefilter narrows its candidate windows to core occurrences, and its
// exact-plan-equality argument rests on the premise above (the mandatory-death-core theorem). The
// premise must be PROVED per grammar, not recognized by shape: there is a concrete
// automaton where a plausible component satisfies the first-exit intuition while an internal death bypasses
// the core entirely. This probe is the decision procedure: an Aho-Corasick matcher over K is producted with
// the live automaton, and the premise fails exactly when a pair of a live state and a match-free matcher
// state is reachable whose live state is not input-total, since any missing byte there ends a K-avoiding
// death word. The matcher reads only the live prefix; the killing byte is never fed to it, because a core
// completed on the killing byte is too late. Families are nonempty strings by contract; a state that can
// die with no core before the killing byte gets a REFUTED verdict with a reconstructed witness.
//
// WHAT RUNS AS A TEST. The shipped instances and the review's counterexamples, all pinned:
//   - the C-like cumulative row PROVES the family {*/} at its comment-interior state;
//   - a Python-like triple-quote row PROVES the family {three quotes} at its string-interior state;
//   - the RFC 8259 row REFUTES every family at its string-interior state with a one-byte witness, since a
//     control byte kills it immediately: JSON gets no filter and the planner's exhaustive walk stays;
//   - the token set {a, b} has no provable family anywhere although it certifies the window ab, the case
//     that made the naive planner-equality claim false: the checker's refusal is what routes such grammars
//     to the exhaustive walk;
//   - the review's synthetic automaton REFUTES K = {c} with the witness ab, and a repaired variant of the
//     same table PROVES it, so the load-bearing premise correction is itself a checked behavior.
//
// The checker operates on an abstract table view so that hand-built tables and compiled automata run
// through the identical decision procedure.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"

namespace
{
using figures::Token;
using namespace munch::regex;
using munch::dfa::Dfa;

std::size_t failures{0};

void expect(const bool condition, const char* what)
{
    if (!condition)
    {
        ++failures;

        std::cout << "FAIL: " << what << "\n";
    }
}

class Builder_dbg : public munch::core::Builder
{
public:
    using Builder::dfa;
};

/**
 * @brief An automaton as the checker sees it: live states and live transitions only.
 */
struct View
{
    std::size_t states{};

    std::function<std::optional<std::size_t>(std::size_t, unsigned char)> advance;
};

/**
 * @brief Aho-Corasick matcher over a family of nonempty byte strings, reporting only whether a member has
 *        completed; goto and fail links are precomputed, and terminal reachability propagates over fails.
 */
class Matcher
{
public:
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

        std::deque<std::size_t> pending;

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

    [[nodiscard]] std::size_t size() const { return nodes_.size(); }

    /**
     * @brief One byte of matching; std::nullopt once a family member has completed.
     */
    [[nodiscard]] std::optional<std::size_t> step(const std::size_t at, const unsigned char byte) const
    {
        auto node{at};

        while (node != 0 && !nodes_[node].next.contains(byte))
        {
            node = nodes_[node].fail;
        }

        const auto next{nodes_[node].next.contains(byte) ? nodes_[node].next.at(byte) : 0};

        return nodes_[next].terminal ? std::nullopt : std::optional{next};
    }

private:
    struct Node
    {
        std::map<unsigned char, std::size_t> next;

        std::size_t fail{0};

        bool terminal{false};
    };

    std::vector<Node> nodes_;
};

struct Verdict
{
    bool proved{};

    std::string witness;
};

/**
 * @brief The decision procedure: BFS over pairs of a live state and a match-free matcher state.
 *
 * The premise fails exactly when a reachable pair's live state is missing some byte, since appending that
 * byte to the pair's prefix is a death word no family member precedes; the witness is that word. Pairs
 * whose matcher has completed a member are satisfied for every continuation and are not expanded.
 */
Verdict check(const View& view, const std::size_t q, const std::vector<std::string>& family)
{
    const Matcher matcher{family};

    std::map<std::pair<std::size_t, std::size_t>, std::pair<std::pair<std::size_t, std::size_t>, char>> parent;

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

/**
 * @brief The live subautomaton of a compiled DFA as a checker view, with a state finder for the tests.
 */
struct Compiled
{
    explicit Compiled(const Dfa& dfa) : dfa_{dfa}
    {
        std::set<std::size_t> reachable{dfa_.init_state()};

        std::deque<std::size_t> pending{dfa_.init_state()};

        while (!pending.empty())
        {
            const auto state{pending.front()};

            pending.pop_front();

            for (int value{0}; value < 256; ++value)
            {
                if (const auto next{dfa_.advance(state, static_cast<char>(value))}; next && !reachable.contains(*next))
                {
                    reachable.insert(*next);

                    pending.push_back(*next);
                }
            }
        }

        std::set<std::size_t> co;

        for (const auto state : reachable)
        {
            if (dfa_.has_accept_token(state))
            {
                co.insert(state);
            }
        }

        for (bool grew{true}; grew;)
        {
            grew = false;

            for (const auto state : reachable)
            {
                if (co.contains(state))
                {
                    continue;
                }

                for (int value{0}; value < 256 && !co.contains(state); ++value)
                {
                    if (const auto next{dfa_.advance(state, static_cast<char>(value))}; next && co.contains(*next))
                    {
                        co.insert(state);

                        grew = true;
                    }
                }
            }
        }

        for (const auto state : reachable)
        {
            if (co.contains(state))
            {
                live_.insert(state);
            }
        }
    }

    [[nodiscard]] View view() const
    {
        return {.states = live_.size(), .advance = [this](const std::size_t state, const unsigned char byte) {
                    const auto next{dfa_.advance(state, static_cast<char>(byte))};

                    return next && live_.contains(*next) ? next : std::nullopt;
                }};
    }

    /**
     * @brief The live state a completely tokenizable prefix leaves the automaton in, for locating interiors.
     */
    [[nodiscard]] std::size_t after(const std::string_view prefix) const
    {
        auto state{dfa_.init_state()};

        for (const char byte : prefix)
        {
            state = *dfa_.advance(state, byte);
        }

        return state;
    }

private:
    Dfa dfa_;

    std::set<std::size_t> live_;
};

std::string printable(const std::string& word)
{
    std::string out;

    for (const char byte : word)
    {
        out += byte == '\n'             ? std::string{"\\n"} :
               byte == '\t'             ? std::string{"\\t"} :
               byte >= 32 && byte < 127 ? std::string{1, byte} :
                                          std::string{"\\x"};
    }

    return out;
}
} // namespace

int main()
{
    // The C-like cumulative row: the comment interior must prove {*/}.
    {
        Builder_dbg builder;

        figures::c_like(builder, false);
        builder.add_token(figures::string_literal(), Token::String, 2);
        builder.add_token(figures::line_comment(), Token::LineComment, 1);
        builder.add_token(figures::block_comment(), Token::BlockComment, 1);

        const Compiled compiled{builder.dfa()};

        const auto interior{compiled.after("/*x")};

        const auto closer{check(compiled.view(), interior, {"*/"})};

        std::cout << "C comment interior, K={*/}: "
                  << (closer.proved ? "PROVED" : "refuted, witness [" + printable(closer.witness) + "]") << "\n";

        expect(closer.proved, "the C row's comment interior does not prove {*/}");

        // Negative control: a family the interior can be killed around must be refuted.
        const auto wrong{check(compiled.view(), interior, {"@@"})};

        expect(!wrong.proved, "the C row's comment interior proves an unrelated family");
    }

    // A Python-like triple-quote row: the string interior must prove {three quotes}.
    {
        Builder_dbg builder;

        builder.add_token(
                concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::Identifier, 2);
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

        const auto triple{check(compiled.view(), interior, {one + one + one})};

        std::cout << "Python triple interior, K={\"\"\"}: "
                  << (triple.proved ? "PROVED" : "refuted, witness [" + printable(triple.witness) + "]") << "\n";

        expect(triple.proved, "the triple-quote interior does not prove its delimiter family");
    }

    // JSON: the string interior dies on a control byte with an empty prefix, so every family is refuted and
    // the planner's exhaustive walk stays.
    {
        Builder_dbg builder;

        figures::json(builder);

        const Compiled compiled{builder.dfa()};

        const auto interior{compiled.after("\"x")};

        const auto refuted{check(compiled.view(), interior, {"\","})};

        std::cout << "JSON string interior, any K: "
                  << (refuted.proved ? "PROVED" : "refuted, witness [" + printable(refuted.witness) + "]") << "\n";

        expect(!refuted.proved, "the JSON string interior proves a family although a control byte kills it");

        expect(refuted.witness.size() == 1, "the JSON refutation witness is not the immediate one-byte death");
    }

    // {a, b}: certifies the window ab yet proves no family anywhere; the checker's refusal is what routes
    // this grammar to the exhaustive walk, resolving the old vacuous-premise failure.
    {
        Builder_dbg builder;

        builder.add_token(text("a"), Token::Identifier, 2);
        builder.add_token(text("b"), Token::Number, 2);

        const Compiled compiled{builder.dfa()};

        const auto accept{compiled.after("a")};

        const auto refuted{check(compiled.view(), accept, {"a"})};

        expect(!refuted.proved, "the {a, b} accept state proves a family although it dies immediately");

        std::cout << "{a, b} accept state, any K: refuted, witness [" << printable(refuted.witness) << "]\n";
    }

    // The confirmation review's counterexample: S = {q, s}, s dies on b, every first-exit word contains c,
    // yet ab is a death word from q avoiding c. The checker must refute K = {c} with exactly that witness,
    // and the repaired table, where s survives b, must prove it.
    {
        // States 0 = q, 1 = s, 2 = t. Both S-states loop on every byte not named, so the ONLY death in the
        // whole table is s on b: exactly the review's shape, an internal death bypassing every c-bearing
        // first-exit word.
        const auto synthetic{[](const bool repaired) {
            return View{
                    .states = 3,
                    .advance = [repaired](const std::size_t state, const unsigned char byte)
                            -> std::optional<std::size_t> {
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
                    }};
        }};

        const auto broken{check(synthetic(false), 0, {"c"})};

        std::cout << "review counterexample, K={c}: "
                  << (broken.proved ? "PROVED" : "refuted, witness [" + printable(broken.witness) + "]") << "\n";

        expect(!broken.proved, "the counterexample table proves {c} although ab is a c-free death word");

        expect(broken.witness == "ab", "the counterexample witness is not ab");

        const auto fixed{check(synthetic(true), 0, {"c"})};

        // Repairing s removes the table's only death, so the premise holds vacuously: a state with no death
        // words constrains nothing, and the checker must say so rather than hunt for cores that need not
        // exist.
        expect(fixed.proved, "a table with no death words must prove any family vacuously");
    }

    std::cout << (failures == 0 ? "all assertions hold\n" : "ASSERTION FAILURES\n");

    return failures == 0 ? 0 : 1;
}

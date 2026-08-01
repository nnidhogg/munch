/*
 * Asserts the composition measurement of paper/split-points.tex, which compares the certificate against
 * reconstructing the scan state at each line start by a parallel prefix scan.
 *
 * The number that decides whether such a scan is cheap is how many distinct states can occur at a line start, since
 * that is the domain the per-line transition functions range over. Two things are computed here, and the report
 * should quote the first:
 *
 *   1. A STRUCTURAL bound, read off the automaton with no corpus at all. A line start is either a token boundary or
 *      sits mid-token having just consumed a newline, so the mid-token contexts are contained in the targets of the
 *      newline transitions out of live reachable states that can still continue. This holds for every input.
 *   2. The set a corpus actually realizes, which must be a subset of the bound. Corpus-dependent counts are reported
 *      but deliberately not published as headline figures: they say more about the corpus than about the grammar.
 *
 * The third assertion is the hazard a line-local scheme quietly needs: that maximal munch never accepts before a
 * line end and then keeps reading past it, which would make a per-line function depend on the next line's bytes.
 */

#include <cstddef>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/dfa.hpp"

namespace
{
using namespace munch::regex;
using namespace figures;

using State_t = munch::dfa::Dfa::State_t;

// Builder keeps its compiled DFA protected, and build() returns Lexer{dfa()}.
class Exposed final : public munch::core::Builder
{
public:
    using munch::core::Builder::dfa;
};

std::set<State_t> all_states(const munch::dfa::Dfa& dfa)
{
    std::set<State_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        states.insert(key.first);

        states.insert(to);
    }

    for (const auto& [state, token] : dfa.accept_states())
    {
        states.insert(state);
    }

    return states;
}

std::set<State_t> reachable_states(const munch::dfa::Dfa& dfa)
{
    std::set<State_t> reachable{dfa.init_state()};

    std::vector<State_t> work{dfa.init_state()};

    while (!work.empty())
    {
        const auto state{work.back()};

        work.pop_back();

        for (const auto& [key, to] : dfa.transitions())
        {
            if (key.first == state && reachable.insert(to).second)
            {
                work.push_back(to);
            }
        }
    }

    return reachable;
}

bool can_continue(const munch::dfa::Dfa& dfa, const State_t state)
{
    for (int value{0}; value < 256; ++value)
    {
        if (dfa.advance(state, static_cast<char>(value)))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief The states a scan can occupy at a line start while still inside a token, over every possible input.
 *
 * A line start follows a newline. If that newline was consumed as part of a token, the scan sits in the target of a
 * newline transition, and the token continues only if that target has some outgoing transition. Anything else makes
 * the line start a token boundary, which needs no state at all.
 */
std::set<State_t> structural_line_contexts(const munch::dfa::Dfa& dfa)
{
    const auto reachable{reachable_states(dfa)};

    std::set<State_t> contexts;

    for (const auto state : reachable)
    {
        if (const auto to{dfa.advance(state, '\n')}; to && can_continue(dfa, *to))
        {
            contexts.insert(*to);
        }
    }

    return contexts;
}

// Describes a state by what it accepts, so the report can characterize the contexts rather than count them blindly.
std::string describe(const munch::dfa::Dfa& dfa, const State_t state)
{
    const auto accept{dfa.has_accept_token(state)};

    if (!accept)
    {
        return "non-accepting";
    }

    switch (static_cast<Token>(accept->id()))
    {
    case Token::Whitespace:
        return "accepts whitespace";
    case Token::Newline:
        return "accepts newline";
    case Token::BlockComment:
        return "accepts block comment";
    case Token::LineComment:
        return "accepts line comment";
    case Token::String:
        return "accepts string";
    default:
        return "accepts other";
    }
}

// Joins the descriptions so the report's characterization of the contexts is asserted, not just their count.
std::string describe_all(const munch::dfa::Dfa& dfa, const std::set<State_t>& states)
{
    std::set<std::string> parts;

    for (const auto state : states)
    {
        parts.insert(describe(dfa, state));
    }

    std::string joined;

    for (const auto& part : parts)
    {
        joined += joined.empty() ? "" : ", ";
        joined += part;
    }

    return joined;
}

struct Measurement
{
    std::set<State_t> realized;
    std::size_t line_starts{0};
    std::size_t boundaries{0};
    std::size_t lookahead_crossings{0};
    std::size_t documents{0};
};

/**
 * @brief Longest-match scan recording the state at every offset, plus whether a token ever read past a line end
 *        after it had already accepted.
 */
void measure(const munch::dfa::Dfa& dfa, const std::string& text, Measurement& into)
{
    std::vector<std::optional<State_t>> context(text.size() + 1);

    std::size_t offset{0};

    while (offset < text.size())
    {
        auto state{dfa.init_state()};

        std::optional<std::size_t> accept_at;

        std::size_t consumed{0};

        std::vector<State_t> trajectory;

        while (offset + consumed < text.size())
        {
            const auto next{dfa.advance(state, text[offset + consumed])};

            if (!next)
            {
                break;
            }

            state = *next;

            ++consumed;

            trajectory.push_back(state);

            if (dfa.has_accept_token(state))
            {
                accept_at = consumed;
            }
        }

        if (!accept_at || *accept_at == 0)
        {
            return; // input this token set does not accept; it carries no guarantee and is not counted
        }

        for (std::size_t index{*accept_at}; index < consumed; ++index)
        {
            if (text[offset + index] == '\n')
            {
                ++into.lookahead_crossings;

                break;
            }
        }

        for (std::size_t index{1}; index < *accept_at; ++index)
        {
            context[offset + index] = trajectory[index - 1];
        }

        offset += *accept_at;
    }

    ++into.documents;

    for (std::size_t at{1}; at < text.size(); ++at)
    {
        if (text[at - 1] != '\n')
        {
            continue;
        }

        ++into.line_starts;

        if (context[at])
        {
            into.realized.insert(*context[at]);
        }
        else
        {
            ++into.boundaries;
        }
    }
}

// Deterministic, so the corpus-dependent counts below are reproducible rather than incidental.
std::vector<std::string> corpus(const std::size_t count)
{
    const std::vector<std::string> lines{
            "int a = 1;\n",
            "x = 2;\n",
            "/* a comment\nspanning lines */\n",
            "/* short */\n",
            "\"a string\"\n",
            "// line comment\n",
            "void f() {\n",
            "}\n",
            "\n",
            "  indented = 3;\n",
            "/*\n*\n*/\n",
            "y = \"has // slashes\";\n"};

    auto seed{20260801U};

    const auto next{[&seed](const unsigned bound) {
        seed = seed * 1664525U + 1013904223U;

        return (seed >> 8U) % bound;
    }};

    std::vector<std::string> documents;

    for (std::size_t index{0}; index < count; ++index)
    {
        std::string text;

        for (auto rows{6U + next(20)}; rows > 0; --rows)
        {
            text += lines[next(static_cast<unsigned>(lines.size()))];
        }

        documents.push_back(text);
    }

    return documents;
}

} // namespace

int main()
{
    int failures{0};

    const auto check{[&failures](const std::string& what, const auto actual, const auto expected) {
        const auto agrees{actual == expected};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << what << ": " << actual << '\n';

        if (!agrees)
        {
            std::cout << "         report says " << expected << '\n';

            failures += 1;
        }
    }};

    // The published row: cumulative on the split-friendly tokenization, exactly as the applicability table's row of
    // the same name is. Measuring the conventional variant instead answers a different question.
    Exposed builder;

    c_like(builder, true);
    builder.add_token(string_literal(), Token::String, 2);
    builder.add_token(line_comment(), Token::LineComment, 1);
    builder.add_token(block_comment(), Token::BlockComment, 1);

    const auto dfa{builder.dfa()};

    const auto structural{structural_line_contexts(dfa)};

    std::cout << "split-friendly C-like with block comments\n";

    check("states in the automaton", all_states(dfa).size(), std::size_t{14});

    check("mid-token contexts possible at a line start, over every input", structural.size(), std::size_t{1});

    check("what those contexts are", describe_all(dfa, structural), std::string{"non-accepting"});

    for (const auto state : structural)
    {
        std::cout << "         context: " << describe(dfa, state) << '\n';
    }

    Measurement measured;

    for (const auto& text : corpus(400))
    {
        measure(dfa, text, measured);
    }

    check("mid-token contexts a 400 document corpus realizes", measured.realized.size(), std::size_t{1});

    auto contained{true};

    for (const auto state : measured.realized)
    {
        contained = contained && structural.contains(state);
    }

    check("every realized context lies inside the structural bound", contained, true);

    check("tokens accepting before a line end and then reading past it", measured.lookahead_crossings, std::size_t{0});

    // Corpus-dependent, so reported rather than published as a property of the grammar.
    std::cout << "         corpus: " << measured.documents << " documents, " << measured.line_starts << " line starts, "
              << measured.boundaries << " already token boundaries ("
              << (measured.line_starts == 0 ? 0 : 100 * measured.boundaries / measured.line_starts) << "%)\n";

    // The conventional variant is measured too, because it is the grammar a hand-written C lexer actually has and
    // its context count is the one a reader is likely to care about.
    {
        Exposed conventional;

        c_like(conventional, false);
        conventional.add_token(string_literal(), Token::String, 2);
        conventional.add_token(line_comment(), Token::LineComment, 1);
        conventional.add_token(block_comment(), Token::BlockComment, 1);

        const auto other{conventional.dfa()};

        const auto other_contexts{structural_line_contexts(other)};

        std::cout << "\nconventional C-like with block comments\n";

        check("states in the automaton", all_states(other).size(), std::size_t{13});

        check("mid-token contexts possible at a line start, over every input", other_contexts.size(), std::size_t{2});

        check("what those contexts are", describe_all(other, other_contexts),
              std::string{"accepts whitespace, non-accepting"});

        for (const auto state : other_contexts)
        {
            std::cout << "         context: " << describe(other, state) << '\n';
        }
    }

    std::cout
            << (failures == 0 ? "\nthe composition figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? 0 : 1;
}

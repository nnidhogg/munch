/*
 * Asserts the composition measurement of paper/split-points/split-points.tex, which compares the certificate against
 * reconstructing the scan state at each line start by a parallel prefix scan.
 *
 * The number that decides whether such a scan is cheap is how many distinct states can occur at a line start, since
 * that is the domain the per-line transition functions range over. Two things are computed here, and the report should
 * quote the first:
 *
 *   1. A structural bound, read off the automaton with no corpus at all. A line start is either a token boundary or
 *      sits mid-token having just consumed a newline, so the mid-token contexts are contained in the targets of the
 *      newline transitions out of live reachable states that can still continue. This holds for every input.
 *   2. The set a corpus actually realizes, which must be a subset of the bound. Corpus-dependent counts are reported
 *      but deliberately not published as headline figures: they say more about the corpus than about the grammar.
 *
 * The third assertion is the hazard a line-local scheme quietly needs: that maximal munch never accepts before a line
 * end and then keeps reading past it, which would make a per-line function depend on the next line's bytes.
 */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/dfa/dfa.hpp"
#include "random.hpp"

namespace
{
using namespace figures;

/**
 * @brief The documents of the corpus the realized contexts are counted over.
 */
constexpr std::size_t corpus_documents{400};

/**
 * @brief The fewest lines in a corpus document.
 */
constexpr unsigned fewest_lines{6};

/**
 * @brief How many line counts a corpus document's length is drawn from, so a document holds six to twenty-five lines.
 */
constexpr unsigned line_spread{20};

/**
 * @brief An automaton state.
 */
using State_t = munch::dfa::Dfa::State_t;

/**
 * @brief What a corpus realizes at its line starts.
 */
struct Measurement
{
    /**
     * @brief The mid-token states the line starts occupy.
     */
    std::set<State_t> realized{};

    /**
     * @brief The line starts.
     */
    std::size_t line_starts{0};

    /**
     * @brief The line starts that are token boundaries.
     */
    std::size_t boundaries{0};

    /**
     * @brief The tokens that accepted before a line end and then read past it.
     */
    std::size_t lookahead_crossings{0};

    /**
     * @brief The documents the token set accepted.
     */
    std::size_t documents{0};
};

/**
 * @brief One document's longest-match scan: the mid-token state at every offset, and the tokens that read past a line
 *        end after they had already accepted.
 */
struct Document_scan
{
    /**
     * @brief Per offset, the state a scan sits in there while inside a token, std::nullopt at a token boundary.
     */
    std::vector<std::optional<State_t>> contexts{};

    /**
     * @brief The tokens that accepted before a line end and then read past it, up to where the scan stopped.
     */
    std::size_t lookahead_crossings{0};

    /**
     * @brief Whether the token set accepted the whole document.
     */
    bool accepted{false};
};

/**
 * @brief Collects every state the automaton names: the initial state, every transition's ends and every accepting
 *        state.
 * @param dfa The automaton.
 * @return The states.
 */
std::set<State_t> all_states(const munch::dfa::Dfa& dfa)
{
    std::set<State_t> states{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        states.insert(from);

        states.insert(to);
    }

    for (const auto& [state, token] : dfa.accept_states())
    {
        states.insert(state);
    }

    return states;
}

/**
 * @brief Collects the states reachable from the initial state.
 * @param dfa The automaton.
 * @return The states.
 */
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
            const auto& [from, label]{key};

            if (from != state)
            {
                continue;
            }

            const auto [where, inserted]{reachable.insert(to)};

            if (inserted)
            {
                work.push_back(to);
            }
        }
    }

    return reachable;
}

/**
 * @brief Returns whether a state has an outgoing transition.
 * @param dfa The automaton.
 * @param state The state.
 * @return True when some byte leads out of it.
 */
bool can_continue(const munch::dfa::Dfa& dfa, const State_t state)
{
    const auto leads_out{
            [&dfa, state](const int value) { return dfa.advance(state, static_cast<char>(value)).has_value(); }};

    return std::ranges::any_of(std::views::iota(0, byte_values), leads_out);
}

/**
 * @brief Bounds the states a scan can occupy at a line start while still inside a token, over every possible input.
 *
 * A line start follows a newline. If that newline was consumed as part of a token, the scan sits in the target of a
 * newline transition, and the token continues only if that target has some outgoing transition. Anything else makes the
 * line start a token boundary, which needs no state at all.
 * @param dfa The automaton.
 * @return The states.
 */
std::set<State_t> structural_line_contexts(const munch::dfa::Dfa& dfa)
{
    const auto reachable{reachable_states(dfa)};

    std::set<State_t> contexts{};

    for (const auto state : reachable)
    {
        if (const auto to{dfa.advance(state, '\n')}; to && can_continue(dfa, *to))
        {
            contexts.insert(*to);
        }
    }

    return contexts;
}

/**
 * @brief Describes a state by what it accepts, so the report can characterize the contexts rather than count them
 *        blindly.
 * @param dfa The automaton.
 * @param state The state.
 * @return `non-accepting`, or `accepts` followed by the token kind.
 */
std::string describe(const munch::dfa::Dfa& dfa, const State_t state)
{
    const auto accept{dfa.has_accept_token(state)};

    if (!accept)
    {
        return "non-accepting";
    }

    switch (static_cast<Token>(accept->id()))
    {
    case Token::whitespace:
        return "accepts whitespace";
    case Token::newline:
        return "accepts newline";
    case Token::block_comment:
        return "accepts block comment";
    case Token::line_comment:
        return "accepts line comment";
    case Token::string:
        return "accepts string";
    default:
        return "accepts other";
    }
}

/**
 * @brief Joins the states' distinct descriptions, so the report's characterization of the contexts is asserted, not
 *        just their count.
 * @param dfa The automaton.
 * @param states The states.
 * @return The descriptions, sorted and separated by commas.
 */
std::string describe_all(const munch::dfa::Dfa& dfa, const std::set<State_t>& states)
{
    std::set<std::string> parts{};

    for (const auto state : states)
    {
        parts.insert(describe(dfa, state));
    }

    return joined(parts, ", ");
}

/**
 * @brief Scans a document with longest match, recording the state at every offset, plus whether a token ever read past
 *        a line end after it had already accepted.
 * @param dfa The automaton.
 * @param text The document.
 * @return The contexts and crossings, the crossings counted up to where a rejected document stopped.
 */
Document_scan scan_contexts(const munch::dfa::Dfa& dfa, const std::string& text)
{
    Document_scan scanned{.contexts = std::vector<std::optional<State_t>>(text.size() + 1)};

    std::size_t offset{0};

    while (offset < text.size())
    {
        auto state{dfa.init_state()};

        std::optional<std::size_t> accept_at{};

        std::size_t consumed{0};

        std::vector<State_t> trajectory{};

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
            return scanned;
        }

        const auto read_past_accept{std::string_view{text}.substr(offset + *accept_at, consumed - *accept_at)};

        if (read_past_accept.contains('\n'))
        {
            ++scanned.lookahead_crossings;
        }

        for (std::size_t index{1}; index < *accept_at; ++index)
        {
            scanned.contexts[offset + index] = trajectory[index - 1];
        }

        offset += *accept_at;
    }

    scanned.accepted = true;

    return scanned;
}

/**
 * @brief Measures what a corpus realizes at its line starts.
 *
 * A document this token set does not accept carries no guarantee and is not counted, beyond the crossings its scan met
 * before it stopped.
 * @param dfa The automaton.
 * @param documents The corpus.
 * @return The realized contexts and the counts.
 */
Measurement measure(const munch::dfa::Dfa& dfa, const std::vector<std::string>& documents)
{
    Measurement measured{};

    for (const auto& text : documents)
    {
        const auto [contexts, crossings, accepted]{scan_contexts(dfa, text)};

        measured.lookahead_crossings += crossings;

        if (!accepted)
        {
            continue;
        }

        ++measured.documents;

        for (std::size_t at{1}; at < text.size(); ++at)
        {
            if (text[at - 1] != '\n')
            {
                continue;
            }

            ++measured.line_starts;

            if (contexts[at])
            {
                measured.realized.insert(*contexts[at]);
            }
            else
            {
                ++measured.boundaries;
            }
        }
    }

    return measured;
}

/**
 * @brief Draws a deterministic corpus, so the corpus-dependent counts are reproducible rather than incidental:
 *        documents of six to twenty-five lines drawn from a fixed 32-bit stream.
 * @param count The documents.
 * @return The documents.
 */
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

    return drawn(lines, count, fewest_lines, line_spread);
}

} // namespace

/**
 * @brief Asserts the composition figures of both C-like variants with block comments, and prints the verdict.
 * @return EXIT_SUCCESS when every figure agrees with the report, EXIT_FAILURE otherwise.
 */
int main()
{
    Figure_check check{};

    // The states, contexts and kinds are the report's figures for the variant.
    const auto check_variant{[&check](
                                     const std::string_view heading, const bool split_friendly,
                                     const std::size_t states, const std::size_t contexts,
                                     const std::string_view kinds) {
        Builder_dbg builder{};

        c_like_with_block_comments(builder, split_friendly);

        auto dfa{builder.dfa()};

        auto structural{structural_line_contexts(dfa)};

        std::cout << heading;

        const auto state_count{all_states(dfa).size()};

        check("states in the automaton", state_count, states);

        check("mid-token contexts possible at a line start, over every input", structural.size(), contexts);

        const auto context_kinds{describe_all(dfa, structural)};

        check("what those contexts are", context_kinds, std::string{kinds});

        for (const auto state : structural)
        {
            std::cout << "         context: " << describe(dfa, state) << '\n';
        }

        return std::pair{std::move(dfa), std::move(structural)};
    }};

    // The published row: the split-friendly tokenization, cumulative exactly as the applicability table's row of the
    // same name is.
    const auto [dfa, structural]{
            check_variant("split-friendly C-like with block comments\n", true, 14, 1, "non-accepting")};

    const auto documents_measured{corpus(corpus_documents)};

    const auto [realized, line_starts, boundaries, lookahead_crossings, documents]{measure(dfa, documents_measured)};

    const auto realized_label{std::format("mid-token contexts a {} document corpus realizes", corpus_documents)};

    check(realized_label, realized.size(), std::size_t{1});

    const auto bounded{[&structural](const State_t state) { return structural.contains(state); }};

    const auto contained{std::ranges::all_of(realized, bounded)};

    check("every realized context lies inside the structural bound", contained, true);

    check("tokens accepting before a line end and then reading past it", lookahead_crossings, std::size_t{0});

    // Corpus-dependent, so reported rather than published as a property of the grammar.
    const auto boundary_percent{line_starts == 0 ? 0 : 100 * boundaries / line_starts};

    std::cout << "         corpus: " << documents << " documents, " << line_starts << " line starts, " << boundaries
              << " already token boundaries (" << boundary_percent << "%)\n";

    // The conventional variant, the grammar a hand-written C lexer has: its states and line-start contexts.
    std::ignore = check_variant(
            "\nconventional C-like with block comments\n", false, 13, 2, "accepts whitespace, non-accepting");

    const auto failures{check.failures()};

    std::cout
            << (failures == 0 ? "\nthe composition figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

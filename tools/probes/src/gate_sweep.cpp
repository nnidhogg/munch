#include "munch/tools/probes/gate_sweep.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/dfa/unroll_start.hpp"
#include "munch/regex/regex.hpp"
#include "munch/tools/probes/builder_dbg.hpp"
#include "munch/tools/probes/gate_evidence.hpp"
#include "munch/tools/probes/gate_search.hpp"
#include "munch/tools/probes/gate_totals.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace munch::tools::probes
{
namespace
{
using dfa::Dfa;

/**
 * @brief The multiplier of the sweep's 32-bit linear congruential stream.
 */
constexpr std::uint32_t lcg_multiplier{1664525U};

/**
 * @brief The increment of the sweep's 32-bit linear congruential stream.
 */
constexpr std::uint32_t lcg_increment{1013904223U};

/**
 * @brief The seed the sweep's stream starts from.
 */
constexpr std::uint32_t sweep_seed{20260803U};

/**
 * @brief The most DFA states a swept grammar may compile to; a grammar past it is skipped.
 */
constexpr std::size_t state_limit{400};

/**
 * @brief The deepest nesting of a swept token's regex.
 */
constexpr std::size_t regex_depth{3};

/**
 * @brief Returns whether a token of the grammar matches the empty string, which is whether the initial state accepts.
 * @param dfa The grammar's automaton.
 * @return True when the initial state accepts.
 */
bool nullable_grammar(const Dfa& dfa)
{
    return dfa.has_accept_token(dfa.init_state()).has_value();
}

/**
 * @brief Advances the sweep's 32-bit linear congruential stream and draws its high bits.
 *
 * The stream is the one recovery_lcg's Lcg advances, but its draws are the state's high half alone, a sequence none of
 * Lcg's draws yields, so the sweep keeps its own.
 * @param seed The stream's state, advanced.
 * @return The advanced state shifted right by 16.
 */
std::uint32_t next_draw(std::uint32_t& seed)
{
    seed = seed * lcg_multiplier + lcg_increment;

    return seed >> 16U;
}

/**
 * @brief Draws a regex over {a, b, c} from a 32-bit linear congruential stream: at depth 0, or on one draw in three,
 *        one of six atoms; otherwise a concatenation, a choice, a plus or a star of regexes one level shallower.
 * @param seed The stream's state, advanced by every draw.
 * @param depth The deepest nesting left.
 * @return The regex.
 */
regex::Regex random_regex(std::uint32_t& seed, const std::size_t depth)
{
    using namespace regex;

    // Draws the left operand before the right, so a seed yields one grammar whichever order the compiler evaluates
    // arguments in.
    const auto operands{[&seed, depth] {
        auto left{random_regex(seed, depth - 1)};

        auto right{random_regex(seed, depth - 1)};

        return std::pair{std::move(left), std::move(right)};
    }};

    if (depth == 0 || next_draw(seed) % 3U == 0)
    {
        constexpr std::array<std::string_view, 6> atoms{"a", "b", "c", "ab", "bc", "ca"};

        const auto drawn{next_draw(seed) % atoms.size()};

        return text(atoms[drawn]);
    }

    const auto kind{next_draw(seed) % 4U};

    switch (kind)
    {
    case 0:
    {
        auto [left, right]{operands()};

        return concat(std::move(left), std::move(right));
    }

    case 1:
    {
        auto [left, right]{operands()};

        return choice(std::move(left), std::move(right));
    }

    case 2:
        return plus(random_regex(seed, depth - 1));

    default:
        return kleene(random_regex(seed, depth - 1));
    }
}

/**
 * @brief Searches the shortest certified windows of a grammar certifying no byte, and adds the outcome to the sweep's
 *        counts: rescued, proved to have none, or inconclusive, and witnessed-rescued when the bounded search completes
 *        a tokenization around a certified word up to two bytes past the shortest certified length.
 * @param totals The gate's totals.
 * @param sweep The sweep's counts, added to.
 * @param dfa The grammar's positive-width automaton.
 * @param lexer The grammar's lexer.
 * @param live The automaton's trim states.
 * @param reentrant Whether a live transition re-enters the initial state.
 */
void sweep_windows(
        Gate_totals& totals, Sweep& sweep, const Dfa& dfa, const core::Lexer& lexer, const States_t& live,
        const bool reentrant)
{
    const auto [length, found, exhausted, visited]{shortest_windows(dfa, live)};

    totals.visited_total += visited;

    totals.visited_max = std::max(totals.visited_max, visited);

    if (length == 0 && exhausted)
    {
        ++sweep.proved_none;

        return;
    }

    if (length == 0)
    {
        ++sweep.inconclusive;

        return;
    }

    ++sweep.rescued;

    const auto words{certified_words_upto(dfa, live, reentrant, length + 2, kept_windows)};

    const auto witnessed{find_witness(totals, dfa, lexer, live, words)};

    if (witnessed)
    {
        ++sweep.witnessed_rescued;
    }
}

/**
 * @brief Checks the model on one grammar, decided through its positive-width equivalent, and adds what it finds to the
 *        sweep's counts: the length-one agreement, the shortest window and its witness where no byte is certified, and
 *        the backup check over every certified two-byte window, each certificate and refusal cross-checked against the
 *        shipped window decision. The counts reached before an exception stand.
 * @param totals The gate's totals.
 * @param sweep The sweep's counts, added to.
 * @param builder The grammar.
 */
void sweep_one(Gate_totals& totals, Sweep& sweep, const Builder_dbg& builder)
{
    if (nullable_grammar(builder.dfa()))
    {
        ++sweep.nullable;
    }

    const auto dfa{munch::dfa::unroll_start(builder.dfa())};

    const auto lexer{builder.build()};

    const auto live{live_states(dfa)};

    if (live.empty())
    {
        return;
    }

    ++sweep.usable;

    const auto reentrant{is_init_reentrant(dfa, live)};

    std::size_t certified_bytes{0};

    for (const auto symbol : every_byte())
    {
        const std::string one{symbol};

        const auto at{predicted(dfa, live, one, reentrant)};

        totals.cross_check(lexer, one, at);

        const auto model{at.has_value()};

        const auto shipped{lexer.is_split_point(symbol)};

        if (shipped)
        {
            ++certified_bytes;
        }

        if (model != shipped)
        {
            ++sweep.disagreements;
        }
    }

    if (certified_bytes > 0)
    {
        ++sweep.with_certificate;
    }
    else
    {
        sweep_windows(totals, sweep, dfa, lexer, live, reentrant);
    }

    std::vector<std::string> windows{};

    for (const auto& [pair, at] : certified_pairs(dfa, live, reentrant))
    {
        totals.cross_check(lexer, pair, at);

        windows.push_back(pair);
    }

    // The sweep's rewinding executions are not added to the named rows' totals.
    const auto [disagreements, exercised, tokenizable, prefixes]{backup_disagreements(dfa, lexer, live, windows)};

    sweep.disagreements += disagreements;
}

} // namespace

Sweep random_grammars(Gate_totals& totals, const std::size_t count)
{
    Sweep sweep{};

    auto seed{sweep_seed};

    for (std::size_t index{0}; index < count; ++index)
    {
        Builder_dbg builder{};

        builder.set_state_limit(state_limit);

        constexpr std::size_t fewest_tokens{2};

        constexpr std::size_t token_counts{4};

        const auto tokens{fewest_tokens + (seed >> 8U) % token_counts};

        for (std::size_t token{0}; token < tokens; ++token)
        {
            builder.add_token(random_regex(seed, regex_depth), token, 1 + token % 2);
        }

        try
        {
            sweep_one(totals, sweep, builder);
        }
        catch (const std::exception&)
        {
            // A grammar over the state limit is skipped.
        }
    }

    return sweep;
}

} // namespace munch::tools::probes

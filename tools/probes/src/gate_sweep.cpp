#include "munch/tools/probes/gate_sweep.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
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
// Implements gate_sweep.hpp: the grammar generator and the check of one grammar are private to this unit.

using dfa::Dfa;

/**
 * @brief Whether a token of the grammar matches the empty string, which is whether the initial state accepts.
 * @param dfa The grammar's automaton.
 * @return True when the initial state accepts.
 */
bool nullable_grammar(const Dfa& dfa)
{
    return dfa.has_accept_token(dfa.init_state()).has_value();
}

/**
 * @brief Draws a regex over {a, b, c} from a 32-bit linear congruential stream: at depth 0, or on one draw in three,
 *        one of six atoms; otherwise a concatenation, a choice, a plus or a star of regexes one level shallower.
 * @param seed The stream's state, advanced by every draw.
 * @param depth The deepest nesting left.
 * @return The regex.
 */
regex::Regex random_regex(unsigned& seed, const std::size_t depth)
{
    using namespace regex;

    const auto next{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    if (depth == 0 || next() % 3 == 0)
    {
        constexpr std::string_view atoms[]{"a", "b", "c", "ab", "bc", "ca"};

        return text(atoms[next() % std::size(atoms)]);
    }

    // The two operands are drawn through locals, the left one first.
    switch (next() % 4)
    {
    case 0:
    {
        auto left{random_regex(seed, depth - 1)};

        auto right{random_regex(seed, depth - 1)};

        return concat(std::move(left), std::move(right));
    }

    case 1:
    {
        auto left{random_regex(seed, depth - 1)};

        auto right{random_regex(seed, depth - 1)};

        return choice(std::move(left), std::move(right));
    }

    case 2:
        return plus(random_regex(seed, depth - 1));

    default:
        return kleene(random_regex(seed, depth - 1));
    }
}

/**
 * @brief Checks the model on one grammar, decided through its positive-width equivalent, and adds what it finds to
 *        the sweep's counts: the length-one agreement, the shortest window and its witness where no byte is certified,
 *        and the backup check over every certified two-byte window, each certificate and refusal cross-checked
 *        against the shipped window decision. The counts reached before an exception stand.
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

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const std::string one(1, static_cast<char>(symbol));

        const auto at{predicted(dfa, live, one, reentrant)};

        totals.cross_check(lexer, one, at);

        const auto model{at.has_value()};

        const auto shipped{lexer.is_split_point(static_cast<char>(symbol))};

        certified_bytes += shipped ? 1 : 0;

        sweep.disagreements += model != shipped ? 1 : 0;
    }

    sweep.with_certificate += certified_bytes > 0 ? 1 : 0;

    if (certified_bytes == 0)
    {
        const auto [length, found, exhausted, visited]{shortest_windows(dfa, live)};

        totals.visited_total += visited;

        totals.visited_max = std::max(totals.visited_max, visited);

        sweep.rescued += length > 0 ? 1 : 0;

        sweep.proved_none += length == 0 && exhausted ? 1 : 0;

        sweep.inconclusive += length == 0 && !exhausted ? 1 : 0;

        // A grammar is witnessed-rescued only when the bounded search completes a tokenization around a certified
        // word, searched up to two bytes past the shortest certified length.
        if (length > 0)
        {
            const auto words{certified_words_upto(dfa, live, reentrant, length + 2, 400)};

            sweep.witnessed_rescued += find_witness(totals, dfa, lexer, live, words) ? 1 : 0;
        }
    }

    std::vector<std::string> windows{};

    for (const auto& [pair, at] : certified_pairs(dfa, live, reentrant))
    {
        totals.cross_check(lexer, pair, at);

        windows.push_back(pair);
    }

    // The sweep's rewinding executions are not added to the named rows' totals.
    sweep.disagreements += backup_disagreements(dfa, lexer, live, windows).disagreements;
}
} // namespace

Sweep random_grammars(Gate_totals& totals, const std::size_t count)
{
    Sweep sweep{};

    unsigned seed{20260803};

    for (std::size_t index{0}; index < count; ++index)
    {
        Builder_dbg builder{};

        builder.set_state_limit(400);

        const auto tokens{2 + (seed >> 8U) % 4};

        for (std::size_t token{0}; token < tokens; ++token)
        {
            builder.add_token(random_regex(seed, 3), token, 1 + token % 2);
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

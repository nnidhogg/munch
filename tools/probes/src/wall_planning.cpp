#include "munch/tools/probes/wall_planning.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/probes/lcg64.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_table.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements wall_planning.hpp: the generated corpora and the splice are private to this unit.

/**
 * @brief One deterministic tokenizable corpus of at least 1,200 bytes from one Lcg64 seeded 0x5eed0000 plus the trial
 *        times 2654435761: bare runs of one to eight letters from a to f and strings of up to eleven content bytes,
 *        each content byte a letter or, one draw in eight, the other string type's delimiter, or a space when the
 *        corpus has one string type.
 * @param trial The trial, which seeds the stream.
 * @param ticks Whether half the strings are backtick strings, each type's delimiter mixed into the other's content.
 * @return The corpus.
 */
std::string corpus(const std::size_t trial, const bool ticks)
{
    Lcg64 lcg{0x5eed0000ULL + trial * 2654435761ULL};

    std::string text{};

    while (text.size() < 1200)
    {
        if (lcg.next(10) < 6)
        {
            const auto length{1 + lcg.next(8)};

            for (std::size_t at{0}; at < length; ++at)
            {
                text += static_cast<char>('a' + lcg.next(6));
            }
        }
        else
        {
            const auto tick{ticks && lcg.next(2) == 0};

            const auto delimiter{tick ? '`' : '"'};

            text += delimiter;

            const auto length{lcg.next(12)};

            for (std::size_t at{0}; at < length; ++at)
            {
                const auto roll{lcg.next(8)};

                if (roll == 0)
                {
                    text += tick ? '"' : (ticks ? '`' : ' ');
                }
                else
                {
                    text += static_cast<char>('a' + lcg.next(6));
                }
            }

            text += delimiter;
        }
    }

    return text;
}

/**
 * @brief Scans every chunk between the cuts alone and reassembles the boundaries in the input's coordinates.
 * @param table The table.
 * @param text The input.
 * @param cuts The cuts, strictly ascending, each inside the input.
 * @return The spliced token starts followed by the input's size, std::nullopt when a chunk does not tokenize
 *         completely.
 */
std::optional<std::vector<std::size_t>> spliced_boundaries(
        const Table& table, const std::string_view text, const std::vector<std::size_t>& cuts)
{
    std::vector<std::size_t> edges{0};

    edges.insert(edges.end(), cuts.begin(), cuts.end());

    edges.push_back(text.size());

    std::vector<std::size_t> spliced{};

    for (std::size_t piece{0}; piece + 1 < edges.size(); ++piece)
    {
        const auto local{serial_boundaries(table, text.substr(edges[piece], edges[piece + 1] - edges[piece]))};

        if (!local)
        {
            return std::nullopt;
        }

        for (std::size_t at{0}; at + 1 < local->size(); ++at)
        {
            spliced.push_back(edges[piece] + (*local)[at]);
        }
    }

    spliced.push_back(text.size());

    return spliced;
}

} // namespace

std::optional<std::size_t> window_walk(
        const Table& table, const Carry& carry, const bool reentrant, const std::string_view window, const int flavor)
{
    if (window.empty() || table.accept[table.init] != 0)
    {
        return std::nullopt;
    }

    constexpr auto before{std::numeric_limits<std::size_t>::max()};

    std::set<std::pair<std::size_t, std::size_t>> cloud{};

    for (const auto state : live_states(table))
    {
        if (flavor < 0 || carry.state_flavor[state] == flavor)
        {
            cloud.emplace(state, before);
        }
    }

    if (cloud.empty())
    {
        return std::nullopt;
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        auto accepting{false};

        for (const auto& [state, origin] : cloud)
        {
            accepting = accepting || table.accept[state] != 0;
        }

        std::set<std::pair<std::size_t, std::size_t>> next{};

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{table.next[state][byte]}; to != kDead)
            {
                const auto begins{state == table.init && !reentrant};

                next.emplace(static_cast<std::size_t>(to), begins ? at : origin);
            }
        }

        if (accepting)
        {
            if (const auto to{table.next[table.init][byte]}; to != kDead)
            {
                next.emplace(static_cast<std::size_t>(to), at);
            }
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    const auto origin{cloud.begin()->second};

    for (const auto& [state, at] : cloud)
    {
        if (at != origin)
        {
            return std::nullopt;
        }
    }

    return origin == before ? std::nullopt : std::optional{origin};
}

std::size_t check_theorem(const Table& table, const Carry& carry, const std::string& alphabet)
{
    Lcg64 lcg{0x5eed5eed5eed5eedULL};

    std::size_t checked{0};

    for (std::size_t trial{0}; trial < 200; ++trial)
    {
        std::size_t state{table.init};

        auto predicted{carry.seed};

        for (std::size_t at{0}; at < 400; ++at)
        {
            const auto byte{static_cast<unsigned char>(alphabet[lcg.next(alphabet.size())])};

            auto to{table.next[state][byte]};

            if (to == kDead)
            {
                if (table.accept[state] == 0)
                {
                    break;
                }

                to = table.next[table.init][byte];

                if (to == kDead)
                {
                    break;
                }
            }

            state = static_cast<std::size_t>(to);

            predicted = carry.sigma[byte][static_cast<std::size_t>(predicted)];

            if (carry.state_flavor[state] != predicted)
            {
                return checked;
            }

            ++checked;
        }
    }

    return checked;
}

Plan plan(
        const Table& table, const Carry& carry, const bool reentrant, const std::string_view input,
        const std::size_t chunks, const bool flip)
{
    std::vector<int> flavor_at(input.size() + 1);

    flavor_at[0] = carry.seed;

    for (std::size_t at{0}; at < input.size(); ++at)
    {
        flavor_at[at + 1] = carry.sigma[static_cast<unsigned char>(input[at])][static_cast<std::size_t>(flavor_at[at])];
    }

    const auto flavors{static_cast<int>(carry.group.begin()->size())};

    Plan result{};

    std::size_t last{0};

    for (std::size_t target{1}; target < chunks; ++target)
    {
        const auto aim{target * input.size() / chunks};

        auto cut{std::optional<std::size_t>{}};

        for (auto at{std::max(aim, last + 1)}; at + 2 <= input.size() && !cut; ++at)
        {
            for (std::size_t length{2}; length <= 4 && at + length <= input.size() && !cut; ++length)
            {
                const auto window{input.substr(at, length)};

                if (window_walk(table, carry, reentrant, window, -1))
                {
                    ++result.unconditional_certificates;
                }

                const auto asked{flip ? (flavor_at[at] + 1) % flavors : flavor_at[at]};

                if (const auto origin{window_walk(table, carry, reentrant, window, asked)};
                    origin && at + *origin > last && at + *origin < input.size())
                {
                    cut = at + *origin;
                }
            }
        }

        if (cut)
        {
            result.cuts.push_back(*cut);

            last = *cut;
        }
    }

    return result;
}

Tally run(const Table& table, const Carry& carry, const bool ticks)
{
    const auto reentrant{is_init_reentrant(table)};

    Tally tally{};

    for (std::size_t trial{0}; trial < 30; ++trial)
    {
        const auto text{corpus(trial, ticks)};

        const auto serial{serial_boundaries(table, text)};

        if (!serial)
        {
            ++tally.splice_mismatches;

            continue;
        }

        const std::set<std::size_t> boundaries{serial->begin(), serial->end()};

        const auto planned{plan(table, carry, reentrant, text, 8, false)};

        tally.cuts += planned.cuts.size();

        tally.unconditional += planned.unconditional_certificates;

        for (const auto cut : planned.cuts)
        {
            tally.off_boundary += boundaries.contains(cut) ? 0 : 1;
        }

        if (const auto spliced{spliced_boundaries(table, text, planned.cuts)}; !spliced || *spliced != *serial)
        {
            ++tally.splice_mismatches;
        }

        const auto wrong{plan(table, carry, reentrant, text, 8, true)};

        for (const auto cut : wrong.cuts)
        {
            tally.teeth_bad += boundaries.contains(cut) ? 0 : 1;
        }
    }

    return tally;
}

} // namespace munch::tools::probes

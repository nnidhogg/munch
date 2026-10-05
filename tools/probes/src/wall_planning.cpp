#include "munch/tools/probes/wall_planning.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/probes/lcg64.hpp"
#include "munch/tools/probes/wall_carry.hpp"
#include "munch/tools/probes/wall_table.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The size a generated corpus grows to at least.
 */
constexpr std::size_t corpus_floor{1200};

/**
 * @brief The random walks the carry theorem is checked over.
 */
constexpr std::size_t theorem_trials{200};

/**
 * @brief The bytes each random walk of the carry theorem runs for at most.
 */
constexpr std::size_t theorem_steps{400};

/**
 * @brief The generated corpora of the planning campaign.
 */
constexpr std::size_t planning_trials{30};

/**
 * @brief The chunks every corpus of the planning campaign is planned in.
 */
constexpr std::size_t planning_chunks{8};

/**
 * @brief The shortest window the planner tries.
 */
constexpr std::size_t shortest_window{2};

/**
 * @brief The longest window the planner tries.
 */
constexpr std::size_t longest_window{4};

/**
 * @brief Returns one deterministic tokenizable corpus of at least 1,200 bytes from one Lcg64 seeded 0x5EED0000 plus the
 *        trial times 2654435761: bare runs of one to eight letters from a to f and strings of up to eleven content
 *        bytes, each content byte a letter or, one draw in eight, the other string type's delimiter, or a space when
 *        the corpus has one string type.
 * @param trial The trial, which seeds the stream.
 * @param ticks Whether half the strings are backtick strings, each type's delimiter mixed into the other's content.
 * @return The corpus.
 */
std::string corpus(const std::size_t trial, const bool ticks)
{
    Lcg64 lcg{0x5EED0000ULL + trial * 2654435761ULL};

    std::string text{};

    const auto letter{[&lcg] { return static_cast<char>('a' + lcg.next(6)); }};

    const auto append_run{[&lcg, &text, &letter] {
        const auto length{1 + lcg.next(8)};

        for (std::size_t at{0}; at < length; ++at)
        {
            text += letter();
        }
    }};

    const auto append_string{[&lcg, &text, &letter, ticks] {
        const auto tick{ticks && lcg.next(2) == 0};

        const auto delimiter{tick ? '`' : '"'};

        text += delimiter;

        const auto stray_of{[tick, ticks] {
            if (tick)
            {
                return '"';
            }

            return ticks ? '`' : ' ';
        }};

        const auto stray{stray_of()};

        const auto length{lcg.next(12)};

        for (std::size_t at{0}; at < length; ++at)
        {
            const auto roll{lcg.next(8)};

            if (roll == 0)
            {
                text += stray;

                continue;
            }

            text += letter();
        }

        text += delimiter;
    }};

    while (text.size() < corpus_floor)
    {
        if (lcg.next(10) < 6)
        {
            append_run();

            continue;
        }

        append_string();
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

    for (const auto& [begin, end] : edges | std::views::pairwise)
    {
        const auto chunk{text.substr(begin, end - begin)};

        const auto local{serial_boundaries(table, chunk)};

        if (!local)
        {
            return std::nullopt;
        }

        // The last entry is the chunk's size, not a token start.
        const auto starts{*local | std::views::take(local->size() - 1)};

        for (const auto start : starts)
        {
            spliced.push_back(begin + start);
        }
    }

    spliced.push_back(text.size());

    return spliced;
}

} // namespace

std::optional<std::size_t> window_walk(
        const Table& table, const Carry& carry, const bool reentrant, const std::string_view window, const int flavor)
{
    if (window.empty() || table.accept[table.init] == Flag::on)
    {
        return std::nullopt;
    }

    std::set<std::pair<std::size_t, std::size_t>> cloud{};

    for (const auto state : live_states(table))
    {
        if (flavor < 0 || carry.state_flavor[state] == flavor)
        {
            cloud.emplace(state, before_window);
        }
    }

    if (cloud.empty())
    {
        return std::nullopt;
    }

    const auto is_accepting{[&table](const std::size_t state) { return table.accept[state] == Flag::on; }};

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(window[at])};

        const auto accepting{std::ranges::any_of(cloud | std::views::keys, is_accepting)};

        std::set<std::pair<std::size_t, std::size_t>> next{};

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{table.next[state][byte]}; to != dead)
            {
                const auto begins{state == table.init && !reentrant};

                next.emplace(static_cast<std::size_t>(to), begins ? at : origin);
            }
        }

        if (const auto to{table.next[table.init][byte]}; accepting && to != dead)
        {
            next.emplace(static_cast<std::size_t>(to), at);
        }

        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    const auto& [first_state, origin]{*cloud.begin()};

    const auto is_first_origin{[origin](const std::size_t at) { return at == origin; }};

    const auto agree{std::ranges::all_of(cloud | std::views::values, is_first_origin)};

    if (!agree)
    {
        return std::nullopt;
    }

    if (origin == before_window)
    {
        return std::nullopt;
    }

    return origin;
}

std::size_t check_theorem(const Table& table, const Carry& carry, const std::string& alphabet)
{
    Lcg64 lcg{0x5EED5EED5EED5EEDULL};

    std::size_t checked{0};

    for (std::size_t trial{0}; trial < theorem_trials; ++trial)
    {
        std::size_t state{table.init};

        auto predicted{carry.seed};

        for (std::size_t at{0}; at < theorem_steps; ++at)
        {
            const auto drawn{lcg.next(alphabet.size())};

            const auto byte{static_cast<unsigned char>(alphabet[drawn])};

            // A dead move ends the trial unless the state accepts, and then the token restarts from the initial state.
            const auto to{scan_step(table, state, byte)};

            if (!to)
            {
                break;
            }

            state = *to;

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
        const auto byte{static_cast<unsigned char>(input[at])};

        const auto flavor{static_cast<std::size_t>(flavor_at[at])};

        flavor_at[at + 1] = carry.sigma[byte][flavor];
    }

    const auto flavors{static_cast<int>(carry.width)};

    Plan result{};

    const auto first_cut{[&](const std::size_t from, const std::size_t last) -> std::optional<std::size_t> {
        for (auto at{from}; at + shortest_window <= input.size(); ++at)
        {
            const auto asked{flip ? (flavor_at[at] + 1) % flavors : flavor_at[at]};

            for (auto length{shortest_window}; length <= longest_window && at + length <= input.size(); ++length)
            {
                const auto window{input.substr(at, length)};

                if (window_walk(table, carry, reentrant, window, -1))
                {
                    ++result.unconditional_certificates;
                }

                const auto origin{window_walk(table, carry, reentrant, window, asked)};

                if (origin && at + *origin > last && at + *origin < input.size())
                {
                    return at + *origin;
                }
            }
        }

        return std::nullopt;
    }};

    std::size_t last{0};

    for (std::size_t target{1}; target < chunks; ++target)
    {
        const auto aim{target * input.size() / chunks};

        const auto cut{first_cut(std::max(aim, last + 1), last)};

        if (!cut)
        {
            continue;
        }

        result.cuts.push_back(*cut);

        last = *cut;
    }

    return result;
}

Planning_tally run_planning(const Table& table, const Carry& carry, const bool ticks)
{
    const auto reentrant{is_init_reentrant(table)};

    Planning_tally tally{};

    for (std::size_t trial{0}; trial < planning_trials; ++trial)
    {
        const auto text{corpus(trial, ticks)};

        const auto serial{serial_boundaries(table, text)};

        if (!serial)
        {
            ++tally.splice_mismatches;

            continue;
        }

        const std::set<std::size_t> boundaries{serial->begin(), serial->end()};

        const auto is_off_boundary{[&boundaries](const std::size_t cut) { return !boundaries.contains(cut); }};

        constexpr auto right_flavors{false};

        const auto [cuts, unconditional_certificates]{
                plan(table, carry, reentrant, text, planning_chunks, right_flavors)};

        tally.cuts += cuts.size();

        tally.unconditional += unconditional_certificates;

        tally.off_boundary += static_cast<std::size_t>(std::ranges::count_if(cuts, is_off_boundary));

        if (const auto spliced{spliced_boundaries(table, text, cuts)}; !spliced || *spliced != *serial)
        {
            ++tally.splice_mismatches;
        }

        constexpr auto wrong_flavors{true};

        const auto [wrong_cuts, wrong_unconditional]{
                plan(table, carry, reentrant, text, planning_chunks, wrong_flavors)};

        tally.wrong_flavor_off_boundary += static_cast<std::size_t>(std::ranges::count_if(wrong_cuts, is_off_boundary));
    }

    return tally;
}

} // namespace munch::tools::probes

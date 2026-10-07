#include "munch/dfa/simulator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <experimental/mdspan>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "munch/dfa/unroll_start.hpp"

namespace munch::dfa
{
namespace
{
/**
 * @brief Two-dimensional row-major view over a flat table: the transition table as `(class, state)`, the core matcher
 *        as `(matched, symbol)`.
 *
 * The transition table's rows are per class rather than per state, so the row offset of a lookup depends only on the
 * input character, which is known before the state it is consumed in: the offset computation stays off the
 * state-to-state dependency chain that limits how fast the run() loop can advance.
 * @tparam Entry The viewed entry type, const-qualified for reading.
 */
template <typename Entry>
using Table_view_t = std::mdspan<Entry, std::dextents<std::size_t, 2>>;

/**
 * @brief Closes a set of seed states backward over a predecessor index: every state with a path into a seed.
 * @tparam Entry The predecessor index's state type.
 * @tparam Seed The seed predicate's type.
 * @param predecessors Per state, the states with a transition into it.
 * @param is_seed Whether a state is a seed.
 * @return Per state, whether it is a seed or some seed is reachable from it.
 */
template <typename Entry, typename Seed>
std::vector<bool> closed_backward(const std::vector<std::vector<Entry>>& predecessors, const Seed& is_seed)
{
    const auto states{predecessors.size()};

    std::vector<bool> closed(states, false);

    std::vector<std::size_t> pending{};

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_seed(state))
        {
            continue;
        }

        closed[state] = true;

        pending.push_back(state);
    }

    while (!pending.empty())
    {
        const auto state{pending.back()};

        pending.pop_back();

        for (const auto from : predecessors[state])
        {
            if (closed[from])
            {
                continue;
            }

            closed[from] = true;

            pending.push_back(from);
        }
    }

    return closed;
}

} // namespace

Simulator::Simulator(const Dfa& dfa) : Simulator{dfa, {}}
{}

Simulator::Simulator(const Dfa& dfa, const std::span<const std::size_t> ignored) : Simulator{dfa, ignored, {}}
{}

Simulator::Simulator(
        const Dfa& dfa, const std::span<const std::size_t> ignored,
        const std::span<const std::pair<std::size_t, std::uint64_t>> payloads)
    : Simulator{dfa, unrolled_start(dfa), ignored, payloads}
{}

bool Simulator::nullable() const noexcept
{
    return empty_state_ != no_state_;
}

std::string_view Simulator::mandatory_core() const noexcept
{
    return mandatory_core_;
}

bool Simulator::init_reentrant() const noexcept
{
    return init_reentrant_;
}

std::optional<std::size_t> Simulator::step(const std::size_t state, const unsigned char symbol) const noexcept
{
    const auto to{entry(symbol, state)};

    return to == no_state_ ? std::nullopt : std::optional<std::size_t>{to};
}

std::vector<std::vector<unsigned char>> Simulator::symbol_classes() const
{
    const auto states{state_count()};

    std::vector<std::vector<unsigned char>> classes{};

    for (std::size_t value{0}; value < symbol_count; ++value)
    {
        const auto symbol_class{row_offsets_[value] / states};

        if (symbol_class == classes.size())
        {
            classes.emplace_back();
        }

        classes[symbol_class].push_back(static_cast<unsigned char>(value));
    }

    return classes;
}

bool Simulator::Death_words::has_chain(const std::size_t state) const noexcept
{
    return depth[state] != no_death_word_ && depth[state] >= 2U;
}

Simulator::Simulator(
        const Dfa& dfa, const std::optional<Dfa>& unrolled, const std::span<const std::size_t> ignored,
        const std::span<const std::pair<std::size_t, std::uint64_t>> payloads)
    : init_state_{unrolled ? unrolled->init_state() : dfa.init_state()}
    , empty_state_{unrolled ? static_cast<Entry_t>(dfa.init_state()) : no_state_}
{
    const Dfa& compiled{unrolled ? *unrolled : dfa};

    // The fresh start is one more state, which may be the one the given DFA stayed under the sentinel by.
    const auto states{compiled.state_count()};

    require_indexable(states);

    const auto classes{classify(compiled)};

    const auto class_count{static_cast<std::size_t>(std::ranges::max(classes)) + 1U};

    // The table is class_count rows of states entries; on a 32-bit size_t the product can wrap where the per-state
    // vectors still allocate, so the product is checked before the table is sized from it.
    if (table_size_overflows(states, class_count, std::numeric_limits<std::size_t>::max()))
    {
        throw std::runtime_error{"DFA transition table size overflows std::size_t"};
    }

    index_rows(classes, states);

    fill_accepts(compiled, payloads);

    fill_transitions(compiled, classes, class_count);

    const auto reverse{predecessors()};

    const auto co_accessible{co_accessible_states(reverse)};

    const auto reachable{reachable_states()};

    // Whether the initial-state exemption survives is kept for the window walk, beside the liveness it reads.
    init_reentrant_ = reenters_init(reachable);

    mark_live(reachable, co_accessible);

    split_points_ = derive_split_points({}, reachable, co_accessible, reverse, init_reentrant_);

    split_points_ignoring_ = derive_split_points(ignored, reachable, co_accessible, reverse, init_reentrant_);

    derive_mandatory_core(reverse);
}

std::optional<Dfa> Simulator::unrolled_start(const Dfa& dfa)
{
    require_indexable(dfa.state_count());

    if (!dfa.has_accept_token(dfa.init_state()))
    {
        return std::nullopt;
    }

    return unroll_start(dfa);
}

void Simulator::require_indexable(const std::size_t states)
{
    if (states == 0U || states >= no_state_)
    {
        throw std::runtime_error{"DFA has too many states to be indexed by a transition table entry"};
    }
}

Simulator::Classes_t Simulator::classify(const Dfa& dfa)
{
    // The signature of a symbol is the sorted set of transitions it labels. Symbols with equal signatures would have
    // identical table rows, which is exactly when they may share a class.
    using Signature_t = std::vector<std::pair<Dfa::State_t, Dfa::State_t>>;

    std::array<Signature_t, symbol_count> signatures{};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        const auto symbol{static_cast<unsigned char>(label.symbol())};

        signatures[symbol].emplace_back(from, to);
    }

    std::map<Signature_t, Class_t> classes{};

    Classes_t result{};

    for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
    {
        std::ranges::sort(signatures[symbol]);

        const auto fresh{static_cast<Class_t>(classes.size())};

        const auto [entry, inserted]{classes.try_emplace(std::move(signatures[symbol]), fresh)};

        const auto& [signature, symbol_class]{*entry};

        result[symbol] = symbol_class;
    }

    return result;
}

void Simulator::index_rows(const Classes_t& classes, const std::size_t states)
{
    const auto row_offset{[states](const Class_t symbol_class) { return symbol_class * states; }};

    std::ranges::transform(classes, row_offsets_.begin(), row_offset);
}

void Simulator::fill_accepts(const Dfa& dfa, const std::span<const std::pair<std::size_t, std::uint64_t>> payloads)
{
    const auto states{dfa.state_count()};

    accept_table_.assign(states, Accept{});

    flags_.assign(states, 0);

    for (const auto& [state, token] : dfa.accept_states())
    {
        accept_table_[state].token = token;

        flags_[state] |= accept_flag_;
    }

    for (const auto& [token, word] : payloads)
    {
        for (std::size_t state{0}; state < states; ++state)
        {
            if (!is_accepting(state) || accept_table_[state].token.id() != token)
            {
                continue;
            }

            accept_table_[state].payload = word;
        }
    }
}

void Simulator::fill_transitions(const Dfa& dfa, const Classes_t& classes, const std::size_t class_count)
{
    const auto states{dfa.state_count()};

    table_.assign(states * class_count, no_state_);

    const Table_view_t<Entry_t> transitions{table_.data(), class_count, states};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        const auto row{classes[static_cast<unsigned char>(label.symbol())]};

        transitions[row, from] = static_cast<Entry_t>(to);
    }
}

std::vector<std::vector<Simulator::Entry_t>> Simulator::predecessors() const
{
    const auto states{flags_.size()};

    std::vector<std::vector<Entry_t>> reverse(states);

    for (const auto row : distinct_rows())
    {
        for (std::size_t state{0}; state < states; ++state)
        {
            if (const auto to{table_[row + state]}; to != no_state_)
            {
                reverse[to].push_back(static_cast<Entry_t>(state));
            }
        }
    }

    return reverse;
}

std::vector<std::size_t> Simulator::distinct_rows() const
{
    std::vector<std::size_t> rows{row_offsets_.begin(), row_offsets_.end()};

    std::ranges::sort(rows);

    const auto repeats{std::ranges::unique(rows)};

    rows.erase(repeats.begin(), repeats.end());

    return rows;
}

std::vector<bool> Simulator::co_accessible_states(const std::vector<std::vector<Entry_t>>& predecessors) const
{
    const auto accepting{[this](const std::size_t state) { return is_accepting(state); }};

    return closed_backward(predecessors, accepting);
}

std::vector<bool> Simulator::reachable_states() const
{
    std::vector<bool> reachable(flags_.size(), false);

    reachable[init_state_] = true;

    std::vector<Entry_t> pending{static_cast<Entry_t>(init_state_)};

    while (!pending.empty())
    {
        const auto state{pending.back()};

        pending.pop_back();

        for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
        {
            if (const auto to{entry(symbol, state)}; to != no_state_ && !reachable[to])
            {
                reachable[to] = true;

                pending.push_back(to);
            }
        }
    }

    return reachable;
}

Simulator::Entry_t Simulator::entry(const std::size_t symbol, const std::size_t state) const noexcept
{
    return table_[row_offsets_[symbol] + state];
}

bool Simulator::reenters_init(const std::vector<bool>& reachable) const
{
    const auto all_states{std::views::iota(std::size_t{0}, flags_.size())};

    const auto all_symbols{std::views::iota(std::size_t{0}, symbol_count)};

    const auto reenters{[&](const std::size_t symbol) {
        const auto enters_init{[&](const std::size_t state) {
            return reachable[state] && entry(symbol, state) == static_cast<Entry_t>(init_state_);
        }};

        return std::ranges::any_of(all_states, enters_init);
    }};

    return std::ranges::any_of(all_symbols, reenters);
}

void Simulator::mark_live(const std::vector<bool>& reachable, const std::vector<bool>& co_accessible)
{
    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (reachable[state] && co_accessible[state])
        {
            flags_[state] |= live_flag_;
        }
    }
}

bool Simulator::consumes(
        const std::size_t symbol, const std::size_t state, const std::vector<bool>& co_accessible) const
{
    const auto to{entry(symbol, state)};

    return to != no_state_ && co_accessible[to];
}

std::array<std::uint64_t, 4> Simulator::derive_split_points(
        const std::span<const std::size_t> ignored, const std::vector<bool>& reachable,
        const std::vector<bool>& co_accessible, const std::vector<std::vector<Entry_t>>& predecessors,
        const bool init_reentrant) const
{
    std::array<std::uint64_t, 4> points{};

    const auto states{accept_table_.size()};

    const std::set<std::size_t> discarded{ignored.begin(), ignored.end()};

    std::vector<bool> accepts_discarded(states, false);

    for (std::size_t state{0}; state < states; ++state)
    {
        accepts_discarded[state] = is_accepting(state) && discarded.contains(accept_table_[state].token.id());
    }

    const auto accepts_kept{[this, &accepts_discarded](const std::size_t state) {
        return is_accepting(state) && !accepts_discarded[state];
    }};

    const auto reaches_kept{closed_backward(predecessors, accepts_kept)};

    // The classes cost a refinement over the whole table, and only a state that accepts a discarded kind ever asks for
    // them, so they are computed on the first such question and never for a lexer that discards nothing.
    std::optional<std::vector<std::size_t>> observed{};

    const auto class_after{
            [this, &observed, &accepts_discarded, states](const std::size_t symbol, const std::size_t state) {
                if (!observed)
                {
                    observed = observed_classes(accepts_discarded);
                }

                const auto to{entry(symbol, state)};

                const auto target{to == no_state_ ? states : static_cast<std::size_t>(to)};

                return (*observed)[target];
            }};

    const auto all_states{std::views::iota(std::size_t{0}, states)};

    for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
    {
        const auto harmless{[&](const std::size_t state) {
            if (!reachable[state] || !consumes(symbol, state, co_accessible) ||
                (state == init_state_ && !init_reentrant))
            {
                return true;
            }

            // The state accepts a discarded kind, its successor reaches no kept kind, and the restart on the symbol
            // lands in the class the interrupted scan steps into.
            const auto to{static_cast<std::size_t>(entry(symbol, state))};

            return accepts_discarded[state] && !reaches_kept[to] &&
                   class_after(symbol, state) == class_after(symbol, init_state_);
        }};

        const auto safe{std::ranges::all_of(all_states, harmless)};

        if (safe && consumes(symbol, init_state_, co_accessible))
        {
            points[symbol >> 6U] |= std::uint64_t{1} << (symbol & 63U);
        }
    }

    return points;
}

std::vector<std::size_t> Simulator::observed_classes(const std::vector<bool>& accepts_discarded) const
{
    const auto states{accept_table_.size()};

    // What colours a state before its token is told apart.
    enum class Shade : std::uint8_t
    {
        nonaccepting,
        discarded,
        kept
    };

    // A pair, so that no kept token ID can collide with the discarded or the nonaccepting colour.
    const auto colour{[&](const std::size_t state) -> std::pair<Shade, std::size_t> {
        if (state == states || !is_accepting(state))
        {
            return {Shade::nonaccepting, 0};
        }

        if (accepts_discarded[state])
        {
            return {Shade::discarded, 0};
        }

        return {Shade::kept, accept_table_[state].token.id()};
    }};

    std::vector<std::size_t> current(states + 1U);

    std::map<std::pair<Shade, std::size_t>, std::size_t> first_classes{};

    for (std::size_t state{0}; state <= states; ++state)
    {
        const auto state_colour{colour(state)};

        const auto [found, inserted]{first_classes.try_emplace(state_colour, first_classes.size())};

        const auto& [known_colour, number]{*found};

        current[state] = number;
    }

    auto count{first_classes.size()};

    const auto rows{distinct_rows()};

    for (;;)
    {
        std::map<std::vector<std::size_t>, std::size_t> signatures{};

        std::vector<std::size_t> next(states + 1U);

        for (std::size_t state{0}; state <= states; ++state)
        {
            std::vector<std::size_t> signature{current[state]};

            for (const auto row : rows)
            {
                const auto to{state == states ? no_state_ : table_[row + state]};

                const auto target{to == no_state_ ? states : static_cast<std::size_t>(to)};

                signature.push_back(current[target]);
            }

            const auto fresh{signatures.size()};

            const auto [found, inserted]{signatures.try_emplace(std::move(signature), fresh)};

            const auto& [state_signature, number]{*found};

            next[state] = number;
        }

        current = std::move(next);

        if (signatures.size() == count)
        {
            return current;
        }

        count = signatures.size();
    }
}

void Simulator::derive_mandatory_core(const std::vector<std::vector<Entry_t>>& predecessors)
{
    const auto depths{death_depths(predecessors)};

    const auto words{chain_death_words(depths)};

    auto candidates{core_candidates(words)};

    const auto longer{[&words](const std::size_t left, const std::size_t right) {
        return words.depth[left] > words.depth[right];
    }};

    // Longest first, so the first proved candidate is the answer and every shorter proposal goes untried; a chain of
    // nested proposals then costs one product search rather than one per link. Stability keeps equally long proposals
    // in state order.
    std::ranges::stable_sort(candidates, longer);

    // The longest core is the first candidate's death word without its killing byte.
    const auto longest{candidates.empty() ? std::size_t{0} : words.depth[candidates.front()] - 1U};

    // States are capped below the 32-bit sentinel and at most one candidate proposes per state, so a stamp holds any
    // proof ordinal and a prefix cell every matcher position a supported table can reach. Both widths are pinned here,
    // integrality included.
    static_assert(
            std::numeric_limits<Stamp_t>::is_integer &&
            std::numeric_limits<Stamp_t>::max() >= std::numeric_limits<std::uint32_t>::max() - 1U);

    static_assert(
            std::numeric_limits<Prefix_t>::is_integer &&
            std::numeric_limits<Prefix_t>::max() >= std::numeric_limits<std::uint32_t>::max() - 1U);

    // One stamped buffer serves every proof: an entry from an older search reads as unseen under the current stamp, so
    // nothing is cleared or reallocated between candidates, and a four-byte stamp is wrap-safe because there are fewer
    // candidates than stamps.
    std::vector<Stamp_t> seen(flags_.size() * longest, 0);

    // The stamp is the proof's one-based ordinal, distinct for every proof and never the buffer's virgin zero.
    for (const auto [at, candidate] : std::views::enumerate(candidates))
    {
        const auto core{spell_core(words, candidate)};

        const auto stamp{static_cast<Stamp_t>(at + 1)};

        if (proves_core(candidate, core, stamp, seen))
        {
            mandatory_core_ = core;

            break;
        }
    }
}

Simulator::Death_words Simulator::death_depths(const std::vector<std::vector<Entry_t>>& predecessors) const
{
    const auto states{flags_.size()};

    Death_words words{
            .depth = std::vector<std::size_t>(states, no_death_word_),
            .entered_by = std::vector<char>(states, 0),
            .onward = std::vector<std::size_t>(states, 0)};

    std::vector<std::size_t> frontier{};

    const auto all_symbols{std::views::iota(std::size_t{0}, symbol_count)};

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_live(state))
        {
            continue;
        }

        const auto has_live_target{
                [this, state](const std::size_t symbol) { return advance_live(state, symbol).has_value(); }};

        const auto killing{std::ranges::find_if_not(all_symbols, has_live_target)};

        if (killing == all_symbols.end())
        {
            continue;
        }

        words.depth[state] = 1;

        words.entered_by[state] = static_cast<char>(*killing);

        frontier.push_back(state);
    }

    for (std::size_t head{0}; head < frontier.size(); ++head)
    {
        const auto to{frontier[head]};

        for (const auto from : predecessors[to])
        {
            if (!is_live(from) || words.depth[from] != no_death_word_)
            {
                continue;
            }

            words.depth[from] = words.depth[to] + 1U;

            frontier.push_back(from);
        }
    }

    return words;
}

std::optional<std::size_t> Simulator::advance_live(const std::size_t state, const std::size_t symbol) const
{
    const auto to{entry(symbol, state)};

    if (to == no_state_ || !is_live(to))
    {
        return std::nullopt;
    }

    return to;
}

Simulator::Death_words Simulator::chain_death_words(Death_words words) const
{
    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (!is_live(state) || !words.has_chain(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
        {
            const auto to{advance_live(state, symbol)};

            if (to && words.depth[*to] != no_death_word_ && words.depth[*to] + 1U == words.depth[state])
            {
                words.entered_by[state] = static_cast<char>(symbol);

                words.onward[state] = *to;

                break;
            }
        }
    }

    return words;
}

std::vector<std::size_t> Simulator::core_candidates(const Death_words& words) const
{
    std::vector<std::size_t> candidates{};

    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (!is_live(state) || (state == init_state_ && !init_reentrant_))
        {
            continue;
        }

        if (!words.has_chain(state))
        {
            continue;
        }

        candidates.push_back(state);
    }

    return candidates;
}

std::string Simulator::spell_core(const Death_words& words, const std::size_t origin)
{
    std::string core{};

    for (auto state{origin}; words.depth[state] > 1U; state = words.onward[state])
    {
        core.push_back(words.entered_by[state]);
    }

    return core;
}

bool Simulator::proves_core(
        const std::size_t origin, const std::string_view core, const Stamp_t stamp, std::vector<Stamp_t>& seen) const
{
    const auto length{core.size()};

    const auto matcher{core_matcher(core)};

    const Table_view_t<const Prefix_t> next_matched{matcher.data(), length, symbol_count};

    seen[origin * length] = stamp;

    std::vector<std::pair<std::size_t, std::size_t>> pending{{origin, 0}};

    while (!pending.empty())
    {
        const auto [state, matched]{pending.back()};

        pending.pop_back();

        for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
        {
            const auto to{advance_live(state, symbol)};

            if (!to)
            {
                return false;
            }

            const auto next{next_matched[matched, symbol]};

            if (next == length)
            {
                continue;
            }

            const auto cell{*to * length + next};

            if (seen[cell] == stamp)
            {
                continue;
            }

            seen[cell] = stamp;

            pending.emplace_back(*to, next);
        }
    }

    return true;
}

std::vector<Simulator::Prefix_t> Simulator::core_matcher(const std::string_view core)
{
    const auto length{core.size()};

    std::vector<Prefix_t> fall(length, 0);

    for (std::size_t at{1}; at < length; ++at)
    {
        auto matched{fall[at - 1U]};

        while (matched != 0U && core[at] != core[matched])
        {
            matched = fall[matched - 1U];
        }

        if (core[at] == core[matched])
        {
            ++matched;
        }

        fall[at] = matched;
    }

    std::vector<Prefix_t> matcher(length * symbol_count, 0);

    const Table_view_t<Prefix_t> next_matched{matcher.data(), length, symbol_count};

    for (std::size_t at{0}; at < length; ++at)
    {
        for (std::size_t symbol{0}; symbol < symbol_count; ++symbol)
        {
            if (core[at] == static_cast<char>(symbol))
            {
                next_matched[at, symbol] = at + 1U;
            }
            else if (at > 0U)
            {
                next_matched[at, symbol] = next_matched[fall[at - 1U], symbol];
            }
        }
    }

    return matcher;
}

} // namespace munch::dfa

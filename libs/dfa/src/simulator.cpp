#include "munch/dfa/simulator.hpp"

#include <algorithm>
#include <cstdint>
#include <experimental/mdspan>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::dfa
{
namespace
{
/**
 * @brief Two-dimensional `(class, state)` view over a transition table.
 *
 * Rows are per class rather than per state, so the row offset of a lookup depends only on the input character,
 * which is known before the state it is consumed in: the offset computation stays off the state-to-state
 * dependency chain that limits how fast the run() loop can advance.
 * @tparam Entry The viewed entry type, const-qualified for reading.
 */
template <typename Entry>
using Table_view_t = std::mdspan<Entry, std::dextents<std::size_t, 2>>;

/**
 * @brief Returns the highest state identifier the DFA uses.
 *
 * The caller derives the table column count as one past this value; returning the identifier itself, rather than the
 * count, lets the caller reject an oversized DFA before the increment, which would wrap for a hand-built DFA whose
 * highest state is the largest std::size_t.
 * @param dfa The DFA whose states are inspected.
 * @return The highest state identifier used by the DFA.
 */
std::size_t highest_state(const Dfa& dfa)
{
    auto highest{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        highest = std::max({highest, key.first, to});
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        highest = std::max(highest, state);
    }

    return highest;
}

} // namespace

Simulator::Simulator(const Dfa& dfa) : Simulator{dfa, {}}
{}

Simulator::Simulator(const Dfa& dfa, const std::span<const std::size_t> ignored) : Simulator{dfa, ignored, {}}
{}

Simulator::Simulator(
        const Dfa& dfa, const std::span<const std::size_t> ignored,
        const std::span<const std::pair<std::size_t, std::uint64_t>> payloads)
    : init_state_{dfa.init_state()}
{
    const auto highest{highest_state(dfa)};

    if (highest >= no_state_ - 1)
    {
        throw std::runtime_error("DFA has too many states to be indexed by a transition table entry");
    }

    const auto states{highest + 1};

    const auto classes{classify(dfa)};

    const auto class_count{static_cast<std::size_t>(std::ranges::max(classes)) + 1};

    // The table is class_count rows of states entries; on a 32-bit size_t the product can wrap where the
    // per-state vectors still allocate, leaving an undersized table under an mdspan of the unwrapped extents.
    // On a 64-bit size_t this call is dead code, the entry-width guard above caps states below 2^32 and
    // classes never exceed 256, so no test on the platforms that run the suite can pin this call site; the
    // helper's arithmetic is pinned in its own right, and this line is the wiring a 32-bit build, outside the
    // supported 64-bit contract, relies on as defense in depth.
    if (table_size_overflows(states, class_count, std::numeric_limits<std::size_t>::max()))
    {
        throw std::runtime_error("DFA transition table size overflows std::size_t");
    }

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        row_offsets_[symbol] = classes[symbol] * states;
    }

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
            if (is_accepting(state) && accept_table_[state].token.id() == token)
            {
                accept_table_[state].payload = word;
            }
        }
    }

    table_.assign(states * class_count, no_state_);

    const Table_view_t<Entry_t> transitions{table_.data(), class_count, states};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        transitions[classes[static_cast<unsigned char>(label.symbol())], from] = static_cast<Entry_t>(to);
    }

    // Only transitions that can still end in acceptance can lie on an emitted token. A pattern denoting the empty
    // language, such as any_of() over an empty set, leaves reachable states behind from which no accepting state
    // is reachable; a scan entering one always fails, so the bytes it consumes are consumed by no token and must
    // not be allowed to de-certify anything. Mark the states from which acceptance is still reachable.
    std::vector<std::vector<Entry_t>> predecessors(states);

    // Once per class row, not once per symbol value. The table is compressed by symbol equivalence class, so walking
    // all 256 values would push the same edge once per symbol sharing a class and leave the index larger than the
    // table it indexes.
    const std::set<std::size_t> rows{row_offsets_.begin(), row_offsets_.end()};

    for (const auto row : rows)
    {
        for (std::size_t state{0}; state < states; ++state)
        {
            if (const auto to{table_[row + state]}; to != no_state_)
            {
                predecessors[to].push_back(static_cast<Entry_t>(state));
            }
        }
    }

    std::vector<bool> co_accessible(states, false);

    std::vector<Entry_t> pending;

    for (std::size_t state{0}; state < states; ++state)
    {
        if (is_accepting(state))
        {
            co_accessible[state] = true;

            pending.push_back(static_cast<Entry_t>(state));
        }
    }

    while (!pending.empty())
    {
        const auto state{pending.back()};

        pending.pop_back();

        for (const auto from : predecessors[state])
        {
            if (!co_accessible[from])
            {
                co_accessible[from] = true;

                pending.push_back(from);
            }
        }
    }

    // Symmetrically, only states a scan can actually arrive in may de-certify a symbol. A Dfa handed to the
    // Simulator directly can number states no input reaches, and an unreachable live island, or an unreachable
    // transition back into the initial state, would otherwise cost certificates that no scan can ever contradict.
    // Builder's subset construction supplies reachability on its own, so this only matters for a hand-built Dfa.
    std::vector<bool> reachable(states, false);

    reachable[init_state_] = true;

    pending.assign(1, static_cast<Entry_t>(init_state_));

    while (!pending.empty())
    {
        const auto state{pending.back()};

        pending.pop_back();

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            if (const auto to{table_[row_offsets_[symbol] + state]}; to != no_state_ && !reachable[to])
            {
                reachable[to] = true;

                pending.push_back(to);
            }
        }
    }

    // A symbol only the initial state consumes can only begin a token, but the exemption is valid only while the
    // initial state cannot be reached again after consuming input: a nullable pattern such as kleene minimizes to
    // an accepting start state with a self-loop, where the "first byte of a token" reasoning no longer holds.
    const auto init_reentrant{
            std::ranges::any_of(std::views::iota(std::size_t{0}, symbol_count_), [&](const std::size_t symbol) {
                return std::ranges::any_of(std::views::iota(std::size_t{0}, states), [&](const std::size_t state) {
                    return reachable[state] &&
                           table_[row_offsets_[symbol] + state] == static_cast<Entry_t>(init_state_);
                });
            })};

    // Persist what the window walk needs at call time: liveness per state, and whether the initial-state
    // exemption survives. Everything else it uses, the tables already carry.
    init_reentrant_ = init_reentrant;

    for (std::size_t state{0}; state < states; ++state)
    {
        if (reachable[state] && co_accessible[state])
        {
            flags_[state] |= live_flag_;
        }
    }

    const auto consumes{[this, &co_accessible](const std::size_t symbol, const std::size_t state) {
        const auto to{table_[row_offsets_[symbol] + state]};

        return to != no_state_ && co_accessible[to];
    }};

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        const auto safe{std::ranges::all_of(std::views::iota(std::size_t{0}, states), [&](const std::size_t state) {
            return !reachable[state] || (state == init_state_ && !init_reentrant) || !consumes(symbol, state);
        })};

        // A symbol no live state consumes is certified vacuously: no input this lexer accepts can contain it, so it is
        // useless to a caller and searching for one scans to the end of the input for nothing. Since a certified
        // symbol is consumed only by the initial state, it can occur in valid input exactly when the initial state
        // consumes it into a state that can still accept, and only those are reported.
        if (safe && consumes(symbol, init_state_))
        {
            split_points_[symbol >> 6U] |= std::uint64_t{1} << (symbol & 63U);
        }
    }

    derive_split_points_ignoring(ignored, reachable, co_accessible, predecessors, init_reentrant);

    derive_mandatory_core();
}

// Condition three of the weaker certificate asks whether every token still reachable from a state is discarded. That
// needs no per-state set of token IDs: it is the complement of "some kept token is still reachable", which one
// backward closure settles for every state at once.
//
// Two details keep this linear in the table. The closure walks the reverse index the constructor already built for
// co-accessibility, rather than rescanning every symbol and source state for each state it reaches, which would be
// quadratic in the state count. And whether a state accepts a discarded token is resolved once per state, rather
// than searching the ignored list from inside the symbol loop.
void Simulator::derive_split_points_ignoring(
        const std::span<const std::size_t> ignored, const std::vector<bool>& reachable,
        const std::vector<bool>& co_accessible, const std::vector<std::vector<Entry_t>>& predecessors,
        const bool init_reentrant)
{
    const auto states{accept_table_.size()};

    const std::set<std::size_t> discarded{ignored.begin(), ignored.end()};

    std::vector<bool> accepts_discarded(states, false);

    for (std::size_t state{0}; state < states; ++state)
    {
        accepts_discarded[state] = is_accepting(state) && discarded.contains(accept_table_[state].token.id());
    }

    std::vector<bool> reaches_kept(states, false);

    std::vector<std::size_t> work;

    for (std::size_t state{0}; state < states; ++state)
    {
        if (is_accepting(state) && !accepts_discarded[state])
        {
            reaches_kept[state] = true;

            work.push_back(state);
        }
    }

    while (!work.empty())
    {
        const auto to{work.back()};

        work.pop_back();

        for (const auto from : predecessors[to])
        {
            if (!reaches_kept[from])
            {
                reaches_kept[from] = true;

                work.push_back(from);
            }
        }
    }

    const auto consumes{[this, &co_accessible](const std::size_t symbol, const std::size_t state) {
        const auto to{table_[row_offsets_[symbol] + state]};

        return to != no_state_ && co_accessible[to];
    }};

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        const auto safe{std::ranges::all_of(std::views::iota(std::size_t{0}, states), [&](const std::size_t state) {
            if (!reachable[state] || !consumes(symbol, state) || (state == init_state_ && !init_reentrant))
            {
                return true;
            }

            // The left chunk must end on a complete token the caller discards, every token the severed one could
            // still become must be discarded too, and the restart must land where the interrupted scan already is.
            return accepts_discarded[state] && !reaches_kept[state] &&
                   table_[row_offsets_[symbol] + state] == table_[row_offsets_[symbol] + init_state_];
        })};

        // Vacuity is judged as for the exact map: a symbol no live state consumes is useless to a caller.
        if (safe && consumes(symbol, init_state_))
        {
            split_points_ignoring_[symbol >> 6U] |= std::uint64_t{1} << (symbol & 63U);
        }
    }
}

Simulator::Classes_t Simulator::classify(const Dfa& dfa)
{
    // The signature of a symbol is the sorted set of transitions it labels. Symbols with equal signatures would have
    // identical table rows, which is exactly when they may share a class.
    using Signature_t = std::vector<std::pair<Dfa::State_t, Dfa::State_t>>;

    std::array<Signature_t, symbol_count_> signatures;

    for (const auto& [key, to] : dfa.transitions())
    {
        signatures[static_cast<unsigned char>(key.second.symbol())].emplace_back(key.first, to);
    }

    std::map<Signature_t, Class_t> classes;

    Classes_t result{};

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        std::ranges::sort(signatures[symbol]);

        result[symbol] =
                classes.try_emplace(std::move(signatures[symbol]), static_cast<Class_t>(classes.size())).first->second;
    }

    return result;
}

namespace
{
/**
 * @brief The anchored machinery's view of the compiled tables: enough to walk, nothing more.
 */
struct Walk_view
{
    std::size_t states{};
    std::size_t init{};
    std::function<std::optional<std::size_t>(std::size_t, unsigned char)> step;
    std::function<bool(std::size_t)> accepting;
};

/**
 * @brief The maximal-munch jump table over the tail: the committed end of the token starting at each
 *        offset, and complete tokenizability of every suffix, built right to left.
 */
struct Jump_table
{
    std::vector<std::optional<std::size_t>> end;
    std::vector<char> tokenizes; // indexed to tail size inclusive
};

Jump_table build_jump_table(const Walk_view& view, const std::string_view tail)
{
    Jump_table table{
            .end = std::vector<std::optional<std::size_t>>(tail.size()),
            .tokenizes = std::vector<char>(tail.size() + 1, 0)};

    table.tokenizes[tail.size()] = 1;

    for (std::size_t offset{tail.size()}; offset-- > 0;)
    {
        auto state{view.init};

        std::optional<std::size_t> last;

        for (std::size_t at{offset}; at < tail.size(); ++at)
        {
            const auto next{view.step(state, static_cast<unsigned char>(tail[at]))};

            if (!next)
            {
                break;
            }

            state = *next;

            if (view.accepting(state))
            {
                last = at + 1;
            }
        }

        table.end[offset] = last;

        table.tokenizes[offset] = static_cast<char>(last && table.tokenizes[*last] != 0);
    }

    return table;
}

/**
 * @brief States reachable from the initial state by at least one transition, each with a shortest
 *        witnessing word: the crossing entries, whose witnesses double as the repairs that realize them.
 */
std::vector<std::pair<std::size_t, std::string>> crossing_entries(const Walk_view& view)
{
    std::map<std::size_t, std::string> seen;

    std::vector<std::size_t> frontier;

    const auto push{[&](const std::size_t state, const std::string& via) {
        if (seen.emplace(state, via).second)
        {
            frontier.push_back(state);
        }
    }};

    for (int byte{0}; byte < 256; ++byte)
    {
        if (const auto to{view.step(view.init, static_cast<unsigned char>(byte))})
        {
            push(*to, std::string(1, static_cast<char>(byte)));
        }
    }

    for (std::size_t at{0}; at < frontier.size(); ++at)
    {
        const auto from{frontier[at]};

        const auto via{seen.at(from)};

        for (int byte{0}; byte < 256; ++byte)
        {
            if (const auto to{view.step(from, static_cast<unsigned char>(byte))})
            {
                push(*to, via + static_cast<char>(byte));
            }
        }
    }

    return {seen.begin(), seen.end()};
}

/**
 * @brief One crossing scenario's first in-tail boundary: the last in-tail accept of the maximal run from
 *        the entry, counting zero when the entry itself accepts; nothing when the run never accepts.
 */
std::optional<std::size_t> scenario_boundary(
        const Walk_view& view, const std::string_view tail, const std::size_t entry)
{
    std::optional<std::size_t> last;

    if (view.accepting(entry))
    {
        last = 0;
    }

    auto state{entry};

    for (std::size_t at{0}; at < tail.size(); ++at)
    {
        const auto next{view.step(state, static_cast<unsigned char>(tail[at]))};

        if (!next)
        {
            break;
        }

        state = *next;

        if (view.accepting(state))
        {
            last = at + 1;
        }
    }

    return last;
}

} // namespace

std::optional<std::size_t> Simulator::is_split_window(const std::string_view window) const
{
    // An accepting initial state is the compiled signature of a nullable token set, which the soundness theorem
    // excludes; refuse rather than answer beyond the proved scope. The empty window certifies nothing either.
    if (window.empty() || is_accepting(init_state_))
    {
        return std::nullopt;
    }

    // The pre-window origin: a hypothesis whose token began before the window. Any value no in-window offset can
    // take serves as the marker.
    constexpr std::size_t before{std::numeric_limits<std::size_t>::max()};

    // The cloud of hypotheses (state, origin). A set, exactly as the proof's model: the seed and the rename can
    // propose the identical pair and must coalesce.
    std::set<std::pair<std::size_t, std::size_t>> cloud;

    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (is_live(state))
        {
            cloud.emplace(state, before);
        }
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto row{row_offsets_[static_cast<unsigned char>(window[at])]};

        // A token can only end where the automaton accepted, so a boundary before this byte is possible exactly
        // where some tracked state accepts. The test runs on the cloud as it stands, ahead of the step.
        const auto accepting{std::ranges::any_of(
                cloud | std::views::keys, [this](const std::size_t state) { return is_accepting(state); })};

        std::set<std::pair<std::size_t, std::size_t>> next;

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{table_[row + state]}; to != no_state_ && is_live(to))
            {
                // Reading from the initial state begins a token here, so the origin is this offset rather than
                // whatever the hypothesis carried in; valid only while nothing re-enters the initial state. A
                // hypothesis that cannot consume the byte is an impossible history and is dropped, never
                // restarted.
                const auto begins{state == init_state_ && !init_reentrant_};

                next.emplace(to, begins ? at : origin);
            }
        }

        // One fresh hypothesis wherever the automaton had just accepted, the only place a token can begin. The
        // initial cloud contains an accepting live state whenever the grammar is usable, so no first-byte special
        // case exists.
        if (accepting)
        {
            if (const auto to{table_[row + init_state_]}; to != no_state_ && is_live(to))
            {
                next.emplace(to, at);
            }
        }

        // The empty cloud is absorbing: no live history crosses this window, and the walk refuses.
        if (next.empty())
        {
            return std::nullopt;
        }

        cloud.swap(next);
    }

    // Certified exactly when every surviving hypothesis agrees on one in-window origin; unanimity at the
    // pre-window marker means the window never resolves where the covering token began.
    const auto origin{cloud.begin()->second};

    if (!std::ranges::all_of(cloud | std::views::values, [origin](const std::size_t at) { return at == origin; }))
    {
        return std::nullopt;
    }

    return origin == before ? std::nullopt : std::optional{origin};
}

// The planner's accelerator licence. A live state alive on every byte survives any window that lacks its
// forced exit, so a window certifying at all must carry that exit: the derivation proposes each such state's
// shortest exit and keeps the longest one that survives the proof, which exhausts core-avoiding death words
// over the live tables and refutes the candidate on the first one found. The matcher reads only the live
// prefix; the killing byte is never fed to it, since a core completing on the killing byte is too late.
// Refusal leaves the core empty and the planner exhaustive: the core is an accelerator's licence, never a
// certificate. Nullable sets skip the derivation outright, because the window certificate refuses them
// wholesale and a core would license nothing.
void Simulator::derive_mandatory_core()
{
    if (nullable())
    {
        return;
    }

    const auto states{flags_.size()};

    const auto advance_live{[this](const std::size_t state, const std::size_t symbol) -> std::optional<std::size_t> {
        const auto to{table_[row_offsets_[symbol] + state]};

        return to != no_state_ && is_live(to) ? std::optional<std::size_t>{to} : std::nullopt;
    }};

    // Shortest death words by search from the deaths backward: depth one where some byte has no live
    // target, and each layer of the reverse traversal one byte deeper, every live transition read once. A
    // state no death word leaves keeps infinity and proposes nothing.
    constexpr auto infinity{std::numeric_limits<std::size_t>::max()};

    std::vector<std::size_t> depth(states, infinity);

    std::vector<char> step(states, 0);

    std::vector<std::size_t> onward(states, 0);

    std::vector<std::size_t> frontier;

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            if (!advance_live(state, symbol))
            {
                depth[state] = 1;

                step[state] = static_cast<char>(symbol);

                frontier.push_back(state);

                break;
            }
        }
    }

    std::vector<std::vector<std::size_t>> sources(states);

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            const auto to{advance_live(state, symbol)};

            if (to && (sources[*to].empty() || sources[*to].back() != state))
            {
                sources[*to].push_back(state);
            }
        }
    }

    for (std::size_t head{0}; head < frontier.size(); ++head)
    {
        const auto to{frontier[head]};

        for (const auto from : sources[to])
        {
            if (depth[from] == infinity)
            {
                depth[from] = depth[to] + 1;

                frontier.push_back(from);
            }
        }
    }

    // The death word is never stored: with the depths final, each state keeps one byte and one successor,
    // chosen as the smallest byte stepping one layer shallower, and a word is spelled by walking the chain.
    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_live(state) || depth[state] == infinity || depth[state] < 2)
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            const auto to{advance_live(state, symbol)};

            if (to && depth[*to] != infinity && depth[*to] + 1 == depth[state])
            {
                step[state] = static_cast<char>(symbol);

                onward[state] = *to;

                break;
            }
        }
    }

    // Candidates come from input-total states of the required set: a non-re-entrant initial state is
    // exempt, since its window hypothesis renames rather than survives. The core is the death word with the
    // killing byte removed, and a depth of at least two is input-totality itself, since the seeding pass
    // gave depth one to every live state missing a byte. Only the state is kept; its core is spelled when
    // its proof runs.
    std::vector<std::size_t> candidates;

    std::size_t longest{0};

    for (std::size_t state{0}; state < states; ++state)
    {
        if (!is_live(state) || (state == init_state_ && !init_reentrant_))
        {
            continue;
        }

        if (depth[state] == infinity || depth[state] < 2)
        {
            continue;
        }

        candidates.push_back(state);

        longest = std::max(longest, depth[state] - 1);
    }

    const auto materialize{[&](const std::size_t origin) {
        std::string core;

        for (auto state{origin}; depth[state] > 1; state = onward[state])
        {
            core.push_back(step[state]);
        }

        return core;
    }};

    // One stamped buffer serves every proof: an entry from an older search reads as unseen under the
    // current stamp, so nothing is cleared or reallocated between candidates, and a four-byte stamp is
    // wrap-safe because there are fewer candidates than stamps.
    // The widths carry the wrap argument: states are capped below the 32-bit sentinel and at most one
    // candidate proposes per state, so a stamp holds any proof ordinal, and a prefix cell must hold every
    // matcher position a supported table can reach. Both types are pinned here, integrality included, so
    // no narrowing or rounding representation can slip in silently.
    using Stamp_t = std::uint32_t;

    using Prefix_t = std::size_t;

    static_assert(
            std::numeric_limits<Stamp_t>::is_integer &&
            std::numeric_limits<Stamp_t>::max() >= std::numeric_limits<std::uint32_t>::max() - 1);

    static_assert(
            std::numeric_limits<Prefix_t>::is_integer &&
            std::numeric_limits<Prefix_t>::max() >= std::numeric_limits<std::uint32_t>::max() - 1);

    std::vector<Stamp_t> seen(states * longest, 0);

    // The matcher precomputed as a table per candidate, one lookup per transition: a graph search defeats
    // the usual amortization of chained failure links, so paying them once here keeps a proof's cost at the
    // pairs it visits.
    std::vector<Prefix_t> matcher;

    // The proof, per candidate from its own proposing state: a stack-driven reachability search over pairs
    // of a live state and a matcher prefix, refuted the moment any reachable pair meets a byte with no live
    // target, since the word spelled to that point is a core-avoiding death word. Pairs whose matcher
    // completed the core are satisfied for every continuation and are not expanded; visiting order carries
    // nothing, only the reachable set.
    const auto proved{[&](const std::size_t origin, const std::string& core, const Stamp_t stamp) {
        const auto length{core.size()};

        std::vector<Prefix_t> fall(length, 0);

        for (std::size_t at{1}; at < length; ++at)
        {
            auto matched{fall[at - 1]};

            while (matched != 0 && core[at] != core[matched])
            {
                matched = fall[matched - 1];
            }

            if (core[at] == core[matched])
            {
                ++matched;
            }

            fall[at] = matched;
        }

        matcher.assign(length * symbol_count_, 0);

        for (std::size_t at{0}; at < length; ++at)
        {
            for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
            {
                if (core[at] == static_cast<char>(symbol))
                {
                    matcher[at * symbol_count_ + symbol] = at + 1;
                }
                else if (at > 0)
                {
                    matcher[at * symbol_count_ + symbol] = matcher[fall[at - 1] * symbol_count_ + symbol];
                }
            }
        }

        seen[origin * length] = stamp;

        std::vector<std::pair<std::size_t, std::size_t>> pending{{origin, 0}};

        while (!pending.empty())
        {
            const auto [state, matched]{pending.back()};

            pending.pop_back();

            for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
            {
                const auto to{advance_live(state, symbol)};

                if (!to)
                {
                    return false;
                }

                const auto next{matcher[matched * symbol_count_ + symbol]};

                if (next == length)
                {
                    continue;
                }

                if (seen[*to * length + next] != stamp)
                {
                    seen[*to * length + next] = stamp;

                    pending.emplace_back(*to, next);
                }
            }
        }

        return true;
    }};

    // Longest first, so the first proved candidate is the answer and every shorter proposal goes untried; a
    // chain of nested proposals then costs one product search rather than one per link. Stability keeps the
    // state-order tie between equally long proposals where it has always been.
    std::ranges::stable_sort(
            candidates, [&depth](const auto left, const auto right) { return depth[left] > depth[right]; });

    // The stamp is the proof's one-based ordinal, so distinctness holds by construction rather than by
    // increment discipline, and the ordinal never reaches the buffer's virgin zero.
    for (const auto [at, candidate] : std::views::enumerate(candidates))
    {
        const auto core{materialize(candidate)};

        if (proved(candidate, core, static_cast<Stamp_t>(at + 1)))
        {
            mandatory_core_ = core;

            break;
        }
    }
}

std::optional<std::size_t> Simulator::lag() const
{
    // The post-accept nonaccepting region: nonaccepting successors of accepting states, closed under
    // nonaccepting transitions; a cycle inside it is the unboundedness witness, and otherwise the lag is
    // the longest path measured in states.
    std::vector<char> in_region(flags_.size(), 0);

    std::vector<std::size_t> frontier;

    const auto enter{[&](const std::size_t state) {
        if (in_region[state] == 0)
        {
            in_region[state] = 1;

            frontier.push_back(state);
        }
    }};

    // An accepting state no input reaches opens no stretch; for an accepting state, live means reachable.
    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (!is_accepting(state) || !is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            const auto to{table_[row_offsets_[symbol] + state]};

            if (to != no_state_ && !is_accepting(to))
            {
                enter(to);
            }
        }
    }

    for (std::size_t at{0}; at < frontier.size(); ++at)
    {
        const auto from{frontier[at]};

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            const auto to{table_[row_offsets_[symbol] + from]};

            if (to != no_state_ && !is_accepting(to))
            {
                enter(to);
            }
        }
    }

    // Longest path by repeated sink peeling; anything left over closes a cycle.
    std::vector<std::size_t> depth(flags_.size(), 0);

    auto remaining{frontier};

    std::size_t longest{0};

    for (bool shrank{true}; shrank && !remaining.empty();)
    {
        shrank = false;

        std::vector<std::size_t> keep;

        for (const auto state : remaining)
        {
            std::optional<std::size_t> deepest{0};

            for (std::size_t symbol{0}; symbol < symbol_count_ && deepest; ++symbol)
            {
                const auto to{table_[row_offsets_[symbol] + state]};

                if (to == no_state_ || is_accepting(to))
                {
                    continue;
                }

                if (in_region[to] == 2)
                {
                    deepest = std::max(*deepest, depth[to]);
                }
                else
                {
                    deepest = std::nullopt; // a successor still unresolved: not yet a sink
                }
            }

            if (deepest)
            {
                depth[state] = 1 + *deepest;

                in_region[state] = 2;

                longest = std::max(longest, depth[state]);

                shrank = true;
            }
            else
            {
                keep.push_back(state);
            }
        }

        remaining = std::move(keep);
    }

    if (!remaining.empty())
    {
        return std::nullopt; // the leftover states close a nonaccepting cycle: unbounded
    }

    return longest;
}

bool Simulator::rescue_free() const
{
    // As in lag(): an accepting state no input reaches cannot refute rescue-freeness.
    for (std::size_t state{0}; state < flags_.size(); ++state)
    {
        if (!is_accepting(state) || !is_live(state))
        {
            continue;
        }

        for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
        {
            const auto opened{table_[row_offsets_[symbol] + state]};

            if (opened == no_state_ || is_accepting(opened))
            {
                continue;
            }

            // A stretch opens on this byte; the gate needs it dead from the initial state, where dead
            // means no transition or one that can never reach acceptance.
            const auto entered{table_[row_offsets_[symbol] + init_state_]};

            if (entered != no_state_ && is_live(entered))
            {
                return false;
            }
        }
    }

    return true;
}

std::optional<std::size_t> Simulator::next_anchored_start(const std::string_view tail, const std::size_t from) const
{
    // Nullable token sets sit outside the model, exactly as for is_split_window().
    if (is_accepting(init_state_) || from >= tail.size())
    {
        return std::nullopt;
    }

    const Walk_view view{
            .states = flags_.size(),
            .init = init_state_,
            .step = [this](const std::size_t state, const unsigned char symbol) -> std::optional<std::size_t> {
                const auto to{table_[row_offsets_[symbol] + state]};

                return to == no_state_ ? std::nullopt : std::optional<std::size_t>{to};
            },
            .accepting = [this](const std::size_t state) { return is_accepting(state); }};

    const auto table{build_jump_table(view, tail)};

    // Every completing scenario votes for its boundary chain; a position is anchored-certified when
    // every completing scenario contains it. No completing scenario means the tail is beyond repair and
    // every position only vacuously invariant, which is deliberately a refusal.
    std::vector<std::size_t> votes(tail.size(), 0);

    std::size_t completing{0};

    const auto vote{[&](const std::size_t first) {
        ++completing;

        for (auto at{first}; at < tail.size(); at = *table.end[at])
        {
            ++votes[at];
        }
    }};

    if (table.tokenizes[0] != 0)
    {
        vote(0);
    }

    for (const auto& [entry, via] : crossing_entries(view))
    {
        const auto boundary{scenario_boundary(view, tail, entry)};

        if (boundary && table.tokenizes[*boundary] != 0)
        {
            vote(*boundary);
        }
    }

    if (completing == 0)
    {
        return std::nullopt;
    }

    for (auto at{from}; at < tail.size(); ++at)
    {
        if (votes[at] == completing)
        {
            return at;
        }
    }

    return std::nullopt;
}

std::optional<std::string> Simulator::minimal_repair(const std::string_view tail) const
{
    if (is_accepting(init_state_))
    {
        return std::nullopt;
    }

    const Walk_view view{
            .states = flags_.size(),
            .init = init_state_,
            .step = [this](const std::size_t state, const unsigned char symbol) -> std::optional<std::size_t> {
                const auto to{table_[row_offsets_[symbol] + state]};

                return to == no_state_ ? std::nullopt : std::optional<std::size_t>{to};
            },
            .accepting = [this](const std::size_t state) { return is_accepting(state); }};

    const auto table{build_jump_table(view, tail)};

    if (table.tokenizes[0] != 0)
    {
        return std::string{};
    }

    // The minimal repair is a shortest path to a completing crossing entry; the entries carry shortest
    // witnesses by construction, so the cheapest completing one is the answer, and none completing is a
    // certificate that no repair of any length exists.
    std::optional<std::string> best;

    for (const auto& [entry, via] : crossing_entries(view))
    {
        const auto boundary{scenario_boundary(view, tail, entry)};

        if (boundary && table.tokenizes[*boundary] != 0 && (!best || via.size() < best->size()))
        {
            best = via;
        }
    }

    return best;
}

} // namespace munch::dfa

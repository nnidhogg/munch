#include "munch/dfa/simulator.hpp"

#include <algorithm>
#include <experimental/mdspan>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
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
    if (states > std::numeric_limits<std::size_t>::max() / class_count)
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
            if ((flags_[state] & accept_flag_) != 0 && accept_table_[state].token.id() == token)
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
        if ((flags_[state] & accept_flag_) != 0)
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
    auto init_reentrant{false};

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        for (std::size_t state{0}; state < states; ++state)
        {
            init_reentrant = init_reentrant || (reachable[state] && table_[row_offsets_[symbol] + state] ==
                                                                            static_cast<Entry_t>(init_state_));
        }
    }

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
        auto safe{true};

        for (std::size_t state{0}; safe && state < states; ++state)
        {
            safe = !reachable[state] || (state == init_state_ && !init_reentrant) || !consumes(symbol, state);
        }

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
        accepts_discarded[state] =
                (flags_[state] & accept_flag_) != 0 && discarded.contains(accept_table_[state].token.id());
    }

    std::vector<bool> reaches_kept(states, false);

    std::vector<std::size_t> work;

    for (std::size_t state{0}; state < states; ++state)
    {
        if ((flags_[state] & accept_flag_) != 0 && !accepts_discarded[state])
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
        auto safe{true};

        for (std::size_t state{0}; safe && state < states; ++state)
        {
            if (!reachable[state] || !consumes(symbol, state) || (state == init_state_ && !init_reentrant))
            {
                continue;
            }

            // The left chunk must end on a complete token the caller discards, every token the severed one could
            // still become must be discarded too, and the restart must land where the interrupted scan already is.
            safe = accepts_discarded[state] && !reaches_kept[state] &&
                   table_[row_offsets_[symbol] + state] == table_[row_offsets_[symbol] + init_state_];
        }

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

std::optional<std::size_t> Simulator::is_split_window(const std::string_view window) const
{
    // An accepting initial state is the compiled signature of a nullable token set, which the soundness theorem
    // excludes; refuse rather than answer beyond the proved scope. The empty window certifies nothing either.
    if (window.empty() || (flags_[init_state_] & accept_flag_) != 0)
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
        if ((flags_[state] & live_flag_) != 0)
        {
            cloud.emplace(state, before);
        }
    }

    for (std::size_t at{0}; at < window.size(); ++at)
    {
        const auto row{row_offsets_[static_cast<unsigned char>(window[at])]};

        // A token can only end where the automaton accepted, so a boundary before this byte is possible exactly
        // where some tracked state accepts. The test runs on the cloud as it stands, ahead of the step.
        auto accepting{false};

        for (const auto& state : cloud | std::views::keys)
        {
            accepting = accepting || (flags_[state] & accept_flag_) != 0;
        }

        std::set<std::pair<std::size_t, std::size_t>> next;

        for (const auto& [state, origin] : cloud)
        {
            if (const auto to{table_[row + state]}; to != no_state_ && (flags_[to] & live_flag_) != 0)
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
            if (const auto to{table_[row + init_state_]}; to != no_state_ && (flags_[to] & live_flag_) != 0)
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

    for (const auto& at : cloud | std::views::values)
    {
        if (at != origin)
        {
            return std::nullopt;
        }
    }

    return origin == before ? std::nullopt : std::optional{origin};
}

} // namespace munch::dfa

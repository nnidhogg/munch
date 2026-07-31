#include "munch/dfa/simulator.hpp"

#include <algorithm>
#include <experimental/mdspan>
#include <map>
#include <ranges>
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
 * @brief Returns the number of table columns the DFA needs, i.e. one past its highest state identifier.
 * @param dfa The DFA whose states are counted.
 * @return One past the highest state identifier used by the DFA.
 */
std::size_t count_states(const Dfa& dfa)
{
    auto highest_state{dfa.init_state()};

    for (const auto& [key, to] : dfa.transitions())
    {
        highest_state = std::max({highest_state, key.first, to});
    }

    for (const auto& state : dfa.accept_states() | std::views::keys)
    {
        highest_state = std::max(highest_state, state);
    }

    return highest_state + 1;
}

} // namespace

Simulator::Simulator(const Dfa& dfa) : init_state_{dfa.init_state()}
{
    const auto states{count_states(dfa)};

    if (states >= no_state_)
    {
        throw std::runtime_error("DFA has too many states to be indexed by a transition table entry");
    }

    const auto classes{classify(dfa)};

    const auto class_count{static_cast<std::size_t>(std::ranges::max(classes)) + 1};

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        row_offsets_[symbol] = classes[symbol] * states;
    }

    accept_table_.assign(states, std::nullopt);

    flags_.assign(states, 0);

    for (const auto& [state, token] : dfa.accept_states())
    {
        accept_table_[state] = token;

        flags_[state] |= accept_flag_;
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

    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        for (std::size_t state{0}; state < states; ++state)
        {
            if (const auto to{table_[row_offsets_[symbol] + state]}; to != no_state_)
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

    // A symbol only the initial state consumes can only begin a token, but the exemption is valid only while the
    // initial state cannot be reached again after consuming input: a nullable pattern such as kleene minimizes to
    // an accepting start state with a self-loop, where the "first byte of a token" reasoning no longer holds.
    auto init_reentrant{false};

    for (const auto entry : table_)
    {
        init_reentrant = init_reentrant || entry == static_cast<Entry_t>(init_state_);
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
            safe = (state == init_state_ && !init_reentrant) || !consumes(symbol, state);
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

} // namespace munch::dfa

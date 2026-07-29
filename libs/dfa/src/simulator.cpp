#include "munch/dfa/simulator.hpp"

#include <algorithm>
#include <experimental/mdspan>
#include <map>
#include <ranges>
#include <stdexcept>

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

    // A symbol only the initial state consumes can only begin a token; see is_split_point().
    for (std::size_t symbol{0}; symbol < symbol_count_; ++symbol)
    {
        auto safe{true};

        for (std::size_t state{0}; safe && state < states; ++state)
        {
            safe = state == init_state_ || table_[row_offsets_[symbol] + state] == no_state_;
        }

        if (safe)
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

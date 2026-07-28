#include "lexer/dfa/simulator.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace lexer::dfa
{
namespace
{
/**
 * @brief Returns the number of table rows the DFA needs, i.e. one past its highest state identifier.
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

    table_.assign(states * symbol_count_, no_state_);

    accept_table_.assign(states, std::nullopt);

    const Table_view_t<Entry_t> transitions{table_.data(), states};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        transitions[from, static_cast<unsigned char>(label.symbol())] = static_cast<Entry_t>(to);
    }

    for (const auto& [state, token] : dfa.accept_states())
    {
        accept_table_[state] = token;
    }
}

} // namespace lexer::dfa

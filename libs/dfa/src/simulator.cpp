#include "lexer/dfa/simulator.hpp"

#include <algorithm>
#include <map>
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

Simulator::Classes_t Simulator::classify(const Dfa& dfa)
{
    // The signature of a symbol is the sorted set of transitions it labels. Symbols with equal signatures would have
    // identical table columns, which is exactly when they may share a class.
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

    table_.assign(states * class_count, no_state_);

    accept_table_.assign(states, std::nullopt);

    const Table_view_t<Entry_t> transitions{table_.data(), class_count, states};

    for (const auto& [key, to] : dfa.transitions())
    {
        const auto& [from, label]{key};

        transitions[classes[static_cast<unsigned char>(label.symbol())], from] = static_cast<Entry_t>(to);
    }

    for (const auto& [state, token] : dfa.accept_states())
    {
        accept_table_[state] = token;
    }
}

} // namespace lexer::dfa

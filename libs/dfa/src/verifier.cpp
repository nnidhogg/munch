#include "munch/dfa/verifier.hpp"

#include <algorithm>
#include <boost/container_hash/hash.hpp>
#include <compare>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace munch::dfa
{

struct Verifier::Trimmed
{
    State_t state_count{};

    Transitions_t transitions{};

    Accept_states_t accept_states{};
};

namespace
{
/**
 * @brief Each state's outgoing transitions, the marked symbol and the target, in ascending order of the symbol.
 */
using Successors = std::map<Verifier::State_t, std::vector<std::pair<Marked, Verifier::State_t>>>;

/**
 * @brief Each state's predecessors, one entry per transition into it.
 */
using Predecessors = std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>>;

/**
 * @brief Collects each state's outgoing transitions and sorts them by marked symbol.
 * @param transitions The transition table.
 * @return The successors of every state with an outgoing transition.
 */
[[nodiscard]] Successors successors_of(const Verifier::Transitions_t& transitions)
{
    Successors successors{};

    for (const auto& [key, to] : transitions)
    {
        successors[key.first].emplace_back(key.second, to);
    }

    for (auto& edges : successors | std::views::values)
    {
        std::ranges::sort(edges);
    }

    return successors;
}

/**
 * @brief Collects each state's predecessors.
 * @param transitions The transition table.
 * @return The predecessors of every state with an incoming transition.
 */
[[nodiscard]] Predecessors predecessors_of(const Verifier::Transitions_t& transitions)
{
    Predecessors predecessors{};

    for (const auto& [key, to] : transitions)
    {
        predecessors[to].push_back(key.first);
    }

    return predecessors;
}

/**
 * @brief The highest state identifier a table names: the start, the ends of every transition and the accepting states.
 * @param start The start state.
 * @param transitions The transition table.
 * @param accept_states The accepting states.
 * @return The highest identifier.
 */
[[nodiscard]] Verifier::State_t highest_state(
        const Verifier::State_t start, const Verifier::Transitions_t& transitions,
        const Verifier::Accept_states_t& accept_states)
{
    auto highest{start};

    for (const auto& [key, to] : transitions)
    {
        highest = std::max({highest, key.first, to});
    }

    for (const auto state : accept_states)
    {
        highest = std::max(highest, state);
    }

    return highest;
}

/**
 * @brief The states that can reach an accepting state, found by a backward search from the accepting states.
 * @param transitions The transition table.
 * @param accept_states The accepting states.
 * @return The coaccessible states.
 */
[[nodiscard]] std::unordered_set<Verifier::State_t> coaccessible(
        const Verifier::Transitions_t& transitions, const Verifier::Accept_states_t& accept_states)
{
    const auto predecessors{predecessors_of(transitions)};

    std::unordered_set<Verifier::State_t> live{accept_states};

    std::vector<Verifier::State_t> pending{accept_states.cbegin(), accept_states.cend()};

    while (!pending.empty())
    {
        const auto state{pending.back()};

        pending.pop_back();

        const auto entry{predecessors.find(state)};

        if (entry == predecessors.cend())
        {
            continue;
        }

        for (const auto from : entry->second)
        {
            if (live.insert(from).second)
            {
                pending.push_back(from);
            }
        }
    }

    return live;
}

/**
 * @brief A configuration of the armed-run automaton: the DFA state of the unarmed run, begun at the last boundary, and
 *        the DFA states of the armed runs, begun at earlier boundaries and still alive, sorted and without repeats.
 */
struct Configuration
{
    Dfa::State_t unarmed{};

    std::vector<Dfa::State_t> armed{};

    auto operator<=>(const Configuration&) const = default;
};

/**
 * @brief The armed-run automaton under exploration: the configuration each state reads as, the interned
 *        configurations with their states, and the transitions and accepting states found so far.
 *
 * State zero is the unconsumed start, which reads as the configuration of one fresh unarmed run and is not interned;
 * a step reaching that configuration reaches a state of its own.
 */
struct Exploration
{
    std::vector<Configuration> sources{};

    std::map<Configuration, Verifier::State_t> interned{};

    Verifier::Transitions_t transitions{};

    Verifier::Accept_states_t accept_states{};
};

/**
 * @brief Whether a DFA state accepts.
 * @param dfa The DFA.
 * @param state The state.
 * @return True when the state is accepting.
 */
[[nodiscard]] bool is_accepting(const Dfa& dfa, const Dfa::State_t state)
{
    return dfa.accept_states().contains(state);
}

/**
 * @brief Whether a configuration accepts: its unarmed run accepts and no armed run does.
 * @param dfa The DFA.
 * @param configuration The configuration.
 * @return True when the input may end in the configuration.
 */
[[nodiscard]] bool is_accepting(const Dfa& dfa, const Configuration& configuration)
{
    return is_accepting(dfa, configuration.unarmed) &&
           std::ranges::none_of(configuration.armed, [&dfa](const auto state) { return is_accepting(dfa, state); });
}

/**
 * @brief The bytes the DFA's transitions carry, ascending as unsigned bytes and without repeats.
 * @param dfa The DFA.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet_of(const Dfa& dfa)
{
    std::vector<unsigned char> alphabet{};

    for (const auto& key : dfa.transitions() | std::views::keys)
    {
        alphabet.push_back(static_cast<unsigned char>(key.second.symbol()));
    }

    std::ranges::sort(alphabet);

    const auto repeats{std::ranges::unique(alphabet)};

    alphabet.erase(repeats.begin(), repeats.end());

    return alphabet;
}

/**
 * @brief Reads one marked symbol into a configuration.
 *
 * The byte is read into every run, a run with no transition dropping. The step is refused when the unarmed run drops
 * or an armed run reaches an accepting state. On a boundary the unarmed run must accept, joins the armed runs, and a
 * fresh unarmed run begins at the DFA's initial state.
 * @param dfa The DFA.
 * @param from The configuration read from.
 * @param symbol The marked symbol.
 * @return The configuration after the symbol, or std::nullopt when the step is refused.
 */
[[nodiscard]] std::optional<Configuration> successor(const Dfa& dfa, const Configuration& from, const Marked symbol)
{
    const auto byte{static_cast<char>(symbol.byte)};
    const auto unarmed{dfa.advance(from.unarmed, byte)};

    if (!unarmed)
    {
        return std::nullopt;
    }

    Configuration next{.unarmed = *unarmed, .armed = {}};

    for (const auto state : from.armed)
    {
        const auto moved{dfa.advance(state, byte)};

        if (!moved)
        {
            continue;
        }

        if (is_accepting(dfa, *moved))
        {
            return std::nullopt;
        }

        next.armed.push_back(*moved);
    }

    if (symbol.boundary_after)
    {
        if (!is_accepting(dfa, next.unarmed))
        {
            return std::nullopt;
        }

        next.armed.push_back(next.unarmed);
        next.unarmed = dfa.init_state();
    }

    std::ranges::sort(next.armed);

    const auto repeats{std::ranges::unique(next.armed)};

    next.armed.erase(repeats.begin(), repeats.end());

    return next;
}

/**
 * @brief Returns the state of a configuration, interning it as a new state when it is first found.
 * @param dfa The DFA.
 * @param exploration The exploration, extended in place.
 * @param configuration The configuration.
 * @return The configuration's state.
 */
[[nodiscard]] Verifier::State_t intern(const Dfa& dfa, Exploration& exploration, Configuration configuration)
{
    const auto [entry, inserted]{exploration.interned.try_emplace(configuration, exploration.sources.size())};

    if (inserted)
    {
        if (is_accepting(dfa, configuration))
        {
            exploration.accept_states.insert(entry->second);
        }

        exploration.sources.push_back(std::move(configuration));
    }

    return entry->second;
}

/**
 * @brief Reads every marked symbol over the alphabet from one state, interning the configurations reached.
 * @param dfa The DFA.
 * @param alphabet The bytes read.
 * @param exploration The exploration, extended in place.
 * @param state The state read from.
 */
void expand(
        const Dfa& dfa, const std::vector<unsigned char>& alphabet, Exploration& exploration,
        const Verifier::State_t state)
{
    // The state's configuration, read from a copy while interning grows the sources.
    const auto from{exploration.sources[state]};

    for (const auto byte : alphabet)
    {
        for (const auto boundary_after : {false, true})
        {
            const Marked symbol{.byte = byte, .boundary_after = boundary_after};

            auto next{successor(dfa, from, symbol)};

            if (!next)
            {
                continue;
            }

            const auto to{intern(dfa, exploration, std::move(*next))};

            exploration.transitions.emplace(Verifier::Key_t{state, symbol}, to);
        }
    }
}

} // namespace

std::size_t Verifier::Hash::operator()(const Key_t& key) const noexcept
{
    std::size_t seed{};
    boost::hash_combine(seed, key.first);
    boost::hash_combine(seed, key.second.byte);
    boost::hash_combine(seed, key.second.boundary_after);
    return seed;
}

Verifier::Trimmed Verifier::trim(
        const State_t start, const Transitions_t& transitions, const Accept_states_t& accept_states)
{
    auto live{coaccessible(transitions, accept_states)};
    auto successors{successors_of(transitions)};

    // A transition into the start is redirected to a copy of the start, an identifier no state uses, which takes the
    // start's transitions and its acceptance.
    const auto copy{highest_state(start, transitions, accept_states) + 1};

    auto redirected{false};

    for (auto& edges : successors | std::views::values)
    {
        for (auto& to : edges | std::views::values)
        {
            if (to == start)
            {
                to = copy;
                redirected = true;
            }
        }
    }

    if (redirected)
    {
        successors[copy] = successors[start];

        if (live.contains(start))
        {
            live.insert(copy);
        }
    }

    std::unordered_map<State_t, State_t> renumbered{{start, 0}};

    std::vector<State_t> order{start};

    Trimmed trimmed{};

    for (std::size_t next{}; next < order.size(); ++next)
    {
        const auto edges{successors.find(order[next])};

        if (edges == successors.cend())
        {
            continue;
        }

        for (const auto& [symbol, to] : edges->second)
        {
            if (!live.contains(to))
            {
                continue;
            }

            const auto [entry, inserted]{renumbered.try_emplace(to, order.size())};

            if (inserted)
            {
                order.push_back(to);
            }

            trimmed.transitions.emplace(Key_t{next, symbol}, entry->second);
        }
    }

    for (const auto& [state, number] : renumbered)
    {
        if (accept_states.contains(state == copy ? start : state))
        {
            trimmed.accept_states.insert(number);
        }
    }

    trimmed.state_count = order.size();

    return trimmed;
}

Verifier::Verifier(const State_t start, Transitions_t transitions, Accept_states_t accept_states)
    : Verifier{trim(start, transitions, accept_states)}
{}

Verifier::Verifier(Trimmed parts)
    : start_{0}
    , state_count_{parts.state_count}
    , transitions_{std::move(parts.transitions)}
    , accept_states_{std::move(parts.accept_states)}
{}

Verifier::State_t Verifier::start() const noexcept
{
    return start_;
}

Verifier::State_t Verifier::state_count() const noexcept
{
    return state_count_;
}

std::optional<Verifier::State_t> Verifier::step(const State_t state, const Marked symbol) const
{
    const auto iterator{transitions_.find({state, symbol})};

    return iterator != transitions_.cend() ? std::optional{iterator->second} : std::nullopt;
}

bool Verifier::accepts(const State_t state) const
{
    return accept_states_.contains(state);
}

const Verifier::Transitions_t& Verifier::transitions() const noexcept
{
    return transitions_;
}

const Verifier::Accept_states_t& Verifier::accept_states() const noexcept
{
    return accept_states_;
}

Verifier armed_run(const Dfa& dfa)
{
    if (is_accepting(dfa, dfa.init_state()))
    {
        throw std::invalid_argument{"armed_run: the DFA's initial state accepts, so a token matches the empty string"};
    }

    const auto alphabet{alphabet_of(dfa)};

    Exploration exploration{
            .sources = {Configuration{.unarmed = dfa.init_state(), .armed = {}}},
            .interned = {},
            .transitions = {},
            .accept_states = {0},
    };

    for (Verifier::State_t state{}; state < exploration.sources.size(); ++state)
    {
        expand(dfa, alphabet, exploration, state);
    }

    return Verifier{0, std::move(exploration.transitions), std::move(exploration.accept_states)};
}

} // namespace munch::dfa

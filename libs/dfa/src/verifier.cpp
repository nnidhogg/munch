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
namespace
{
/**
 * @brief Each state's outgoing transitions, the marked symbol and the target, in ascending order of the symbol.
 */
using Successors_t = std::map<Verifier::State_t, std::vector<std::pair<Marked, Verifier::State_t>>>;

/**
 * @brief Each state's predecessors, one entry per transition into it.
 */
using Predecessors_t = std::unordered_map<Verifier::State_t, std::vector<Verifier::State_t>>;

/**
 * @brief A configuration of the armed-run automaton: the DFA state of the unarmed run, begun at the last boundary, and
 *        the DFA states of the armed runs, begun at earlier boundaries and still alive, sorted and without repeats.
 */
struct Configuration
{
    /**
     * @brief Ordered member by member, so configurations key the map of interned ones.
     */
    auto operator<=>(const Configuration&) const = default;

    /**
     * @brief The DFA state of the unarmed run, begun at the last boundary.
     */
    Dfa::State_t unarmed{};

    /**
     * @brief The DFA states of the armed runs, sorted and without repeats.
     */
    std::vector<Dfa::State_t> armed{};
};

/**
 * @brief The armed-run automaton under exploration: the configuration each state reads as, the interned configurations
 *        with their states, and the transitions and accepting states found so far.
 *
 * State zero is the unconsumed start, which reads as the configuration of one fresh unarmed run and is not interned; a
 * step reaching that configuration reaches a state of its own.
 */
struct Exploration
{
    /**
     * @brief The configuration each state reads as, by state.
     */
    std::vector<Configuration> sources{};

    /**
     * @brief The interned configurations with their states.
     */
    std::map<Configuration, Verifier::State_t> interned{};

    /**
     * @brief The transitions found so far.
     */
    Verifier::Transitions_t transitions{};

    /**
     * @brief The accepting states found so far.
     */
    Verifier::Accept_states_t accept_states{};
};

/**
 * @brief Sorts a list and drops the repeats.
 * @tparam T The element type.
 * @param values The list, sorted and without repeats on return.
 */
template <typename T>
void dedup(std::vector<T>& values)
{
    std::ranges::sort(values);

    const auto repeats{std::ranges::unique(values)};

    values.erase(repeats.begin(), repeats.end());
}

/**
 * @brief Collects each state's outgoing transitions and sorts them by marked symbol.
 * @param transitions The transition table.
 * @return The successors of every state with an outgoing transition.
 */
[[nodiscard]] Successors_t successors_of(const Verifier::Transitions_t& transitions)
{
    Successors_t successors{};

    for (const auto& [key, to] : transitions)
    {
        const auto& [from, symbol]{key};

        successors[from].emplace_back(symbol, to);
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
[[nodiscard]] Predecessors_t predecessors_of(const Verifier::Transitions_t& transitions)
{
    Predecessors_t predecessors{};

    for (const auto& [key, to] : transitions)
    {
        const auto& [from, symbol]{key};

        predecessors[to].push_back(from);
    }

    return predecessors;
}

/**
 * @brief Returns the highest state identifier a table names: the start, the ends of every transition and the accepting
 *        states.
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
        const auto& [from, symbol]{key};

        highest = std::max({highest, from, to});
    }

    for (const auto state : accept_states)
    {
        highest = std::max(highest, state);
    }

    return highest;
}

/**
 * @brief Returns the states that can reach an accepting state, found by a backward search from the accepting states.
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

        const auto& [to, sources]{*entry};

        for (const auto from : sources)
        {
            const auto [position, inserted]{live.insert(from)};

            if (inserted)
            {
                pending.push_back(from);
            }
        }
    }

    return live;
}

/**
 * @brief Returns whether a DFA state accepts.
 * @param dfa The DFA.
 * @param state The state.
 * @return True when the state is accepting.
 */
[[nodiscard]] bool is_accepting(const Dfa& dfa, const Dfa::State_t state)
{
    return dfa.accept_states().contains(state);
}

/**
 * @brief Returns whether a configuration accepts: its unarmed run accepts and no armed run does.
 * @param dfa The DFA.
 * @param configuration The configuration.
 * @return True when the input may end in the configuration.
 */
[[nodiscard]] bool is_accepting(const Dfa& dfa, const Configuration& configuration)
{
    const auto& [unarmed, armed]{configuration};

    /**
     * @brief Returns whether a DFA state accepts.
     * @param state The state.
     * @return True when it does.
     */
    const auto accepts{[&dfa](const Dfa::State_t state) { return is_accepting(dfa, state); }};

    return is_accepting(dfa, unarmed) && std::ranges::none_of(armed, accepts);
}

/**
 * @brief Returns the bytes the DFA's transitions carry, ascending as unsigned bytes and without repeats.
 * @param dfa The DFA.
 * @return The alphabet.
 */
[[nodiscard]] std::vector<unsigned char> alphabet_of(const Dfa& dfa)
{
    std::vector<unsigned char> alphabet{};

    for (const auto& [from, label] : dfa.transitions() | std::views::keys)
    {
        alphabet.push_back(static_cast<unsigned char>(label.symbol()));
    }

    dedup(alphabet);

    return alphabet;
}

/**
 * @brief Reads one marked symbol into a configuration.
 *
 * The byte is read into every run, a run with no transition dropping. The step is refused when the unarmed run drops or
 * an armed run reaches an accepting state. On a boundary the unarmed run must accept, joins the armed runs, and a fresh
 * unarmed run begins at the DFA's initial state.
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

    if (symbol.boundary_after && !is_accepting(dfa, next.unarmed))
    {
        return std::nullopt;
    }

    if (symbol.boundary_after)
    {
        next.armed.push_back(next.unarmed);

        next.unarmed = dfa.init_state();
    }

    dedup(next.armed);

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

    const auto& [key, state]{*entry};

    if (!inserted)
    {
        return state;
    }

    if (is_accepting(dfa, configuration))
    {
        exploration.accept_states.insert(state);
    }

    exploration.sources.push_back(std::move(configuration));

    return state;
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

/**
 * @brief The parts of a trimmed table: the state count, the transitions and the accepting states, the start numbered
 *        zero.
 */
struct Verifier::Trimmed
{
    /**
     * @brief The number of states kept.
     */
    State_t state_count{};

    /**
     * @brief The transitions between the kept states, renumbered.
     */
    Transitions_t transitions{};

    /**
     * @brief The kept accepting states, renumbered.
     */
    Accept_states_t accept_states{};
};

std::size_t Verifier::Hash::operator()(const Key_t& key) const noexcept
{
    const auto& [state, symbol]{key};

    std::size_t seed{};

    boost::hash_combine(seed, state);

    boost::hash_combine(seed, symbol.byte);

    boost::hash_combine(seed, symbol.boundary_after);

    return seed;
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

Verifier::Trimmed Verifier::trim(
        const State_t start, const Transitions_t& transitions, const Accept_states_t& accept_states)
{
    auto live{coaccessible(transitions, accept_states)};

    auto successors{successors_of(transitions)};

    // A transition into the start is redirected to a copy of the start, an identifier no state uses, which takes the
    // start's transitions and its acceptance.
    const auto copy{highest_state(start, transitions, accept_states) + 1};

    auto redirected{false};

    for (auto& to : successors | std::views::values | std::views::join | std::views::values)
    {
        if (to == start)
        {
            to = copy;

            redirected = true;
        }
    }

    if (redirected)
    {
        successors[copy] = successors[start];
    }

    if (redirected && live.contains(start))
    {
        live.insert(copy);
    }

    std::unordered_map<State_t, State_t> renumbered{{start, 0}};

    std::vector<State_t> order{start};

    Trimmed trimmed{};

    for (std::size_t next{0}; next < order.size(); ++next)
    {
        const auto found{successors.find(order[next])};

        if (found == successors.cend())
        {
            continue;
        }

        const auto& [from, edges]{*found};

        for (const auto& [symbol, to] : edges)
        {
            if (!live.contains(to))
            {
                continue;
            }

            const auto [entry, inserted]{renumbered.try_emplace(to, order.size())};

            const auto& [original, number]{*entry};

            if (inserted)
            {
                order.push_back(to);
            }

            trimmed.transitions.emplace(Key_t{next, symbol}, number);
        }
    }

    for (const auto& [state, number] : renumbered)
    {
        const auto named{state == copy ? start : state};

        if (accept_states.contains(named))
        {
            trimmed.accept_states.insert(number);
        }
    }

    trimmed.state_count = order.size();

    return trimmed;
}

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
    const auto found{transitions_.find({state, symbol})};

    if (found == transitions_.cend())
    {
        return std::nullopt;
    }

    const auto& [key, to]{*found};

    return to;
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

    for (Verifier::State_t state{0}; state < exploration.sources.size(); ++state)
    {
        expand(dfa, alphabet, exploration, state);
    }

    return Verifier{0, std::move(exploration.transitions), std::move(exploration.accept_states)};
}

} // namespace munch::dfa

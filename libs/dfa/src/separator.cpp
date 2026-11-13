#include "munch/dfa/separator.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace munch::dfa
{
namespace
{
/**
 * @brief The first step of a shortest route from a state to the end of a walk, with the route's length.
 *
 * A forward walk ends at the initial state it starts from, so its routes lead back along the access word; a backward
 * walk ends at one of its targets, so its routes lead on along a completion.
 */
struct Hop
{
    /**
     * @brief The number of bytes the route reads, zero at the end of the walk.
     */
    std::size_t length{};

    /**
     * @brief The byte of the step, meaningless at the end of the walk.
     */
    unsigned char byte{};

    /**
     * @brief The state at the other end of the step, the state itself at the end of the walk.
     */
    std::size_t next{};
};

/**
 * @brief Per state, the first step of its shortest route to the end of a walk, nothing where the walk never reaches it.
 */
using Routes_t = std::vector<std::optional<Hop>>;

/**
 * @brief A transition as the backward walks read it: the byte and the state it leaves.
 */
struct Edge
{
    /**
     * @brief The byte the transition reads.
     */
    unsigned char byte{};

    /**
     * @brief The state the transition leaves.
     */
    std::size_t from{};
};

/**
 * @brief Marks the states accepting a kind the caller keeps.
 * @param simulator The compiled token set.
 * @param ignored The IDs of the discarded tokens.
 * @return One flag per state, set where the state accepts a token whose ID is not discarded.
 */
[[nodiscard]] std::vector<bool> kept_states(const Simulator& simulator, const std::span<const std::size_t> ignored)
{
    std::vector<bool> kept(simulator.state_count(), false);

    for (std::size_t state{0}; state < kept.size(); ++state)
    {
        const auto token{simulator.accepted(state)};

        kept[state] = token.has_value() && !std::ranges::contains(ignored, token->id());
    }

    return kept;
}

/**
 * @brief Returns the lowest byte of every symbol class, the bytes a walk over the table reads.
 * @param simulator The compiled token set.
 * @return The bytes, ascending.
 */
[[nodiscard]] std::vector<unsigned char> class_bytes(const Simulator& simulator)
{
    std::vector<unsigned char> bytes{};

    for (const auto& members : simulator.symbol_classes())
    {
        bytes.push_back(members.front());
    }

    return bytes;
}

/**
 * @brief Walks forward from the initial state, breadth first, recording every reachable state's shortest access word.
 * @param simulator The compiled token set.
 * @param bytes The bytes the walk reads, one per symbol class and ascending, so that each word is the first in byte
 *        order among the shortest.
 * @return Per state, the last step of its access word, leading back toward the initial state; nothing where no input
 *         reaches the state.
 */
[[nodiscard]] Routes_t access_routes(const Simulator& simulator, const std::vector<unsigned char>& bytes)
{
    Routes_t routes(simulator.state_count());

    const auto init{simulator.init_state()};

    routes[init] = Hop{.length = 0, .byte = 0, .next = init};

    std::deque frontier{init};

    while (!frontier.empty())
    {
        const auto state{frontier.front()};

        frontier.pop_front();

        const auto length{routes[state]->length + 1};

        for (const auto byte : bytes)
        {
            const auto target{simulator.step(state, byte)};

            if (!target || routes[*target])
            {
                continue;
            }

            routes[*target] = Hop{.length = length, .byte = byte, .next = state};

            frontier.push_back(*target);
        }
    }

    return routes;
}

/**
 * @brief Returns the reverse index of the table: for each state, the transitions stepping into it.
 * @param simulator The compiled token set.
 * @param bytes The bytes the walks read, one per symbol class and ascending.
 * @return Per state, the transitions into it, ordered by the state they leave and then by byte.
 */
[[nodiscard]] std::vector<std::vector<Edge>> predecessors(
        const Simulator& simulator, const std::vector<unsigned char>& bytes)
{
    std::vector<std::vector<Edge>> into(simulator.state_count());

    for (std::size_t state{0}; state < into.size(); ++state)
    {
        for (const auto byte : bytes)
        {
            const auto target{simulator.step(state, byte)};

            if (target)
            {
                into[*target].push_back(Edge{.byte = byte, .from = state});
            }
        }
    }

    return into;
}

/**
 * @brief Marks the states accepting some token, of whatever kind.
 * @param simulator The compiled token set.
 * @return One flag per state, set where the state accepts.
 */
[[nodiscard]] std::vector<bool> accepting_states(const Simulator& simulator)
{
    std::vector<bool> accepts(simulator.state_count(), false);

    for (std::size_t state{0}; state < accepts.size(); ++state)
    {
        accepts[state] = simulator.is_accepting(state);
    }

    return accepts;
}

/**
 * @brief Walks backward from a set of target states, breadth first, recording every state's shortest completion into
 *        one of them.
 * @param into The reverse index of the table.
 * @param targets One flag per state, set where the walk ends.
 * @return Per state, the first step of its shortest completion, leading on toward a target; nothing where no target is
 *         reachable from the state.
 */
[[nodiscard]] Routes_t completion_routes(const std::vector<std::vector<Edge>>& into, const std::vector<bool>& targets)
{
    Routes_t routes(into.size());

    std::deque<std::size_t> frontier{};

    for (std::size_t state{0}; state < targets.size(); ++state)
    {
        if (targets[state])
        {
            routes[state] = Hop{.length = 0, .byte = 0, .next = state};

            frontier.push_back(state);
        }
    }

    while (!frontier.empty())
    {
        const auto state{frontier.front()};

        frontier.pop_front();

        const auto length{routes[state]->length + 1};

        for (const auto& [byte, from] : into[state])
        {
            if (routes[from])
            {
                continue;
            }

            routes[from] = Hop{.length = length, .byte = byte, .next = state};

            frontier.push_back(from);
        }
    }

    return routes;
}

/**
 * @brief Spells the shortest access word of a reachable state from the forward walk's routes.
 * @param access The forward walk's routes.
 * @param state The state, one the walk reached.
 * @return The word, the bytes from the initial state to the state.
 */
[[nodiscard]] std::string access_word(const Routes_t& access, const std::size_t state)
{
    std::string word{};

    for (auto at{state}; access[at]->length > 0; at = access[at]->next)
    {
        word.push_back(static_cast<char>(access[at]->byte));
    }

    std::ranges::reverse(word);

    return word;
}

/**
 * @brief Spells the shortest completion of a state from a backward walk's routes.
 * @param completions The backward walk's routes.
 * @param state The state, one from which a target is reachable.
 * @return The word, the bytes from the state to a target.
 */
[[nodiscard]] std::string completion_word(const Routes_t& completions, const std::size_t state)
{
    std::string word{};

    for (auto at{state}; completions[at]->length > 0; at = completions[at]->next)
    {
        word.push_back(static_cast<char>(completions[at]->byte));
    }

    return word;
}

/**
 * @brief Finds a shortest token spelling beginning with a kept spelling followed by the byte, the second clause's
 *        failure.
 *
 * Every reachable state accepting a kept kind is the end of a kept spelling, and a spelling begins with that spelling
 * and the byte exactly when the byte leads from the state into one from which acceptance is reachable. The shortest
 * such spelling over every state is the state's access word, the byte and the completion after it, the first in state
 * order on a tie.
 * @param simulator The compiled token set.
 * @param symbol The byte the kept tokens are joined by.
 * @param kept The states accepting a kept kind.
 * @param access The forward walk's routes.
 * @param accepting The routes of the backward walk from the accepting states.
 * @return The failure with its kept spelling and token spelling, or std::nullopt when the clause holds.
 */
[[nodiscard]] std::optional<Separator_failure> after_kept_failure(
        const Simulator& simulator, const char symbol, const std::vector<bool>& kept, const Routes_t& access,
        const Routes_t& accepting)
{
    const auto byte{static_cast<unsigned char>(symbol)};

    std::optional<std::pair<std::size_t, std::size_t>> shortest{};

    for (std::size_t state{0}; state < kept.size(); ++state)
    {
        if (!kept[state] || !access[state])
        {
            continue;
        }

        const auto next{simulator.step(state, byte)};

        if (!next || !accepting[*next])
        {
            continue;
        }

        const auto length{access[state]->length + 1 + accepting[*next]->length};

        const std::pair candidate{length, state};

        if (!shortest || candidate < *shortest)
        {
            shortest = candidate;
        }
    }

    if (!shortest)
    {
        return std::nullopt;
    }

    const auto [length, state]{*shortest};

    const auto prefix{access_word(access, state)};

    const auto next{*simulator.step(state, byte)};

    const auto completion{completion_word(accepting, next)};

    const auto spelling{prefix + symbol + completion};

    return Separator_failure{.clause = Separator_clause::after_kept, .kept = prefix, .spelling = spelling};
}

/**
 * @brief Finds a shortest token spelling beginning with the byte followed by the first byte of a kept spelling, the
 *        third clause's failure.
 *
 * A byte b begins a kept spelling exactly when its state from the initial one reaches a state accepting a kept kind,
 * and a spelling begins with the separator and b exactly when b leads from the separator's state into one from which
 * acceptance is reachable. The shortest such spelling is the two bytes and the completion after them, the first in byte
 * order on a tie, and the kept spelling named beside it is b and its shortest completion into a kept kind.
 * @param simulator The compiled token set.
 * @param alone The state the separator leads to from the initial one.
 * @param symbol The byte the kept tokens are joined by.
 * @param accepting The routes of the backward walk from the accepting states.
 * @param keeping The routes of the backward walk from the states accepting a kept kind.
 * @return The failure with its kept spelling and token spelling, or std::nullopt when the clause holds.
 */
[[nodiscard]] std::optional<Separator_failure> before_kept_failure(
        const Simulator& simulator, const std::size_t alone, const char symbol, const Routes_t& accepting,
        const Routes_t& keeping)
{
    const auto init{simulator.init_state()};

    std::optional<std::pair<std::size_t, unsigned char>> shortest{};

    for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
    {
        const auto byte{static_cast<unsigned char>(value)};

        const auto first{simulator.step(init, byte)};

        if (!first || !keeping[*first])
        {
            continue;
        }

        const auto next{simulator.step(alone, byte)};

        if (!next || !accepting[*next])
        {
            continue;
        }

        const auto length{2 + accepting[*next]->length};

        const std::pair candidate{length, byte};

        if (!shortest || candidate < *shortest)
        {
            shortest = candidate;
        }
    }

    if (!shortest)
    {
        return std::nullopt;
    }

    const auto [length, byte]{*shortest};

    const auto letter{static_cast<char>(byte)};

    const auto first{*simulator.step(init, byte)};

    const auto next{*simulator.step(alone, byte)};

    const auto kept_completion{completion_word(keeping, first)};

    const auto kept{letter + kept_completion};

    const auto completion{completion_word(accepting, next)};

    const auto spelling{std::string{symbol, letter} + completion};

    return Separator_failure{.clause = Separator_clause::before_kept, .kept = kept, .spelling = spelling};
}

} // namespace

Separator_failure separator_failure(
        const Simulator& simulator, const char symbol, const std::span<const std::size_t> ignored)
{
    const auto kept{kept_states(simulator, ignored)};

    const auto byte{static_cast<unsigned char>(symbol)};

    const auto alone{simulator.step(simulator.init_state(), byte)};

    const auto discarded_alone{alone && simulator.is_accepting(*alone) && !kept[*alone]};

    if (!discarded_alone)
    {
        return {.clause = Separator_clause::not_discarded, .kept = {}, .spelling = std::string{symbol}};
    }

    const auto bytes{class_bytes(simulator)};

    const auto access{access_routes(simulator, bytes)};

    const auto into{predecessors(simulator, bytes)};

    const auto accepts{accepting_states(simulator)};

    const auto accepting{completion_routes(into, accepts)};

    auto after{after_kept_failure(simulator, symbol, kept, access, accepting)};

    if (after)
    {
        return std::move(*after);
    }

    const auto keeping{completion_routes(into, kept)};

    auto before{before_kept_failure(simulator, *alone, symbol, accepting, keeping)};

    if (before)
    {
        return std::move(*before);
    }

    return {.clause = std::nullopt, .kept = {}, .spelling = {}};
}

} // namespace munch::dfa

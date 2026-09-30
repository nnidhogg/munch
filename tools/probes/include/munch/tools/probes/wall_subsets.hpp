#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_SUBSETS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_SUBSETS_HPP

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "munch/tools/probes/wall_table.hpp"

/**
 * @brief The zero-lag premise and the subset graph it licenses, with the floors that bound the wall from below,
 *        Premise, zero_lag, Subset_graph, subset_graph, floors, Verdict and decide.
 */
namespace munch::tools::probes
{
/**
 * @brief The zero-lag premise's answer: once a run accepts, every later state accepts, so every rollback, at a byte or
 *        at the end of input, is zero bytes wide.
 */
struct Premise
{
    /**
     * @brief Whether the premise holds.
     */
    bool holds{true};

    /**
     * @brief The first state a run that accepted earlier reaches without accepting, when the premise fails.
     */
    std::size_t witness_state{};

    /**
     * @brief The death that exposes the stale accept at the witness state, 256 for the end of input.
     */
    int witness_byte{};
};

/**
 * @brief Decides the zero-lag premise by a breadth-first walk of the (state, seen-accept) product from the initial
 *        state, seen recording an accept at or before the current position; the end of input is a death available at
 *        every position, so a seen run at a non-accepting state is a violation.
 * @param table The table.
 * @return The premise, with the first violation the walk meets when it fails.
 */
[[nodiscard]] Premise zero_lag(const Table& table);

/**
 * @brief The subset graph under the bare-state dynamics the zero-lag premise licenses: each node a set of live
 *        hypotheses, node 0 the live states, and an edge per byte on which members advance through their transitions, a
 *        dying accepting member restarts through the initial state's transition on the byte, and a dying non-accepting
 *        member is eliminated.
 */
struct Subset_graph
{
    /**
     * @brief Per node, its member states, ascending.
     */
    std::vector<std::vector<std::size_t>> subsets{};

    /**
     * @brief Per node, the node every byte leads to.
     */
    std::vector<std::array<std::size_t, 256>> successor{};
};

/**
 * @brief Builds the subset graph from the live states, nodes numbered in the order a breadth-first walk, bytes
 *        ascending, first meets them.
 * @param table The table.
 * @param budget The most nodes the graph may hold.
 * @return The graph, std::nullopt as soon as it holds more nodes than the budget.
 */
[[nodiscard]] std::optional<Subset_graph> subset_graph(const Table& table, std::size_t budget);

/**
 * @brief The floor of every node, the least width its forward closure reaches, by reverse propagation from the nodes in
 *        ascending width: the first propagation to touch a node carries the smallest width it reaches, so every node
 *        and reverse edge is visited once.
 * @param graph The subset graph.
 * @return Per node, its floor.
 */
[[nodiscard]] std::vector<std::size_t> floors(const Subset_graph& graph);

/**
 * @brief The wall verdict of a table's subset graph, whose quantities are state-granular lower bounds on
 *        origin-distinguished readings.
 */
struct Verdict
{
    /**
     * @brief The subset graph's node count.
     */
    std::size_t nodes{};

    /**
     * @brief The largest width of a node on a cycle.
     */
    std::size_t sustained{};

    /**
     * @brief The floor of the start node.
     */
    std::size_t floor_start{};

    /**
     * @brief The largest floor of a node the start node reaches or is: two or more is an absolute wall.
     */
    std::size_t wall_floor{};

    /**
     * @brief Whether the graph exceeded the decider's budget of 128 nodes, every other field then zero.
     */
    bool bounded{};
};

/**
 * @brief Decides a table's wall from its subset graph. A direct arrival and a restart arrival at one state merge
 *        although their tokens began at different places, so a wall floor of two or more is a real wall, while a
 *        smaller floor does not conclude the absence of an origin-level wall.
 * @param table The table.
 * @return The verdict, bounded when the graph holds more than 128 nodes.
 */
[[nodiscard]] Verdict decide(const Table& table);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_WALL_SUBSETS_HPP

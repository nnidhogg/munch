#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_HPP

#include <compare>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "munch/dfa/dfa.hpp"

namespace munch::dfa
{
/**
 * @brief One symbol a verifier reads: a byte, and whether a token boundary follows it.
 *
 * A marked string is an input with its boundary marking, one symbol per byte. The boundary before the first byte and
 * the one after the last are implicit, so the final symbol of a segmentation carries no boundary.
 */
struct Marked
{
    /**
     * @brief Orders marked symbols by byte, then by the boundary bit.
     */
    auto operator<=>(const Marked&) const = default;

    /**
     * @brief The byte read, as an unsigned value; the DFA's labels carry it as a char.
     */
    unsigned char byte{};

    /**
     * @brief Whether a token boundary follows the byte.
     */
    bool boundary_after{};
};

/**
 * @brief A deterministic, trim automaton over marked symbols that accepts exactly the segmentations of a policy.
 *
 * The verifier accepts a marked string exactly when the marking is the policy's segmentation of the bytes, so its
 * projection to bytes is the policy's domain. It starts in one unconsumed state, distinct from every state a step
 * reaches, which accepts exactly when the empty input is in the domain. Trim means every state is reachable from the
 * start and can reach an accepting state; the constructor trims the table it is given, keeping the start in every case,
 * and numbers the states it keeps densely from zero, the start first and the rest in breadth-first order over the
 * marked symbols in ascending order.
 */
class Verifier
{
public:
    /**
     * @brief Type representing a verifier state identifier.
     */
    using State_t = std::size_t;

    /**
     * @brief Verifier transition key type: a state and the marked symbol read from it.
     */
    using Key_t = std::pair<State_t, Marked>;

    /**
     * @brief Hash functor for computing transition key hashes.
     */
    struct Hash
    {
        /**
         * @brief Computes the hash of a transition key.
         * @param key The transition key to hash.
         * @return The hash value.
         */
        std::size_t operator()(const Key_t& key) const noexcept;
    };

    /**
     * @brief Transition table for the verifier, from a state and a marked symbol to a state.
     */
    using Transitions_t = std::unordered_map<Key_t, State_t, Hash>;

    /**
     * @brief The accepting states of the verifier.
     */
    using Accept_states_t = std::unordered_set<State_t>;

    /**
     * @brief Constructs the trim verifier of the given table.
     *
     * Keeps the states reachable from the start that can reach an accepting state, and the start itself whether or not
     * it can, then renumbers them densely from zero with the start at zero. A transition is kept when both of its ends
     * are, and an accepting state when it is kept. A transition into the start is redirected to a copy of the start,
     * with the start's transitions and acceptance, so that no kept transition enters the start.
     * @param start The start state.
     * @param transitions The transition table.
     * @param accept_states The accepting states.
     */
    Verifier(State_t start, Transitions_t transitions, Accept_states_t accept_states);

    /**
     * @brief Returns the start state, the unconsumed one.
     * @return The start state identifier, zero.
     */
    [[nodiscard]] State_t start() const noexcept;

    /**
     * @brief Returns the number of states, which are numbered densely from zero.
     * @return The state count.
     */
    [[nodiscard]] State_t state_count() const noexcept;

    /**
     * @brief Advances the verifier from a state on a marked symbol.
     * @param state The current state.
     * @param symbol The marked symbol read.
     * @return The next state if a transition exists, otherwise std::nullopt.
     */
    [[nodiscard]] std::optional<State_t> step(State_t state, Marked symbol) const;

    /**
     * @brief Returns whether a state accepts.
     * @param state The state to check.
     * @return True if the state is accepting.
     */
    [[nodiscard]] bool accepts(State_t state) const;

    /**
     * @brief Returns the transition table of the trim verifier.
     * @return Reference to the transitions map.
     */
    [[nodiscard]] const Transitions_t& transitions() const noexcept;

    /**
     * @brief Returns the accepting states of the trim verifier.
     * @return Reference to the accepting states.
     */
    [[nodiscard]] const Accept_states_t& accept_states() const noexcept;

private:
    /**
     * @brief The parts of a trimmed table: the state count, the transitions and the accepting states, the start
     *        numbered zero.
     */
    struct Trimmed;

    /**
     * @brief Constructs a verifier from trimmed parts.
     * @param parts The trimmed parts.
     */
    explicit Verifier(Trimmed parts);

    /**
     * @brief Trims a table to the start and the states reachable from it that can reach acceptance, renumbered densely
     *        from zero in breadth-first order with the start first and a copy of the start taking its incoming
     *        transitions.
     * @param start The start state.
     * @param transitions The transition table.
     * @param accept_states The accepting states.
     * @return The trimmed parts.
     */
    [[nodiscard]] static Trimmed trim(
            State_t start, const Transitions_t& transitions, const Accept_states_t& accept_states);

    /**
     * @brief The start state, zero.
     */
    State_t start_{};

    /**
     * @brief The number of states kept.
     */
    State_t state_count_{};

    /**
     * @brief The transitions, from a state and a marked symbol to a state.
     */
    Transitions_t transitions_;

    /**
     * @brief The accepting states.
     */
    Accept_states_t accept_states_;
};

/**
 * @brief Builds the armed-run automaton of a DFA: the verifier of maximal munch over the DFA's tokens.
 *
 * A configuration is the DFA state of the unarmed run, begun at the last boundary, and the set of DFA states of the
 * armed runs, begun at earlier boundaries and still alive. A step reads the byte into every run, a run with no
 * transition dropping; the step is refused when an armed run reaches an accepting state or the unarmed run drops. On a
 * boundary the unarmed run must be accepting, joins the armed set, and a fresh unarmed run begins at the DFA's initial
 * state. A configuration accepts when its unarmed run accepts and no armed run does. The unconsumed start state is its
 * own state, accepting, and a step from it reads the byte as the configuration of one fresh unarmed run does.
 *
 * Built by exploration from the start over the bytes the DFA's transitions carry, each configuration interned as it is
 * found, so that the states are the reachable configurations only; the verifier's constructor then trims the result by
 * coaccessibility. The DFA's tokens are not read: "some token accepts" is the DFA state accepting.
 * @param dfa The DFA of the token set, whose initial state does not accept.
 * @return The armed-run verifier.
 * @throws std::invalid_argument If the DFA's initial state accepts.
 */
[[nodiscard]] Verifier armed_run(const Dfa& dfa);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_VERIFIER_HPP

#ifndef MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_HPP
#define MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_HPP

#include <compare>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "munch/dfa/dfa.hpp"

namespace munch::verifier
{
/**
 * @brief One symbol a verifier reads: a byte, and whether a token boundary follows it.
 *
 * A marked string is an input with its boundary marking, one symbol per byte. The boundary before the first byte and
 * the one after the last are implicit, so the final symbol of a segmentation carries no boundary.
 *
 * Every position, length, origin, cut and context a decision takes or returns counts marked symbols. Over a verifier of
 * bytes, such as armed_run()'s, that is bytes; over a table whose symbols each stand for a class of codepoints, one
 * symbol per codepoint, it is codepoints; and over the character overloads of merge_tower(), token_automaton() and
 * ripple_radius(), which read each character as its UTF-8 bytes, it is UTF-8 bytes.
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
     * @brief The kind of the segment each marked step closes, by the state it is read from and its symbol, whose
     *        boundary bit is set.
     */
    using Close_kinds_t = std::unordered_map<Key_t, std::size_t, Hash>;

    /**
     * @brief The kind of the last segment at each accepting state but the start, where the empty input has no segment.
     */
    using Final_kinds_t = std::unordered_map<State_t, std::size_t>;

    /**
     * @brief The kinds a verifier records: of the segment each marked step closes, and of the last segment at each
     *        accepting state.
     *
     * A kind is a number the policy gives a segment, under armed_run() the token maximal munch emits for it.
     * modulo_dependence(), which deletes the discarded kinds from the segmentations it compares, reads them; the other
     * decisions read the marking alone.
     */
    struct Kinds
    {
        /**
         * @brief The kind of the segment each marked step closes.
         */
        Close_kinds_t closes;

        /**
         * @brief The kind of the last segment at each accepting state.
         */
        Final_kinds_t finals;
    };

    /**
     * @brief Constructs the trim verifier of the given table.
     *
     * Keeps the states reachable from the start that can reach an accepting state, and the start itself whether or not
     * it can, then renumbers them densely from zero with the start at zero. A transition is kept when both of its ends
     * are, and an accepting state when it is kept. A transition into the start is redirected to a copy of the start,
     * with the start's transitions, acceptance and kinds, so that no kept transition enters the start. The kinds of the
     * kept steps and states are kept with them; a table given without kinds records none.
     * @param start The start state.
     * @param transitions The transition table.
     * @param accept_states The accepting states.
     * @param kinds The kinds of the segments the marked steps close and of the last segments at the accepting states.
     */
    Verifier(State_t start, Transitions_t transitions, Accept_states_t accept_states, Kinds kinds = {});

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
     * @brief Returns the kind of the segment that closes when a byte is read from a state with a boundary after it.
     * @param state The state read from.
     * @param byte The byte read.
     * @return The kind, or std::nullopt where the verifier records none for that step or the step does not exist.
     */
    [[nodiscard]] std::optional<std::size_t> close_kind(State_t state, unsigned char byte) const;

    /**
     * @brief Returns the kind of the last segment of an input that ends in a state.
     * @param state The state, accepting.
     * @return The kind, or std::nullopt where the verifier records none, as at the start, where the empty input has no
     *         segment.
     */
    [[nodiscard]] std::optional<std::size_t> final_kind(State_t state) const;

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
     * @brief The parts of a trimmed table: the state count, the transitions, the accepting states and the kinds, the
     *        start numbered zero.
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
     *        transitions, the kept steps and states keeping their kinds.
     * @param start The start state.
     * @param transitions The transition table.
     * @param accept_states The accepting states.
     * @param kinds The kinds of the table's steps and states.
     * @return The trimmed parts.
     */
    [[nodiscard]] static Trimmed trim(
            State_t start, const Transitions_t& transitions, const Accept_states_t& accept_states, const Kinds& kinds);

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

    /**
     * @brief The kinds of the segments the marked steps close and of the last segments at the accepting states.
     */
    Kinds kinds_;
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
 * coaccessibility. Acceptance reads no token, "some token accepts" being the DFA state accepting, and the kinds read
 * the tokens: the segment a marked step closes is of the token the unarmed run's DFA state accepts after the byte, and
 * the last segment at an accepting state of the token its unarmed run's state accepts, which is the token maximal munch
 * emits for the segment, the start recording none.
 * @param dfa The DFA of the token set, whose initial state does not accept.
 * @return The armed-run verifier.
 * @throws std::invalid_argument If the DFA's initial state accepts.
 */
[[nodiscard]] Verifier armed_run(const dfa::Dfa& dfa);

} // namespace munch::verifier

#endif // MUNCH_LIBS_VERIFIER_INCLUDE_MUNCH_VERIFIER_VERIFIER_HPP

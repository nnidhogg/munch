#ifndef MUNCH_PAPER_FIGURES_MODULO_HPP
#define MUNCH_PAPER_FIGURES_MODULO_HPP

/*
 * The condition modulo discarded tokens as the report published it, and the exact decision the report proves, both
 * evaluated on the compiled tables through the simulator's view. Shared by the figure programs so that the sweep and
 * the applicability table judge the published condition by one copy of it.
 *
 * The published condition is a copy because the library does not ship it: the rule it ships is the stronger one the
 * report describes beside it, read through Lexer::is_split_point_ignoring(). The exact decision is the report's
 * construction as written: the complete-scan automaton over obligation sets, the failure automaton beside it, the two
 * chunk-completeness products, the kept-output product under the z^l K_t coding, and the finite output-equality test
 * with group delays. A negative answer carries an input and a cut, which decide_exactly() replays through the real
 * scanner before reporting it. Exhausting the budget is reported as such and never as an answer.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/simulator.hpp"

namespace figures
{
/**
 * @brief The column entry of a byte no transition consumes.
 */
inline constexpr int no_transition{-1};

/**
 * @brief The most product vertices the exact decision builds before it reports the budget exhausted.
 */
inline constexpr std::size_t decision_budget{std::size_t{1} << 22U};

/**
 * @brief What a replay names when the scanner leaves the left chunk unconsumed.
 */
inline constexpr std::string_view left_incomplete{"left chunk incomplete"};

/**
 * @brief What a replay names when the scanner leaves the right chunk unconsumed.
 */
inline constexpr std::string_view right_incomplete{"right chunk incomplete"};

/**
 * @brief What a replay names when both chunks scan completely and their kept streams differ from the whole input's.
 */
inline constexpr std::string_view streams_differ{"kept streams differ"};

/**
 * @brief The label a product path carries for the cut, between the bytes it spells.
 */
inline constexpr int cut_marker{-1};

/**
 * @brief The phase of a marked input u # v before any byte is read.
 */
inline constexpr int phase_start{0};

/**
 * @brief The phase inside a nonempty u.
 */
inline constexpr int phase_left{1};

/**
 * @brief The phase just past the marker, where the next byte must be the candidate.
 */
inline constexpr int phase_marked{2};

/**
 * @brief The phase inside v past the candidate.
 */
inline constexpr int phase_right{3};

/**
 * @brief Evaluates the relaxed condition as the report published it on the compiled tables.
 *
 * A state consuming the symbol live must be the non-re-entrant initial state, or accept a discarded token, reach no
 * kept token, and advance on the symbol to the very state the initial state advances to. Vacuous certificates are
 * withheld as the shipped predicates withhold them.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param symbol The symbol.
 * @return True when the published condition admits the symbol.
 */
inline bool published_condition(const munch::core::Lexer& lexer, const Kinds_t& ignored, const unsigned char symbol)
{
    const auto& simulator{lexer.simulator()};

    const auto states{simulator.state_count()};

    const auto discarded{[&](const std::size_t state) {
        const auto token{simulator.accepted(state)};

        return token.has_value() && ignored.contains(token->id());
    }};

    std::vector<bool> reaches_kept(states, false);

    const auto reaches_a_kept_accept{[&](const std::size_t state) {
        if (simulator.is_accepting(state) && !discarded(state))
        {
            return true;
        }

        const auto steps_into_reaching{[&](const std::size_t byte) {
            const auto to{simulator.step(state, static_cast<unsigned char>(byte))};

            return to.has_value() && reaches_kept[*to];
        }};

        const auto bytes{std::views::iota(std::size_t{0}, static_cast<std::size_t>(byte_values))};

        return std::ranges::any_of(bytes, steps_into_reaching);
    }};

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (std::size_t state{0}; state < states; ++state)
        {
            if (reaches_kept[state] || !reaches_a_kept_accept(state))
            {
                continue;
            }

            reaches_kept[state] = true;

            changed = true;
        }
    }

    const auto init{simulator.init_state()};

    const auto consumes{[&](const std::size_t state) {
        const auto to{simulator.step(state, symbol)};

        return to.has_value() && simulator.is_live(*to);
    }};

    for (std::size_t state{0}; state < states; ++state)
    {
        const auto consumes_live{simulator.is_live(state) && consumes(state)};

        const auto exempt_restart{state == init && !simulator.init_reentrant()};

        if (!consumes_live || exempt_restart)
        {
            continue;
        }

        const auto restarts_alike{simulator.step(state, symbol) == simulator.step(init, symbol)};

        if (!discarded(state) || reaches_kept[state] || !restarts_alike)
        {
            return false;
        }
    }

    return consumes(init);
}

/**
 * @brief Returns a byte's column of the transition table: the state each state steps to on it.
 * @param simulator The compiled tables.
 * @param byte The byte.
 * @return Per state, the state it steps to, no_transition where it has none.
 */
inline std::vector<int> transition_column(const munch::dfa::Simulator& simulator, const unsigned char byte)
{
    std::vector<int> column{};

    for (std::size_t state{0}; state < simulator.state_count(); ++state)
    {
        const auto to{simulator.step(state, byte)};

        column.push_back(to.has_value() ? static_cast<int>(*to) : no_transition);
    }

    return column;
}

/**
 * @brief What the exact decision answered for one symbol.
 */
struct Exact_verdict
{
    /**
     * @brief The decision's answer.
     */
    enum class Outcome : std::uint8_t
    {
        /**
         * @brief Every cut before the symbol is safe.
         */
        safe,

        /**
         * @brief A replayed witness shows a cut that is not.
         */
        unsafe,

        /**
         * @brief The decision gave no answer within its budget.
         */
        budget
    };

    /**
     * @brief The answer.
     */
    Outcome outcome{Outcome::budget};

    /**
     * @brief Which of the three tests failed, when unsafe, or why there is no answer.
     */
    std::string reason{};

    /**
     * @brief The replayed witness, when unsafe: a completely tokenizable input.
     */
    std::string text{};

    /**
     * @brief The offset of the witness's cut.
     */
    std::size_t cut{0};

    /**
     * @brief The largest product the decision built, in vertices.
     */
    std::size_t vertices{0};
};

/**
 * @brief The three finite automata of the exact decision, stepped on one compiled token set.
 *
 * Bytes the tables do not tell apart step every machine identically, so one representative per class is stepped and the
 * witness is spelled in representatives; the candidate symbol represents its own class.
 */
class Scan_machines
{
public:
    /**
     * @brief The most DFA states an obligation set carries.
     */
    static constexpr std::size_t max_states{512};

    /**
     * @brief The bits in one word of an obligation set.
     */
    static constexpr std::size_t word_bits{64};

    /**
     * @brief The token in progress between tokens, when the next byte starts one from the initial state.
     */
    static constexpr int fresh{-1};

    /**
     * @brief The failing attempt once it has no transition, so it can never accept.
     */
    static constexpr int dead{-2};

    /**
     * @brief The keep guess between tokens, before the next token's first byte makes one.
     */
    static constexpr int no_guess{-1};

    /**
     * @brief The keep guess that the token in progress is discarded.
     */
    static constexpr int guess_discarded{0};

    /**
     * @brief The keep guess that the token in progress is kept.
     */
    static constexpr int guess_kept{1};

    /**
     * @brief The output letter z, emitted once per byte of a kept token.
     */
    static constexpr int z{1};

    /**
     * @brief The output letter of the kept kind numbered zero; the kind numbered k emits first_kind_letter + k at its
     *        token's end.
     */
    static constexpr int first_kind_letter{2};

    /**
     * @brief An output word: letters, and in a group delay their inverses as negated letters.
     */
    using Word_t = std::vector<int>;

    /**
     * @brief A set of DFA states, one bit each.
     */
    struct Bits
    {
        /**
         * @brief Orders sets by their words, so that vertices holding them can key a map.
         */
        auto operator<=>(const Bits&) const = default;

        /**
         * @brief Adds a state to the set.
         * @param index The state.
         */
        void set(const std::size_t index)
        {
            const auto bit{std::uint64_t{1} << (index % word_bits)};

            words[index / word_bits] |= bit;
        }

        /**
         * @brief The bits, state s at bit s % word_bits of word s / word_bits.
         */
        std::array<std::uint64_t, max_states / word_bits> words{};
    };

    /**
     * @brief The complete-scan automaton's state: a token in progress from `current`, or between tokens when `current`
     *        is fresh.
     */
    struct Success
    {
        /**
         * @brief Orders states member by member, so that vertices holding them can key a map.
         */
        auto operator<=>(const Success&) const = default;

        /**
         * @brief The tokens already committed whose match could still be extended, each a state carried until it dies.
         */
        Bits obligations{};

        /**
         * @brief The state of the token in progress, or fresh.
         */
        int current{fresh};
    };

    /**
     * @brief The failure automaton's state: the complete scan's, and once failing the state of the attempt that must
     *        never accept, dead once it has no transition.
     */
    struct Failure
    {
        /**
         * @brief Orders states member by member, so that vertices holding them can key a map.
         */
        auto operator<=>(const Failure&) const = default;

        /**
         * @brief The tokens already committed whose match could still be extended.
         */
        Bits obligations{};

        /**
         * @brief The state of the token in progress or of the failing attempt, fresh or dead.
         */
        int current{fresh};

        /**
         * @brief Whether the failing attempt has begun.
         */
        bool failing{false};
    };

    /**
     * @brief The kept-output automaton's state: the complete scan's, with the guess whether the token in progress is
     *        kept, made at its first byte and checked at its end.
     */
    struct Output
    {
        /**
         * @brief Orders states member by member, so that vertices holding them can key a map.
         */
        auto operator<=>(const Output&) const = default;

        /**
         * @brief The tokens already committed whose match could still be extended.
         */
        Bits obligations{};

        /**
         * @brief The state of the token in progress, or fresh.
         */
        int current{fresh};

        /**
         * @brief The keep guess for the token in progress, or no_guess.
         */
        int keep{no_guess};
    };

    /**
     * @brief Reads the tables' acceptance, liveness and kinds and picks one representative byte per class.
     * @param simulator The compiled tables, which must outlive the machines.
     * @param ignored The ignored kinds.
     * @param candidate The candidate symbol, which stands for its own class.
     */
    Scan_machines(const munch::dfa::Simulator& simulator, const Kinds_t& ignored, const unsigned char candidate)
        : simulator_{simulator}, init_{static_cast<int>(simulator.init_state())}
    {
        read_states(ignored);

        pick_representatives(candidate);
    }

    /**
     * @brief Returns whether the tables have few enough states for the obligation sets to carry.
     * @return Whether they do.
     */
    [[nodiscard]] bool fits() const { return simulator_.state_count() <= max_states; }

    /**
     * @brief Returns the representative bytes, one per class of bytes some state consumes, in column order.
     * @return The bytes.
     */
    [[nodiscard]] const std::vector<unsigned char>& alphabet() const { return alphabet_; }

    /**
     * @brief Steps the complete-scan automaton on one byte: the token in progress moves on, and where it accepts, it
     *        may also be committed and become an obligation.
     * @param from The state.
     * @param byte The byte.
     * @return The successor states, the continuing one first.
     */
    [[nodiscard]] std::vector<Success> success_step(const Success& from, const unsigned char byte) const
    {
        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return {};
        }

        const auto to{token_step(from.current, byte)};

        if (to == no_state)
        {
            return {};
        }

        std::vector<Success> result{};

        result.push_back(Success{.obligations = *obligations, .current = to});

        const auto state{static_cast<std::size_t>(to)};

        if (!accepting_[state])
        {
            return result;
        }

        auto committed{*obligations};

        committed.set(state);

        result.push_back(Success{.obligations = committed, .current = fresh});

        return result;
    }

    /**
     * @brief Steps the failure automaton on one byte: before failing it steps as the complete scan and, between tokens,
     *        may begin the failing attempt; once failing only the attempt moves on.
     * @param from The state.
     * @param byte The byte.
     * @return The successor states, the complete scan's first.
     */
    [[nodiscard]] std::vector<Failure> failure_step(const Failure& from, const unsigned char byte) const
    {
        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return {};
        }

        if (from.failing && from.current == dead)
        {
            return {Failure{.obligations = *obligations, .current = dead, .failing = true}};
        }

        std::vector<Failure> result{};

        if (from.failing)
        {
            const auto failing{failing_step(*obligations, from.current, byte)};

            if (failing)
            {
                result.push_back(*failing);
            }

            return result;
        }

        const Success scanning{.obligations = from.obligations, .current = from.current};

        for (const auto& [next_obligations, next_current] : success_step(scanning, byte))
        {
            result.push_back(Failure{.obligations = next_obligations, .current = next_current, .failing = false});
        }

        if (from.current != fresh)
        {
            return result;
        }

        const auto failing{failing_step(*obligations, init_, byte)};

        if (failing)
        {
            result.push_back(*failing);
        }

        return result;
    }

    /**
     * @brief Steps the kept-output automaton on one byte, emitting z per byte of a kept token and the kind's letter at
     *        its end; a fresh state guesses both ways.
     * @param from The state.
     * @param byte The byte.
     * @return The successor states, each with the word it emits, the discarded guess first.
     */
    [[nodiscard]] std::vector<std::pair<Output, Word_t>> output_step(const Output& from, const unsigned char byte) const
    {
        const auto obligations{advance(from.obligations, byte)};

        if (!obligations)
        {
            return {};
        }

        const auto to{token_step(from.current, byte)};

        if (to == no_state)
        {
            return {};
        }

        const auto state{static_cast<std::size_t>(to)};

        const auto guesses{from.current == fresh ? std::vector{guess_discarded, guess_kept} : std::vector{from.keep}};

        const auto ends_as{discarded_[state] ? guess_discarded : guess_kept};

        std::vector<std::pair<Output, Word_t>> result{};

        for (const auto keep : guesses)
        {
            const Word_t emitted{keep == guess_kept ? Word_t{z} : Word_t{}};

            result.emplace_back(Output{.obligations = *obligations, .current = to, .keep = keep}, emitted);

            if (!accepting_[state] || ends_as != keep)
            {
                continue;
            }

            auto committed{*obligations};

            committed.set(state);

            auto output{emitted};

            if (keep == guess_kept)
            {
                output.push_back(kind_[state] + first_kind_letter);
            }

            result.emplace_back(Output{.obligations = committed, .current = fresh, .keep = no_guess}, output);
        }

        return result;
    }

private:
    /**
     * @brief The token step's answer when the token in progress can no longer accept.
     */
    static constexpr int no_state{-1};

    /**
     * @brief The kind number of a state that accepts no kept kind.
     */
    static constexpr int no_kind{-1};

    /**
     * @brief Reads, per state, whether it accepts, whether it is live, whether it accepts a discarded kind and the
     *        number of the kept kind it accepts, the kept kinds numbered densely in order of first occurrence.
     * @param ignored The ignored kinds.
     */
    void read_states(const Kinds_t& ignored)
    {
        const auto states{simulator_.state_count()};

        accepting_.resize(states);

        live_.resize(states);

        discarded_.resize(states);

        kind_.resize(states, no_kind);

        std::map<std::size_t, int> kept_kinds{};

        for (std::size_t state{0}; state < states; ++state)
        {
            accepting_[state] = simulator_.is_accepting(state);

            live_[state] = simulator_.is_live(state);

            const auto token{simulator_.accepted(state)};

            discarded_[state] = token.has_value() && ignored.contains(token->id());

            if (!token.has_value() || discarded_[state])
            {
                continue;
            }

            const auto next{static_cast<int>(kept_kinds.size())};

            const auto [slot, inserted]{kept_kinds.try_emplace(token->id(), next)};

            kind_[state] = slot->second;
        }
    }

    /**
     * @brief Picks one representative per class of bytes with identical columns, over the bytes some state consumes at
     *        all, the lowest byte of a class unless the candidate is in it, so that a witness spells the cut with it.
     * @param candidate The candidate symbol.
     */
    void pick_representatives(const unsigned char candidate)
    {
        std::map<std::vector<int>, unsigned char> classes{};

        const auto is_transition{[](const int to) { return to != no_transition; }};

        for (int value{0}; value < byte_values; ++value)
        {
            const auto symbol{static_cast<unsigned char>(value)};

            const auto column{transition_column(simulator_, symbol)};

            if (!std::ranges::any_of(column, is_transition))
            {
                continue;
            }

            const auto [slot, inserted]{classes.try_emplace(column, symbol)};

            if (symbol == candidate)
            {
                slot->second = candidate;
            }
        }

        std::ranges::copy(classes | std::views::values, std::back_inserter(alphabet_));
    }

    /**
     * @brief Moves every obligation on: one that accepts refutes the commitment it stands for, one with no transition
     *        or no acceptance ahead is dropped.
     * @param obligations The obligations.
     * @param byte The byte.
     * @return The moved obligations, or std::nullopt when one accepts.
     */
    [[nodiscard]] std::optional<Bits> advance(const Bits& obligations, const unsigned char byte) const
    {
        Bits moved{};

        for (std::size_t word{0}; word < obligations.words.size(); ++word)
        {
            for (auto bits{obligations.words[word]}; bits != 0; bits &= bits - 1)
            {
                const auto offset{static_cast<std::size_t>(std::countr_zero(bits))};

                const auto state{word * word_bits + offset};

                const auto to{simulator_.step(state, byte)};

                if (!to)
                {
                    continue;
                }

                if (accepting_[*to])
                {
                    return std::nullopt;
                }

                if (live_[*to])
                {
                    moved.set(*to);
                }
            }
        }

        return moved;
    }

    /**
     * @brief Moves the token in progress on, from the initial state when fresh; a state that can no longer accept ends
     *        it.
     * @param current The token's state, or fresh.
     * @param byte The byte.
     * @return The live state it moves to, or no_state.
     */
    [[nodiscard]] int token_step(const int current, const unsigned char byte) const
    {
        const auto from{current == fresh ? init_ : current};

        const auto to{simulator_.step(static_cast<std::size_t>(from), byte)};

        return to && live_[*to] ? static_cast<int>(*to) : no_state;
    }

    /**
     * @brief Moves the failing attempt on, which must not accept; with no transition, or none that could ever accept,
     *        it is dead and stays so.
     * @param obligations The moved obligations.
     * @param current The attempt's state.
     * @param byte The byte.
     * @return The successor, or std::nullopt when the attempt accepts.
     */
    [[nodiscard]] std::optional<Failure> failing_step(
            const Bits& obligations, const int current, const unsigned char byte) const
    {
        const auto to{simulator_.step(static_cast<std::size_t>(current), byte)};

        if (!to || !live_[*to])
        {
            return Failure{.obligations = obligations, .current = dead, .failing = true};
        }

        if (accepting_[*to])
        {
            return std::nullopt;
        }

        return Failure{.obligations = obligations, .current = static_cast<int>(*to), .failing = true};
    }

    /**
     * @brief The compiled tables.
     */
    const munch::dfa::Simulator& simulator_;

    /**
     * @brief The initial state.
     */
    int init_{0};

    /**
     * @brief Per state, whether it accepts.
     */
    std::vector<bool> accepting_{};

    /**
     * @brief Per state, whether it is live.
     */
    std::vector<bool> live_{};

    /**
     * @brief Per state, whether it accepts a discarded kind.
     */
    std::vector<bool> discarded_{};

    /**
     * @brief Per state, the number of the kept kind it accepts, or no_kind.
     */
    std::vector<int> kind_{};

    /**
     * @brief The representative bytes, one per class.
     */
    std::vector<unsigned char> alphabet_{};
};

/**
 * @brief Returns the phase a marked input is in after one more byte.
 * @param phase The phase before it.
 * @return phase_left before the marker, phase_right after it.
 */
constexpr int next_phase(const int phase)
{
    return phase < phase_marked ? phase_left : phase_right;
}

/**
 * @brief A witness spelled from a product path: an input and the cut the marker stood at.
 */
struct Witness
{
    /**
     * @brief The input.
     */
    std::string text{};

    /**
     * @brief The offset of the cut.
     */
    std::size_t cut{0};
};

/**
 * @brief Spells the labels of a path as the input and the cut the marker stood at.
 * @param labels The labels, bytes and one cut_marker.
 * @return The witness.
 */
inline Witness spelled(const std::vector<int>& labels)
{
    Witness witness{};

    for (const auto label : labels)
    {
        if (label == cut_marker)
        {
            witness.cut = witness.text.size();

            continue;
        }

        witness.text.push_back(static_cast<char>(label));
    }

    return witness;
}

/**
 * @brief A vertex of a chunk-completeness product: the whole scan on uv beside the failing scan of one chunk, left on u
 *        up to the marker or right on v from it.
 */
struct Domain_vertex
{
    /**
     * @brief Orders vertices member by member, so that they can key a map.
     */
    auto operator<=>(const Domain_vertex&) const = default;

    /**
     * @brief The whole scan.
     */
    Scan_machines::Success whole{};

    /**
     * @brief The chunk's failing scan.
     */
    Scan_machines::Failure fragment{};

    /**
     * @brief Whether the chunk's scan is running.
     */
    bool fragment_active{false};

    /**
     * @brief The phase of the marked input.
     */
    int phase{phase_start};
};

/**
 * @brief Searches a chunk-completeness product breadth first for an accepting path: a completely tokenizable input one
 *        of whose chunks the scanner does not consume.
 * @param machines The machines.
 * @param candidate The candidate symbol, the byte after the marker.
 * @param left Whether the chunk is the left one.
 * @param budget The most vertices the product may hold.
 * @param largest The largest product built so far, raised to this one's size.
 * @return The witness of the first accepting vertex reached, or std::nullopt when there is none.
 * @throws std::length_error When the product outgrows the budget.
 */
inline std::optional<Witness> incomplete_chunk(
        const Scan_machines& machines, const unsigned char candidate, const bool left, const std::size_t budget,
        std::size_t& largest)
{
    std::vector vertices{Domain_vertex{.whole = {}, .fragment = {}, .fragment_active = left, .phase = phase_start}};

    std::vector parents{std::pair<std::size_t, int>{0, 0}};

    std::map<Domain_vertex, std::size_t> ids{{vertices.front(), 0}};

    const auto push{[&](const Domain_vertex& vertex, const std::size_t from, const int label) {
        const auto [slot, inserted]{ids.try_emplace(vertex, vertices.size())};

        if (!inserted)
        {
            return;
        }

        vertices.push_back(vertex);

        parents.emplace_back(from, label);
    }};

    const auto labels_to{[&parents](const std::size_t vertex) {
        std::vector<int> labels{};

        for (auto id{vertex}; id != 0; id = parents[id].first)
        {
            labels.push_back(parents[id].second);
        }

        std::ranges::reverse(labels);

        return labels;
    }};

    const auto step_on{[&](const Domain_vertex& vertex, const std::size_t at, const unsigned char byte) {
        const auto fragments{
                vertex.fragment_active ? machines.failure_step(vertex.fragment, byte) : std::vector{vertex.fragment}};

        const auto phase{next_phase(vertex.phase)};

        for (const auto& whole : machines.success_step(vertex.whole, byte))
        {
            for (const auto& fragment : fragments)
            {
                const Domain_vertex next{
                        .whole = whole,
                        .fragment = fragment,
                        .fragment_active = vertex.fragment_active,
                        .phase = phase};

                push(next, at, byte);
            }
        }
    }};

    for (std::size_t at{0}; at < vertices.size(); ++at)
    {
        largest = std::max(largest, vertices.size());

        if (vertices.size() > budget)
        {
            throw std::length_error{"budget"};
        }

        // A copy, since pushing may reallocate the vertices.
        const auto vertex{vertices[at]};

        const auto fragment_final{vertex.fragment.failing};

        const auto between_tokens{vertex.whole.current == Scan_machines::fresh};

        if (vertex.phase == phase_right && between_tokens && (left || fragment_final))
        {
            const auto labels{labels_to(at)};

            return spelled(labels);
        }

        if (vertex.phase == phase_left && (!left || fragment_final))
        {
            const Domain_vertex marked{
                    .whole = vertex.whole,
                    .fragment = {},
                    .fragment_active = !left,
                    .phase = phase_marked};

            push(marked, at, cut_marker);
        }

        for (const auto byte : machines.alphabet())
        {
            if (vertex.phase == phase_marked && byte != candidate)
            {
                continue;
            }

            step_on(vertex, at, byte);
        }
    }

    return std::nullopt;
}

/**
 * @brief A vertex of the kept-output product: the whole scan beside the scan restarted at the marker.
 */
struct Output_vertex
{
    /**
     * @brief Orders vertices member by member, so that they can key a map.
     */
    auto operator<=>(const Output_vertex&) const = default;

    /**
     * @brief The whole scan.
     */
    Scan_machines::Output whole{};

    /**
     * @brief The scan restarted at the marker.
     */
    Scan_machines::Output split{};

    /**
     * @brief The phase of the marked input.
     */
    int phase{phase_start};
};

/**
 * @brief An edge of the kept-output product, carrying both scans' output words.
 */
struct Output_edge
{
    /**
     * @brief The vertex it leaves.
     */
    std::size_t source{0};

    /**
     * @brief The vertex it enters.
     */
    std::size_t target{0};

    /**
     * @brief The byte it reads, or cut_marker.
     */
    int label{0};

    /**
     * @brief The whole scan's output.
     */
    Scan_machines::Word_t h{};

    /**
     * @brief The restarted scan's output.
     */
    Scan_machines::Word_t g{};
};

/**
 * @brief The kept-output product as a graph.
 */
struct Output_graph
{
    /**
     * @brief The edges, in the order built.
     */
    std::vector<Output_edge> edges{};

    /**
     * @brief Per vertex, the edges leaving it.
     */
    std::vector<std::vector<std::size_t>> out{};

    /**
     * @brief Per vertex, the edges entering it.
     */
    std::vector<std::vector<std::size_t>> in{};

    /**
     * @brief Per vertex, whether both scans are between tokens past the candidate.
     */
    std::vector<bool> accepting{};
};

/**
 * @brief Builds the kept-output product breadth first, each scan emitting z per byte of a kept token and the kind's
 *        letter at its end; an accepting path is a completely tokenizable input both of whose chunks are consumed,
 *        carrying the two kept streams as words.
 * @param machines The machines.
 * @param candidate The candidate symbol, the byte after the marker.
 * @param budget The most vertices the product may hold.
 * @param largest The largest product built so far, raised to this one's size.
 * @return The graph.
 * @throws std::length_error When the product outgrows the budget.
 */
inline Output_graph output_product(
        const Scan_machines& machines, const unsigned char candidate, const std::size_t budget, std::size_t& largest)
{
    std::vector vertices{Output_vertex{}};

    std::map<Output_vertex, std::size_t> ids{{vertices.front(), 0}};

    Output_graph graph{};

    graph.out.emplace_back();

    graph.in.emplace_back();

    graph.accepting.push_back(false);

    // h is the whole scan's output along the edge and g the restarted scan's.
    const auto link{[&](const std::size_t from, const Output_vertex& vertex, const int label,
                        const Scan_machines::Word_t& h, const Scan_machines::Word_t& g) {
        const auto [slot, inserted]{ids.try_emplace(vertex, vertices.size())};

        if (inserted)
        {
            vertices.push_back(vertex);

            graph.out.emplace_back();

            graph.in.emplace_back();

            graph.accepting.push_back(false);
        }

        const auto target{slot->second};

        const auto edge{graph.edges.size()};

        graph.out[from].push_back(edge);

        graph.in[target].push_back(edge);

        graph.edges.push_back(Output_edge{.source = from, .target = target, .label = label, .h = h, .g = g});
    }};

    const auto step_on{[&](const Output_vertex& vertex, const std::size_t at, const unsigned char byte) {
        const auto phase{next_phase(vertex.phase)};

        const auto splits{machines.output_step(vertex.split, byte)};

        for (const auto& [whole, h] : machines.output_step(vertex.whole, byte))
        {
            for (const auto& [split, g] : splits)
            {
                const Output_vertex next{.whole = whole, .split = split, .phase = phase};

                link(at, next, byte, h, g);
            }
        }
    }};

    for (std::size_t at{0}; at < vertices.size(); ++at)
    {
        largest = std::max(largest, vertices.size());

        if (vertices.size() > budget)
        {
            throw std::length_error{"budget"};
        }

        // A copy, since linking may reallocate the vertices.
        const auto vertex{vertices[at]};

        const auto whole_between{vertex.whole.current == Scan_machines::fresh};

        const auto split_between{vertex.split.current == Scan_machines::fresh};

        graph.accepting[at] = vertex.phase == phase_right && whole_between && split_between;

        if (vertex.phase == phase_left && split_between)
        {
            const Output_vertex marked{.whole = vertex.whole, .split = {}, .phase = phase_marked};

            link(at, marked, cut_marker, {}, {});
        }

        for (const auto byte : machines.alphabet())
        {
            if (vertex.phase == phase_marked && byte != candidate)
            {
                continue;
            }

            step_on(vertex, at, byte);
        }
    }

    return graph;
}

/**
 * @brief Returns the delay of two outputs A, B, the reduced group word A^{-1} B, after one more edge.
 * @param delay The delay before the edge.
 * @param edge The edge.
 * @return The delay after it, reduced.
 */
inline Scan_machines::Word_t delayed(const Scan_machines::Word_t& delay, const Output_edge& edge)
{
    Scan_machines::Word_t result{};

    const auto push{[&result](const int letter) {
        if (!result.empty() && result.back() == -letter)
        {
            result.pop_back();

            return;
        }

        result.push_back(letter);
    }};

    for (const auto letter : edge.h | std::views::reverse)
    {
        push(-letter);
    }

    std::ranges::for_each(delay, push);

    std::ranges::for_each(edge.g, push);

    return result;
}

/**
 * @brief The marker of a vertex with no edge on a path.
 */
inline constexpr std::size_t no_edge{std::numeric_limits<std::size_t>::max()};

/**
 * @brief The shortest accepting suffixes of a product graph, found backwards from its accepting vertices.
 */
struct Accepting_suffixes
{
    /**
     * @brief Per vertex, the first edge of a shortest path to an accepting vertex, or no_edge.
     */
    std::vector<std::size_t> onward{};

    /**
     * @brief Per vertex, whether an accepting vertex is reachable from it.
     */
    std::vector<bool> live{};
};

/**
 * @brief Finds the shortest accepting suffixes of a graph, breadth first backwards from its accepting vertices.
 * @param graph The graph.
 * @return The suffixes and the live vertices.
 */
inline Accepting_suffixes accepting_suffixes(const Output_graph& graph)
{
    const auto count{graph.out.size()};

    Accepting_suffixes suffixes{.onward = std::vector(count, no_edge), .live = std::vector(count, false)};

    auto& [onward, live]{suffixes};

    std::deque<std::size_t> queue{};

    for (std::size_t vertex{0}; vertex < count; ++vertex)
    {
        if (!graph.accepting[vertex])
        {
            continue;
        }

        live[vertex] = true;

        queue.push_back(vertex);
    }

    while (!queue.empty())
    {
        const auto vertex{queue.front()};

        queue.pop_front();

        for (const auto edge : graph.in[vertex])
        {
            const auto source{graph.edges[edge].source};

            if (live[source])
            {
                continue;
            }

            live[source] = true;

            onward[source] = edge;

            queue.push_back(source);
        }
    }

    return suffixes;
}

/**
 * @brief The shortest stems of a product graph from its initial vertex over live vertices, each carrying its delay.
 */
struct Shortest_stems
{
    /**
     * @brief Per vertex, the last edge of its shortest stem, or no_edge.
     */
    std::vector<std::size_t> stem{};

    /**
     * @brief Per vertex, whether a stem reaches it.
     */
    std::vector<bool> reached{};

    /**
     * @brief Per vertex, the delay its stem carries.
     */
    std::vector<Scan_machines::Word_t> delay{};
};

/**
 * @brief Finds the shortest stems from the initial vertex over the live vertices, breadth first.
 * @param graph The graph.
 * @param live Per vertex, whether an accepting vertex is reachable from it.
 * @return The stems, the reached vertices and their delays.
 */
inline Shortest_stems shortest_stems(const Output_graph& graph, const std::vector<bool>& live)
{
    const auto count{graph.out.size()};

    Shortest_stems stems{
            .stem = std::vector(count, no_edge),
            .reached = std::vector(count, false),
            .delay = std::vector<Scan_machines::Word_t>(count)};

    auto& [stem, reached, delay]{stems};

    std::deque<std::size_t> queue{};

    reached[0] = true;

    queue.push_back(0);

    while (!queue.empty())
    {
        const auto vertex{queue.front()};

        queue.pop_front();

        for (const auto edge : graph.out[vertex])
        {
            const auto target{graph.edges[edge].target};

            if (!live[target] || reached[target])
            {
                continue;
            }

            reached[target] = true;

            stem[target] = edge;

            delay[target] = delayed(delay[vertex], graph.edges[edge]);

            queue.push_back(target);
        }
    }

    return stems;
}

/**
 * @brief Decides whether every accepting path of the graph carries equal outputs, by the delays of shortest stems.
 *
 * Where it does not, an accepting path that does not is built from the stem and a shortest accepting suffix at the
 * failing edge or vertex.
 * @param graph The graph.
 * @return The edges of an accepting path with unequal outputs, or std::nullopt when every one carries equal outputs.
 */
inline std::optional<std::vector<std::size_t>> unequal_path(const Output_graph& graph)
{
    const auto count{graph.out.size()};

    const auto [onward, live]{accepting_suffixes(graph)};

    if (!live[0])
    {
        // No accepting path at all, so nothing to compare.
        return std::nullopt;
    }

    const auto [stem, reached, delay]{shortest_stems(graph, live)};

    const auto path_to{[&](const std::size_t vertex) {
        std::vector<std::size_t> path{};

        for (auto at{vertex}; stem[at] != no_edge; at = graph.edges[stem[at]].source)
        {
            path.push_back(stem[at]);
        }

        std::ranges::reverse(path);

        return path;
    }};

    const auto path_from{[&](std::vector<std::size_t> path, const std::size_t vertex) {
        for (auto at{vertex}; onward[at] != no_edge; at = graph.edges[onward[at]].target)
        {
            path.push_back(onward[at]);
        }

        return path;
    }};

    const auto unequal{[&graph](const std::vector<std::size_t>& path) {
        Scan_machines::Word_t h{};

        Scan_machines::Word_t g{};

        for (const auto edge : path)
        {
            const auto& [source, target, label, edge_h, edge_g]{graph.edges[edge]};

            h.insert(h.end(), edge_h.begin(), edge_h.end());

            g.insert(g.end(), edge_g.begin(), edge_g.end());
        }

        return h != g;
    }};

    // The paths share one accepting suffix and the stems' delays differ, so one carries unequal outputs.
    const auto around{[&](const std::size_t vertex, const std::size_t edge) {
        const auto target{graph.edges[edge].target};

        auto through{path_to(vertex)};

        through.push_back(edge);

        const auto first{path_from(path_to(target), target)};

        const auto second{path_from(through, target)};

        return unequal(first) ? first : second;
    }};

    for (std::size_t vertex{0}; vertex < count; ++vertex)
    {
        if (!reached[vertex])
        {
            continue;
        }

        if (graph.accepting[vertex] && !delay[vertex].empty())
        {
            return path_to(vertex);
        }

        for (const auto edge : graph.out[vertex])
        {
            const auto target{graph.edges[edge].target};

            if (!live[target] || delayed(delay[vertex], graph.edges[edge]) == delay[target])
            {
                continue;
            }

            return around(vertex, edge);
        }
    }

    return std::nullopt;
}

/**
 * @brief Replays a witness through the real scanner and names what it shows.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param text The witness's input.
 * @param cut The witness's cut.
 * @return left_incomplete, right_incomplete or streams_differ, or std::nullopt when it shows nothing.
 */
inline std::optional<std::string> replayed(
        const munch::core::Lexer& lexer, const Kinds_t& ignored, const std::string& text, const std::size_t cut)
{
    if (cut == 0 || cut >= text.size())
    {
        return std::nullopt;
    }

    const auto [whole, whole_used]{scan(lexer, text)};

    if (whole_used != text.size())
    {
        return std::nullopt;
    }

    const auto [left, left_used]{scan(lexer, text.substr(0, cut))};

    if (left_used != cut)
    {
        return std::string{left_incomplete};
    }

    const auto [right, right_used]{scan(lexer, text.substr(cut))};

    if (right_used != text.size() - cut)
    {
        return std::string{right_incomplete};
    }

    Stream_t spliced{left};

    spliced.insert(spliced.end(), right.begin(), right.end());

    if (without(spliced, ignored) == without(whole, ignored))
    {
        return std::nullopt;
    }

    return std::string{streams_differ};
}

/**
 * @brief Decides exactly whether every cut before the symbol, in every completely tokenizable input, leaves two
 *        completely tokenizable chunks whose kept streams concatenate to the whole input's.
 *
 * Unsafe answers are replayed through the real scanner and reported with the input and cut that show them; an unsafe
 * answer whose witness does not replay is reported as a budget outcome with a reason, never as an answer.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param symbol The symbol.
 * @param budget The most vertices a product may hold.
 * @return The verdict.
 */
inline Exact_verdict decide_exactly(
        const munch::core::Lexer& lexer, const Kinds_t& ignored, const unsigned char symbol,
        const std::size_t budget = decision_budget)
{
    Exact_verdict verdict{};

    const Scan_machines machines{lexer.simulator(), ignored, symbol};

    if (!machines.fits())
    {
        verdict.reason = "more states than the machines carry";

        return verdict;
    }

    const auto report{[&](const Witness& witness, const std::string_view expected) {
        verdict.text = witness.text;

        verdict.cut = witness.cut;

        const auto shown{replayed(lexer, ignored, witness.text, witness.cut)};

        if (shown != expected)
        {
            verdict.reason = std::format("a witness for {} that does not replay", expected);

            return verdict;
        }

        verdict.outcome = Exact_verdict::Outcome::unsafe;

        verdict.reason = expected;

        return verdict;
    }};

    try
    {
        for (const auto left : {true, false})
        {
            const auto witness{incomplete_chunk(machines, symbol, left, budget, verdict.vertices)};

            if (witness)
            {
                return report(*witness, left ? left_incomplete : right_incomplete);
            }
        }

        const auto graph{output_product(machines, symbol, budget, verdict.vertices)};

        const auto path{unequal_path(graph)};

        if (!path)
        {
            verdict.outcome = Exact_verdict::Outcome::safe;

            return verdict;
        }

        const auto label_of{[&graph](const std::size_t edge) { return graph.edges[edge].label; }};

        std::vector<int> labels{};

        std::ranges::transform(*path, std::back_inserter(labels), label_of);

        return report(spelled(labels), streams_differ);
    }
    catch (const std::length_error&)
    {
        verdict.reason = "budget exhausted";

        return verdict;
    }
}

} // namespace figures

#endif // MUNCH_PAPER_FIGURES_MODULO_HPP

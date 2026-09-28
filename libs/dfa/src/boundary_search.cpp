#include "munch/dfa/boundary_search.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
namespace
{
/**
 * @brief A state as the search stores it: the width of the Simulator's table entry, which bounds the states a compiled
 *        token set can have, so every state a scan stands in fits; narrowed because a position holds a set of them and
 *        a search holds many positions.
 */
using State_t = std::uint32_t;

/**
 * @brief One scan's position in a search that guesses token boundaries: the run of the segment being read, and the runs
 *        of the segments already closed.
 *
 * A closed run is carried because a later accept on it proves the close was not the longest match, which is how a
 * guessed marking is held to maximal munch without tracking how far back the last accept was. The guesses that survive
 * are exactly the greedy segmentation, which is what makes guessing boundaries sound.
 */
struct Position
{
    /**
     * @brief Ordered member by member, so positions order the keys that hold them.
     */
    auto operator<=>(const Position&) const = default;

    /**
     * @brief The state of the segment being read.
     */
    std::size_t reading{};

    /**
     * @brief The states of the closed segments still alive, sorted and distinct.
     */
    std::vector<State_t> closed{};
};

/**
 * @brief A key of the search: the positions of the scans in progress, how far a window matcher beside them has read,
 *        whether the latest token start sits at the origin of the occurrence it is reading, or for a gap claim whether
 *        the gap is a boundary, and whether the event searched for has happened on this branch.
 *
 * The decisions share this key because they share what a branch's future depends on: where each scan stands, and
 * whether the event has already happened, since after it a branch only has to reach a close. The event is one bit
 * rather than a record of where it happened, so two branches agreeing on the scans and the bit have the same futures
 * and are searched once. rescue() walks one scan and marks a branch once a closed run has survived a byte;
 * boundary_difference() walks two and marks a branch once the two markings have diverged; window_occurrence() walks one
 * beside a matcher that guesses where its window's occurrence begins and reads the window from there, and marks a
 * branch once the whole window has been read; window_counterexample() walks the same matcher and marks a branch once
 * the whole window has been read with the covering token beginning elsewhere than the origin; boundary_counterexample()
 * and crossing_counterexample() walk it too and mark a branch once the gap has been settled with no boundary there, or
 * with one; segmentation_difference() walks two scans over one guessed marking, either of them dead, and marks nothing,
 * its event ending the input where it happens. The matcher's progress and the origin bit are the two things besides the
 * scans a future depends on, so they are in the key, zero and clear for the decisions that match nothing or ask no
 * origin.
 */
struct Key
{
    /**
     * @brief Ordered member by member, so keys index the map of keys visited.
     */
    auto operator<=>(const Key&) const = default;

    /**
     * @brief The positions of the scans in progress.
     */
    std::vector<Position> scans{};

    /**
     * @brief How far the window matcher has read into the occurrence it guessed.
     */
    std::size_t matched{};

    /**
     * @brief The origin bit: whether the latest token start sits at the origin, or for a gap claim whether the gap is a
     *        boundary.
     */
    bool at_origin{};

    /**
     * @brief Whether the event searched for has happened on this branch.
     */
    bool marked{};
};

/**
 * @brief What a search found.
 */
struct Outcome
{
    /**
     * @brief The input the search stopped at, empty when it found none.
     */
    std::string witness{};

    /**
     * @brief Whether the search settled the question rather than stopping at the cap.
     */
    bool exhaustive{};
};

/**
 * @brief What one byte does to a key: the keys it leads to, and whether the input may end on it.
 */
struct Step
{
    /**
     * @brief The keys the byte leads to, none when no scan survives it; a view of the expansion's own buffer, good
     *        until the expansion is next called, whose keys the search moves out.
     */
    std::span<Key> successors{};

    /**
     * @brief Whether the input may end on the byte as the witness looked for; the successors are not read then.
     */
    bool ends{};
};

/**
 * @brief How the search reached a key: the number of the key the byte was read from, zero for the start, and the byte.
 */
struct Parent
{
    /**
     * @brief The number of the key the byte was read from, zero for the start.
     */
    std::size_t from{};

    /**
     * @brief The byte read.
     */
    char byte{};
};

/**
 * @brief The keys visited, each with the parent that led to it.
 */
using Seen_t = std::map<Key, Parent>;

/**
 * @brief The keys visited in the order the search admitted them, so that a key's number is its index here: the start is
 *        number zero, and the keys still to expand are those past the one being expanded.
 */
using Admitted_t = std::vector<Seen_t::const_iterator>;

/**
 * @brief One guess a byte opens for a scan beside a window matcher.
 */
struct Guess
{
    /**
     * @brief How far the matcher has read into the occurrence it guessed, after the byte.
     */
    std::size_t matched{};

    /**
     * @brief Whether the segment being read closes on the byte.
     */
    bool closes{};
};

/**
 * @brief What the origin bit of an origin search claims of an occurrence: that the token covering the window's final
 *        byte begins at the origin, the window certificate, or that the gap at the origin is a boundary, or that a
 *        token crosses it.
 */
enum class Origin_claim : std::uint8_t
{
    /**
     * @brief The token covering the final byte begins at the origin, the claim window_counterexample() refutes.
     */
    covers,

    /**
     * @brief The gap is a boundary, a token beginning there or the input ending there, the claim
     *        boundary_counterexample() refutes.
     */
    cut,

    /**
     * @brief A token crosses the gap, the claim crossing_counterexample() refutes.
     */
    crossed
};

/**
 * @brief Sorts a set of closed runs and drops the repeats, so that two positions holding the same runs compare equal.
 * @param states The runs, in any order.
 */
void dedup(std::vector<State_t>& states)
{
    std::ranges::sort(states);

    const auto duplicates{std::ranges::unique(states)};

    states.erase(duplicates.begin(), duplicates.end());
}

/**
 * @brief Advances a position by one byte.
 * @param simulator The compiled token set the position scans.
 * @param position The position, advanced in place.
 * @param byte The byte read.
 * @return False abandons the branch: either the segment being read died, so it can never close and the input can never
 *         be finished, or a closed run accepted and the marking is not the greedy one. True leaves in closed exactly
 *         the runs that survived the byte.
 */
[[nodiscard]] bool advance(const Simulator& simulator, Position& position, const unsigned char byte)
{
    const auto next{simulator.step(position.reading, byte)};

    if (!next)
    {
        return false;
    }

    position.reading = *next;

    std::vector<State_t> survived{};

    for (const auto state : position.closed)
    {
        const auto moved{simulator.step(state, byte)};

        if (!moved)
        {
            continue;
        }

        if (simulator.is_accepting(*moved))
        {
            return false;
        }

        survived.push_back(static_cast<State_t>(*moved));
    }

    dedup(survived);

    position.closed = std::move(survived);

    return true;
}

/**
 * @brief Closes the segment being read: its run stays alive as a closed run and a fresh one starts.
 * @param simulator The compiled token set the position scans.
 * @param position The position at the close.
 * @return The position after it.
 */
[[nodiscard]] Position close(const Simulator& simulator, Position position)
{
    position.closed.push_back(static_cast<State_t>(position.reading));

    dedup(position.closed);

    position.reading = simulator.init_state();

    return position;
}

/**
 * @brief Returns the position a scan starts in: the initial state, with no closed runs.
 * @param simulator The compiled token set the scan reads.
 * @return The initial position.
 */
[[nodiscard]] Position initial(const Simulator& simulator)
{
    return {.reading = simulator.init_state(), .closed = {}};
}

/**
 * @brief Returns the dead position the marked-language product adjoins on a side: the state one past the table, which
 *        no scan stands in, with no closed runs. A side that has died stays dead, and advance() and close() are never
 *        asked of it.
 * @param simulator The compiled token set the side scans.
 * @return The dead position.
 */
[[nodiscard]] Position dead(const Simulator& simulator)
{
    return {.reading = simulator.state_count(), .closed = {}};
}

/**
 * @brief Reads one marked symbol, a byte with or without a close, into a position: the byte advances it, and the close
 *        closes the segment being read, which must accept. A side that cannot read the symbol dies, and a dead side
 *        stays dead.
 * @param simulator The compiled token set the position scans.
 * @param position The position, moved in place.
 * @param byte The byte read.
 * @param closes Whether a token boundary follows the byte.
 * @return True when the side is alive after the symbol, false when it is dead.
 */
[[nodiscard]] bool read_marked(
        const Simulator& simulator, Position& position, const unsigned char byte, const bool closes)
{
    // The dead position stands in the state one past the table.
    const auto is_dead{position.reading == simulator.state_count()};

    if (is_dead || !advance(simulator, position, byte) || (closes && !simulator.is_accepting(position.reading)))
    {
        position = dead(simulator);

        return false;
    }

    if (closes)
    {
        position = close(simulator, position);
    }

    return true;
}

/**
 * @brief Returns whether the token set tokenizes an input completely, by the scan itself: maximal munch from every
 *        token boundary to the end.
 * @param simulator The compiled token set.
 * @param input The input.
 * @return True when the scan consumes every byte.
 */
[[nodiscard]] bool tokenizes(const Simulator& simulator, const std::string_view input)
{
    for (std::size_t at{0}; at < input.size();)
    {
        const auto [token, length]{simulator.run(input.substr(at))};

        if (!token || length == 0)
        {
            return false;
        }

        at += length;
    }

    return true;
}

/**
 * @brief Returns the bytes that led the search to a key, read back through the parents it recorded.
 * @param admitted The keys visited, by number.
 * @param at The number of the key the last byte was read from.
 * @param last The last byte.
 * @return The input, first byte first.
 */
[[nodiscard]] std::string trail(const Admitted_t& admitted, const std::size_t at, const char last)
{
    std::string out{last};

    for (auto number{at}; number != 0;)
    {
        const auto& [key, parent]{*admitted[number]};

        out.push_back(parent.byte);

        number = parent.from;
    }

    std::ranges::reverse(out);

    return out;
}

/**
 * @brief Admits the keys a byte led to that the search has not seen, each recorded with its parent and numbered after
 *        every key admitted before it, while the ceiling allows.
 *
 * Admitting an unseen key is where the search grows, so the ceiling is checked here and nowhere else: a key that would
 * pass it is not admitted, and the question stays open.
 * @param seen The keys visited, grown in place.
 * @param admitted The keys visited, by number, grown in place.
 * @param successors The keys the byte led to, moved from.
 * @param parent The number of the key the byte was read from, and the byte.
 * @param cap The most keys the search may hold at once.
 * @return False when an unseen key would pass the ceiling and the search must give up, true otherwise.
 */
[[nodiscard]] bool admit(
        Seen_t& seen, Admitted_t& admitted, const std::span<Key> successors, const Parent parent, const std::size_t cap)
{
    for (auto& next : successors)
    {
        if (seen.contains(next))
        {
            continue;
        }

        if (seen.size() >= cap)
        {
            return false;
        }

        const auto [key, inserted]{seen.try_emplace(std::move(next), parent)};

        admitted.push_back(key);
    }

    return true;
}

/**
 * @brief Runs the breadth-first search over inputs that every boundary-guessing decision shares: from a start key,
 *        every byte in turn, the keys it reaches recorded with the byte that led there, until an input ends as a
 *        witness or no key is left. Bytes are explored in order and keys by length, so a witness is a shortest one.
 * @tparam Expand The expansion's callable type.
 * @param start The key before any byte.
 * @param cap The most keys the search may hold at once, the start key among them: a key that would pass the ceiling is
 *        never admitted and the search gives up instead. A cap of zero holds nothing, not even the start.
 * @param expand Given a key and a byte, the keys the byte leads to and whether the input may end on that byte as the
 *        witness looked for, as a Step.
 * @return The witness and whether the question was settled: an empty witness with the search exhausted is a proof that
 *         none exists, an empty witness with it stopped at the cap says nothing.
 */
template <typename Expand>
[[nodiscard]] Outcome search(const Key& start, const std::size_t cap, Expand expand)
{
    // The start key is held like any other, so a cap of zero affords no search at all.
    if (cap == 0)
    {
        return {.witness = {}, .exhaustive = false};
    }

    Seen_t seen{{start, Parent{.from = 0, .byte = '\0'}}};

    Admitted_t admitted{seen.cbegin()};

    // Keys are expanded in the order they were admitted, which is breadth-first.
    for (std::size_t at{0}; at < admitted.size(); ++at)
    {
        const auto& [key, parent]{*admitted[at]};

        for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
        {
            const auto byte{static_cast<unsigned char>(value)};

            const auto read{static_cast<char>(byte)};

            const auto [successors, ends]{expand(key, byte)};

            if (ends)
            {
                return {.witness = trail(admitted, at, read), .exhaustive = true};
            }

            const Parent reached{.from = at, .byte = read};

            if (!admit(seen, admitted, successors, reached, cap))
            {
                return {.witness = {}, .exhaustive = false};
            }
        }
    }

    return {.witness = {}, .exhaustive = true};
}

/**
 * @brief Returns a search's outcome as a decision's result, the witness moved into it.
 * @tparam Result The decision's result type, a witness and whether the search was exhaustive.
 * @param outcome The search's outcome.
 * @return The result.
 */
template <typename Result>
[[nodiscard]] Result as(Outcome outcome)
{
    return {.witness = std::move(outcome.witness), .exhaustive = outcome.exhaustive};
}

/**
 * @brief Returns the guesses a byte opens for a scan beside a window matcher: every move of the matcher, each without a
 *        close and, where the segment being read accepts on the byte, with one.
 *
 * The matcher's moves are the same for every decision that walks one: outside the occurrence it stays outside, and
 * begins one where the byte is the window's first, which is the guess; inside, it reads the window's next byte or the
 * guess was wrong; through, it stays through. The guesses come in the order the search explores them, the matcher
 * staying outside before it begins, and a guess without a close before the same guess with one.
 * @param window The window; its byte at the matcher's progress is read only while the occurrence is not through.
 * @param at The key the byte is read from.
 * @param byte The byte read.
 * @param closes Whether the segment being read accepts on the byte, so that it may close.
 * @param out The buffer the guesses are written to, cleared first and kept by the caller across bytes.
 * @return A view of the guesses in the buffer, good until it is next written.
 */
[[nodiscard]] std::span<const Guess> guesses(
        const std::string_view window, const Key& at, const unsigned char byte, const bool closes,
        std::vector<Guess>& out)
{
    out.clear();

    /**
     * @brief Writes one move of the matcher, without a close and, where the segment may close, with one.
     * @param matched How far the matcher has read after the byte.
     */
    const auto branch{[&](const std::size_t matched) {
        out.push_back(Guess{.matched = matched, .closes = false});

        if (closes)
        {
            out.push_back(Guess{.matched = matched, .closes = true});
        }
    }};

    if (at.marked)
    {
        branch(at.matched);

        return out;
    }

    // Outside the occurrence the matcher may stay outside; outside or inside, it may read the window's next byte.
    if (at.matched == 0)
    {
        branch(0);
    }

    if (static_cast<unsigned char>(window[at.matched]) == byte)
    {
        branch(at.matched + 1);
    }

    return out;
}

/**
 * @brief Runs the search window_counterexample(), boundary_counterexample() and crossing_counterexample() share: an
 *        occurrence of the window whose origin claim fails, in a completely tokenizable input.
 *
 * The key holds one scan, how far the matcher has read into the occurrence it guessed, and the origin bit, the next
 * byte counted as the occurrence's first while none has begun; it marks a branch once the occurrence has been read
 * through with the claim failed. The input's first byte begins a token. For the covering claim the bit records whether
 * the latest token start sits at the origin, so a later close clears it, and the claim is read off it before the final
 * byte, the token being read then being the covering one. For the gap claims the bit records whether the gap is a
 * boundary, so a close after the gap keeps it, and the claim is read off it once the final byte and its close are,
 * which is one step later than the final byte for the gap after the window: that gap is cut exactly when the final byte
 * closes its token. The occurrence refutes the cut claim with the bit clear and the crossed claim with it set.
 * @param simulator The compiled token set.
 * @param window The window, non-empty.
 * @param origin The offset into the window the claim is about, inside it for the covering claim and at most its length
 *        for a gap claim.
 * @param cap The most search states to hold at once, as search() reads it.
 * @param claim What the bit claims of the origin.
 * @return The witness, the shortest input on which an occurrence fails the claim, and whether the search settled the
 *         question.
 */
[[nodiscard]] Outcome origin_search(
        const Simulator& simulator, const std::string_view window, const std::size_t origin, const std::size_t cap,
        const Origin_claim claim)
{
    const Key start{.scans = {initial(simulator)}, .matched = 0, .at_origin = origin == 0, .marked = false};

    // Two buffers for the whole search, cleared per byte: the guesses, and the keys handed back as the step's view.
    std::vector<Guess> branches{};

    std::vector<Key> successors{};

    /**
     * @brief Returns the keys a byte leads to from a key, and whether the input may end on it with the claim refuted.
     * @param at The key the byte is read from.
     * @param byte The byte read.
     * @return The step.
     */
    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto position{at.scans.front()};

        if (!advance(simulator, position, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto closes{simulator.is_accepting(position.reading)};

        for (const auto [matched, cuts] : guesses(window, at, byte, closes, branches))
        {
            // The occurrence is read through on this byte once, and a marked branch depends on its scan alone.
            const auto through{!at.marked && matched == window.size()};

            // The bit after the byte and its close, a close beginning a token at gap matched: without one the covering
            // claim's bit carries inside the occurrence and clears outside it, and a gap claim's bit carries once the
            // gap is behind.
            const auto covering{cuts ? matched == origin : matched > 0 && at.at_origin};

            const auto boundary{(cuts && matched == origin) || (matched > origin && at.at_origin)};

            const auto bit{claim == Origin_claim::covers ? covering : boundary};

            // An occurrence read through that keeps the claim is dropped, the covering claim read off the bit before
            // the byte and a gap claim off the bit after it; one that fails the claim is the counterexample.
            const auto kept{claim == Origin_claim::covers ? at.at_origin : bit == (claim == Origin_claim::cut)};

            if (through && kept)
            {
                continue;
            }

            const auto marked{at.marked || through};

            // The input may end here when the counterexample is established and the segment being read closes on this
            // byte; the closed runs still alive never accepted, as every kept branch requires.
            if (marked && cuts)
            {
                return {.successors = successors, .ends = true};
            }

            const auto scan{cuts ? close(simulator, position) : position};

            // Past the counterexample the bit is clear, so that the branches agree.
            successors.push_back(
                    Key{.scans = {scan}, .matched = matched, .at_origin = bit && !marked, .marked = marked});
        }

        return {.successors = successors, .ends = false};
    }};

    return search(start, cap, expand);
}

/**
 * @brief Returns the verdict on a gap of a window, read off its two refutations.
 *
 * A refutation that found a witness settles its side, one that exhausted without a witness proves its claim, and one
 * the cap stopped leaves the verdict open. Both claims proved at once prove the window absent: no completely
 * tokenizable input holds an occurrence a token crosses at the gap, and none holds one cut there, so none holds an
 * occurrence at all. That is a verdict on the window, which boundary_profile() spreads over every gap; it is never
 * must, whose witness cut at the gap shows the window occurring, as never's witness crossed there does.
 * @param crossed What boundary_counterexample() found at the gap.
 * @param cut What crossing_counterexample() found at the gap.
 * @return The verdict, absent when both refutations exhausted without a witness.
 */
[[nodiscard]] Gap verdict(const Refutation& crossed, const Refutation& cut)
{
    if (!crossed.exhaustive || !cut.exhaustive)
    {
        return Gap::undetermined;
    }

    if (crossed.witness.empty() && cut.witness.empty())
    {
        return Gap::absent;
    }

    if (crossed.witness.empty())
    {
        return Gap::must;
    }

    if (cut.witness.empty())
    {
        return Gap::never;
    }

    return Gap::may;
}

} // namespace

Rescue rescue(const Simulator& simulator, const std::size_t cap)
{
    // The key marks a branch once some closed run has survived a byte, which is the rollback looked for. A run that
    // survives its first byte raises the mark; one that survived earlier leaves it raised, so the two need no telling
    // apart.
    const Key start{.scans = {initial(simulator)}, .matched = 0, .at_origin = false, .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors{};

    /**
     * @brief Returns the keys a byte leads to from a key, and whether the input may end on it after a rollback.
     * @param at The key the byte is read from.
     * @param byte The byte read.
     * @return The step.
     */
    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto position{at.scans.front()};

        if (!advance(simulator, position, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto rescued{at.marked || !position.closed.empty()};

        const auto closes{simulator.is_accepting(position.reading)};

        // The input may end here when the segment being read closes on this byte; with a rollback behind it, that is
        // the witness, and the closed runs still alive never accepted, as every kept branch requires.
        if (closes && rescued)
        {
            return {.successors = successors, .ends = true};
        }

        successors.push_back(Key{.scans = {position}, .matched = 0, .at_origin = false, .marked = rescued});

        if (closes)
        {
            const auto closed{close(simulator, position)};

            successors.push_back(Key{.scans = {closed}, .matched = 0, .at_origin = false, .marked = rescued});
        }

        return {.successors = successors, .ends = false};
    }};

    return as<Rescue>(search(start, cap, expand));
}

Difference boundary_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // The key holds the two scans' positions, mine first, and marks a branch once the two markings have diverged.
    const Key start{.scans = {initial(simulator), initial(other)}, .matched = 0, .at_origin = false, .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors{};

    /**
     * @brief Returns the keys a byte leads to from a key, and whether the input may end on it with the markings
     *        diverged.
     * @param at The key the byte is read from.
     * @param byte The byte read.
     * @return The step.
     */
    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto mine{at.scans[0]};

        auto theirs{at.scans[1]};

        if (!advance(simulator, mine, byte) || !advance(other, theirs, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto mine_closes{simulator.is_accepting(mine.reading)};

        const auto theirs_closes{other.is_accepting(theirs.reading)};

        // The input may end here only if both sides close their last segment on this byte.
        if (mine_closes && theirs_closes && at.marked)
        {
            return {.successors = successors, .ends = true};
        }

        // Each side closes or not, mine the slower choice, so the four markings come in the search's order.
        for (const auto& [mine_closed, theirs_closed] :
             {std::pair{false, false}, std::pair{false, true}, std::pair{true, false}, std::pair{true, true}})
        {
            if ((mine_closed && !mine_closes) || (theirs_closed && !theirs_closes))
            {
                continue;
            }

            const auto mine_next{mine_closed ? close(simulator, mine) : mine};

            const auto theirs_next{theirs_closed ? close(other, theirs) : theirs};

            const auto diverged{at.marked || mine_closed != theirs_closed};

            successors.push_back(
                    Key{.scans = {mine_next, theirs_next}, .matched = 0, .at_origin = false, .marked = diverged});
        }

        return {.successors = successors, .ends = false};
    }};

    return as<Difference>(search(start, cap, expand));
}

Occurrence window_occurrence(const Simulator& simulator, const std::string_view window, const std::size_t cap)
{
    // The key holds one scan and how far the matcher has read into the occurrence it guessed, and marks a branch once
    // the whole window has been read; the empty window has been read before any byte.
    const Key start{.scans = {initial(simulator)}, .matched = 0, .at_origin = false, .marked = window.empty()};

    // Two buffers for the whole search, cleared per byte: the guesses, and the keys handed back as the step's view.
    std::vector<Guess> branches{};

    std::vector<Key> successors{};

    /**
     * @brief Returns the keys a byte leads to from a key, and whether the input may end on it with the window read.
     * @param at The key the byte is read from.
     * @param byte The byte read.
     * @return The step.
     */
    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto position{at.scans.front()};

        if (!advance(simulator, position, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto accepting{simulator.is_accepting(position.reading)};

        for (const auto [matched, closes] : guesses(window, at, byte, accepting, branches))
        {
            const auto marked{matched == window.size()};

            // The input may end here when the whole window has been read and the segment being read closes on this
            // byte; the closed runs still alive never accepted, as every kept branch requires.
            if (marked && closes)
            {
                return {.successors = successors, .ends = true};
            }

            const auto scan{closes ? close(simulator, position) : position};

            successors.push_back(Key{.scans = {scan}, .matched = matched, .at_origin = false, .marked = marked});
        }

        return {.successors = successors, .ends = false};
    }};

    return as<Occurrence>(search(start, cap, expand));
}

Counterexample window_counterexample(
        const Simulator& simulator, const std::string_view window, const std::size_t origin, const std::size_t cap)
{
    if (window.empty() || origin >= window.size())
    {
        throw std::invalid_argument{"window_counterexample: the window is empty or its origin lies outside it"};
    }

    return as<Counterexample>(origin_search(simulator, window, origin, cap, Origin_claim::covers));
}

Refutation boundary_counterexample(
        const Simulator& simulator, const std::string_view window, const std::size_t gap, const std::size_t cap)
{
    if (window.empty() || gap > window.size())
    {
        throw std::invalid_argument{"boundary_counterexample: the window is empty or its gap lies past its end"};
    }

    return as<Refutation>(origin_search(simulator, window, gap, cap, Origin_claim::cut));
}

Refutation crossing_counterexample(
        const Simulator& simulator, const std::string_view window, const std::size_t gap, const std::size_t cap)
{
    if (window.empty() || gap > window.size())
    {
        throw std::invalid_argument{"crossing_counterexample: the window is empty or its gap lies past its end"};
    }

    return as<Refutation>(origin_search(simulator, window, gap, cap, Origin_claim::crossed));
}

std::vector<Gap_verdict> boundary_profile(
        const Simulator& simulator, const std::string_view window, const std::size_t cap)
{
    if (window.empty())
    {
        throw std::invalid_argument{"boundary_profile: the empty window has no occurrence to hold a gap in"};
    }

    // Absence is a property of the window rather than of a gap: given at every gap, both claims proved there, and
    // settled by the first proof of it, the occurrence search's, one search where a gap costs two, or a gap's, both of
    // its refutations exhausted without a witness, which can come under a cap that stops the occurrence search.
    const Refutation proved{.witness = {}, .exhaustive = true};

    std::vector absent(window.size() + 1, Gap_verdict{.verdict = Gap::absent, .crossed = proved, .cut = proved});

    const auto [witness, exhaustive]{window_occurrence(simulator, window, cap)};

    if (exhaustive && witness.empty())
    {
        return absent;
    }

    std::vector<Gap_verdict> profile{};

    for (std::size_t gap{0}; gap <= window.size(); ++gap)
    {
        const auto crossed{boundary_counterexample(simulator, window, gap, cap)};

        const auto cut{crossing_counterexample(simulator, window, gap, cap)};

        const auto decided{verdict(crossed, cut)};

        if (decided == Gap::absent)
        {
            return absent;
        }

        profile.push_back(Gap_verdict{.verdict = decided, .crossed = crossed, .cut = cut});
    }

    return profile;
}

Separation segmentation_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // The key holds the two scans' positions, mine first, either of them dead, and marks nothing: the event, that
    // exactly one side accepts the marked run, ends the input where it happens. Both sides accept the empty run.
    const Key start{.scans = {initial(simulator), initial(other)}, .matched = 0, .at_origin = false, .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors{};

    /**
     * @brief Returns the keys a byte leads to from a key, and whether the input may end on it with exactly one side
     *        accepting.
     * @param at The key the byte is read from.
     * @param byte The byte read.
     * @return The step.
     */
    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        // The two marked symbols the byte makes, without a close and with one, each read into both sides at once.
        for (const auto closes : {false, true})
        {
            auto mine{at.scans[0]};

            auto theirs{at.scans[1]};

            const auto mine_alive{read_marked(simulator, mine, byte, closes)};

            const auto theirs_alive{read_marked(other, theirs, byte, closes)};

            if (!mine_alive && !theirs_alive)
            {
                continue;
            }

            const auto mine_accepts{mine_alive && simulator.is_accepting(mine.reading)};

            const auto theirs_accepts{theirs_alive && other.is_accepting(theirs.reading)};

            // The input ends here when exactly one side accepts the marked run read so far.
            if (mine_accepts != theirs_accepts)
            {
                return {.successors = successors, .ends = true};
            }

            successors.push_back(Key{.scans = {mine, theirs}, .matched = 0, .at_origin = false, .marked = false});
        }

        return {.successors = successors, .ends = false};
    }};

    auto [witness, exhaustive]{search(start, cap, expand)};

    if (witness.empty())
    {
        return {.witness = std::move(witness), .half = std::nullopt, .exhaustive = exhaustive};
    }

    // The half is a fact about the witness: the side accepting the run tokenizes its bytes completely, and the other
    // side either does not, or does and cuts them apart.
    const auto both_tokenize{tokenizes(simulator, witness) && tokenizes(other, witness)};

    const auto half{both_tokenize ? Separation_half::boundary : Separation_half::domain};

    return {.witness = std::move(witness), .half = half, .exhaustive = exhaustive};
}

} // namespace munch::dfa

#include "munch/dfa/boundary_search.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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
 * with one; domain_difference() walks two, the second guessed to fail, with the origin bit recording whether that scan
 * stands at a token start, where it may begin to fail, and marks a branch once it has; segmentation_difference() walks
 * two scans over one guessed marking, either of them dead, and marks nothing, its event ending the input where it
 * happens; modulo_counterexample() walks two, the whole input's and one chunk's guessed to fail, with the matcher's
 * progress holding the phase of the cut and the origin bit and the mark as for domain_difference(). The matcher's
 * progress and the origin bit are the two things besides the scans a future depends on, so they are in the key, zero
 * and clear for the decisions that match nothing or ask no origin.
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
     * @brief How far the window matcher has read into the occurrence it guessed, or for a cut the phase of the cut.
     */
    std::size_t matched{};

    /**
     * @brief The origin bit: whether the latest token start sits at the origin, or for a gap claim whether the gap is a
     *        boundary, or for a scan guessed to fail whether it stands at a token start.
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
 * @brief One move a byte opens for a scan guessed to fail: its position after the byte, whether it stands at a token
 *        start there, and whether it has begun to fail.
 */
struct Failing_move
{
    /**
     * @brief The position after the byte.
     */
    Position position{};

    /**
     * @brief Whether the scan stands at a token start after the byte, where it may begin to fail.
     */
    bool at_start{};

    /**
     * @brief Whether the scan has begun to fail.
     */
    bool failing{};
};

/**
 * @brief The phase of a cut before any byte is read, where no cut may stand.
 */
constexpr std::size_t before_input{0};

/**
 * @brief The phase of a cut inside the chunk before it, where the cut may stand before a byte holding the symbol.
 */
constexpr std::size_t before_cut{1};

/**
 * @brief The phase of a cut past it, the chunk from it being read.
 */
constexpr std::size_t past_cut{2};

/**
 * @brief A letter of the words the kept-output product emits: length_letter, the kinds' letters after it, and in a
 *        delay their inverses as negated letters.
 */
using Letter_t = std::int64_t;

/**
 * @brief A word over the letters, a scan's output or the delay between two outputs.
 */
using Word_t = std::vector<Letter_t>;

/**
 * @brief The letter z, emitted once for each byte of a kept token.
 */
constexpr Letter_t length_letter{1};

/**
 * @brief The letter of the first kept kind; the kept kinds take the letters from it on, in the order of their first
 *        accepting states.
 */
constexpr Letter_t first_kind_letter{2};

/**
 * @brief The letter of a state that accepts no kept kind, a discarded one or none at all.
 */
constexpr Letter_t no_letter{0};

/**
 * @brief The guess a scan emitting kept tokens makes at a token's first byte about the kind it closes as, checked where
 *        it closes.
 */
enum class Keep : std::uint8_t
{
    /**
     * @brief Between tokens, before the next token's first byte makes the guess.
     */
    none,

    /**
     * @brief The token being read closes as a discarded kind.
     */
    discarded,

    /**
     * @brief The token being read closes as a kept kind.
     */
    kept
};

/**
 * @brief A scan emitting kept tokens: its position and the guess for the token being read.
 */
struct Kept_scan
{
    /**
     * @brief Ordered member by member, so scans order the vertices that hold them.
     */
    auto operator<=>(const Kept_scan&) const = default;

    /**
     * @brief The position.
     */
    Position position{};

    /**
     * @brief The guess for the token being read, none between tokens.
     */
    Keep keep{};
};

/**
 * @brief One move of a scan emitting kept tokens on a byte: the scan after it and the word the byte emits.
 */
struct Kept_move
{
    /**
     * @brief The scan after the byte.
     */
    Kept_scan scan{};

    /**
     * @brief The word the byte emits: length_letter on a byte of a kept token, and its kind's letter where it closes.
     */
    Word_t emitted{};
};

/**
 * @brief A vertex of the kept-output product: the whole input's scan, the scan restarted at the cut, which reads the
 *        chunk before the cut until then, and the phase of the cut.
 */
struct Kept_vertex
{
    /**
     * @brief Ordered member by member, so vertices index the map of vertices visited.
     */
    auto operator<=>(const Kept_vertex&) const = default;

    /**
     * @brief The whole input's scan.
     */
    Kept_scan whole{};

    /**
     * @brief The scan of the chunks, restarted at the cut.
     */
    Kept_scan split{};

    /**
     * @brief The phase of the cut.
     */
    std::size_t phase{};
};

/**
 * @brief An edge of the kept-output product: the vertices it joins, the byte it reads and the two scans' words.
 */
struct Kept_edge
{
    /**
     * @brief The number of the vertex it leaves.
     */
    std::size_t source{};

    /**
     * @brief The number of the vertex it enters.
     */
    std::size_t target{};

    /**
     * @brief The byte it reads.
     */
    char byte{};

    /**
     * @brief The word the whole input's scan emits on it.
     */
    Word_t whole{};

    /**
     * @brief The word the scan of the chunks emits on it.
     */
    Word_t split{};
};

/**
 * @brief The kept-output product as a graph, its vertices numbered in the order the walk admitted them, the start
 *        first.
 */
struct Kept_graph
{
    /**
     * @brief The edges, in the order built.
     */
    std::vector<Kept_edge> edges{};

    /**
     * @brief Per vertex, the edges leaving it.
     */
    std::vector<std::vector<std::size_t>> out{};

    /**
     * @brief Per vertex, the edges entering it.
     */
    std::vector<std::vector<std::size_t>> in{};

    /**
     * @brief Per vertex, whether an input may end there: past the cut, with both scans between tokens.
     */
    std::vector<bool> accepting{};
};

/**
 * @brief The marker of a vertex with no edge on a path.
 */
constexpr std::size_t no_edge{std::numeric_limits<std::size_t>::max()};

/**
 * @brief The shortest accepting suffixes of the kept-output product, found backwards from its accepting vertices.
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
 * @brief The shortest stems of the kept-output product from its start over the live vertices, each with its delay.
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
     * @brief Per vertex, the delay its stem carries, the reduced word between the two scans' outputs along it.
     */
    std::vector<Word_t> delay{};
};

/**
 * @brief A cut of an input that is not safe modulo the discarded kinds, and how it fails.
 */
struct Failing_cut
{
    /**
     * @brief The offset of the cut.
     */
    std::size_t cut{};

    /**
     * @brief How it fails.
     */
    Cut_failure failure{};
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
 * @brief Advances the closed runs of a position by one byte.
 * @param simulator The compiled token set the position scans.
 * @param closed The closed runs, advanced in place.
 * @param byte The byte read.
 * @return False abandons the branch: a closed run accepted and the marking is not the greedy one. True leaves in closed
 *         exactly the runs that survived the byte.
 */
[[nodiscard]] bool advance_closed(const Simulator& simulator, std::vector<State_t>& closed, const unsigned char byte)
{
    std::vector<State_t> survived{};

    for (const auto state : closed)
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

    closed = std::move(survived);

    return true;
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

    return advance_closed(simulator, position.closed, byte);
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
 * @brief Advances by one byte the position of a scan guessed to fail: the closed runs as advance() moves them, and the
 *        run of the segment at which the scan finds no token, which may die and then stands in the state one past the
 *        table, where it stays.
 * @param simulator The compiled token set the position scans.
 * @param position The position, advanced in place.
 * @param byte The byte read.
 * @return False abandons the branch: either the failing segment's run accepted, so the scan finds a token there after
 *         all, or a closed run accepted and the marking is not the greedy one. True leaves in closed exactly the runs
 *         that survived the byte.
 */
[[nodiscard]] bool fail(const Simulator& simulator, Position& position, const unsigned char byte)
{
    // The dead run stands in the state one past the table, as a dead side of the marked-language product does.
    const auto nowhere{simulator.state_count()};

    if (position.reading != nowhere)
    {
        const auto next{simulator.step(position.reading, byte)};

        if (next && simulator.is_accepting(*next))
        {
            return false;
        }

        position.reading = next.value_or(nowhere);
    }

    return advance_closed(simulator, position.closed, byte);
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

/**
 * @brief Returns the moves a byte opens for a scan guessed to fail: one that has begun to fail fails on, one standing
 *        at a token start may begin to there, and one that has not begun scans on, without a close and, where its
 *        segment accepts, with one.
 *
 * The scan domain_difference() guesses to fail, and modulo_counterexample() each chunk. Failing reads on with fail(),
 * whose run of the failing segment starts at the initial state the token start stands in; scanning on reads with
 * advance() and close(). The moves come in the order the searches explore them, the failing one first.
 * @param simulator The compiled token set the scan reads.
 * @param position The scan's position before the byte.
 * @param at_start Whether it stands at a token start.
 * @param failing Whether it has begun to fail.
 * @param byte The byte read.
 * @param out The buffer the moves are written to, cleared first and kept by the caller across bytes.
 * @return A view of the moves in the buffer, good until it is next written.
 */
[[nodiscard]] std::span<const Failing_move> failing_moves(
        const Simulator& simulator, const Position& position, const bool at_start, const bool failing,
        const unsigned char byte, std::vector<Failing_move>& out)
{
    out.clear();

    auto failed{position};

    if ((failing || at_start) && fail(simulator, failed, byte))
    {
        out.push_back(Failing_move{.position = failed, .at_start = false, .failing = true});
    }

    if (failing)
    {
        return out;
    }

    auto going{position};

    if (!advance(simulator, going, byte))
    {
        return out;
    }

    out.push_back(Failing_move{.position = going, .at_start = false, .failing = false});

    if (simulator.is_accepting(going.reading))
    {
        out.push_back(Failing_move{.position = close(simulator, going), .at_start = true, .failing = false});
    }

    return out;
}

/**
 * @brief Runs the search domain_difference() asks in each order: an input one token set tokenizes completely and the
 *        scan of the other fails on.
 *
 * The key holds two scans, the complete side's first, the origin bit recording whether the failed side stands at a
 * token start, set at the input's start and at each of its closes, and the mark whether the failed side has begun to
 * fail. The complete side is the scan of rescue() and boundary_difference(), closing where its segment accepts. Before
 * its mark the failed side is the same scan, and at a token start it may also begin to fail, reading on from there with
 * fail(), whose run of the failing segment starts at the initial state the token start stands in; after its mark it
 * fails on. The input may end once the failed side has begun to fail and the complete side's segment closes on the
 * byte, the closed runs of both sides still alive never having accepted, as every kept branch requires.
 * @param complete The token set that tokenizes the witness completely.
 * @param failed The token set whose scan fails on the witness.
 * @param cap The most search states to hold at once, as search() reads it.
 * @return The witness, the shortest input the one tokenizes completely and the other does not, and whether the search
 *         settled the question.
 */
[[nodiscard]] Outcome exclusion_search(const Simulator& complete, const Simulator& failed, const std::size_t cap)
{
    // The input's start is a token start of the failed side, which may fail from its first byte.
    const Key start{.scans = {initial(complete), initial(failed)}, .matched = 0, .at_origin = true, .marked = false};

    // Two buffers for the whole search, cleared per byte: the failed side's moves, and the keys handed back as the
    // step's view.
    std::vector<Failing_move> moves{};

    std::vector<Key> successors{};

    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto whole{at.scans[0]};

        if (!advance(complete, whole, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto whole_closes{complete.is_accepting(whole.reading)};

        // Each failed-side move goes beside the complete side, without a close and, where it may close, with one.
        const auto beside{[&](const Position& theirs, const bool at_start, const bool marked) {
            successors.push_back(Key{.scans = {whole, theirs}, .matched = 0, .at_origin = at_start, .marked = marked});

            if (whole_closes)
            {
                const auto closed{close(complete, whole)};

                successors.push_back(
                        Key{.scans = {closed, theirs}, .matched = 0, .at_origin = at_start, .marked = marked});
            }
        }};

        for (const auto& [theirs, at_start, failing] :
             failing_moves(failed, at.scans[1], at.at_origin, at.marked, byte, moves))
        {
            if (failing && whole_closes)
            {
                return {.successors = successors, .ends = true};
            }

            beside(theirs, at_start, failing);
        }

        return {.successors = successors, .ends = false};
    }};

    return search(start, cap, expand);
}

/**
 * @brief Runs the search modulo_counterexample() asks of one chunk: a completely tokenizable input with a cut before a
 *        byte holding the symbol, past the first, that leaves the chunk unconsumed.
 *
 * The search exclusion_search() runs with the one token set on both sides, the second scan reading the chunk alone. The
 * key holds the whole input's scan, the chunk's scan guessed to fail with the origin bit and the mark as there, and in
 * the matcher's progress the phase of the cut, which may stand before a byte holding the symbol once a byte has been
 * read. The chunk before the cut is scanned from the input's start and must have begun to fail when the cut comes,
 * after which it is dropped and the key holds it as the initial position, failed; the chunk from the cut is held as the
 * initial position until the cut and scanned from there, a token start. The input may end once past the cut, with the
 * chunk failed and the whole input's segment closing on the byte, the closed runs still alive never having accepted, as
 * every kept branch requires.
 * @param simulator The compiled token set.
 * @param symbol The byte the cut stands before.
 * @param chunk The chunk the scan fails on, Cut_failure::left_incomplete for the one before the cut and
 *        Cut_failure::right_incomplete for the one from it.
 * @param cap The most search states to hold at once, as search() reads it.
 * @return The witness, the shortest input with such a cut, and whether the search settled the question.
 */
[[nodiscard]] Outcome chunk_search(
        const Simulator& simulator, const unsigned char symbol, const Cut_failure chunk, const std::size_t cap)
{
    const auto left{chunk == Cut_failure::left_incomplete};

    // The input's start is a token start of the chunk before the cut, which may fail from its first byte.
    const Key start{
            .scans = {initial(simulator), initial(simulator)},
            .matched = before_input,
            .at_origin = left,
            .marked = false};

    // Two buffers for the whole search, cleared per byte: the chunk's moves, and the keys handed back as the step's
    // view.
    std::vector<Failing_move> moves{};

    std::vector<Key> successors{};

    const auto expand{[&](const Key& at, const unsigned char byte) -> Step {
        successors.clear();

        auto whole{at.scans[0]};

        if (!advance(simulator, whole, byte))
        {
            return {.successors = successors, .ends = false};
        }

        const auto whole_closes{simulator.is_accepting(whole.reading)};

        // Each move of the chunk goes beside the whole input's scan, without a close and, where it may close, with one.
        const auto beside{[&](const Position& part, const bool at_start, const bool failing, const std::size_t phase) {
            successors.push_back(
                    Key{.scans = {whole, part}, .matched = phase, .at_origin = at_start, .marked = failing});

            if (whole_closes)
            {
                const auto closed{close(simulator, whole)};

                successors.push_back(
                        Key{.scans = {closed, part}, .matched = phase, .at_origin = at_start, .marked = failing});
            }
        }};

        // The chunk scanned on the byte; true once past the cut it has failed and the whole input may end here.
        const auto scan_chunk{
                [&](const Position& part, const bool at_start, const bool failing, const std::size_t phase) {
                    for (const auto& [moved, moved_start, moved_failing] :
                         failing_moves(simulator, part, at_start, failing, byte, moves))
                    {
                        if (phase == past_cut && moved_failing && whole_closes)
                        {
                            return true;
                        }

                        beside(moved, moved_start, moved_failing, phase);
                    }

                    return false;
                }};

        // Past the cut the chunk before it has failed, so the input may end wherever the whole input's segment closes.
        if (at.matched == past_cut && left)
        {
            if (!whole_closes)
            {
                beside(at.scans[1], false, true, past_cut);
            }

            return {.successors = successors, .ends = whole_closes};
        }

        if (at.matched == past_cut)
        {
            const auto failed{scan_chunk(at.scans[1], at.at_origin, at.marked, past_cut)};

            return {.successors = successors, .ends = failed};
        }

        // Without a cut before the byte, the chunk before the cut reads it and the chunk from the cut waits.
        if (left)
        {
            std::ignore = scan_chunk(at.scans[1], at.at_origin, at.marked, before_cut);
        }
        else
        {
            beside(at.scans[1], false, false, before_cut);
        }

        const auto cuts{at.matched == before_cut && byte == symbol};

        if (!cuts || (left && !at.marked))
        {
            return {.successors = successors, .ends = false};
        }

        // With the cut before the byte, the chunk before it has failed and is dropped, or the chunk from it begins.
        if (left)
        {
            if (!whole_closes)
            {
                beside(initial(simulator), false, true, past_cut);
            }

            return {.successors = successors, .ends = whole_closes};
        }

        const auto failed{scan_chunk(initial(simulator), true, false, past_cut)};

        return {.successors = successors, .ends = failed};
    }};

    return search(start, cap, expand);
}

/**
 * @brief Returns the letter each state's kind emits where a kept token closes on it: the kept kinds numbered from
 *        first_kind_letter in the order of their first accepting states, and no_letter for a state accepting a
 *        discarded kind or none.
 * @param simulator The compiled token set.
 * @param ignored The IDs of the discarded tokens.
 * @return The letters, one per state.
 */
[[nodiscard]] std::vector<Letter_t> kind_letters(const Simulator& simulator, const std::span<const std::size_t> ignored)
{
    std::vector letters(simulator.state_count(), no_letter);

    std::map<std::size_t, Letter_t> kinds{};

    for (std::size_t state{0}; state < letters.size(); ++state)
    {
        const auto token{simulator.accepted(state)};

        if (!token || std::ranges::contains(ignored, token->id()))
        {
            continue;
        }

        const auto next{first_kind_letter + static_cast<Letter_t>(kinds.size())};

        const auto [entry, inserted]{kinds.try_emplace(token->id(), next)};

        const auto& [id, letter]{*entry};

        letters[state] = letter;
    }

    return letters;
}

/**
 * @brief Returns the moves a byte opens for a scan emitting kept tokens: the token being read reads on, guessed both
 *        ways at its first byte, and where it accepts as the kind it was guessed to be, it may also close.
 *
 * A byte of a token guessed kept emits length_letter, and a kept token's close emits its kind's letter after it, so
 * that the words of a scan code its kept stream, every kind's letter ending exactly one run of length_letter. A wrong
 * guess cannot close, so it dies with its branch. The moves come in the order the walk explores them, the discarded
 * guess before the kept one and a move without a close before the same move with one.
 * @param simulator The compiled token set.
 * @param letters The letter each state's kind emits, as kind_letters() numbers them.
 * @param scan The scan before the byte.
 * @param byte The byte read.
 * @param out The buffer the moves are written to, cleared first and kept by the caller across bytes.
 * @return A view of the moves in the buffer, good until it is next written.
 */
[[nodiscard]] std::span<const Kept_move> kept_moves(
        const Simulator& simulator, const std::vector<Letter_t>& letters, const Kept_scan& scan,
        const unsigned char byte, std::vector<Kept_move>& out)
{
    out.clear();

    auto position{scan.position};

    if (!advance(simulator, position, byte))
    {
        return out;
    }

    const auto accepting{simulator.is_accepting(position.reading)};

    const auto letter{letters[position.reading]};

    for (const auto keep : {Keep::discarded, Keep::kept})
    {
        // Between tokens the byte begins one, guessed both ways; inside one it keeps the guess its first byte made.
        if (scan.keep != Keep::none && scan.keep != keep)
        {
            continue;
        }

        const auto kept{keep == Keep::kept};

        const Word_t emitted{kept ? Word_t{length_letter} : Word_t{}};

        out.push_back(Kept_move{.scan = {.position = position, .keep = keep}, .emitted = emitted});

        const auto closes_as_guessed{accepting && (letter != no_letter) == kept};

        if (!closes_as_guessed)
        {
            continue;
        }

        auto closing{emitted};

        if (kept)
        {
            closing.push_back(letter);
        }

        const Kept_scan closed{.position = close(simulator, position), .keep = Keep::none};

        out.push_back(Kept_move{.scan = closed, .emitted = std::move(closing)});
    }

    return out;
}

/**
 * @brief Builds the kept-output product breadth first: the whole input's scan beside the scan of the chunks, each
 *        emitting its kept tokens, the second restarted where the cut is guessed before a byte holding the symbol past
 *        the first, which it may be only where that scan stands between tokens, the chunk before the cut consumed.
 *
 * A vertex is accepting past the cut with both scans between tokens, so an accepting path is a completely tokenizable
 * input and a cut both of whose chunks the scan consumes, carrying the two kept streams as the words along it. The walk
 * holds its vertices as the searches hold their keys: one beyond the cap is never admitted, and the walk gives up
 * instead.
 * @param simulator The compiled token set.
 * @param letters The letter each state's kind emits, as kind_letters() numbers them.
 * @param symbol The byte the cut stands before.
 * @param cap The most vertices to hold at once, the start among them; zero holds nothing.
 * @return The graph, or std::nullopt when the cap stopped the walk.
 */
[[nodiscard]] std::optional<Kept_graph> kept_product(
        const Simulator& simulator, const std::vector<Letter_t>& letters, const unsigned char symbol,
        const std::size_t cap)
{
    if (cap == 0)
    {
        return std::nullopt;
    }

    const Kept_scan fresh{.position = initial(simulator), .keep = Keep::none};

    std::map<Kept_vertex, std::size_t> numbers{};

    std::vector<std::map<Kept_vertex, std::size_t>::const_iterator> admitted{};

    Kept_graph graph{};

    // The number of a vertex, admitted when it is new while the ceiling allows, nothing when it would pass it.
    const auto number_of{[&](const Kept_vertex& vertex) -> std::optional<std::size_t> {
        const auto found{numbers.find(vertex)};

        if (found != numbers.end())
        {
            return found->second;
        }

        if (numbers.size() >= cap)
        {
            return std::nullopt;
        }

        const auto number{admitted.size()};

        const auto [entry, inserted]{numbers.try_emplace(vertex, number)};

        admitted.push_back(entry);

        const auto& [whole, split, phase]{vertex};

        const auto between{whole.keep == Keep::none && split.keep == Keep::none};

        graph.out.emplace_back();

        graph.in.emplace_back();

        graph.accepting.push_back(phase == past_cut && between);

        return number;
    }};

    std::ignore = number_of(Kept_vertex{.whole = fresh, .split = fresh, .phase = before_input});

    // Two buffers for the whole walk, cleared per byte: the whole input's moves and the moves of the chunks' scan.
    std::vector<Kept_move> whole_buffer{};

    std::vector<Kept_move> split_buffer{};

    // Every move of the one scan beside every move of the other, each an edge; false when the cap stops the walk.
    const auto link{[&](const std::size_t from, const std::span<const Kept_move> wholes,
                        const std::span<const Kept_move> splits, const std::size_t phase, const char byte) {
        for (const auto& [whole, whole_word] : wholes)
        {
            for (const auto& [split, split_word] : splits)
            {
                const auto target{number_of(Kept_vertex{.whole = whole, .split = split, .phase = phase})};

                if (!target)
                {
                    return false;
                }

                const auto edge{graph.edges.size()};

                graph.out[from].push_back(edge);

                graph.in[*target].push_back(edge);

                graph.edges.push_back(Kept_edge{
                        .source = from,
                        .target = *target,
                        .byte = byte,
                        .whole = whole_word,
                        .split = split_word});
            }
        }

        return true;
    }};

    // Vertices are expanded in the order they were admitted, which is breadth-first.
    for (std::size_t at{0}; at < admitted.size(); ++at)
    {
        const auto& [vertex, number]{*admitted[at]};

        const auto& [whole, split, phase]{vertex};

        const auto onward{phase == before_input ? before_cut : phase};

        for (std::size_t value{0}; value < Simulator::symbol_count; ++value)
        {
            const auto byte{static_cast<unsigned char>(value)};

            const auto read{static_cast<char>(byte)};

            const auto wholes{kept_moves(simulator, letters, whole, byte, whole_buffer)};

            const auto splits{kept_moves(simulator, letters, split, byte, split_buffer)};

            if (!link(at, wholes, splits, onward, read))
            {
                return std::nullopt;
            }

            // The cut may stand before the byte where the chunk before it is consumed, the chunks' scan restarting.
            const auto cuts{phase == before_cut && byte == symbol && split.keep == Keep::none};

            if (!cuts)
            {
                continue;
            }

            const auto restarted{kept_moves(simulator, letters, fresh, byte, split_buffer)};

            if (!link(at, wholes, restarted, past_cut, read))
            {
                return std::nullopt;
            }
        }
    }

    return graph;
}

/**
 * @brief Returns the delay of two outputs A and B, the reduced group word A^-1 B, after one more edge.
 * @param delay The delay before the edge.
 * @param edge The edge, whose whole-input word extends A and whose chunks' word extends B.
 * @return The delay after it, reduced.
 */
[[nodiscard]] Word_t delayed(const Word_t& delay, const Kept_edge& edge)
{
    Word_t result{};

    const auto push{[&result](const Letter_t letter) {
        if (!result.empty() && result.back() == -letter)
        {
            result.pop_back();

            return;
        }

        result.push_back(letter);
    }};

    for (const auto letter : edge.whole | std::views::reverse)
    {
        push(-letter);
    }

    std::ranges::for_each(delay, push);

    std::ranges::for_each(edge.split, push);

    return result;
}

/**
 * @brief Finds the shortest accepting suffixes of the kept-output product, breadth first backwards from its accepting
 *        vertices.
 * @param graph The graph.
 * @return The suffixes and the live vertices.
 */
[[nodiscard]] Accepting_suffixes accepting_suffixes(const Kept_graph& graph)
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
 * @brief Finds the shortest stems of the kept-output product from its start over the live vertices, breadth first, each
 *        with the delay it carries.
 * @param graph The graph.
 * @param live Per vertex, whether an accepting vertex is reachable from it.
 * @return The stems, the vertices they reach and their delays.
 */
[[nodiscard]] Shortest_stems shortest_stems(const Kept_graph& graph, const std::vector<bool>& live)
{
    const auto count{graph.out.size()};

    Shortest_stems stems{
            .stem = std::vector(count, no_edge),
            .reached = std::vector(count, false),
            .delay = std::vector<Word_t>(count)};

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
 * @brief Decides whether every accepting path of the kept-output product carries equal outputs, by the delays of
 *        shortest stems, returning one that does not.
 *
 * Every path into a live vertex carries one delay exactly when every accepting path carries equal outputs: a common
 * accepting suffix fixes the delay of every path into its vertex, and with one delay per vertex an accepting path's,
 * empty at an accepting vertex, is its outputs' equality. So the test is that the delay along every edge between live
 * vertices carries the stem's delay at its source to the stem's at its target, and that the stem of every accepting
 * vertex carries none. Where an edge does not, of the two paths through its target that share a shortest accepting
 * suffix, the stem of the target and the stem of the source with the edge, one carries unequal outputs, each of fewer
 * edges than twice the vertices.
 * @param graph The graph.
 * @return The edges of an accepting path with unequal outputs, or std::nullopt when every one carries equal outputs.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> unequal_path(const Kept_graph& graph)
{
    const auto count{graph.out.size()};

    const auto [onward, live]{accepting_suffixes(graph)};

    // With no accepting path at all there is nothing to compare.
    if (!live[0])
    {
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
        Word_t whole{};

        Word_t split{};

        for (const auto edge : path)
        {
            const auto& [source, target, byte, whole_word, split_word]{graph.edges[edge]};

            whole.insert(whole.end(), whole_word.begin(), whole_word.end());

            split.insert(split.end(), split_word.begin(), split_word.end());
        }

        return whole != split;
    }};

    // The two paths share an accepting suffix and their stems' delays differ, so one of them carries unequal outputs.
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
 * @brief Runs the comparison modulo_counterexample() makes once neither chunk fails: a completely tokenizable input
 *        with a cut before a byte holding the symbol, past the first, both of whose chunks the scan consumes and whose
 *        kept tokens differ from the whole input's.
 * @param simulator The compiled token set.
 * @param ignored The IDs of the discarded tokens.
 * @param symbol The byte the cut stands before.
 * @param cap The most vertices the product holds at once, as kept_product() reads it.
 * @return The witness, an input with unequal kept streams of fewer bytes than twice the product's vertices, and whether
 *         the walk settled the question.
 */
[[nodiscard]] Outcome stream_search(
        const Simulator& simulator, const std::span<const std::size_t> ignored, const unsigned char symbol,
        const std::size_t cap)
{
    const auto letters{kind_letters(simulator, ignored)};

    const auto graph{kept_product(simulator, letters, symbol, cap)};

    if (!graph)
    {
        return {.witness = {}, .exhaustive = false};
    }

    const auto path{unequal_path(*graph)};

    if (!path)
    {
        return {.witness = {}, .exhaustive = true};
    }

    const auto byte_of{[&graph](const std::size_t edge) { return graph->edges[edge].byte; }};

    std::string witness{};

    std::ranges::transform(*path, std::back_inserter(witness), byte_of);

    return {.witness = std::move(witness), .exhaustive = true};
}

/**
 * @brief Returns the kept tokens of an input, by the scan itself: maximal munch from every token boundary to the end,
 *        the tokens of the discarded kinds deleted.
 * @param simulator The compiled token set.
 * @param ignored The IDs of the discarded tokens.
 * @param input The input.
 * @return Each kept token's ID and length, in order, or std::nullopt when the scan does not consume every byte.
 */
[[nodiscard]] std::optional<std::vector<std::pair<std::size_t, std::size_t>>> kept_tokens(
        const Simulator& simulator, const std::span<const std::size_t> ignored, const std::string_view input)
{
    std::vector<std::pair<std::size_t, std::size_t>> kept{};

    for (std::size_t at{0}; at < input.size();)
    {
        const auto [token, length]{simulator.run(input.substr(at))};

        if (!token || length == 0)
        {
            return std::nullopt;
        }

        if (!std::ranges::contains(ignored, token->id()))
        {
            kept.emplace_back(token->id(), length);
        }

        at += length;
    }

    return kept;
}

/**
 * @brief Returns the first cut before a byte holding the symbol, past the first, at which a completely tokenizable
 *        input is not safe modulo the discarded kinds, and how it fails there, read off the scans of its chunks.
 * @param simulator The compiled token set.
 * @param ignored The IDs of the discarded tokens.
 * @param input The input.
 * @param symbol The byte the cuts stand before.
 * @return The cut and its failure, or std::nullopt when the input is not completely tokenizable or every cut is safe.
 */
[[nodiscard]] std::optional<Failing_cut> first_unsafe_cut(
        const Simulator& simulator, const std::span<const std::size_t> ignored, const std::string_view input,
        const char symbol)
{
    const auto whole{kept_tokens(simulator, ignored, input)};

    if (!whole)
    {
        return std::nullopt;
    }

    for (std::size_t cut{1}; cut < input.size(); ++cut)
    {
        if (input[cut] != symbol)
        {
            continue;
        }

        const auto left{kept_tokens(simulator, ignored, input.substr(0, cut))};

        if (!left)
        {
            return Failing_cut{.cut = cut, .failure = Cut_failure::left_incomplete};
        }

        const auto right{kept_tokens(simulator, ignored, input.substr(cut))};

        if (!right)
        {
            return Failing_cut{.cut = cut, .failure = Cut_failure::right_incomplete};
        }

        auto spliced{*left};

        spliced.insert(spliced.end(), right->begin(), right->end());

        if (spliced != *whole)
        {
            return Failing_cut{.cut = cut, .failure = Cut_failure::streams_differ};
        }
    }

    return std::nullopt;
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

Exclusion domain_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // One search per side: the inputs the first set tokenizes completely and the second fails on, then the reverse. A
    // search the cap stops leaves the question open, a shorter witness possibly lying on its side.
    const Exclusion unsettled{.witness = {}, .side = std::nullopt, .exhaustive = false};

    auto [first, first_settled]{exclusion_search(simulator, other, cap)};

    if (!first_settled)
    {
        return unsettled;
    }

    auto [second, second_settled]{exclusion_search(other, simulator, cap)};

    if (!second_settled)
    {
        return unsettled;
    }

    if (first.empty() && second.empty())
    {
        return {.witness = {}, .side = std::nullopt, .exhaustive = true};
    }

    // The shorter witness, the first set's on a tie; no input is a witness on both sides.
    const auto first_wins{second.empty() || (!first.empty() && first.size() <= second.size())};

    if (first_wins)
    {
        return {.witness = std::move(first), .side = Tokenizing_side::first, .exhaustive = true};
    }

    return {.witness = std::move(second), .side = Tokenizing_side::second, .exhaustive = true};
}

Separation segmentation_difference(const Simulator& simulator, const Simulator& other, const std::size_t cap)
{
    // The key holds the two scans' positions, mine first, either of them dead, and marks nothing: the event, that
    // exactly one side accepts the marked run, ends the input where it happens. Both sides accept the empty run.
    const Key start{.scans = {initial(simulator), initial(other)}, .matched = 0, .at_origin = false, .marked = false};

    // One buffer for the whole search: cleared per byte, handed back as the step's view.
    std::vector<Key> successors{};

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

Unsafe_cut modulo_counterexample(
        const Simulator& simulator, const char symbol, const std::span<const std::size_t> ignored,
        const std::size_t cap)
{
    const auto byte{static_cast<unsigned char>(symbol)};

    // One search per chunk, then, where neither fails anywhere, the product of kept outputs. A search the cap stops
    // leaves the question open, a shorter witness possibly lying where it stopped.
    const Unsafe_cut unsettled{.witness = {}, .cut = 0, .failure = std::nullopt, .exhaustive = false};

    auto [left, left_settled]{chunk_search(simulator, byte, Cut_failure::left_incomplete, cap)};

    if (!left_settled)
    {
        return unsettled;
    }

    auto [right, right_settled]{chunk_search(simulator, byte, Cut_failure::right_incomplete, cap)};

    if (!right_settled)
    {
        return unsettled;
    }

    // The shorter chunk witness, the left one's on a tie, or with no chunk failing, one whose kept streams differ.
    auto [witness, exhaustive]{[&]() -> Outcome {
        if (left.empty() && right.empty())
        {
            return stream_search(simulator, ignored, byte, cap);
        }

        const auto left_wins{right.empty() || (!left.empty() && left.size() <= right.size())};

        if (left_wins)
        {
            return {.witness = std::move(left), .exhaustive = true};
        }

        return {.witness = std::move(right), .exhaustive = true};
    }()};

    if (!exhaustive)
    {
        return unsettled;
    }

    if (witness.empty())
    {
        return {.witness = {}, .cut = 0, .failure = std::nullopt, .exhaustive = true};
    }

    // Every witness has a cut that is not safe; the first and how it fails are read off the scans of its chunks.
    const auto [cut, failure]{*first_unsafe_cut(simulator, ignored, witness, symbol)};

    return {.witness = std::move(witness), .cut = cut, .failure = failure, .exhaustive = true};
}

} // namespace munch::dfa

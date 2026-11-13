#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEPARATOR_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEPARATOR_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "munch/dfa/simulator.hpp"

namespace munch::dfa
{
/**
 * @brief The clause of the separator condition a byte fails.
 *
 * A byte c is a separator modulo the discarded kinds when c alone is a token of a discarded kind, no token spelling
 * begins with u c for u a kept spelling, and no token spelling begins with c b for b the first byte of a kept spelling,
 * a kept spelling being a byte string whose scan as one token gives a kind not discarded. A byte that is not fails one
 * or more of the three, and the first in this order is the one reported.
 */
enum class Separator_clause : std::size_t
{
    /**
     * @brief The byte alone is no token, or a token of a kept kind.
     */
    not_discarded,

    /**
     * @brief A token spelling begins with a kept spelling followed by the byte, so the byte can extend the kept token
     *        before it.
     */
    after_kept,

    /**
     * @brief A token spelling begins with the byte followed by the first byte of a kept spelling, so the byte can run
     *        into the kept token after it.
     */
    before_kept
};

/**
 * @brief What the check of the separator condition found: the clause a byte fails and the words that show it.
 */
struct Separator_failure
{
    /**
     * @brief The first clause the byte fails; nothing when the byte is a separator.
     */
    std::optional<Separator_clause> clause{};

    /**
     * @brief The kept spelling the clause names: for after_kept the one the token spelling begins with, for before_kept
     *        a shortest one beginning with the byte after the separator in the token spelling; empty otherwise.
     */
    std::string kept{};

    /**
     * @brief The token spelling that fails the clause: for not_discarded the byte alone, whatever its scan gives, and
     *        for after_kept and before_kept a shortest token spelling the clause forbids; empty when the byte is a
     *        separator.
     */
    std::string spelling{};
};

/**
 * @brief Decides whether a byte is a separator modulo the discarded kinds, so that the spellings of any kept tokens
 *        joined by single copies of it scan as those tokens, returning the clause it fails and the words that show it.
 *
 * The question a constrained decoder asks before it inserts a byte between tokens. The grammar side of the
 * constrained-decoding paper takes a sequence of kept tokens for realizable when some byte string scans as it once the
 * discarded tokens are deleted, and a token set can leave a sequence with no such string at all. Three clauses settle
 * it for a byte c, generalizing the condition that paper states for its JSON lexers: c alone is a token of a discarded
 * kind, no token spelling begins with u c for u a kept spelling, and no token spelling begins with c b for b the first
 * byte of a kept spelling, a kept spelling being a byte string the scan reads as one token of a kind not discarded, its
 * kind the one priority gives. Under the three, u1 c u2 c ... c un scans as u1, c, u2, ..., un for any kept spellings
 * ui: an accepted prefix of the input from ui longer than ui would begin with ui c, and one from c longer than c would
 * begin with c and the first byte of u(i+1), so maximal munch closes every ui and every c where it ends. Every sequence
 * of kept kinds that have spellings is then realizable, and a decoder separating tokens by c never meets one the lexer
 * cannot produce. Over the kept a and aa with # and a# discarded, the byte # fails the second clause on a#, and no byte
 * string scans as a followed by a, since a#a scans as a# and a, and aa as one token. The third clause covers the
 * separator's own kind as well: over the kept a with one discarded kind spelled # or #a, only that kind begins with #,
 * yet a#a scans as a and #a, one kept a where two were joined, and the third clause fails on #a. The condition is
 * sufficient and not necessary, so a failing clause shows only that the proof does not apply: over the kept ab with #
 * and #ac discarded, the byte # fails the third clause on #ac, while ab joined by # scans as ab every time, the scan
 * from # dying at the b and rolling back to #.
 *
 * Decided on the compiled machine, with no input guessed and no cap, since each clause is a question about the token
 * automaton's language that reachability over its states answers. The first reads the state the byte leads to from the
 * initial one. The second asks whether a reachable state accepting a kept kind steps on the byte into a state from
 * which acceptance is reachable, and the third whether the state the byte leads to steps on some byte b into one, b
 * being a byte whose state from the initial one reaches a state accepting a kept kind. One breadth-first walk forward
 * from the initial state gives every state its shortest access word, and two backward, from the accepting states and
 * from those accepting a kept kind, give every state its shortest completion, so the decision takes time linear in the
 * table's size and the token spelling is a shortest one its clause forbids, on a tie the one leaving the first state
 * for the second clause and the one through the lowest byte for the third. A state no input reaches ends no kept
 * spelling, and a nullable token set is decided through the positive-width equivalent the simulator compiled, as every
 * decision over it is.
 * @param simulator The compiled token set.
 * @param symbol The byte the kept tokens are joined by.
 * @param ignored The IDs of the tokens the caller discards before the stream is used; empty keeps every kind, so the
 *        first clause fails.
 * @return The first clause the byte fails with the words that show it, or no clause and empty words when the byte is a
 *         separator.
 */
[[nodiscard]] Separator_failure separator_failure(
        const Simulator& simulator, char symbol, std::span<const std::size_t> ignored);

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_SEPARATOR_HPP

#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/tools/audit/token_set.hpp"

namespace munch::tools::audit
{
/**
 * @brief The shape of a token that consumes a byte mid-token, read off its pattern, which names the edit its author
 *        would actually make rather than the narrowing the price applies uniformly.
 */
enum class Shape : std::uint8_t
{
    /**
     * @brief A run over a class the byte is in, `[ \t\n]+`: the byte leaves the class and becomes a token of its
     *        own, which is the price's own edit.
     */
    run,

    /**
     * @brief A body ending in the byte, `"//"[^\n]*\n`, the byte nowhere else in it and the token's last component
     *        the terminator itself: the terminator can be left to the token after it, which is the edit priced, so
     *        a last component admitting more than the byte, the `"xb"` of `[a]"xb"` or the `[xb]` of `[a][xb]`,
     *        is no terminated token here. The terminator is read by what it matches, not by its spelling: `\n`,
     *        `[\n]` and `\n{1}` are one terminator.
     */
    terminated,

    /**
     * @brief A fixed opener followed by a body the byte is in, a block comment from its slash-star or a string from
     *        its quote: the body can be scanned in a start condition of its own, the opener alone staying in this
     *        one. The opener is read by what it matches, as the terminator is: `"x"`, `[x]` and `x{1}` are one
     *        opener.
     */
    delimited,

    /**
     * @brief The byte is in the token's fixed spelling, which no edit short of removing the token frees.
     */
    fixed,

    /**
     * @brief None of the above.
     */
    other,
};

/**
 * @brief Where the byte stands after an edit, against the set before any.
 */
struct Outcome
{
    /**
     * @brief Whether the byte certifies exactly.
     */
    bool exact;

    /**
     * @brief Whether the byte certifies once discarded tokens are deleted.
     */
    bool modulo;

    /**
     * @brief The other bytes that certify exactly now and did not before.
     */
    std::vector<unsigned char> gained;
};

/**
 * @brief What one token's shape offers for the byte: the edit the shape names, applied to that token alone with the
 *        rest of the set as it stands, and the certificate after it.
 */
struct Choice
{
    /**
     * @brief The token.
     */
    std::size_t token;

    /**
     * @brief The shape, terminated or delimited, which says what the edit was.
     */
    Shape shape;

    /**
     * @brief Where the byte stands after the edit.
     */
    Outcome after;
};

/**
 * @brief One step towards a byte certifying: the token narrowed, and where the byte stood afterwards.
 */
struct Price_step
{
    /**
     * @brief The token the byte was excluded from.
     */
    std::size_t token;

    /**
     * @brief The token's shape, which names the edit an author would make where the step narrows.
     */
    Shape shape;

    /**
     * @brief Whether the byte, left with no token to match it, was given a token of its own in this step.
     */
    bool separated;

    /**
     * @brief Whether that token of its own is discarded, as the token it was taken from was; false where none was
     *        separated, whatever the narrowed token is.
     */
    bool separated_discarded;

    /**
     * @brief Whether the byte certifies exactly after this step.
     */
    bool exact;

    /**
     * @brief Whether the byte certifies once discarded tokens are deleted after this step.
     */
    bool modulo;
};

/**
 * @brief What it costs to make one byte certify: the tokens that must stop admitting it, taken one at a time with
 *        the certificate recomputed after each, and the tokens that cannot.
 *
 * The necessity theorem is what makes the answer complete rather than a suggestion: a byte certifies exactly when
 * no live state but a non-re-entrant start consumes it, so every token consuming it mid-token has to change, and
 * the only change the analysis makes is to exclude the byte from the token's character sets. A token that cannot
 * lose the byte, every word of it holding the byte in a text or a class of the one byte, takes no step. It is
 * listed as immovable when every word of it holds such a fixed occurrence past the word's first byte: an edit that
 * keeps any of its words keeps that occurrence, so while the token remains in any narrowed form the byte cannot
 * certify. Where some word holds the byte fixed only as its first byte, the occurrence a token begins with, nothing
 * follows about the byte: such a token is listed as undecided, and the report says the procedure decides nothing
 * rather than that the byte cannot certify.
 *
 * Which tokens consume the byte is read from the tables again after every edit, since narrowing one token leaves
 * the byte to any token whose match it had won, and the steps run on until the byte certifies or every token the
 * byte is left to has been answered, by a step or by an obstruction.
 *
 * A byte no token begins with is reported by neither certificate, since no occurrence of it can begin a token, and
 * no narrowing changes that: it is given a token of its own before any other edit and priced from there, the
 * language gaining the byte's token rather than losing the byte, so that the tokens consuming it mid-token are
 * answered as for any other byte.
 */
struct Pricing
{
    /**
     * @brief The byte priced.
     */
    unsigned char byte;

    /**
     * @brief Whether it certified exactly before any edit.
     */
    bool exact_before;

    /**
     * @brief Whether it certified modulo discarded tokens before any edit.
     */
    bool modulo_before;

    /**
     * @brief Where the byte stands once given a token of its own, before any other edit, when no token of the set
     *        begins with it; absent where one does.
     *
     * The token given is visible, since no token of the set says what it would be discarded as, and it stays through
     * every step, so no step separates the byte again.
     */
    std::optional<Outcome> given;

    /**
     * @brief The edits in the order they were made, each with the certificate after it; empty when nothing had to
     *        change.
     *
     * After each step the consumers are read again from the recompiled table and the first not yet answered, in rule
     * order, the order the set lists them, which for a set read from a file is the file's and not the order of the ids,
     * is narrowed next; a token an earlier edit exposed is taken as soon as it is first in that order, and no token is
     * answered twice.
     */
    std::vector<Price_step> steps;

    /**
     * @brief The tokens every word of which holds the byte fixed past its first byte, `[x]\n[x]` or `\n{2}`, which
     *        no narrowing frees: the byte cannot certify while one of them stays.
     */
    std::vector<std::size_t> immovable;

    /**
     * @brief The tokens the narrowing is not applied to and decides nothing about: those that cannot lose the byte
     *        while some word of them holds it fixed only as its first byte.
     *
     * The narrowing is defined where a pattern can lose the byte, which a fixed occurrence cannot, so these tokens
     * take no step. Nothing follows about the byte either: the occurrence that word holds is the one a token begins
     * with, which the initial state consumes before any input, and an edit this analysis does not make, the class
     * of `\n[\nx]`, `[\n][\nx]`, `\n{1}[\nx]` or `(\n[xy])[\nx]` narrowed to `[x]`, or an alternative of
     * `(\n[\nx]|\ny)` dropped, can keep the word and certify the byte.
     */
    std::vector<std::size_t> undecided;

    /**
     * @brief The other bytes that certify exactly after every step and did not before.
     */
    std::vector<unsigned char> gained;

    /**
     * @brief What the consuming tokens' shapes offer, one entry per token and shape, each an edit on its own.
     */
    std::vector<Choice> choices;

    /**
     * @brief Where the byte stands once every consuming token takes the edit its shape names, the terminated ones
     *        their terminator left, the delimited ones cut to their opener, the rest narrowed as the steps narrow
     *        them; absent when no shape offered an edit.
     */
    std::optional<Outcome> together;
};

/**
 * @brief The shape of a token for a byte it consumes mid-token.
 * @param regex The token's pattern.
 * @param byte The byte.
 * @return The shape: run, terminated or delimited when the pattern has that form, a pattern both terminated and
 *         delimited being terminated; else fixed when the byte is in a fixed spelling, else other.
 */
[[nodiscard]] Shape shape_of(const regex::Regex& regex, unsigned char byte);

/**
 * @brief Prices one byte over a token set.
 *
 * The tokens consuming the byte are narrowed one step at a time: the set is compiled again after every edit, the
 * consumers read from it again, and the first not yet answered in rule order, the order the set lists them, is narrowed
 * next, so that a token an earlier narrowing exposes is narrowed as well, and the byte's certificates are read off
 * after every step; a byte that no token can match any more after a narrowing is given a token of its own, discarded
 * when the token it was taken from was, since the language should lose the token's reach, never the byte, and a byte no
 * token begins with is given one, visible, before any other edit. Beside the steps, each consuming token's shape is
 * read and the edit it names tried on its own: a delimited token cut down to its opener, a terminated one to its body.
 * The set given is not changed.
 * @param set The token set.
 * @param byte The byte.
 * @return The pricing.
 */
[[nodiscard]] Pricing price(const Token_set& set, unsigned char byte);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP

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
     * @brief A body ending in the byte, `"//"[^\n]*\n`, the byte nowhere else in it: the terminator can be left to
     *        the token after it.
     */
    terminated,

    /**
     * @brief A fixed opener followed by a body the byte is in, a block comment from its slash-star or a string from
     *        its quote: the body can be scanned in a start condition of its own, the opener alone staying in this
     *        one.
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
     * @brief Whether that token of its own is discarded, as the token it was taken from was.
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
 * the only change the analysis makes is to exclude the byte from the token's character sets. A token whose fixed
 * spelling holds the byte cannot be narrowed and is listed as immovable; while any remains, the byte cannot certify
 * without removing the token.
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
     * @brief The edits, in rule order, each with the certificate after it; empty when nothing had to change.
     */
    std::vector<Price_step> steps;

    /**
     * @brief The tokens that consume the byte in a fixed spelling and so cannot lose it.
     */
    std::vector<std::size_t> immovable;

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
 * Every token the blame names for the byte is narrowed in rule order, the set compiled again after each, and the
 * byte's certificates read off; a byte that no token can match any more after a narrowing is given a token of its
 * own, discarded when the token it was taken from was, since the language should lose the token's reach, never the
 * byte. Beside the steps, each consuming token's shape is read and the edit it names tried on its own: a delimited
 * token cut down to its opener, a terminated one to its body. The set given is not changed.
 * @param set The token set.
 * @param byte The byte.
 * @return The pricing.
 */
[[nodiscard]] Pricing price(const Token_set& set, unsigned char byte);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP

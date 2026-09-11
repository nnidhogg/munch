#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP

#include <cstddef>
#include <vector>

#include "munch/tools/audit/token_set.hpp"

namespace munch::tools::audit
{
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
};

/**
 * @brief Prices one byte over a token set.
 *
 * Every token the blame names for the byte is narrowed in rule order, the set compiled again after each, and the
 * byte's certificates read off; a byte that no token can match any more after a narrowing is given a token of its
 * own, discarded when the token it was taken from was, since the language should lose the token's reach, never the
 * byte. The set given is not changed.
 * @param set The token set.
 * @param byte The byte.
 * @return The pricing.
 */
[[nodiscard]] Pricing price(const Token_set& set, unsigned char byte);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_PRICE_HPP

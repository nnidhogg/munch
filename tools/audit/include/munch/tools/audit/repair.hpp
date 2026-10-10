#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPAIR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPAIR_HPP

#include <string>
#include <vector>

#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/token_set.hpp"

/**
 * @brief What a token of one byte's own would certify over a token set that certifies no byte, Repairs, one Repair per
 *        byte, decided by repairs(); and the repairs rendered as JSON, repair_json(), and as the report's own section,
 *        repair_section().
 */
namespace munch::tools::audit
{
/**
 * @brief One byte that certifies once the token set is given a token of that byte's own, and the other bytes the token
 *        certifies with it.
 */
struct Repair
{
    /**
     * @brief The byte.
     */
    unsigned char byte{};

    /**
     * @brief The other bytes that certify after the addition and did not before, in the sense the byte does.
     */
    std::vector<unsigned char> gained{};
};

/**
 * @brief What a token of one byte's own, added to a token set that certifies no byte at the lowest priority, would
 *        certify: the bytes that certify exactly once their token is visible, and the bytes that certify once the
 *        discarded tokens are deleted once their token is discarded, each byte tried on its own.
 */
struct Repairs
{
    /**
     * @brief The bytes that certify exactly as a visible token of their own, ascending.
     */
    std::vector<Repair> visible{};

    /**
     * @brief The bytes that certify once the discarded tokens are deleted as a discarded token of their own, the ones
     *        that did so before left out, ascending.
     */
    std::vector<Repair> discarded{};
};

/**
 * @brief Returns what a token of one byte's own would certify over a set that certifies no byte: every byte tried as a
 *        visible token of its own and as a discarded one, the bytes already certified once the discarded tokens are
 *        deleted not tried as discarded.
 *
 * Each candidate is the set compiled again with one rule added and the byte's certificate read off it, so the cost is
 * that of at most 512 compilations of the set. Nothing is claimed beyond that decision: not whether the input format
 * can carry the byte, nor that no larger edit does better.
 * @param set The set as given, which is not changed.
 * @param report The set's report.
 * @return The repairs, each list ascending.
 */
[[nodiscard]] Repairs repairs(const Token_set& set, const Report& report);

/**
 * @brief Renders the repairs as the JSON object of their account: one array per kind of token, each entry the byte's
 *        value and the values of the other bytes certified with it.
 * @param repairs The repairs.
 * @return The JSON text, one line.
 */
[[nodiscard]] std::string repair_json(const Repairs& repairs);

/**
 * @brief Renders the repairs as the report's own section: a heading, one row per byte that certifies once its token is
 *        added, the visible tokens first, each with the other bytes certified with it, and one row saying so when no
 *        byte does.
 * @param repairs The repairs.
 * @return The section, a blank line before its heading and a newline after every row.
 */
[[nodiscard]] std::string repair_section(const Repairs& repairs);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPAIR_HPP

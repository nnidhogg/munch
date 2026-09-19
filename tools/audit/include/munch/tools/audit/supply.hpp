#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SUPPLY_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SUPPLY_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/core/lexer.hpp"
#include "munch/tools/audit/report.hpp"

namespace munch::tools::audit
{
/**
 * @brief The gaps between consecutive anchors of one inventory on an input, in bytes: the distance from each anchor
 *        to the next, so the run before the first anchor and the run after the last are in none of them.
 *
 * A percentile is the order statistic at floor(q n) of the n gaps ascending, capped at the last, the
 * certified-splitting paper's own convention, so the median of two gaps is the larger.
 */
struct Gaps
{
    /**
     * @brief The median gap.
     */
    std::size_t median;

    /**
     * @brief The ninetieth percentile.
     */
    std::size_t ninetieth;

    /**
     * @brief The ninety-ninth percentile.
     */
    std::size_t ninety_ninth;

    /**
     * @brief The longest gap.
     */
    std::size_t longest;
};

/**
 * @brief What one inventory of certificates supplies on an input: the anchors it finds there and the gaps between
 *        them.
 */
struct Anchors
{
    /**
     * @brief How many positions of the input are anchors: the distinct interior positions a certificate places a
     *        token boundary at, a position before a certified byte or a window occurrence's origin, the input's two
     *        ends left out since neither is a cut.
     */
    std::size_t count;

    /**
     * @brief 1024 times the count over the input's length, the paper's figure; zero on an empty input.
     */
    double per_kibibyte;

    /**
     * @brief The gaps between consecutive anchors, absent when fewer than two anchors leave none.
     */
    std::optional<Gaps> gaps;
};

/**
 * @brief The certified-anchor supply on one input, the guarantee a parallel scanner lives on: how often a certified
 * anchor occurs in real input and how long the stretches between anchors are. A certificate is a property of the token
 * set; how many cuts it offers is a property of the input, which the certified-splitting paper measures on held-out
 * corpus slices and this measures on the input given. Every anchor is a decision of the report's applied to the input's
 * bytes, the exact and the modulo byte certificates and the certified windows, each window an occurrence of one the
 * report enumerated over its byte classes, and every position is counted once, no sampling. What a certificate promises
 * at an anchor, a token boundary of the serial scan, and for the modulo inventory a boundary of that scan once its
 * discarded tokens are deleted, it promises on input the token set tokenizes completely, as
 * core::Lexer::is_split_point() and is_split_window() state it; on an input the scan stops short of, the counts are the
 * occurrences as they stand and no boundary is promised, the exact byte row alone keeping tokenize_all_parallel()'s
 * weaker serial-prefix relation, which the window rows have not got, so the supply says how far the scan went.
 */
struct Supply
{
    /**
     * @brief The input's length in bytes.
     */
    std::size_t bytes;

    /**
     * @brief How many bytes the serial scan of the input tokenized, the input's length when it tokenizes completely
     *        and the offset the scan stopped at otherwise, no token matching there or a zero-width one matching;
     *        absent when the supply was measured over the report alone, with no token set to scan with.
     */
    std::optional<std::size_t> tokenized;

    /**
     * @brief The supply of the bytes certified exactly.
     */
    Anchors exact;

    /**
     * @brief The supply of the bytes certified once the discarded tokens are deleted.
     */
    Anchors modulo;

    /**
     * @brief The supply of the exactly certified bytes together with the certified windows, the inventory a planner
     *        that falls back to windows cuts at; absent when the report found no window.
     */
    std::optional<Anchors> windows;
};

/**
 * @brief Measures the certified-anchor supply of a report's certificates on an input, as occurrences alone.
 *
 * The bytes are looked up in the report's certified sets; the windows are matched through the report's byte classes,
 * every byte of the input and of a certified window standing for its class, since bytes of one class move every state
 * alike and so a window's decision is its class string's. The input is read once end to end and not scanned, so
 * whether it tokenizes completely is left open, the certified-splitting paper's own measurement over a report built
 * from an inventory; the overload taking the token set scans it.
 * @param report The report, its byte classes and windows filled as audit() fills them.
 * @param input The input.
 * @return The supply, how far the scan went left absent.
 */
[[nodiscard]] Supply supply(const Report& report, std::string_view input);

/**
 * @brief Measures the certified-anchor supply of a report's certificates on an input, and how far the token set's
 *        serial scan of the input goes, which says whether the anchors carry the certificates' promise.
 * @param report The report, its byte classes and windows filled as audit() fills them.
 * @param lexer The token set the report audits, compiled.
 * @param input The input.
 * @return The supply, the bytes tokenized filled.
 */
[[nodiscard]] Supply supply(const Report& report, const core::Lexer& lexer, std::string_view input);

/**
 * @brief Renders a supply as the JSON object of its account: the input's name and length, the bytes the serial scan
 *        tokenized or null when the input was not scanned, and one object per inventory holding the anchor count, the
 *        anchors per kibibyte and the four gap figures, each null when the anchors leave no gap; the windows object
 *        null when the report found no window.
 * @param supply The supply.
 * @param path The input's name, as given.
 * @return The JSON text, one line.
 */
[[nodiscard]] std::string supply_json(const Supply& supply, std::string_view path);

/**
 * @brief Renders a supply as the report's own section, a heading naming the input, a row saying how far the serial
 *        scan went when the input was scanned, and one row per inventory.
 * @param supply The supply.
 * @param path The input's name, as given.
 * @return The section, a blank line before its heading and a newline after every row.
 */
[[nodiscard]] std::string supply_section(const Supply& supply, std::string_view path);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SUPPLY_HPP

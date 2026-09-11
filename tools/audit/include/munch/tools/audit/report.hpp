#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPORT_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPORT_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/audit/price.hpp"
#include "munch/tools/audit/token_set.hpp"

namespace munch::tools::audit
{
/**
 * @brief One certified split window, over the class representatives the report enumerates.
 */
struct Certified_window
{
    /**
     * @brief The window's bytes, each the representative of its byte class.
     */
    std::string window;

    /**
     * @brief The origin the window certifies.
     */
    std::size_t origin;
};

/**
 * @brief Why a candidate byte does not certify: a token consumes it mid-token.
 *
 * The byte is a candidate because the initial state consumes it, so it can begin a token; it fails because some
 * live state other than the initial one consumes it too, on the way to the token named here, after the input
 * given, which is a shortest one reaching that state.
 */
struct Blame
{
    /**
     * @brief The byte that does not certify.
     */
    unsigned char byte;

    /**
     * @brief The token whose match path consumes it mid-token.
     */
    std::size_t token;

    /**
     * @brief A shortest input after which that path consumes the byte.
     */
    std::string after;
};

/**
 * @brief What the library decides about one token set, gathered for a reader who did not write it.
 *
 * Bytes are the certificates a planner cuts at; windows the ones it falls back to, enumerated over one
 * representative per byte class so the count is of distinct behaviours rather than of byte strings; the core is
 * what every window must contain; the spans price the plan; lag and rescue-freeness are the recovery figures; and
 * the blame says, for every byte that could have certified, which token stopped it and how.
 */
struct Report
{
    /**
     * @brief Whether some token matches the empty string, in which case the set was decided through its
     *        positive-width equivalent.
     */
    bool nullable{};

    /**
     * @brief The bytes certified exactly: every occurrence begins a token.
     */
    std::vector<unsigned char> exact;

    /**
     * @brief The bytes certified once the discarded tokens are deleted from both streams.
     */
    std::vector<unsigned char> modulo;

    /**
     * @brief The tokens the modulo certificate deletes, by id: the rules read as returning nothing. Filled only when
     *        the set was given as patterns, and shown so that a misread action is visible.
     */
    std::vector<std::size_t> discarded;

    /**
     * @brief The number of byte classes the windows were enumerated over.
     */
    std::size_t classes{};

    /**
     * @brief The width the window enumeration went up to.
     */
    std::size_t window_limit{};

    /**
     * @brief The certified windows over class representatives, shortest first.
     */
    std::vector<Certified_window> windows;

    /**
     * @brief The number of certified windows once each representative stands for every byte of its class, the
     *        windows an input can actually show.
     */
    std::size_t window_count{};

    /**
     * @brief The proved mandatory core, empty when none is.
     */
    std::string mandatory_core;

    /**
     * @brief The anchor-free span over the certified bytes, std::nullopt when unbounded.
     */
    std::optional<std::size_t> byte_span;

    /**
     * @brief The anchor-free span over the certified windows, std::nullopt when unbounded; absent when the
     *        windows were too many to decide over.
     */
    std::optional<std::optional<std::size_t>> window_span;

    /**
     * @brief The lag, std::nullopt when unbounded.
     */
    std::optional<std::size_t> lag;

    /**
     * @brief Whether the rescue-freeness gate passed.
     */
    bool rescue_free{};

    /**
     * @brief Why each candidate byte that does not certify exactly fails, one entry per byte and token.
     */
    std::vector<Blame> blame;

    /**
     * @brief What it would cost to make the bytes worth asking about certify: the newline, and every byte certified
     *        modulo discarded tokens but not exactly. Filled only when the set was given as patterns.
     */
    std::vector<Pricing> prices;
};

/**
 * @brief The report's answer in one sentence: whether a cut has a certificate, of what kind, and where to read on.
 * @param report The report.
 * @return The sentence.
 */
[[nodiscard]] std::string verdict(const Report& report);

/**
 * @brief Why each candidate byte that does not certify exactly fails: for every byte the start state consumes live,
 *        each token consuming it mid-token, with a shortest input reaching the consuming state.
 * @param lexer The token set.
 * @return The blame, by byte then token.
 */
[[nodiscard]] std::vector<Blame> blame(const core::Lexer& lexer);

/**
 * @brief Audits a token set given as patterns, which is what lets the report price its edits as well.
 * @param set The token set.
 * @param window_limit The longest window tried.
 * @return The report, prices included.
 */
[[nodiscard]] Report audit(const Token_set& set, std::size_t window_limit = 3);

/**
 * @brief Audits a compiled token set.
 *
 * Every figure is the library's own decision over the lexer's tables; nothing here is estimated. The window
 * enumeration is the one cost that grows with the grammar: it tries every string of class representatives up to the
 * limit, and the limit is a parameter for that reason.
 * @param lexer The token set.
 * @param window_limit The longest window tried, three by default; four is the planners' own limit.
 * @return The report.
 */
[[nodiscard]] Report audit(const core::Lexer& lexer, std::size_t window_limit = 3);

/**
 * @brief Renders a report as JSON, one object with a member per figure, token ids paired with their names.
 *
 * Byte strings, the windows, the core and the blame's inputs, are JSON strings holding each byte as the code point
 * of its value, so a reader recovers the bytes exactly; spans are numbers, or the string "unbounded", or for the
 * window span the string "undecided" when the windows were too many.
 * @param report The report.
 * @param name The name to print for a token id.
 * @return The JSON text, no trailing newline.
 */
[[nodiscard]] std::string json(const Report& report, const std::function<std::string(std::size_t)>& name);

/**
 * @brief Renders a report as text, the verdict first and then one section per question.
 * @param report The report.
 * @param name The name to print for a token id, the rule's returned expression or its pattern for a flex file.
 * @return The text.
 */
[[nodiscard]] std::string render(const Report& report, const std::function<std::string(std::size_t)>& name);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_REPORT_HPP

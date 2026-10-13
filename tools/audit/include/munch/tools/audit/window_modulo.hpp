#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_MODULO_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_MODULO_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/audit/report.hpp"

/**
 * @brief The windows a token set certifies once the discarded tokens are deleted and refuses exactly, Windows_modulo,
 *        one Window_cut per window, found by windows_modulo(); and the windows rendered as JSON, windows_modulo_json(),
 *        and as the report's own section, windows_modulo_section().
 */
namespace munch::tools::audit
{
/**
 * @brief One window certified once the discarded tokens are deleted, over the class representatives the report
 *        enumerates, and the offset it certifies a cut at.
 */
struct Window_cut
{
    /**
     * @brief The window's bytes, each the representative of its byte class.
     */
    std::string window{};

    /**
     * @brief The offset of the cut, the gap before the window's byte there; a cut, not an origin, since the token
     *        covering the window's last byte may begin anywhere.
     */
    std::size_t cut{};
};

/**
 * @brief What deleting the discarded tokens adds to a token set's certified windows: the windows the exact certificate
 *        refuses that certify a cut once the discarded tokens are deleted, and their count once classes expand.
 */
struct Windows_modulo
{
    /**
     * @brief Whether the set discards some token; where it discards none, deleting them adds nothing and no window was
     *        tried.
     */
    bool discards{};

    /**
     * @brief The number of byte classes the windows were enumerated over.
     */
    std::size_t class_count{};

    /**
     * @brief The windows over class representatives, shortest first, each at its least cut.
     */
    std::vector<Window_cut> windows{};

    /**
     * @brief The number of windows once each representative stands for every byte of its class; std::nullopt when they
     *        are more than a count can hold.
     */
    std::optional<std::size_t> window_count{};
};

/**
 * @brief Returns the windows a compiled token set certifies once the discarded tokens are deleted, up to the report's
 *        window limit, the ones is_split_window() certifies left out.
 *
 * A window `W` certifies a cut at offset `o` once the discarded tokens are deleted when, in every input the set
 * tokenizes completely that holds `W` at some offset `t`, cutting at `t + o` leaves two chunks the set tokenizes
 * completely whose token streams, the discarded tokens deleted, concatenate to the whole input's. The decision is
 * is_split_window()'s cloud of hypotheses, a state and where its token began, with a third field saying how the
 * hypothesis crossed the gap before byte `o`: at a boundary, where it began a token there, harmlessly, where it read
 * the byte from a state that passes is_split_point_ignoring()'s test for it, or harmfully. A direct step keeps the
 * field and a fresh hypothesis takes the field of each accepting one it follows, and the window certifies at `o` when
 * the cloud past its last byte is nonempty and each hypothesis began its token at `o` or crossed the gap harmlessly.
 * The test is computed here over the public Simulator interface, a state's accepted token and its steps: the state
 * accepts a discarded token, its successor reaches no kept one, and the successor cannot be told from the initial
 * state's on the byte by any continuation once every discarded token is one colour. With no token discarded the
 * decision is is_split_window() at `o`, and for one byte it is is_split_point_ignoring(). The windows are searched
 * depth first, each extending its prefix's walks, one per cut, by one step.
 * @param report The set's report, whose byte classes, discarded tokens and window limit the enumeration follows.
 * @param lexer The compiled set.
 * @return The windows, each at its least cut.
 */
[[nodiscard]] Windows_modulo windows_modulo(const Report& report, const core::Lexer& lexer);

/**
 * @brief Renders the windows as the JSON object of their account: the windows, each its bytes and its cut, and their
 *        count once classes expand; null where the set discards no token.
 * @param found The windows found.
 * @return The JSON text, one line.
 */
[[nodiscard]] std::string windows_modulo_json(const Windows_modulo& found);

/**
 * @brief Renders the windows as the report's own section: a heading, and the windows' rows as the report prints its
 *        certified windows, each example with its cut, or one row saying that the set discards no token.
 * @param found The windows found.
 * @return The section, a blank line before its heading and a newline after every row.
 */
[[nodiscard]] std::string windows_modulo_section(const Windows_modulo& found);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_MODULO_HPP

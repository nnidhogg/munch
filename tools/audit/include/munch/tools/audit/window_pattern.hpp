#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_PATTERN_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_PATTERN_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/audit/report.hpp"

/**
 * @brief A window written as positions that each admit one byte's class or every byte, Window_pattern_t, and what it
 *        certifies, Pattern_verdict, decided for every member at once by window_patterns(); and the verdicts rendered
 *        as JSON, window_pattern_json(), and as the report's own section, window_pattern_section().
 */
namespace munch::tools::audit
{
/**
 * @brief A window pattern: one position per byte of the windows it stands for, each a byte, which admits the byte's
 *        class, or std::nullopt, which admits every byte. Its members are the byte strings each of whose bytes its
 *        position admits.
 */
using Window_pattern_t = std::vector<std::optional<unsigned char>>;

/**
 * @brief What a window pattern certifies, and the origins its members' hypotheses carry past its last position.
 */
struct Pattern_verdict
{
    /**
     * @brief The pattern.
     */
    Window_pattern_t pattern{};

    /**
     * @brief The origin every member that occurs certifies, or std::nullopt when the pattern certifies none.
     */
    std::optional<std::size_t> origin{};

    /**
     * @brief The offsets in the window at which the tokens covering its last byte begin, over every member's
     *        hypotheses, ascending: the origin alone when it certifies, none when no member's hypotheses survive.
     */
    std::vector<std::size_t> origins{};

    /**
     * @brief Whether some member's hypothesis carries a token covering the last byte that began before the window.
     */
    bool before{};
};

/**
 * @brief Returns what each window pattern certifies over a compiled token set: an origin only when the walk shows that
 *        every member that occurs in an input the set tokenizes completely is a certified split window at that origin.
 *
 * Each pattern is one walk of is_split_window()'s cloud of hypotheses, a state and where its token began, which at each
 * position reads every byte class the position admits and keeps the union of the clouds they lead to. The step on one
 * byte distributes over a union of clouds, so the cloud past the last position is the union of the members' own clouds,
 * and the pattern certifies at an origin exactly when that cloud is nonempty and every hypothesis in it carries the
 * origin: every member whose own cloud survives is then a certified window there, and a member whose own cloud dies
 * occurs in no completely tokenizable input, which is why such a member, a wildcard ranging over a class no token
 * reads, leaves the verdict alone. A surviving cloud does not show that a member occurs, and the walk does not decide
 * occurrence, so a family none of whose members occurs certifies vacuously, as is_split_window() certifies such a
 * window: over 0, 00 and 01 the pattern 1 0 0 1 certifies at 2, and no completely tokenizable input holds 1001. A walk
 * costs one window's walk times the classes its positions admit, where the members are their product.
 * @param report The set's report, whose byte classes a wildcard ranges over.
 * @param lexer The compiled set.
 * @param patterns The patterns, each of two positions at least.
 * @return One verdict per pattern, in the order given.
 */
[[nodiscard]] std::vector<Pattern_verdict> window_patterns(
        const Report& report, const core::Lexer& lexer, const std::vector<Window_pattern_t>& patterns);

/**
 * @brief Renders the verdicts as the JSON array of their account: one object per pattern, each position the byte's
 *        value or the string `*`, the origin certified or null, the origins carried and whether one is before the
 *        window.
 * @param verdicts The verdicts.
 * @return The JSON text, one line.
 */
[[nodiscard]] std::string window_pattern_json(const std::vector<Pattern_verdict>& verdicts);

/**
 * @brief Renders the verdicts as the report's own section: a heading, and one row per pattern in the order asked,
 *        saying where it certifies or, where it does not, which origins its members carry or that none of them occurs.
 * @param verdicts The verdicts.
 * @return The section, a blank line before its heading and a newline after every row.
 */
[[nodiscard]] std::string window_pattern_section(const std::vector<Pattern_verdict>& verdicts);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_WINDOW_PATTERN_HPP

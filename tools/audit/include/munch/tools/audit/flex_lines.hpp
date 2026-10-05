#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_LINES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_LINES_HPP

#include <cstddef>
#include <string_view>
#include <vector>

/**
 * @brief A flex file as the lines its sections are read from, Lines, and a line with its blanks, line_blanks, trimmed,
 *        trimmed().
 *
 * flex reads its file line by line before it reads anything else: a line at the margin is a declaration or a rule, an
 * indented one is code it copies out, and `%%` at the margin parts the sections, whatever follows it on the line. The
 * section readers in flex_sections.hpp share one Lines cursor, each leaving it where the other takes over.
 */
namespace munch::tools::audit
{
/**
 * @brief The blanks trimmed() drops from a line's ends: the space, the tab and a CRLF line end's return.
 */
constexpr std::string_view line_blanks{" \t\r"};

/**
 * @brief The source as lines, with the cursor the two section readers share.
 */
class Lines
{
public:
    /**
     * @brief Splits the source; a final line without a newline counts as a line.
     * @param source The file's text.
     */
    explicit Lines(std::string_view source);

    /**
     * @brief Returns whether a line remains.
     * @return True while the cursor is inside the file.
     */
    [[nodiscard]] bool more() const noexcept;

    /**
     * @brief Moves the cursor forward.
     * @param count How many lines, one unless told.
     */
    void advance(std::size_t count = 1) noexcept;

    /**
     * @brief Returns the number of the line under the cursor, counted from one.
     * @return The line number.
     */
    [[nodiscard]] std::size_t number() const noexcept;

    /**
     * @brief Returns the source from the line under the cursor to the end of the file, for an action that spans lines.
     * @return The remaining text.
     */
    [[nodiscard]] std::string_view rest() const noexcept;

    /**
     * @brief Returns the line under the cursor.
     * @return The line, without its newline.
     */
    [[nodiscard]] std::string_view current() const noexcept;

    /**
     * @brief Skips lines through the first whose trimmed text is the given one, which a block delimiter is.
     * @param close The delimiter line, `}` for a %top block.
     * @param what What was open, named when the file ends first.
     * @throws Spec_error If the file ends before the delimiter.
     */
    void skip_through(std::string_view close, std::string_view what);

    /**
     * @brief Moves to the first line holding the given mark anywhere, the line under the cursor included, which is how
     *        flex closes a `%{` block: at the line that holds `%}`, wherever on it.
     * @param mark The mark, `%}`.
     * @param what What was open, named when the file ends first.
     * @throws Spec_error If the file ends before the mark.
     */
    void skip_to(std::string_view mark, std::string_view what);

private:
    /**
     * @brief The file's text, which the lines view.
     */
    std::string_view source_;

    /**
     * @brief The lines.
     */
    std::vector<std::string_view> lines_;

    /**
     * @brief The index of the line under the cursor.
     */
    std::size_t at_{0};
};

/**
 * @brief Returns the line without the spaces, tabs and carriage returns at either end, which are a flex line's blanks;
 *        a newline is no blank of a line and stays.
 * @param line The line.
 * @return The trimmed view.
 */
[[nodiscard]] std::string_view trimmed(std::string_view line) noexcept;

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_LINES_HPP

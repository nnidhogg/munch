#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_CURSOR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_CURSOR_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace munch::tools::audit
{
/**
 * @brief A cursor over a span of a file's text: the reading primitives every reader of a generator's file shares,
 *        one byte looked at or taken, a prefix tested, a byte accepted or required, the line the cursor is on, and
 *        a refusal at that line.
 *
 * A reader derives from it and adds the grammar it reads; the cursor holds the text as a view and two offsets, the
 * one under it and the one its span ends at, and nothing else. A refusal names the line of the byte under the
 * cursor, or the text's last line once the text has ended, so that a construct left open is reported where it
 * was, not on the line a final newline would open.
 */
class Cursor
{
public:
    /**
     * @brief Binds the cursor to a span of the text.
     * @param text The whole file.
     * @param begin The offset the cursor starts at.
     * @param end The offset the span ends at, the text's size for the rest of it.
     */
    Cursor(std::string_view text, std::size_t begin, std::size_t end);

    /**
     * @brief Binds the cursor to the whole text.
     * @param text The whole file.
     */
    explicit Cursor(std::string_view text);

    /**
     * @brief Consumes and returns the byte under the cursor.
     * @param what What the syntax expected, spelled with its byte quoted, named when the span has ended instead.
     * @return The byte.
     * @throws Spec_error At the end of the span.
     */
    char next(std::string_view what);

    /**
     * @brief Whether the span has ended.
     * @return True at or past its end.
     */
    [[nodiscard]] bool done() const noexcept;

    /**
     * @brief Consumes the byte under the cursor if it is the one given.
     * @param byte The byte asked for.
     * @return True when consumed.
     */
    [[nodiscard]] bool accept(char byte) noexcept;

    /**
     * @brief Skips blanks and comments of either C style, a block comment left open refused.
     * @throws Spec_error If a block comment never closes.
     */
    void skip_blanks();

    /**
     * @brief Consumes the byte given or refuses.
     * @param byte The byte required.
     * @param what What the syntax expected, spelled with the byte quoted and its purpose, `')' to close the group`.
     * @throws Spec_error If another byte is under the cursor.
     */
    void expect(char byte, std::string_view what);

    /**
     * @brief The line the cursor is on, counted from one; the last line once the text has ended.
     * @return The line.
     */
    [[nodiscard]] std::size_t line() const noexcept;

    /**
     * @brief The offset of the byte under the cursor.
     * @return The offset.
     */
    [[nodiscard]] std::size_t offset() const noexcept;

    /**
     * @brief The byte under the cursor, or nothing once the span has ended.
     * @return The byte.
     */
    [[nodiscard]] std::optional<char> peek() const noexcept;

    /**
     * @brief Refuses the text at the cursor's line.
     * @param message Why.
     * @throws Spec_error Always.
     */
    [[noreturn]] void fail(const std::string& message) const;

    /**
     * @brief Whether the text under the cursor begins with the given characters, within the span.
     * @param prefix The characters.
     * @return True when it does.
     */
    [[nodiscard]] bool at(std::string_view prefix) const noexcept;

    /**
     * @brief The line an offset is on, counted from one.
     * @param offset The offset.
     * @return The line.
     */
    [[nodiscard]] std::size_t line_of(std::size_t offset) const noexcept;

protected:
    /**
     * @brief The whole file.
     */
    std::string_view text_;

    /**
     * @brief The offset of the byte under the cursor.
     */
    std::size_t at_;

    /**
     * @brief The offset the span ends at.
     */
    std::size_t end_;
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_CURSOR_HPP

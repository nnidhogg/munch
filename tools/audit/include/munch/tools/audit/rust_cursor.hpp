#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_CURSOR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_CURSOR_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/cursor.hpp"

/**
 * @brief Rust text read as Rust's lexer reads it, Rust_cursor: blanks and nesting block comments, words, string
 *        literals in their plain, raw and byte forms, String_literal, character literals told from lifetimes, and the
 *        three kinds of delimited group; with whether an attribute opens at a cursor, at_attribute(), a text without
 *        its trivia, compacted(), and the skip over a generic list, skip_generics().
 *
 * The logos reader reads the whole file with these, its items, its attributes, the enums deriving Logos and the
 * callbacks' bodies, so that nothing inside a comment or a literal is mistaken for code.
 */
namespace munch::tools::audit
{
/**
 * @brief A Rust string literal: the text exactly as written, its content decoded, and whether it is a byte string.
 */
struct String_literal
{
    /**
     * @brief The literal as it stands in the source, prefix and quotes included.
     */
    std::string written;

    /**
     * @brief The content, the escapes of a plain string decoded and a raw string's taken verbatim.
     */
    std::string bytes;

    /**
     * @brief Whether the literal is a byte string, `b"..."`, whose pattern is over bytes.
     */
    bool byte_string;
};

/**
 * @brief A cursor over a stretch of Rust source, reading it token by token as far as attributes and enums need.
 *
 * The cursor knows Rust's lexical shapes well enough never to be misled by them: comments of both styles, block
 * comments nesting, string literals in their plain, raw and byte forms, character literals as against lifetimes, and
 * the three kinds of delimited group. A cursor may be bounded to a span of the text, an attribute's content, while
 * still counting lines from the file's start.
 */
class Rust_cursor : public Cursor
{
public:
    /**
     * @brief Binds the cursor to a span of the text.
     * @param text The whole file.
     * @param begin The offset the cursor starts at.
     * @param end The offset the span ends at.
     */
    Rust_cursor(std::string_view text, std::size_t begin, std::size_t end);

    /**
     * @brief Skips blanks and comments.
     * @throws Spec_error If a block comment is never closed.
     */
    void skip_trivia();

    /**
     * @brief Skips one token: a comment, a string or character literal, a delimited group with everything in it, a
     *        word, or a single byte.
     * @throws Spec_error If a literal or a group is left open.
     */
    void skip_token();

    /**
     * @brief Skips the delimited group opening at the cursor, through its close.
     * @throws Spec_error If the group is left open or closed by the wrong delimiter.
     */
    void skip_group();

    /**
     * @brief Consumes the word at the cursor, letters, digits, underscores and non-ASCII bytes, a raw identifier's `r#`
     *        prefix included.
     * @return The word, empty when none stands here.
     */
    [[nodiscard]] std::string_view word();

    /**
     * @brief Consumes and decodes the string literal at the cursor.
     * @return The literal.
     * @throws Spec_error If no string literal stands here, it is left open, or an escape is not Rust's.
     */
    [[nodiscard]] String_literal literal();

    /**
     * @brief A cursor over a span inside this one, counting lines from the same file start.
     * @param begin The span's first offset.
     * @param end The offset the span ends at.
     * @return The cursor.
     */
    [[nodiscard]] Rust_cursor inside(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief The text between two offsets.
     * @param begin The first offset.
     * @param end The offset past the last.
     * @return The text.
     */
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief Whether a string literal, in any of its prefixed forms, opens at the cursor.
     * @return True when one does.
     * @throws Spec_error If it is left open.
     */
    [[nodiscard]] bool at_string() const;

private:
    /**
     * @brief The offset just past the string literal at the cursor, its prefix, hashes and escapes honoured.
     * @return The offset, or std::nullopt when no string literal opens here.
     * @throws Spec_error If the literal is left open.
     */
    [[nodiscard]] std::optional<std::size_t> string_end() const;

    /**
     * @brief Reads one escape of a string literal after its backslash into the literal's bytes: a character's, a byte's
     *        `\xHH`, a scalar's `\u{...}`, or a line's end, which continues the string on the next line, its leading
     *        blanks dropped.
     * @param bytes The literal's bytes, added to.
     * @param byte_string Whether the literal is a byte string, which `\xHH` may take beyond ASCII and has no `\u`.
     * @throws Spec_error If the escape is not one Rust knows for the literal.
     */
    void escape(std::string& bytes, bool byte_string);

    /**
     * @brief Reads a `\xHH` escape after its `\x`.
     * @param byte_string Whether the literal is a byte string; a string's `\x` reaches only `\x7f`.
     * @return The byte.
     * @throws Spec_error If the two hex digits are missing, or a string's escape reaches past ASCII.
     */
    [[nodiscard]] char byte_escape(bool byte_string);

    /**
     * @brief Reads a `\u{...}` escape after its `\u`, underscores among the digits being no digits, as Rust writes them
     *        in a number.
     * @return The scalar.
     * @throws Spec_error If the braces are missing, or the digits are not one to six naming a scalar.
     */
    [[nodiscard]] char32_t unicode_escape();
};

/**
 * @brief Whether an attribute opens where the cursor stands, `#` and `[` with whatever blanks and comments Rust allows
 *        between them, `# [derive(Logos)]` being the attribute `#[derive(Logos)]` is; an inner attribute's `#!` is told
 *        apart by the caller, which asks for it first.
 * @param cursor The cursor, which is not moved.
 * @return True when one does.
 * @throws Spec_error If a block comment after the `#` is left open.
 */
[[nodiscard]] bool at_attribute(const Rust_cursor& cursor);

/**
 * @brief A text without its trivia: the blanks and the comments dropped, as Rust's lexer drops them before anything
 *        reads a type or a path, and every token, string and character literals included, kept as written.
 * @param text The text.
 * @return The text with its trivia dropped.
 * @throws Spec_error If a block comment or a literal is left open.
 */
[[nodiscard]] std::string compacted(std::string_view text);

/**
 * @brief Skips the angle-bracketed generics opening at the cursor, through their close.
 * @param cursor The cursor, at the `<`.
 * @throws Spec_error If the text ends first.
 */
void skip_generics(Rust_cursor& cursor);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_CURSOR_HPP

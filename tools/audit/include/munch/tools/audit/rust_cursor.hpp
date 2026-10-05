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
 *        its trivia, compacted(), the skips over a generic list, skip_generics(), up to a stop, skip_until(), and over
 *        an item of a list, skip_list_item(), a token of punctuation taken byte by byte, expect_spelled(), and the
 *        bytes that open and close a group, is_opening() and is_closing().
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
    std::string written{};

    /**
     * @brief The content, the escapes of a plain string decoded and a raw string's taken verbatim.
     */
    std::string bytes{};

    /**
     * @brief Whether the literal is a byte string, `b"..."`, whose pattern is over bytes.
     */
    bool byte_string{};
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
     * @brief Returns a cursor over a span inside this one, counting lines from the same file start.
     * @param begin The span's first offset.
     * @param end The offset the span ends at.
     * @return The cursor.
     */
    [[nodiscard]] Rust_cursor inside(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief Returns the text between two offsets.
     * @param begin The first offset.
     * @param end The offset past the last.
     * @return The text.
     */
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const noexcept;

    /**
     * @brief Returns a cursor over the inside of the group the cursor has just skipped, its two delimiters left out.
     * @param open The offset of the group's opening delimiter; the cursor stands just past its closing one.
     * @return The cursor over the inside.
     */
    [[nodiscard]] Rust_cursor group_inside(std::size_t open) const noexcept;

    /**
     * @brief Returns the text inside the group the cursor has just skipped, its two delimiters left out.
     * @param open The offset of the group's opening delimiter; the cursor stands just past its closing one.
     * @return The text between the delimiters.
     */
    [[nodiscard]] std::string_view group_text(std::size_t open) const noexcept;

    /**
     * @brief Returns whether a string literal, in any of its prefixed forms, opens at the cursor.
     * @return True when one does.
     * @throws Spec_error If it is left open.
     */
    [[nodiscard]] bool at_string() const;

    /**
     * @brief Returns whether a literal opens at the cursor: a string literal in any of its forms, or the quote of a
     *        character or a byte literal, which a lifetime opens with as well.
     * @return True when one does.
     * @throws Spec_error If a string literal is left open.
     */
    [[nodiscard]] bool at_literal() const;

private:
    /**
     * @brief Returns the offset just past the string literal at the cursor, its prefix, hashes and escapes honoured.
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
 * @brief Returns whether an attribute opens where the cursor stands, `#` and `[` with whatever blanks and comments Rust
 *        allows between them, `# [derive(Logos)]` being the attribute `#[derive(Logos)]` is; an inner attribute's `#!`
 *        is told apart by the caller, which asks for it first.
 * @param cursor The cursor, which is not moved.
 * @return True when one does.
 * @throws Spec_error If a block comment after the `#` is left open.
 */
[[nodiscard]] bool at_attribute(const Rust_cursor& cursor);

/**
 * @brief Returns a text without its trivia: the blanks and the comments dropped, as Rust's lexer drops them before
 *        anything reads a type or a path, and every token, string and character literals included, kept as written.
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

/**
 * @brief Skips tokens up to the first that opens with one of a set of bytes, a group or a literal taken whole.
 * @param cursor The cursor; left at that token, or at the end of its text.
 * @param stops The bytes that stop it.
 * @throws Spec_error If a group or a literal is left open.
 */
void skip_until(Rust_cursor& cursor, std::string_view stops);

/**
 * @brief Skips one item of a comma-separated list, a type or a generic parameter, up to the comma that ends it, a
 *        generic list inside the item taken whole.
 * @param cursor The cursor, at the item; left at the comma, or at the end of its text.
 * @throws Spec_error If a group or a generic list is left open.
 */
void skip_list_item(Rust_cursor& cursor);

/**
 * @brief Takes a token of punctuation at the cursor byte by byte, `::` or `->`.
 * @param cursor The cursor, at the token; left past it.
 * @param token The token.
 * @throws Spec_error If a byte of it is not there.
 */
void expect_spelled(Rust_cursor& cursor, std::string_view token);

/**
 * @brief Returns whether a byte opens a delimited group: `(`, `[` or `{`.
 * @param byte The byte, or nothing at the end.
 * @return True when it does.
 */
[[nodiscard]] constexpr bool is_opening(const std::optional<char> byte) noexcept
{
    return byte == '(' || byte == '[' || byte == '{';
}

/**
 * @brief Returns whether a byte closes a delimited group: `)`, `]` or `}`.
 * @param byte The byte, or nothing at the end.
 * @return True when it does.
 */
[[nodiscard]] constexpr bool is_closing(const std::optional<char> byte) noexcept
{
    return byte == ')' || byte == ']' || byte == '}';
}

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RUST_CURSOR_HPP

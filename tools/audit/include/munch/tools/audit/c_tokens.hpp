#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_C_TOKENS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_C_TOKENS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief The tokens of a stretch of C, C++, Java or C#, read as those languages read them: c_tokens() and
 *        java_tokens(), brace_close() and group_close() over the tokens, spliced(), C's line-splicing phase, which the
 *        actions' readings take on its own, and directive_end(), where the preprocessor ends a directive.
 *
 * What a reader asks of the code a scanner carries, a return, a call, a directive or a brace, is asked of these tokens
 * rather than of its bytes, so that a comment or a literal hides what it holds and the blanks, comments and line
 * splices between the pieces of a word or a call change nothing. The languages are read here and not the generators:
 * flex's action scanner and re2c's block scanner find where an action ends by rules of their own, which their readers
 * keep, and what is read here is the code as the compiler reads it once the generator has copied it out.
 */
namespace munch::tools::audit
{
/**
 * @brief One token of a stretch of C as c_tokens() reads it, with where it stands.
 */
struct C_token
{
    /**
     * @brief The offset of the token's first byte in the stretch.
     */
    std::size_t at;

    /**
     * @brief The offset just past the token's last byte in the stretch, the line splices inside the token counted.
     */
    std::size_t end;

    /**
     * @brief The token's text with its line splices removed: a word, a literal with its quotes, or a punctuator.
     */
    std::string text;
};

/**
 * @brief A stretch of C with its line splices removed, and where each byte of it stood.
 */
struct Spliced
{
    /**
     * @brief The text with every backslash-newline pair deleted.
     */
    std::string text;

    /**
     * @brief The offset in the stretch of each byte of the text, in order.
     */
    std::vector<std::size_t> place;
};

/**
 * @brief The tokens of a stretch of C, as C reads them and as flex's and re2c's action scanners do: a backslash and
 *        the newline after it are deleted first, C's line splicing, so that a word, a literal or a comment may run
 *        over a physical line end; a word runs over letters, digits, underscores, the bytes above ASCII of a UTF-8
 *        identifier and universal character names, `\u03B1`; a string or character literal is one token from its
 *        quote to the closing one, an escaped byte carried, and a C++ raw string, `R"d(...)d"` after an optional
 *        `u8`, `u`, `U` or `L`, from its prefix to its closing delimiter and quote; a `//` comment runs to the end of
 *        its logical line and a block comment to its star-slash, and neither is a token, nor is whitespace, the
 *        newline among it; every other byte is a punctuator of its own, `->` alone one of two. A literal or a
 *        comment left open runs to the end. Each token keeps its place in the stretch as written, splices and all.
 *
 * What read_flex() and read_re2c() ask of an action is read from these rather than from its bytes, so that a `return`
 * in a comment or a literal returns nothing, a call is a call however many blanks or comments part its name from its
 * parenthesis, and a member is one however many part it from its `.` or `->`.
 * @param code The stretch of C.
 * @return The tokens in order.
 */
[[nodiscard]] std::vector<C_token> c_tokens(std::string_view code);

/**
 * @brief The tokens of a stretch of Java or C#, read as those languages read them: no line is spliced, since neither
 *        language joins lines, and a bare carriage return ends a line comment as a newline does.
 * @param code The stretch.
 * @return Its tokens, with where each stands, comments left out.
 */
[[nodiscard]] std::vector<C_token> java_tokens(std::string_view code);

/**
 * @brief Where the brace block opening a stretch of C closes, string and character literals and comments skipped as
 *        c_tokens() skips them, which is how re2c reads an action.
 * @param code The stretch, its first byte the opening brace.
 * @return The offset just past the closing brace, or std::nullopt when the stretch ends first.
 */
[[nodiscard]] std::optional<std::size_t> brace_close(std::string_view code);

/**
 * @brief The index of the token closing the group that opens at an index, the groups of its kind inside it matched: a
 *        `(` by its `)`, a `[` by its `]`, a `{` by its `}`.
 * @tparam Token A token holding what it spells as its `text`.
 * @param tokens The tokens.
 * @param open The index of the group's opener.
 * @param opener What the opener spells.
 * @param closer What the closer spells.
 * @return The index of the closer, the tokens' size when the group never closes, or the index given when the token
 *         there opens no group of its kind and closes none.
 */
template <typename Token>
[[nodiscard]] std::size_t group_close(
        const std::vector<Token>& tokens, const std::size_t open, const std::string_view opener,
        const std::string_view closer) noexcept
{
    auto close{open};

    for (auto depth{0}; close < tokens.size(); ++close)
    {
        depth += tokens[close].text == opener ? 1 : tokens[close].text == closer ? -1 : 0;

        if (depth == 0)
        {
            break;
        }
    }

    return close;
}

/**
 * @brief Splices the lines of a stretch of C as C does before it reads tokens: a backslash and the newline after it,
 *        a carriage return between them or not, are deleted, so a word, a literal or a comment may run over a
 *        physical line end.
 * @param code The stretch of C.
 * @return The spliced text and each byte's place.
 */
[[nodiscard]] Spliced spliced(std::string_view code);

/**
 * @brief Where a preprocessor directive's replacement ends, as the preprocessor reads the directive: at the end of
 *        its logical line, a backslash before the newline joining the next line on, blanks after the backslash
 *        allowed as gcc allows them; a block comment's newlines and a raw string's are their own and end nothing,
 *        an ordinary literal's escapes carry their byte and a literal left open ends with the line, and a line
 *        comment runs with the directive to its end.
 * @param code The stretch of C the directive stands in.
 * @param from The index just past the directive's name, or its parameter list.
 * @return The index of the newline ending the directive, or the stretch's size when it ends the stretch.
 */
[[nodiscard]] std::size_t directive_end(std::string_view code, std::size_t from);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_C_TOKENS_HPP

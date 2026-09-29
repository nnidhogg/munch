#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CURSOR_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CURSOR_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/cursor.hpp"

/**
 * @brief A cursor over an ANTLR grammar's text reading the tokens ANTLR's own lexer reads there, Antlr_cursor: its
 *        blanks, comments and byte order marks, a name, a quoted literal with its escapes, Literal, a brace block and a
 *        run of element options.
 *
 * The grammar, a rule's body and a `->` clause are all read with these, as ANTLR reads them all with one lexer, so a
 * comment ends at a carriage return wherever it stands and a name is ASCII letters, digits and underscores from a
 * letter on.
 */
namespace munch::tools::audit
{
/**
 * @brief A quoted literal as read: its bytes, and whether ANTLR takes it as one character where it needs one.
 */
struct Literal
{
    /**
     * @brief The bytes, each character's UTF-8 encoding, a surrogate pair's the scalar the pair encodes.
     */
    std::string bytes;

    /**
     * @brief Whether ANTLR reads the literal as one character where a range's end or a negated literal needs one
     *        (CharSupport.getCharValueFromGrammarCharLiteral): one character of the basic multilingual plane written
     *        out, or one escape of any kind. A character beyond that plane written out and a surrogate pair of two
     *        escapes are two UTF-16 units to it, and its error 144 calls the literal multi-character, as it calls the
     *        empty one.
     */
    bool single;
};

/**
 * @brief A cursor over a grammar's text, or over a clause of it, reading the tokens ANTLR's lexer reads there.
 */
class Antlr_cursor : public Cursor
{
public:
    /**
     * @brief Binds the cursor to a text, a `//` comment ending at a carriage return as ANTLR's lexer ends one.
     * @param source The text.
     */
    explicit Antlr_cursor(std::string_view source);

    /**
     * @brief Binds the cursor to a text from an offset on, a `//` comment ending at a carriage return as ANTLR's lexer
     *        ends one.
     * @param text The whole text.
     * @param begin The offset the cursor starts at.
     */
    Antlr_cursor(std::string_view text, std::size_t begin);

    /**
     * @brief Skips blanks, comments and byte order marks: ANTLR's lexer reads a mark, U+FEFF, as a token of its own
     *        that it drops wherever one stands outside a literal, a set or an action (ANTLRLexer.g's UnicodeBOM), so a
     *        grammar may open with one or hold one between any two tokens, and it is a blank here.
     * @throws Spec_error If a block comment never closes.
     */
    void skip_blanks();

    /**
     * @brief Reads an identifier at the cursor, as ANTLR's lexer reads a name: a letter, then letters, digits and
     *        underscores (ANTLRLexer.g's NameStartChar and NameChar within ASCII), so that `_x` and `2x` begin none,
     *        which ANTLR's parser rejects as a syntax error at the underscore or the number.
     * @return The identifier, empty when none begins here.
     */
    [[nodiscard]] std::string identifier();

    /**
     * @brief Reads a quoted literal after its opening quote, through the closing one, decoding its escapes; a high
     *        surrogate escape and a low one after it are the one character the pair encodes, and a surrogate standing
     *        alone is refused.
     *
     * ANTLR reads the literal into a UTF-16 string and walks it by code point (CharSupport and
     * LexerATNFactory.stringLiteral), so a high surrogate escape and a low one after it, `'\uD83D\uDE00'`, are the one
     * character the pair encodes, and a surrogate on its own is a transition on a code point no UTF-8 input decodes to,
     * which its lexer never takes: the byte reading has no literal that never matches, so it refuses.
     *
     * ANTLR's lexer counts a braced escape's digits from the literal's opening quote rather than from the escape
     * (ANTLRLexer.g's UNICODE_EXTENDED_ESC), so one whose closing brace stands twelve or more UTF-16 units past the
     * quote, `'abcdef\u{41}'` and a second braced escape in one literal, is its error 156, in its words: the literal
     * from its quote through the closing brace.
     * @return The literal.
     * @throws Spec_error For a lone surrogate, a braced escape ANTLR's error 156 refuses, or as character() refuses a
     *         character.
     */
    [[nodiscard]] Literal literal();

protected:
    /**
     * @brief Reads element options after their `<`, through the `>`, as ANTLR's parser reads them (ANTLRParser.g's
     *        elementOptions): names, dotted or not, each alone or, undotted, with `=` and a value that is a name, a
     *        number, a quoted string or a brace block, parted by commas, or nothing at all between the angles. An
     *        option is metadata on the element before it, `<fail='z'>` on a predicate and `<assoc=right>` on a token,
     *        and no token of the grammar's lexer, so the string among the values is no literal a parser rule uses.
     * @throws Spec_error If the options are of another shape, which ANTLR's parser rejects.
     */
    void element_options();

    /**
     * @brief Reads one character of a literal or a set, an escape decoded.
     * @param closing The byte that closes the literal or set, which a bare one of ends the reading.
     * @return The scalar, or std::nullopt at the closing byte.
     * @throws Spec_error At the text's end, for a raw line break, ANTLR's errors 152 and 50, an escape ANTLR has not
     *         got, its error 156, a Unicode property class, or a malformed Unicode escape.
     */
    [[nodiscard]] std::optional<char32_t> character(char closing);

    /**
     * @brief Skips a brace block from its `{`, however nested.
     * @throws Spec_error If the block never closes.
     */
    void skip_block();

    /**
     * @brief Skips a quoted string after its opening quote, through the closing one, a backslash escaping the byte
     *        after it; at the text's end where the string never closes.
     * @param quote The quote, `'` or `"`.
     */
    void skip_quoted(char quote);
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_CURSOR_HPP

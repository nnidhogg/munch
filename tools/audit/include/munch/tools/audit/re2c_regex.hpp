#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_REGEX_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_REGEX_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/re2c_classes.hpp"
#include "munch/tools/audit/read_re2c.hpp"

/**
 * @brief A re2c regex read to its end and rewritten for the pattern parser, Regex_reader, with what it reads,
 *        Regex_text, and the length of a flex-style `{name}` reference, reference_length().
 */
namespace munch::tools::audit
{
/**
 * @brief A re2c regex read to its end and rewritten into the syntax regex::parse() reads, its class kept where it is
 *        one, which is what re2c's class difference takes.
 *
 * The reader stands on the block's text at the regex, reads under the flags every pattern of the block is translated
 * under and the classes among the definitions in force, and is left where the regex ends, which its owner takes back as
 * its own offset. A class difference left empty under these flags is held rather than thrown, since the flags the block
 * settles on may fill it: the first is kept where the owner keeps its refusals for the settled pass.
 */
/**
 * @brief The length of a flex-style reference at an offset of a regex, `{name}` exactly, a name not opening with a
 *        digit between the braces; zero where there is none, a count `{2,5}` among them.
 * @param text The regex's text.
 * @param at The offset.
 * @return The length, brackets included, or zero.
 */
[[nodiscard]] std::size_t reference_length(std::string_view text, std::size_t at) noexcept;

/**
 * @brief A regex as Regex_reader reads it: as written, rewritten for the pattern parser, and the class it is.
 */
struct Regex_text
{
    /**
     * @brief The regex as written, blanks between tokens kept as one space and none at either end.
     */
    std::string pattern;

    /**
     * @brief The regex rewritten for the pattern parser.
     */
    std::string expression;

    /**
     * @brief The class the whole regex is, where it is one, which a definition keeps for the differences that name it.
     */
    std::optional<Class> points;
};

class Regex_reader : public Cursor
{
public:
    /**
     * @brief Binds the reader to the text at a regex.
     * @param text The whole file.
     * @param begin The offset the regex begins at.
     * @param flags The flags the regex is translated under.
     * @param line_bound Whether the regex ends at the line's end, which a flex-style definition's does.
     * @param classes The classes among the definitions in force, which a class difference takes its operands from.
     * @param deferred The first refusal held for the settled flags, which a class difference left empty sets when it
     *        is not set yet.
     */
    Regex_reader(
            std::string_view text, std::size_t begin, Re2c_flags flags, bool line_bound, const Classes_t& classes,
            std::optional<Spec_error>& deferred);

    /**
     * @brief Reads regex text up to what ends it: a bare `=` for a definition, a `;` closing a definition's body, or
     *        the start of an action, `{`, `:=` or `=>`.
     *
     * The text as written and the text rewritten for the pattern parser are both returned, the rewriting done token
     * by token: bare names become `{name}` unless the flex syntax makes them literals, quoted literals other than an
     * exact double-quoted one become bracket sequences, blanks are dropped, and a class difference `A \ B` becomes
     * the class of the code points left, its operands the char sets re2c takes there. With them comes the class the
     * whole regex is, where it is one, which a definition keeps for the differences that name it.
     * @param definitions The definitions read so far, which a difference's operand may name.
     * @return The pattern as written, its expression, both empty when an action follows at once, and its class.
     * @throws Spec_error If a quote or bracket is left open, or the regex uses a refused construct, an operand of a
     *         class difference that is no char set among them.
     */
    [[nodiscard]] Regex_text regex_text(const regex::Definitions_t& definitions);

private:
    /**
     * @brief A group of the regex being read, the whole regex the outermost, with the term being read in it.
     *
     * The expression is shaped as re2c's grammar shapes it, so that a class difference `A \ B` takes the operands re2c
     * gives it: an alternation is terms joined by `|`, a term is what stands concatenated since the last `|` or the
     * group's opening, and the difference takes the whole term on either side, so `[y] [a-z] \ [x]` and `[a-z] \ [x]
     * [y]` are both differences of a concatenation, which re2c refuses as no char set. A char set is one class atom and
     * nothing more: a bracket, the dot, a one-character literal, a name defined as one, or a group whose alternatives
     * are each one, which re2c merges into one class. The pattern parser has no difference, so the term is replaced by
     * the class of the code points left as soon as the right operand's term ends, at the next `|`, `\`, `/`, the
     * group's close or the regex's end.
     */
    struct Level
    {
        /**
         * @brief Where the current term's text begins in the expression.
         */
        std::size_t term{0};

        /**
         * @brief The atoms placed in the current term.
         */
        std::size_t atoms{0};

        /**
         * @brief The class the term is while it is one class atom and nothing more.
         */
        std::optional<Class> single{};

        /**
         * @brief The left operand of the difference the current term is the right operand of, once a `\` was read.
         */
        std::optional<Class> left{};

        /**
         * @brief Whether every term closed in this group so far was a class.
         */
        bool classes{true};

        /**
         * @brief The union of those, which is the group's class when they all were.
         */
        Class branches{};
    };

    /**
     * @brief A quoted literal rewritten as a bracket sequence, and the class it is.
     */
    struct Rewritten_literal
    {
        /**
         * @brief The bracket sequence.
         */
        std::string expression;

        /**
         * @brief The class the literal is where it is one character, which re2c takes for a char set.
         */
        std::optional<Class> points;
    };

    /**
     * @brief Whether the regex ends at the cursor: at the line's end for a regex bound to its line, and at the top
     *        level at a bare `=`, the `;` of a definition's body, or an action, a `{` opening one unless it is a count,
     *        `{2,5}`, or a flex-style reference, `{name}`, which re2c reads with its flex-syntax flag and which the
     *        parser reads as it stands.
     * @return True when it does.
     */
    [[nodiscard]] bool at_regex_end() const noexcept;

    /**
     * @brief Skips the comments and blanks at the cursor.
     * @return Whether they ran past the end of the line a line-bound regex ends with, a comment closing on it
     *         notwithstanding.
     * @throws Spec_error If a block comment never closes.
     */
    [[nodiscard]] bool comment_ends_regex();

    /**
     * @brief Reads a flex-style reference, `{name}`, when one is at the cursor, the class of the definition it names
     *        placed with it.
     * @return Whether one was read.
     */
    [[nodiscard]] bool take_reference();

    /**
     * @brief Places an atom in the current term: the first atom's class is the term's until anything more joins it.
     * @param points The atom's class, when it is one.
     */
    void place(std::optional<Class> points);

    /**
     * @brief The class a definition is, when it is one.
     * @param name The definition's name.
     * @return The class, or std::nullopt when the definition is no class or there is none.
     */
    [[nodiscard]] std::optional<Class> defined(std::string_view name) const;

    /**
     * @brief Reads a blank, which the pattern as written keeps as a space and the expression drops.
     * @return Whether one was read.
     */
    [[nodiscard]] bool take_blank();

    /**
     * @brief Reads a bracket, when one is at the cursor: copied through with its escapes, and under UTF-8 written as
     *        the code points it admits where it reaches past ASCII.
     *
     * The byte parser's reading of the bracket gives the code points it admits, which under UTF-8 the expression must
     * spell as their encodings, so a bracket the parser reads otherwise is refused there, while under the byte
     * encodings it stands as written for the parser to read when the scanner is built; re2c's [^] is any byte there,
     * which the parser would read as a member.
     * @param definitions The definitions in force, which the parser is given.
     * @return Whether one was read.
     * @throws Spec_error If the bracket is refused, or under UTF-8 is no class whose code points the reading can take.
     */
    [[nodiscard]] bool take_bracket(const regex::Definitions_t& definitions);

    /**
     * @brief Reads text copied through with its escapes, a bracket or an exact double-quoted literal, both of which the
     *        parser reads as they stand, through its closing byte.
     * @param close The closing byte, `]` or `"`.
     * @return The text, its opening and closing bytes included.
     * @throws Spec_error If it never closes, holds a Unicode or a braced escape, is the empty class, or holds a byte
     *         beyond ASCII under UTF-8.
     */
    [[nodiscard]] std::string copied_text(char close);

    /**
     * @brief Whether the regex is read under the UTF-8 encoding.
     * @return True when it is.
     */
    [[nodiscard]] bool in_utf8() const noexcept;

    /**
     * @brief Reads an exact double-quoted literal, when one is at the cursor: copied through as the parser reads it,
     *        and under UTF-8 written as the code points of its characters where one is beyond ASCII.
     * @param definitions The definitions in force, which the parser is given.
     * @return Whether one was read.
     * @throws Spec_error As copied_text() does, or under UTF-8 if the literal is no text whose code points the reading
     *         can take.
     */
    [[nodiscard]] bool take_exact_literal(const regex::Definitions_t& definitions);

    /**
     * @brief Whether a quote opens a case-insensitive literal, which is the flags' to say.
     * @param quote The quote.
     * @return True when it does.
     */
    [[nodiscard]] bool is_insensitive(char quote) const noexcept;

    /**
     * @brief Reads a literal the parser does not read as it stands, when one is at the cursor: a single-quoted one, or
     *        a case-insensitive double-quoted one, which become bracket sequences.
     * @return Whether one was read.
     * @throws Spec_error As literal() does, or under UTF-8 if the literal holds a byte beyond ASCII.
     */
    [[nodiscard]] bool take_literal();

    /**
     * @brief A quoted literal after its opening quote, through the closing one, as a bracket sequence, one bracket per
     *        character and an escape kept as written inside its bracket, an escape naming a code point beyond ASCII
     *        under UTF-8 excepted, which becomes the step of that code point.
     * @param quote The closing quote.
     * @param insensitive Whether a letter's bracket holds both cases.
     * @return The expression, and the class the literal is where it is one character, which re2c takes for a char
     *         set.
     * @throws Spec_error If the literal is empty or never closes, or an escape is a Unicode one.
     */
    [[nodiscard]] Rewritten_literal literal(char quote, bool insensitive);

    /**
     * @brief Reads a bare name, when one is at the cursor: a definition's name, `{name}` to the parser, or under the
     *        flex syntax the literal it spells.
     * @return Whether one was read.
     */
    [[nodiscard]] bool take_name();

    /**
     * @brief Reads the dot, when it is at the cursor: any code point but the newline.
     * @return Whether it was read.
     */
    [[nodiscard]] bool take_dot();

    /**
     * @brief Reads a tag, `@name` or `#name`, when one is at the cursor: it marks a position and matches nothing, so it
     *        is kept as written and dropped from the expression, and it is no char set to re2c.
     * @return Whether one was read.
     */
    [[nodiscard]] bool take_tag();

    /**
     * @brief Reads the `\` of a class difference, when it is at the cursor: the term before it is the left operand, a
     *        chain's earlier difference resolved first, and the term after it, whose text joins the left one's until
     *        the resolution replaces both, is the right one.
     * @return Whether it was read.
     * @throws Spec_error If no term stands before it or the term before it is no class.
     */
    [[nodiscard]] bool take_difference();

    /**
     * @brief Ends the term as the right operand of the difference a `\` opened, when one did: the class of the code
     *        points left replaces the text of both operands, and a class left empty is held for the settled flags.
     * @throws Spec_error If the term is no class.
     */
    void resolve();

    /**
     * @brief Reads the byte at the cursor as an operator: a group's opening or close, an alternative, a trailing
     *        context, or anything else, a repetition, a count or a byte the parser will refuse, which makes the term
     *        more than one class.
     * @throws Spec_error As close_term() does.
     */
    void take_operator();

    /**
     * @brief Opens a group, the mark of `(!R)`, a group that captures nothing, taken with its opening.
     */
    void open_group();

    /**
     * @brief Where the byte after a group's opening stands, past whatever blanks or comments come between, as re2c
     *        reads its regex tokens, a newline and a comment among them where no line bounds the regex.
     * @return The offset.
     */
    [[nodiscard]] std::size_t group_mark() const noexcept;

    /**
     * @brief Closes a group, which is one class atom when each of its alternatives is one, as re2c merges them.
     * @throws Spec_error As close_term() does.
     */
    void close_group();

    /**
     * @brief Ends the term at `|`, `/`, a group's close or the regex's end, joining it to the group's alternatives,
     *        which are a class together only while each is one.
     * @throws Spec_error As resolve() does.
     */
    void close_term();

    /**
     * @brief Ends the term at a `|` or a `/` and opens the next; a trailing context makes the regex no class.
     * @param byte The byte, `|` or `/`.
     * @throws Spec_error As close_term() does.
     */
    void alternative(char byte);

    /**
     * @brief Begins a new term where the expression now ends.
     */
    void open_term();

    /**
     * @brief The flags the regex is translated under.
     */
    Re2c_flags flags_;

    /**
     * @brief Whether the regex ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound_;

    /**
     * @brief The classes among the definitions in force.
     */
    const Classes_t& classes_;

    /**
     * @brief The first refusal held for the settled flags.
     */
    std::optional<Spec_error>& deferred_;

    /**
     * @brief The pattern as written, read so far.
     */
    std::string pattern_;

    /**
     * @brief The expression rewritten for the parser, read so far.
     */
    std::string expression_;

    /**
     * @brief The groups open where the reader stands, the whole regex first.
     */
    std::vector<Level> levels_;
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_REGEX_HPP

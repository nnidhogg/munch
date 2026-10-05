#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief The re2c reader, read_re2c(), which the command reads a file of re2c blocks with, under the flags its command
 *        line gives, Re2c_flags, and the encoding they name, Re2c_encoding.
 */
namespace munch::tools::audit
{
/**
 * @brief The encodings re2c can be told to scan its input in, its `--ascii`, `--ebcdic`, `--ucs2`, `--utf8`, `--utf16`
 *        and `--utf32` options and the `re2c:encoding:...` configurations.
 *
 * A pattern names code points and the generated scanner reads code units, so the encoding is what turns the one into
 * the other. ASCII, the default, and EBCDIC have a code point per byte; UTF-8 has one to four bytes per code point; the
 * rest have a code unit of two or four bytes, which a reading over bytes cannot be.
 */
enum class Re2c_encoding
{
    /**
     * @brief ASCII, one byte a code point, the code space 0 to 255.
     */
    ascii,

    /**
     * @brief EBCDIC, one byte a code point under another mapping than ASCII's.
     */
    ebcdic,

    /**
     * @brief UCS-2, a code unit of two bytes.
     */
    ucs2,

    /**
     * @brief UTF-8, a code point of one to four bytes.
     */
    utf8,

    /**
     * @brief UTF-16, a code unit of two bytes.
     */
    utf16,

    /**
     * @brief UTF-32, a code unit of four bytes.
     */
    utf32
};

/**
 * @brief The re2c command-line flags that change how a file reads, and the encoding a block's configuration sets.
 */
struct Re2c_flags
{
    /**
     * @brief Equal when every flag is the same, as a block's reading compares them across a configuration.
     */
    [[nodiscard]] bool operator==(const Re2c_flags&) const = default;

    /**
     * @brief `-F`, `--flex-syntax`: a definition may be a `name regex` line, a reference is `{name}` only, and a bare
     *        letter is a literal. A file holding a `name regex` line is read this way whether or not the flag is given,
     *        since only that syntax accepts one. re2c has no configuration for it, so no block can turn it on.
     */
    bool flex_syntax{false};

    /**
     * @brief `--case-inverted`: the double-quoted literal is the case-insensitive one and the single-quoted one exact.
     */
    bool case_inverted{false};

    /**
     * @brief `--case-insensitive`: a literal in either quote is case-insensitive.
     */
    bool case_insensitive{false};

    /**
     * @brief The encoding the patterns' code points are matched in, which a `re2c:encoding:...` configuration sets for
     *        its whole block; ASCII unless one does.
     */
    Re2c_encoding encoding{Re2c_encoding::ascii};
};

/**
 * @brief Reads the re2c blocks of a source file, one specification per block with rules.
 *
 * re2c lives inside C: every block opened by a comment beginning `!re2c` or `!rules:re2c` is read, in order, through
 * its close, and the other block kinds, which carry no rules, are skipped along with the code around them. A block is
 * read as Block_reader reads it, as re2c 3.1 does: its close, its `re2c:` configurations, which configure() honours for
 * the whole block the last assignment governing, its definitions in either syntax, and its rules, with their actions,
 * the ranks re2c gives the `<*>` rules and the default rule `*`, and the default rules a block brings in yielding to
 * its own; what re2c refuses of a scanner's mix of rules, conditions with normal rules and end rules without other
 * rules, is refused in its words by Rule_kinds::note() and Rule_kinds::refuse_end_rules(). The end rule `$`, `<!c>`
 * setup rules and the entry rule `<>` are not tokens and are skipped.
 *
 * Each block with rules is a scanner of its own, as re2c compiles it, and comes back as its own specification, named by
 * the line its opener is on, with the definitions and configurations in force when it closed; a block holding only
 * definitions or configurations feeds the ones after it, while a local block's, a rules block's and a use block's stay
 * in them. A definition another block uses, and a rules block a `!use:` directive or a use block takes, are read again
 * where they are used, under the flags in force there, since re2c compiles them at every point of use, as Library_t and
 * Definition_site say. re2c declares no conditions, so the ones a block's rules name are that scanner's, each
 * exclusive.
 *
 * The regex is re2c's, and it is rewritten into the syntax regex::parse() reads, which each rule keeps as its
 * expression beside the pattern as written: a bare name is a definition and becomes `{name}`, or under the flex syntax
 * stays the literal it is; a case-insensitive literal `'abc'` becomes `[aA][bB][cC]` and an exact one in single quotes
 * a bracket sequence too; `[^]`, any byte, is spelled out; blanks between tokens are dropped; a double-quoted literal,
 * a bracket expression, a `{name}` reference, the dot, grouping, alternation and the postfix operators are already the
 * parser's, a group `(!R)`, which captures nothing, being the group of R, and a bracket range written backwards,
 * `[z-a]`, spanning its members as re2c reads it, which the parser is told. Unicode escapes `\u`, `\U` and `\X` are
 * refused, since they need an encoding the byte reading has not got, and a braced hexadecimal escape `\x{...}` because
 * re2c has no such form and answers it with a syntax error, its own being `\xHH`. A class difference `A \ B` takes the
 * operands re2c's grammar gives it, the whole term on either side, everything concatenated since the last `|` or the
 * group's opening, and each must be what re2c calls a char set, one bracket, the dot, a literal of one character, a
 * name defined as one of these or a group of alternatives that each are, which re2c merges into one class; it becomes
 * the class of the code points left, subtracted before any encoding, so that under UTF-8 `[^] \ [\x00-\x7f]` is every
 * code point beyond ASCII, and a term that is more than one class, `[a-z] \ [x] [y]`, `[a-z]* \ [x]` or `[a-z] \ "xy"`,
 * is refused as re2c refuses it, which can only difference char sets. A difference that leaves no code point is refused
 * under the configuration the block leaves and no other, since what the operands hold is that configuration's to say:
 * `[^] \ [\x00-\xff]` is every code point past the bytes once the block turns UTF-8 on, and `"a" \ 'A'` is the exact
 * `a` once it inverts which quote folds. Whether an action returns a token is read by returned().
 *
 * Under the UTF-8 encoding a pattern names code points and the scanner reads their encodings, so a class, the dot,
 * `[^]` and a class difference become the code point ranges they admit, written for the pattern parser as `\u{...}`
 * members and, where the set holds the surrogates, which re2c encodes like any other code point and the parser's
 * escapes cannot name, the bytes of those beside them; an all-ASCII class or literal stands as it is, its encoding
 * being itself. Refused by name there: a byte beyond ASCII written straight into the source, which code points it
 * stands for being the `--input-encoding` option's to say and no file carrying it. The configurations and encodings a
 * reading over bytes cannot follow are refused wherever they come from, a configuration or the flags the caller passes,
 * as configure() and unreadable() say. What a command line asks for beyond the flags is beyond the reading: an
 * `--encoding-policy` there is taken to be the default one, as an `--input-encoding` is taken to be ASCII.
 * @param source The file's text.
 * @param flags The command line's flags, none unless given.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @param includes How a file the code includes by a quoted name is reached, whose macros are read as the file's own;
 *        none unless given, under which a quoted include is refused by name, its definitions being out of sight.
 * @return The scanners, in file order.
 * @throws Spec_error If a block, a brace, a quote or a bracket is left open, a definition or a rule is malformed, or a
 *         regex uses a construct the rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_re2c(
        std::string_view source, Re2c_flags flags = {}, const Returning_t& returning = {},
        const Include_reader_t& includes = {});

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

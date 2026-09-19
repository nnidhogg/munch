#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
/**
 * @brief The encodings re2c can be told to scan its input in, its `--ascii`, `--ebcdic`, `--ucs2`, `--utf8`,
 *        `--utf16` and `--utf32` options and the `re2c:encoding:...` configurations.
 *
 * A pattern names code points and the generated scanner reads code units, so the encoding is what turns the one into
 * the other. ASCII, the default, and EBCDIC have a code point per byte; UTF-8 has one to four bytes per code point;
 * the rest have a code unit of two or four bytes, which a reading over bytes cannot be.
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
    utf32,
};

/**
 * @brief The re2c command-line flags that change how a file reads, and the encoding a block's configuration sets.
 */
struct Re2c_flags
{
    /**
     * @brief `-F`, `--flex-syntax`: a definition may be a `name regex` line, a reference is `{name}` only, and a bare
     *        letter is a literal. A file holding a `name regex` line is read this way whether or not the flag is
     *        given, since only that syntax accepts one. re2c has no configuration for it, so no block can turn it on.
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
     * @brief The encoding the patterns' code points are matched in, which a `re2c:encoding:...` configuration sets
     *        for its whole block; ASCII unless one does.
     */
    Re2c_encoding encoding{Re2c_encoding::ascii};

    /**
     * @brief Whether two sets of flags say the same, which reading a block asks to see whether a configuration
     *        inside it changed what the block was read under.
     * @param other The flags to compare with.
     * @return True when every flag is the same.
     */
    [[nodiscard]] bool operator==(const Re2c_flags& other) const = default;
};

/**
 * @brief Reads the re2c blocks of a source file, one specification per block with rules.
 *
 * re2c lives inside C: every block opened by a comment beginning `!re2c` or `!rules:re2c` is read, in order, through
 * its close, and the other block kinds, which carry no rules, are skipped along with the code around them. A block's
 * close is found as re2c finds it, as the first star-slash between items: one inside a quoted literal, a class, an
 * action or a comment is content, since the file is read by re2c and never by a C compiler. Inside a block, `re2c:`
 * lines are configurations and are recorded as options, the flags among them honoured under every name the manual gives
 * them, the canonical one and its `flags:` aliases, and honoured for the whole block wherever in it they stand, which
 * is the scope re2c gives a configuration; of two assignments to one name the last is the one that governs, so the
 * configuration the whole block leaves is settled before any of its patterns is translated, and a rule standing above
 * the assignment reads under it like every other. The flex syntax has no configuration of its own, only the command
 * line and the evidence of a flex-style definition. `name = regex;` declares a definition, as does a name followed by a
 * blank and then regex under the flex syntax, wherever the name stands, at the line's start, indented or after an item
 * on the same line, the regex running to the end of the line, as re2c -F reads it; a `{` after the blank opens an
 * action or a reference instead and leaves the name a rule's literal, and an action on the definition's line is
 * refused, since re2c answers it with a syntax error. A rule is an optional `<c1, c2>` condition list, a regex, and an
 * action, which is a brace block or `:=` and the code after it, which runs on to the first line beginning with a
 * character that is not a blank, as re2c ends such an action; a `=> c` transition is kept as the action's text and a
 * `:=> c` shortcut rule, which carries no code at all, ends with the condition's name. A rule in `<*>` is active in
 * every condition and ranks below the condition's own rules wherever in the block it stands, since re2c appends the
 * `<*>` rules to each condition's own, a used block's standing where its directive does; so the `<*>` rules are placed
 * after every rule naming a condition, each rank in its own order, and among the rules matching one lexeme a
 * condition's own comes first. The default rule `*` is a rule whose action runs where no other rule matches, over one
 * code unit, which is one byte under every encoding the reading follows, UTF-8 included, where `[^]` is a whole code
 * point and `*` one byte of one; it is read as the class of every byte and placed after every other rule of its
 * scanner, since re2c gives it the lowest priority wherever in the block it stands, a `<*> *` after a named condition's
 * own default rule, which beats it in that condition. A second default rule of a block's own for a condition it already
 * gave one, `<*>` and no condition being one condition to re2c and a named one another, is refused as re2c refuses it,
 * while one a `!use:` directive or a use block brought in yields to the block's own in every condition the own one
 * stands in, wherever the two stand, as re2c has it. The end rule `$` and `<!c>` setup rules are not tokens and are
 * skipped. A scanner's rules name conditions or name none, never both: one holding a rule of each kind, the rules a
 * `!use:` directive brought in counted with its own, is refused as re2c refuses it, which cannot mix conditions with
 * normal rules, at the first rule naming none, and an end rule `$` naming none alone among rules naming one in the
 * words re2c has for that; a rules block is checked where it is used, since re2c compiles it there and nowhere else.
 * Comments in either C style are skipped between items.
 *
 * Each block with rules is a scanner of its own, as re2c compiles it, and comes back as its own specification, named by
 * the line its opener is on, with the definitions and configurations in force when it closed; a block holding only
 * definitions or configurations feeds the ones after it, while a local block's, a rules block's and a use block's stay
 * in them. A definition another block uses is translated again there, out of the regex as written, under that block's
 * own flags, since re2c compiles it at every point of use: `point = [^];` written where the encoding was ASCII admits
 * one byte in its own block and a whole code point in a block that turns UTF-8 on, and a flex-style definition is read
 * under the flex syntax wherever it is used. A definition another block declared that this block's flags cannot read is
 * refused where a rule of this block reaches it, directly or through the definitions the rule names, and nowhere else,
 * since that is where re2c would compile it: one no rule reaches is left out, and so is an alias of it that no rule
 * names; one the block itself declares is read where it stands and refused there. A use block, the one opened
 * `!use:re2c[:name]`, takes the rules block of that name, or the most recent one, named or not, when it names none, and
 * the name in its opener is the block it uses and no name of its own. A used block is read again where it is used,
 * under the flags in force there, since re2c compiles its regexes at every point of use: one rules block can be a
 * scanner under one encoding and another under another, and a configuration of the using block governs the rules it
 * takes as well as the ones it writes. re2c declares no conditions, so the ones a block's rules name are that
 * scanner's, each exclusive.
 *
 * The regex is re2c's, and it is rewritten into the syntax regex::parse() reads, which each rule keeps as its
 * expression beside the pattern as written: a bare name is a definition and becomes `{name}`, or under the flex syntax
 * stays the literal it is; a case-insensitive literal `'abc'` becomes `[aA][bB][cC]` and an exact one in single quotes
 * a bracket sequence too; `[^]`, any byte, is spelled out; blanks between tokens are dropped; a double-quoted literal,
 * a bracket expression, a `{name}` reference, the dot, grouping, alternation and the postfix operators are already the
 * parser's. Unicode escapes `\u`, `\U` and `\X` are refused, since they need an encoding the byte reading has not got,
 * and a braced hexadecimal escape `\x{...}` because re2c has no such form and answers it with a syntax error, its own
 * being `\xHH`. A class difference `A \ B` takes the operands re2c's grammar gives it, the whole term on either side,
 * everything concatenated since the last `|` or the group's opening, and each must be what re2c calls a char set, one
 * bracket, the dot, a literal of one character, a name defined as one of these or a group of alternatives that each
 * are, which re2c merges into one class; it becomes the class of the code points left, subtracted before any encoding,
 * so that under UTF-8 `[^] \ [\x00-\x7f]` is every code point beyond ASCII, and a term that is more than one class,
 * `[a-z] \ [x] [y]`, `[a-z]* \ [x]` or `[a-z] \ "xy"`, is refused as re2c refuses it, which can only difference char
 * sets. Whether an action returns a token is read by returned().
 *
 * Under the UTF-8 encoding a pattern names code points and the scanner reads their encodings, so a class, the dot,
 * `[^]` and a class difference become the code point ranges they admit, written for the pattern parser as `\u{...}`
 * members and, where the set holds the surrogates, which re2c encodes like any other code point and the parser's
 * escapes cannot name, the bytes of those beside them; an all-ASCII class or literal stands as it is, its encoding
 * being itself. Refused by name there: a byte beyond ASCII written straight into the source, which code points it
 * stands for being the `--input-encoding` option's to say and no file carrying it; and an `encoding-policy` other than
 * the default, which changes what becomes of the surrogates. The encodings a reading over bytes cannot follow are
 * refused wherever they come from, a configuration or the flags the caller passes, each with its own reason: EBCDIC
 * gives a byte another code point than ASCII does, and UCS-2, UTF-16 and UTF-32 have a code unit of more than one byte.
 * What a command line asks for beyond the flags is beyond the reading: an `--encoding-policy` there is taken to be the
 * default one, as an `--input-encoding` is taken to be ASCII.
 * @param source The file's text.
 * @param flags The command line's flags, none unless given.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @return The scanners, in file order.
 * @throws Spec_error If a block, a brace, a quote or a bracket is left open, a definition or a rule is malformed, or a
 *        regex uses a construct the rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_re2c(
        std::string_view source, Re2c_flags flags = {}, const Returning_t& returning = {});

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

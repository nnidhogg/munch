#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
/**
 * @brief The re2c command-line flags that change how a file reads and that the file itself does not carry.
 */
struct Re2c_flags
{
    /**
     * @brief `-F`, `--flex-syntax`: a definition may be a `name regex` line, a reference is `{name}` only, and a bare
     *        letter is a literal. A file holding a `name regex` line is read this way whether or not the flag is
     *        given, since only that syntax accepts one.
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
};

/**
 * @brief Reads the re2c blocks of a source file, one specification per block with rules.
 *
 * re2c lives inside C: every block opened by a comment beginning `!re2c` or `!rules:re2c` is read, in order, through
 * its close, and the other block kinds, which carry no rules, are skipped along with the code around them. A block's
 * close is found as re2c finds it, as the first star-slash between items: one inside a quoted literal, a class, an
 * action or a comment is content, since the file is read by re2c and never by a C compiler. Inside a block, `re2c:`
 * lines are configurations and are recorded as options, the flags among them honoured; `name = regex;` declares a
 * definition, as does a `name regex` line under the flex syntax, a name opening the line and followed by a blank with
 * no action on the line; and a rule is an optional `<c1, c2>` condition list, a regex, and an action, which is a brace
 * block or `:=` followed by the rest of the line; a `=> c` or `:=> c` transition is kept as the action's text. The
 * default rule `*`, the end rule `$` and `<!c>` setup rules are not tokens and are skipped. Comments in either C style
 * are skipped between items.
 *
 * Each block with rules is a scanner of its own, as re2c compiles it, and comes back as its own specification, named by
 * the line its opener is on, with the definitions and configurations in force when it closed; a block holding only
 * definitions or configurations feeds the ones after it. re2c declares no conditions, so the ones a block's rules name
 * are that scanner's, each exclusive.
 *
 * The regex is re2c's, and it is rewritten into the syntax regex::parse() reads, which each rule keeps as its
 * expression beside the pattern as written: a bare name is a definition and becomes `{name}`, or under the flex syntax
 * stays the literal it is; a case-insensitive literal `'abc'` becomes `[aA][bB][cC]` and an exact one in single quotes
 * a bracket sequence too; `[^]`, any byte, is spelled out; blanks between tokens are dropped; a double-quoted literal,
 * a bracket expression, a `{name}` reference, the dot, grouping, alternation and the postfix operators are already the
 * parser's. Unicode escapes `\u`, `\U` and `\X` and the class difference `\` are refused, since the first need an
 * encoding the byte reading has not got and the second is not the parser's. Whether an action returns a token is read
 * by returned().
 * @param source The file's text.
 * @param flags The command line's flags, none unless given.
 * @param returning The forms besides `return` an action returns a token through, none unless given.
 * @return The scanners, in file order.
 * @throws Spec_error If a block, a brace, a quote or a bracket is left open, a definition or a rule is malformed, or
 *         a regex uses a construct the rewriting refuses.
 */
[[nodiscard]] std::vector<Lexer_spec> read_re2c(
        std::string_view source, Re2c_flags flags = {}, const Returning_t& returning = {});

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_READ_RE2C_HPP

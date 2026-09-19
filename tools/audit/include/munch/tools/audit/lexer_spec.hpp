#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LEXER_SPEC_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LEXER_SPEC_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/regex/parse.hpp"
#include "munch/tools/audit/token_set.hpp"

namespace munch::tools::audit
{
/**
 * @brief The forms an action returns a token through besides `return` itself: the names a scanner wraps or stores
 *        its returns in, which no file declares, so the caller names them. A listed name followed by a parenthesis
 *        returns its first argument, PHP's `RETURN_TOKEN(T_EXIT)`; followed by `=`, the expression assigned, ninja's
 *        `token = BUILD`; and bare, itself.
 */
using Returning_t = std::vector<std::string>;

/**
 * @brief What one scanner's specification declares, as far as a token set is concerned: its definitions, its start
 *        conditions, its options and its rules in order, whichever generator's file it was read from.
 *
 * A flex file is one scanner; a re2c file holds one per block with rules; a Rust file holds one per enum deriving
 * Logos. Rule order is the priority: flex and re2c take the longest match and, among rules matching it, the first in
 * the file, which is exactly the maximal-munch scan with priority by rule index that the library runs; logos ranks
 * by a number of its own, which each of its rules carries. The code sections and the actions' bodies are carried as
 * text and never interpreted beyond the return the audit looks for.
 */
struct Lexer_spec
{
    /**
     * @brief One rule: its pattern as written, the form of it the pattern parser reads, the start conditions it is
     *        active in, and what its action does with the match as far as the audit needs to know.
     */
    struct Rule
    {
        /**
         * @brief The pattern exactly as it stands in the file.
         */
        std::string pattern;

        /**
         * @brief The pattern in the syntax regex::parse() reads, `{name}` for a definition; the same as the pattern
         *        for a flex file, and the file's dialect rewritten for others.
         */
        std::string expression;

        /**
         * @brief The start conditions the rule is active in, empty when it names none; `*` stands for all.
         */
        std::vector<std::string> conditions;

        /**
         * @brief The action's text, braces included, `|` when it shares the next rule's.
         */
        std::string action;

        /**
         * @brief The expression the action returns, when it returns one through `return` or a form the caller
         *        named; a rule that returns nothing discards its match, which is what whitespace and comment rules
         *        do, and the audit treats the token as discarded.
         */
        std::optional<std::string> token;

        /**
         * @brief The priority the file's generator ranks the rule at among rules matching the same longest lexeme,
         *        its own number with the higher winning, when the generator ranks by a number rather than by file
         *        order, as logos does; std::nullopt when the rule's index is its priority, as flex and re2c rank.
         */
        std::optional<std::size_t> priority;

        /**
         * @brief The line the rule begins on, counted from one, for the report.
         */
        std::size_t line;
    };

    /**
     * @brief A start condition the file declares.
     */
    struct Condition
    {
        /**
         * @brief The condition's name.
         */
        std::string name;

        /**
         * @brief Whether it is exclusive, so that rules naming no condition are inactive in it.
         */
        bool exclusive;
    };

    /**
     * @brief The named patterns, in the syntax regex::parse() reads, `{name}` in an expression expanding to one.
     */
    regex::Definitions_t definitions;

    /**
     * @brief The start conditions besides INITIAL: the ones a flex file declares, the ones a re2c file's rules name.
     */
    std::vector<Condition> conditions;

    /**
     * @brief The file's options, each one entry: flex's `%option` words, re2c's configurations, or the `#[logos]`
     *        keys besides the skips and subpatterns.
     */
    std::vector<std::string> options;

    /**
     * @brief What the patterns are read under besides their syntax: what the file's options leave standing of the
     *        settings the parser has, the case folding flex's `%option caseless` asks of every pattern among them.
     *
     * A reader that spells such a setting into the patterns themselves, as re2c's and logos's do with their case
     * flags, leaves the parser's defaults here.
     */
    regex::Parse_options parse{};

    /**
     * @brief The rules, in file order.
     */
    std::vector<Rule> rules;

    /**
     * @brief The line the rules begin on, counted from one: a flex file's `%%`, a re2c block's opener, a Rust enum's
     *        derive; what names a scanner among a file's.
     */
    std::size_t line{1};
};

/**
 * @brief A specification the reader refuses, with the line it was refused at.
 */
class Spec_error : public std::runtime_error
{
public:
    /**
     * @brief Constructs the error from its message and line.
     * @param message What was refused and why.
     * @param line The line, counted from one.
     */
    Spec_error(const std::string& message, std::size_t line);

    /**
     * @brief The line the refusal points at, counted from one.
     * @return The line.
     */
    [[nodiscard]] std::size_t line() const noexcept;

private:
    /**
     * @brief The line the refusal points at.
     */
    std::size_t line_;
};

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
 * @brief One macro the C around a scanner defines, as far as this reading asks about it: the words of every
 *        replacement a definition gives it, and whether any definition takes parameters.
 *
 * The reading expands nothing. What it asks of a macro is whether using it could put a call, a return or a
 * directive in an action that the action's own text does not show, and that is decided from the words the
 * replacements hold, every definition's together, without deciding which definition is live where.
 */
struct Macro
{
    /**
     * @brief Whether a definition of it takes parameters, `#define CALL(f) f()`.
     */
    bool function_like{false};

    /**
     * @brief The tokens of its replacements, every definition's appended, the parameters left out.
     */
    std::vector<std::string> words;

    /**
     * @brief The tokens of each definition's replacement on its own, in the order the definitions stand, since which
     *        one is live where an action uses the name is not decided by this reading.
     */
    std::vector<std::vector<std::string>> replacements;
};

/**
 * @brief The macros a stretch of C defines, by name.
 */
using Macros_t = std::map<std::string, Macro, std::less<>>;

/**
 * @brief Adds the macros a stretch of C defines to a table: every `#define`, in every arm of every conditional,
 *        since which arm is live is not this reading's to decide. A replacement runs to the end of its logical
 *        line, past a splice and past a block comment.
 * @param code The stretch of C.
 * @param macros The table, added to.
 */
void take_macros(std::string_view code, Macros_t& macros);

/**
 * @brief A file a reader reached through an include: its text and the path it was read from, which its own
 *        includes are resolved beside.
 */
struct Included
{
    /**
     * @brief The file's text.
     */
    std::string text;

    /**
     * @brief The path the file was read from, as the reader spells it, handed back as `from` for the file's own
     *        includes.
     */
    std::string path;
};

/**
 * @brief How an include directive names its file.
 */
enum class Include_form : std::uint8_t
{
    /**
     * @brief `#include "name"`, looked for beside the including file first.
     */
    quoted,

    /**
     * @brief `#include <name>`, looked for on the compiler's include path, a system header unless the reader finds
     *        one beside the file.
     */
    angled,

    /**
     * @brief `#include NAME`, the file named by a macro, which the reading does not expand.
     */
    computed
};

/**
 * @brief How a reader reaches a file the copied code includes, `#include "hooks.h"` or `#include <hooks.h>`: the
 *        file, or std::nullopt when no such file can be found. `from` is the path of the including file as the
 *        reader returned it, empty for the file audited, and `form` how the directive names the file, so that a
 *        quoted name is looked for beside the file including it first and an angle-bracket name on the include
 *        path alone, as a compiler resolves them. The command line does so with its `--include` directories; a
 *        caller giving no reader has every quoted include refused, since what the file defines is out of sight, and
 *        an angle-bracket include the reader does not find is taken for a system header's, which defines nothing of
 *        the scanner's.
 */
using Include_reader_t =
        std::function<std::optional<Included>(std::string_view name, std::string_view from, Include_form form)>;

/**
 * @brief An include directive of a stretch of C, as c_tokens() reads it.
 */
struct Include_directive
{
    /**
     * @brief The name between the quotes or the brackets, or the macro's name for a computed include.
     */
    std::string name;

    /**
     * @brief How many lines into the stretch the directive stands.
     */
    std::size_t line;

    /**
     * @brief How the file is named.
     */
    Include_form form;
};

/**
 * @brief The include directives of a stretch of C, in order.
 * @param code The stretch.
 * @return The directives.
 */
[[nodiscard]] std::vector<Include_directive> includes_of(std::string_view code);

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

/**
 * @brief The values a macro stands for when it is transparent to the reading: the plain values each definition's
 *        replacement holds, numbers, quoted literals, `true` and `false`, one value per definition, so that
 *        `cursors[SLOT]` under `#define SLOT 0` is `cursors[0]` to a reading that compares spellings, and under a
 *        second `#define SLOT 1` is either, since which definition is live where the name is used is not decided
 *        here. A function-like macro whose replacement is plain values stands for them whatever its arguments.
 * @param name The macro's name.
 * @param macros The macros defined.
 * @return The distinct values, in definition order; none for a name that is no macro or for an opaque one, which
 *         macro_use() refuses using where a word of meaning may stand.
 */
[[nodiscard]] std::vector<std::vector<std::string>> plain_values(std::string_view name, const Macros_t& macros);

/**
 * @brief Why a stretch of C is out of this reading's sight for what its macros could put in it, or std::nullopt
 *        when nothing about them is.
 *
 * A macro is opaque when a replacement of it holds a word the reading gives meaning to in an action, a `return`
 * or a call that moves the match among them, or a `#` or `##` that makes new tokens, or `__VA_ARGS__`, or the
 * name of another opaque macro, transitively. Using an opaque macro is refused; so is passing to any macro of the
 * file's an argument that holds such a word or such a name, since `#define CALL(f) f()` makes `CALL(input)` the
 * call the action does not spell. A macro whose replacements hold none of that, `#define MAX 10`, changes nothing
 * the reading looks for and its uses are read as ordinary text.
 * @param code The stretch of C.
 * @param macros The macros the file defines.
 * @param meaningful The words the reading gives meaning to in an action.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> macro_use(
        std::string_view code, const Macros_t& macros, std::span<const std::string_view> meaningful);

/**
 * @brief Why a stretch of C is out of this reading's sight for the conditional directives in it, or std::nullopt
 *        when it holds none: which arm of a `#if` is live is the build's to decide, so a stretch holding one is
 *        refused rather than read with one arm guessed.
 * @param code The stretch of C.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> conditional_use(std::string_view code);

/**
 * @brief Why an action is out of this reading's sight for the preprocessor directives in it, or std::nullopt when it
 *        holds none: a directive inside an action defines or conditions code the reading does not follow, so an
 *        action holding one is refused rather than read with the directive taken for code.
 * @param code The action's text.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> directive_use(std::string_view code);

/**
 * @brief Why the token an action returns is out of this reading's sight, or std::nullopt when the text decides it:
 *        an action that returns different tokens from different places, `if (yyleng >= 2) return 7; return 8;`,
 *        emits a token the text alone does not decide, and one that returns on some path and ends without
 *        returning on another, `if (yyleng >= 2) return 7;`, emits a token on the one and discards the match on
 *        the other; each is refused rather than read by its first return. The text decides the token when every
 *        return agrees and the action's last statement returns on every path through it: a return, a block ending
 *        in one, an `if` whose both branches do, or such a statement followed by one jump, `token = BUILD; break;`.
 *        A loop, a `switch` or an `if` without an `else` may fall through and is refused rather than followed, and
 *        so is a `break`, a `continue` or a `goto` anywhere but as that one jump, since in flex's and re2c's
 *        scanners each ends the action on its path before any return; a `goto` is refused wherever it stands, its
 *        label being out of sight. An action returning nowhere discards its match, as returned() says, unless a
 *        `break` may leave the scan: flex writes each action as a case of a switch, so its `break` discards, where
 *        re2c's actions stand in the loop the file wrote, which a `break` leaves.
 * @param action The action's text.
 * @param returning The names the reader gives meaning to as returns, as returned() takes them.
 * @param break_discards Whether a `break` discards the match, as flex's does, or leaves a loop out of sight, as
 *        re2c's may.
 * @param restarts The labels a `goto` restarts the scan by, discarding the match as a `continue` does: re2c's files
 *        write `goto loop;` to a label before the block; none unless given, under which every `goto` is refused,
 *        and a `goto` to any label but these is refused whatever is given, its target being out of sight.
 * @param chained Whether control falls from the action's end into the next rule's action, as it does in re2c's
 *        generated code for a rule's action, so that one returning nowhere must leave by a jump; false for flex,
 *        whose `YY_BREAK` ends every action, and for re2c's setup and entry rules, whose code runs before the scan.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> returns_undecided(
        std::string_view action, const Returning_t& returning = {}, bool break_discards = true,
        const std::set<std::string, std::less<>>& restarts = {}, bool chained = false);

/**
 * @brief The expression a C action returns, when it returns one.
 *
 * What both readers ask of an action: a rule returning something produces a token, a rule returning nothing
 * discards its match, which is what whitespace and comment rules do. The earliest token among `return` and the
 * names given decides, the action read as c_tokens() reads it, so that a `return` inside a comment or a literal is
 * none and one after them is one.
 * @param action The action's text.
 * @param returning The forms besides `return` that return, none unless given.
 * @return The expression between `return` and its `;`, or what a named form returns, from its first token through its
 *         last; std::nullopt when the action returns nothing.
 */
[[nodiscard]] std::optional<std::string> returned(std::string_view action, const Returning_t& returning = {});

/**
 * @brief Compiles the token set a start condition scans with; token_set() followed by compile().
 * @param spec The specification.
 * @param condition The condition, INITIAL for the default one.
 * @return The compiled lexer.
 * @throws Spec_error As token_set() does.
 */
[[nodiscard]] core::Lexer build(const Lexer_spec& spec, std::string_view condition);

/**
 * @brief Where the brace block opening a stretch of C closes, string and character literals and comments skipped as
 *        c_tokens() skips them, which is how re2c reads an action.
 * @param code The stretch, its first byte the opening brace.
 * @return The offset just past the closing brace, or std::nullopt when the stretch ends first.
 */
[[nodiscard]] std::optional<std::size_t> brace_close(std::string_view code);

/**
 * @brief The token set a start condition scans with: every active rule is a token whose id is its rule index and
 *        whose priority is that index, or its generator's own number turned onto the builder's lower-wins scale
 *        when the rule carries one, and the rules returning nothing are discarded.
 *
 * A generator's number is placed below every index at half the scale's range, so that a rule appended past the
 * set's priorities still has room; two rules of one number tie, and the builder settles a tie by the lower id, which
 * is file order. Every pattern is read under the spec's parse options, so a file that asks for case-insensitive
 * scanning throughout has every letter of every pattern folded, definitions included.
 * @param spec The specification.
 * @param condition The condition, INITIAL for the default one.
 * @return The token set, its expressions parsed against the definitions.
 * @throws Spec_error If a rule's pattern is refused by regex::parse(), naming the rule's line and the reason, or a
 *         rule's priority number lies beyond the scale.
 */
[[nodiscard]] Token_set token_set(const Lexer_spec& spec, std::string_view condition);

/**
 * @brief The rules active in a start condition, in file order.
 *
 * A rule naming no condition is active in INITIAL and in every inclusive condition; a rule naming a condition is
 * active in it; a rule naming `*` is active everywhere.
 * @param spec The specification.
 * @param condition The condition, INITIAL for the default one.
 * @return The indices of the active rules into spec.rules.
 */
[[nodiscard]] std::vector<std::size_t> active_rules(const Lexer_spec& spec, std::string_view condition);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LEXER_SPEC_HPP

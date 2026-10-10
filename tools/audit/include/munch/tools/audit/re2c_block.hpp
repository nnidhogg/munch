#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_BLOCK_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_BLOCK_HPP

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "munch/regex/parse.hpp"
#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/cursor.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/lexer_spec.hpp"
#include "munch/tools/audit/re2c_actions.hpp"
#include "munch/tools/audit/re2c_classes.hpp"
#include "munch/tools/audit/re2c_configuration.hpp"
#include "munch/tools/audit/re2c_regex.hpp"
#include "munch/tools/audit/read_re2c.hpp"

/**
 * @brief One re2c block read item by item, Block_reader, with where the blocks a later one may use begin, Library_t,
 *        where a definition a later block may use stands, Definition_site, what the blocks of one pass share,
 *        Pass_state, and the kinds of rule a scanner holds, Rule_kinds, with the patterns of the end rule and of the
 *        default rule, end_rule and default_rule, and the bound re2c holds `re2c:eof` to, refuse_eof_past_code_unit().
 *
 * A block's close is found as re2c finds it, its regexes are read by Regex_reader and its configurations applied by
 * configure(); what a block leaves for the blocks after it, its flags, its pointer names and its definitions, is kept
 * as offsets and settings rather than as what this block made of them, since re2c compiles a definition and a used
 * block again at every point of use, under the configuration in force there.
 */
namespace munch::tools::audit
{
/**
 * @brief The pattern of the end rule, which matches at the end of the input and no byte.
 */
constexpr std::string_view end_rule{"$"};

/**
 * @brief The pattern of the default rule, which matches one code unit where no other rule matches.
 */
constexpr std::string_view default_rule{"*"};

/**
 * @brief Where each block a later one may use begins, by name: a `rules:re2c:name` block or any other block opened with
 *        a name, kept as the offset just past its opener.
 *
 * An offset is all a use needs, and more faithful than the reading itself would be: re2c compiles a rules block's
 * regexes at every point of use, under the configurations in force there, so a `!use:name;` and a use block read the
 * block's source again rather than copy what it was read as here. The same block can then be one scanner's under one
 * encoding and another's under another, which is what re2c's own multiple-encoding example does.
 */
using Library_t = std::map<std::string, std::size_t, std::less<>>;

/**
 * @brief One definition a later block may use: its name and where its regex stands.
 *
 * An offset is all a later block needs, and more faithful than the expression this one translated: re2c compiles a
 * definition's regex at every point of use, under the configuration in force there, so a definition written where the
 * encoding was ASCII is a whole code point in a block that turns UTF-8 on, and one written under the flex syntax is
 * read under it again wherever it is used.
 */
struct Definition_site
{
    /**
     * @brief The name the definition binds.
     */
    std::string name{};

    /**
     * @brief The offset its regex begins at, just past the `=` or past the name under the flex syntax.
     */
    std::size_t begin{};

    /**
     * @brief Whether the regex ends at the line's end, which a flex-style definition's does.
     */
    bool line_bound{};
};

/**
 * @brief What the blocks read in one pass share and change: the classes among the definitions in force, the names the
 *        configurations give the scan pointers and the API they leave the scanner under.
 *
 * Each pass begins it from what the blocks above left, since each pass rereads its block alone, and a used block
 * changes it as the block using it does, since re2c carries a configuration from a used block into the block using it.
 */
struct Pass_state
{
    /**
     * @brief The classes among the definitions in force, which a block's definitions join and its class differences
     *        take their operands from.
     */
    Classes_t classes{};

    /**
     * @brief The names the configurations give the scan pointers, re2c's own until one is renamed.
     */
    Pointers_t pointers{};

    /**
     * @brief The line of the configuration that left the scanner under an API other than the default one, none while it
     *        reads under the default: the last assignment governs, as for every configuration, and the setting carries
     *        from a block to the blocks after it and from a used block into the block using it, as re2c carries it, so
     *        it is the file's and not one block's, like the pointer names.
     */
    std::optional<std::size_t> api_custom{};
};

/**
 * @brief Refuses the configurations when the last `re2c:eof` setting among them passes 255, the largest code unit, in
 *        re2c's words: re2c 3.1 holds the value to the code unit at the end of every block, a rules block's among them,
 *        and at each `!use:` directive, once the used block's configurations stand.
 * @param options The configurations, as options, each `re2c:eof` value read as a number already.
 * @param line The line the refusal points at.
 * @throws Spec_error If the value passes 255.
 */
void refuse_eof_past_code_unit(const std::vector<std::string>& options, std::size_t line);

/**
 * @brief The kinds of rule a scanner's blocks hold and the names the rules stand in, which re2c holds a scanner to once
 *        it is read: rules naming conditions and rules naming none never together, and an end rule `$` only where the
 *        file's configuration and the other rules make sense of it.
 */
class Rule_kinds
{
public:
    /**
     * @brief Notes which kind a rule is, naming conditions or naming none, and which names it stands in, for re2c's own
     *        checks once the block is read.
     *
     * re2c 3.1 compiles a block's rules with conditions or without, never both: a rule naming a condition, `<*>`
     * included, is one kind and a rule naming none, the empty rule and the default rule among them, the other, and a
     * block holding both is refused once it is read, at the first rule naming none; the end rule `$` is the one rule
     * naming none that re2c counts otherwise, alone beside conditions it is refused in its own words. A used block's
     * rules count with the using block's own, where the directive stands.
     * @param named The conditions the rule names.
     * @param pattern The regex as written.
     * @param line The rule's line.
     */
    void note(const std::vector<std::string>& named, const std::string& pattern, std::size_t line);

    /**
     * @brief Counts a used block's rules with these, where its directive stands: they are of the kinds they are
     *        wherever they stand, the first of a kind the using block's own if it read one before the directive.
     * @param used The used block's kinds.
     */
    void merge(const Rule_kinds& used);

    /**
     * @brief Refuses the scanner as re2c 3.1 refuses one whose rules are of both kinds, ones naming a condition, `<*>`
     *        included, and ones naming none, the rules its `!use:` directives brought in counted with its own; for a
     *        scanner, since a rules block is compiled where it is used and not on its own.
     * @throws Spec_error If rules of both kinds were read, at the first naming none, in re2c's words; or if the only
     *         rule naming none is the end rule `$`, which re2c refuses in words of its own, at that rule.
     */
    void refuse_mixed() const;

    /**
     * @brief Refuses a scanner that fails re2c's own checks of the end rule, in its words: an end rule whose name has
     *        no other rule, `<*>` counted as a name of its own, "doesn't make sense"; an end rule needs `re2c:eof` set;
     *        and `re2c:eof` set needs an end rule in every condition, its own or under `<*>`, and in a block naming
     *        none one end rule.
     *
     * re2c holds a block of its own to them once it is read, its rules all tokens or none, and a rules block only where
     * a use block takes it up, its rules and end rules counted with the using block's, since re2c reads a rules block
     * as a library and holds none of these checks against it until then, only `re2c:eof`'s bound at its end,
     * refuse_eof_past_code_unit(); a block holding no rule at all is held to none of them.
     * @param options The scanner's configurations, as options.
     * @param line The scanner's line, where a check naming no end rule points.
     * @throws Spec_error If one of the checks fails, at the end rule's line or the scanner's; or as
     *         refuse_eof_past_code_unit() does, at the scanner's line. The last value stands, and a negative one leaves
     *         `re2c:eof` unset, as re2c reads every one.
     */
    void refuse_end_rules(const std::vector<std::string>& options, std::size_t line) const;

    /**
     * @brief Returns the conditions the rules name, `*` aside, in the order first named, which are the scanner's
     *        conditions.
     * @return The names.
     */
    [[nodiscard]] const std::vector<std::string>& named() const noexcept;

private:
    /**
     * @brief An end rule `$` a block holds: a name it stands in, the empty name for one naming no condition, and its
     *        line.
     */
    struct End_rule
    {
        /**
         * @brief The name, `*` for one under `<*>` and empty for one naming no condition.
         */
        std::string name{};

        /**
         * @brief The rule's line.
         */
        std::size_t line{};
    };

    /**
     * @brief Whether a rule naming a condition, `<*>` included, was read, the used blocks' rules counted.
     */
    bool conditioned_{false};

    /**
     * @brief The conditions the block's rules name, `*` aside, in the order first named, the end rule and the empty
     *        rule counted with the rest and the used blocks' rules too: re2c compiles a condition for every name a rule
     *        carries, whether or not that rule is a token.
     */
    std::vector<std::string> named_;

    /**
     * @brief The condition names, `*` and the empty name for a rule naming none included, that some rule other than the
     *        end rule stands in, the used blocks' rules counted: re2c refuses an end rule whose name has no other rule,
     *        in its words.
     */
    std::vector<std::string> ruled_;

    /**
     * @brief The end rules `$` read, each name they stand in with the rule's line, the empty name for one naming no
     *        condition; re2c holds them against `re2c:eof` once the block is read.
     */
    std::vector<End_rule> ends_;

    /**
     * @brief The line of the first rule naming no condition, the end rule aside, the used blocks' rules counted.
     */
    std::optional<std::size_t> plain_;

    /**
     * @brief The line of the first end rule `$` naming no condition, the used blocks' rules counted.
     */
    std::optional<std::size_t> plain_end_;
};

/**
 * @brief A cursor over one re2c block, from just past its opener to the comment close that ends it.
 *
 * The block is read item by item: a configuration, a definition or a rule, each ending where re2c's own grammar ends
 * it, and the regex text of a definition or a rule is rewritten for the pattern parser as it is read. The close is
 * found the way re2c finds it, as the first star-slash between items: one inside a quoted literal, a class, an action
 * or a comment is content, since the file is read by re2c and not by a C compiler. Definitions come in two spellings,
 * re2c's `name = regex;` and the flex one, a name followed by a blank and regex to the end of the line, which re2c
 * accepts with its flex-syntax flag wherever the name stands and which a name opening an item, followed by a blank and
 * then something other than a brace, identifies.
 */
class Block_reader : public Cursor
{
public:
    /**
     * @brief Binds the cursor to the source just past a block's opener.
     * @param source The whole file.
     * @param begin The offset just past the opener.
     * @param reading The flags every pattern of the block is translated under, which is the configuration the whole
     *        block leaves once the reading has settled on it.
     * @param configured The flags the configurations have left so far, the same as `reading` at a block's head and the
     *        using block's where a used block is read at its `!use:` directive.
     * @param pass What the blocks read in the same pass share, which the block's definitions and configurations change.
     * @param macros The macros the C around the blocks defines.
     */
    Block_reader(
            std::string_view source, std::size_t begin, Re2c_flags reading, Re2c_flags configured, Pass_state& pass,
            const Macros_t& macros);

    /**
     * @brief Leaves this block's actions and API to the block that uses it: a rules library is compiled where it is
     *        used, under the using block's configurations, and is judged nowhere else.
     */
    void judge_later() noexcept;

    /**
     * @brief Reads a rules block into the specification as used here, a `!use:name;` directive or a use block's opener:
     *        the used block is read again at this point, under the flags this block's patterns are read under, so that
     *        its rules are the rules this block compiles rather than the ones its own block was read as; its
     *        configurations stand where the directive does, and the ones after it may override them in turn; its
     *        default rules are noted, since the block's own override them once the block is read.
     * @param begin The offset just past the used block's opener.
     * @param spec The specification being filled.
     * @param library The named blocks read so far, which the used block's own `!use:name;` items may name.
     * @param returning The forms besides `return` an action returns a token through.
     * @throws Spec_error As read() does for the used block.
     */
    void use(std::size_t begin, Lexer_spec& spec, const Library_t& library, const Returning_t& returning);

    /**
     * @brief Reads every item of the block into the specification, through the block's close.
     * @param spec The specification being filled.
     * @param library The named blocks read so far, which a `!use:name;` item merges into the specification.
     * @param returning The forms besides `return` an action returns a token through.
     * @return The offset just past the close.
     * @throws Spec_error If an item is malformed or left open, a used block is unknown, or the block never closes.
     */
    [[nodiscard]] std::size_t read(Lexer_spec& spec, const Library_t& library, const Returning_t& returning);

    /**
     * @brief Returns the kinds of rule the block holds and the names they stand in, the used blocks' rules counted.
     * @return The kinds.
     */
    [[nodiscard]] const Rule_kinds& kinds() const noexcept;

    /**
     * @brief Returns the flags the block's configurations and the evidence of the flex syntax have left, which the next
     *        pass translates its patterns under and the next block inherits.
     * @return The flags.
     */
    [[nodiscard]] Re2c_flags configured() const noexcept;

    /**
     * @brief Returns the line of the configuration that last set the block's encoding, where a refusal of the encoding
     *        the configurations leave points; the block's opening line while none has set it.
     * @return The line.
     */
    [[nodiscard]] std::size_t encoding_line() const noexcept;

    /**
     * @brief Returns where each definition the block declared stands, in the order it declared them, the ones a `!use:`
     *        directive brought in included.
     * @return The sites.
     */
    [[nodiscard]] const std::vector<Definition_site>& sites() const noexcept;

    /**
     * @brief Returns the first refusal this pass's flags decided and the settled flags may not: a class difference left
     *        empty, which `[^] \ [\x00-\xff]` is under ASCII and is not under UTF-8, held rather than thrown, since the
     *        pass reading under the flags the block leaves is the one that answers it.
     * @return The refusal, at its rule's line, or std::nullopt when the pass decided none.
     */
    [[nodiscard]] const std::optional<Spec_error>& deferred() const noexcept;

private:
    /**
     * @brief Reads a `re2c:` configuration through its `;` into the options, a flag among them into the flags the
     *        configurations leave, never into the ones the patterns of this pass are read under.
     *
     * The configuration is read as re2c 3.1's configuration lexer reads it, the escapes inside a quoted value aside,
     * which are not checked: a name running as far as letters, digits, `_`, `:` and `-` do, or further where a name
     * re2c knows with an `@` in it stands, which must be one re2c knows, then `=` and a value of the kind the name
     * takes, with spaces and tabs around the `=` and the value and nothing else, and `;`. The option keeps the name,
     * `=` and the value, a number in decimal and anything else as written.
     * @param spec The specification being filled.
     * @throws Spec_error If re2c refuses the configuration, at its line and in its words: a name it does not know, a
     *         missing `=` or `;`, or a value that is not of the name's kind; or as configure() does.
     */
    void configuration(Lexer_spec& spec);

    /**
     * @brief Reads a configuration's value at the cursor as re2c 3.1 reads a value of its kind, and leaves the cursor
     *        where the value ends.
     * @param syntax What the value is read as.
     * @return The value, a number in decimal and anything else as written.
     * @throws Spec_error If the value is not of its kind, in re2c's words, as choice_value(), string_value() and
     *         number_value() refuse it, or a number is negative where the configuration takes none.
     */
    [[nodiscard]] std::string configuration_value(const Configuration_syntax& syntax);

    /**
     * @brief Reads one of the words a configuration chooses among at the cursor.
     * @param choices The words.
     * @return The word.
     * @throws Spec_error If none of them stands there, the words listed in re2c's words.
     */
    [[nodiscard]] std::string choice_value(std::span<const std::string_view> choices);

    /**
     * @brief Reads a string value at the cursor as re2c 3.1 reads one: quoted, through the quote closing it, or bare,
     *        through the bytes Configuration_value::string admits, or nothing.
     * @return The value as written, its quotes included.
     * @throws Spec_error If a quoted value meets a newline before its closing quote, escaped or not, in re2c's
     *         words, or the text ends before it.
     */
    [[nodiscard]] std::string string_value();

    /**
     * @brief Reads a number at the cursor as re2c 3.1 reads one, Configuration_value::number.
     * @return The number.
     * @throws Spec_error If no number stands there or it overflows, in re2c's words.
     */
    [[nodiscard]] int number_value();

    /**
     * @brief Reads a `!use:name;` directive, the named block read into the specification where the directive stands.
     * @param spec The specification being filled.
     * @param library The named blocks read so far.
     * @throws Spec_error If the directive is malformed or names no block above this one, or as use() does; or as
     *         refuse_eof_past_code_unit() does, at the directive's line, once the used block's configurations stand.
     */
    void use_directive(Lexer_spec& spec, const Library_t& library);

    /**
     * @brief Reads one item that is no configuration and no directive: a definition in either spelling, a rule, or an
     *        entry or setup rule, which is no token.
     * @param spec The specification being filled.
     * @throws Spec_error If the item is malformed or refused.
     */
    void item(Lexer_spec& spec);

    /**
     * @brief Returns whether the condition list after its `<` opens a `<!...>` setup rule, which is no token, its `!`
     *        standing after blanks or none.
     * @return True when it does.
     */
    [[nodiscard]] bool opens_setup() const noexcept;

    /**
     * @brief Reads a `<...>` condition list after its `<`, through its `>`, a setup rule's `!` dropped.
     * @return The names, `*` for all.
     * @throws Spec_error If the list is never closed.
     */
    [[nodiscard]] std::vector<std::string> conditions();

    /**
     * @brief Reads a flex-style definition when one stands at the cursor, as re2c 3.1 reads one under its flex syntax:
     *        a name followed by a blank and then regex, wherever the name stands, at the line's start, indented or
     *        after an item on the same line, with the regex running to the end of the line; a `{` after the blanks
     *        opens an action or a reference instead and leaves the name a rule's literal.
     *
     * The definition is read as a trial, bound to the line. An action turning up on the line after all is refused,
     * since the definition re2c opened there admits none and re2c answers it with a syntax error. Without the flex flag
     * the trial is made only for a name no definition has, since a defined one opening a rule is normal syntax and an
     * undefined one followed by a blank is nothing else, an undefined symbol to re2c without the flex syntax and a
     * definition under it.
     * @param spec The specification being filled.
     * @return Whether a definition was read; the cursor is where it was when none was.
     * @throws Spec_error If the regex runs on past its line into an action, or as regex_text() does.
     */
    [[nodiscard]] bool take_flex_definition(Lexer_spec& spec);

    /**
     * @brief Reads regex text at the cursor as a Regex_reader reads it, under the flags every pattern of the block is
     *        translated under, and leaves the cursor where the regex ends.
     * @param definitions The definitions read so far, which a difference's operand may name.
     * @param line_bound Whether the regex ends at the line's end, which a flex-style definition's does.
     * @return The pattern as written, its expression, both empty when an action follows at once, and its class.
     * @throws Spec_error As Regex_reader::regex_text() does.
     */
    [[nodiscard]] Regex_text regex_text(const regex::Definitions_t& definitions, bool line_bound);

    /**
     * @brief Reads re2c's own definition when the name read is followed by its `=`: a name, `=`, its body and `;`.
     * @param name The name, read as regex text, one bare name.
     * @param spec The specification being filled.
     * @return Whether a definition was read.
     * @throws Spec_error If the body is empty or not closed with `;`, or as regex_text() does.
     */
    [[nodiscard]] bool take_definition(const std::string& name, Lexer_spec& spec);

    /**
     * @brief Reads an action starting at the cursor: a brace block, or `:=` and the rest of the line, a `=> c` or `:=>
     *        c` transition included in the text.
     * @return The action's text.
     * @throws Spec_error If a brace block never closes.
     */
    [[nodiscard]] std::string action();

    /**
     * @brief Keeps the code of the entry rule `<>` or of a `<!c>` setup rule, whose code re2c writes into the actions
     *        it runs before, for the check of the block's actions.
     *
     * A scan pointer the code moves is so moved in those actions, leaving the next token beginning somewhere other than
     * where a match ended, exactly as a rule's own action moving it would. The pointer's name is the block's, which the
     * block settles, so the check waits for the block's last configuration, and so does the check of how each action a
     * setup rule's code heads leaves, refuse_action(), since a rule after the setup rule may stand in its conditions.
     * @param code The code.
     * @param line The rule's line.
     * @param entry Whether it is the entry rule.
     * @param conditions The conditions a setup rule sets up, `*` for all, none for the entry rule.
     * @throws Spec_error If the code returns, which would return in its condition before any rule's own action.
     */
    void setup_action(std::string code, std::size_t line, bool entry, std::vector<std::string> conditions);

    /**
     * @brief Adds a rule to the specification, the end rule and the empty rule aside, which are no tokens, and keeps
     *        its action for the check of the block's actions.
     *
     * The default rule `*` is a rule like the others as far as its action goes: re2c 3.1 runs it where no other rule
     * matches, over one code unit, which is one byte under every encoding the reading follows, UTF-8 included, where
     * `[^]` is a whole code point and `*` a byte of one. Its priority is the lowest wherever in the block it stands,
     * which read_re2c() settles once the block's rules are all read.
     * @param spec The specification being filled.
     * @param named The conditions the rule names, none for a rule naming none.
     * @param pattern The regex as written.
     * @param expression The regex rewritten for the pattern parser.
     * @param code The rule's action.
     * @param line The rule's line.
     */
    void rule(
            Lexer_spec& spec, std::vector<std::string> named, std::string pattern, std::string expression,
            std::string code, std::size_t line);

    /**
     * @brief Refuses the block's actions when its configurations leave the scanner under the custom API, and every
     *        action that moves a scan pointer or is otherwise refused, the pointers named as the block's own
     *        configurations name them.
     *
     * re2c applies a configuration to the whole block it stands in, so one written after the rules renames the pointer
     * for them too, and the check waits until the block is read rather than asking what a rule stood under. A rule's
     * own action, a setup rule's and the entry rule's are all code re2c runs at a match.
     * @throws Spec_error If the block sets the custom API, at that configuration's line, or an action is refused, at
     *         that action's line.
     */
    void refuse_actions();

    /**
     * @brief The offset just past the block's opener, before which a label restarts the scan and after which it leaves
     *        it.
     */
    std::size_t opener_;

    /**
     * @brief The flags every pattern of the block is translated under, the same for the whole pass except that the
     *        evidence of a flex-style definition turns that flag on for the rest of the block.
     */
    Re2c_flags reading_;

    /**
     * @brief The flags the configurations read so far have left, which the next pass reads the block under.
     */
    Re2c_flags configured_;

    /**
     * @brief What the blocks read in this pass share: the classes, the pointer names and the API.
     */
    Pass_state& pass_;

    /**
     * @brief The macros the C around the blocks defines, which an action may call: a call that moves the pointer or
     *        returns through one is out of sight, so such a call is refused by the macro's name.
     */
    const Macros_t& macros_;

    /**
     * @brief Every action of this block, kept as Action says until the block's configurations are all read.
     */
    std::vector<Action> actions_;

    /**
     * @brief Whether this block's actions are the using block's to refuse, which a block read through a `!use:`
     *        directive leaves to the block using it.
     */
    bool judged_later_{false};

    /**
     * @brief The forms besides `return` an action returns a token through, as read() was given them, for the actions
     *        judged once the block's configurations are all read.
     */
    Returning_t returning_;

    /**
     * @brief Where each definition the block declared stands, for the blocks that use them.
     */
    std::vector<Definition_site> sites_;

    /**
     * @brief The indices into the specification's rules of the default rules the `!use:` directives brought in,
     *        ascending, which the block's own default rules override once the block is read.
     */
    std::vector<std::size_t> used_defaults_;

    /**
     * @brief The kinds of rule the block holds and the names they stand in, the used blocks' rules counted.
     */
    Rule_kinds kinds_;

    /**
     * @brief The first refusal held for the settled flags, the used blocks' counted; see deferred().
     */
    std::optional<Spec_error> deferred_;

    /**
     * @brief The line of the configuration that last set the encoding, for the refusal of one the reading has not got;
     *        the block's opening line while none has.
     */
    std::size_t encoding_line_;
};

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_BLOCK_HPP

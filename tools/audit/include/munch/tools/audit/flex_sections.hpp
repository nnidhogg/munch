#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_SECTIONS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_SECTIONS_HPP

#include <cstddef>
#include <vector>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/flex_actions.hpp"
#include "munch/tools/audit/flex_lines.hpp"
#include "munch/tools/audit/flex_options.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief The two sections of a flex file read as flex reads them: the definitions section, read_definitions(), and the
 *        rules section, read_rules(), with what flex does to the rules once the section is read, share_actions() and
 *        add_default_rule().
 *
 * The sections share one Lines cursor, the definitions section leaving it past its `%%` and the rules section taking it
 * there. Each takes the macros the code it copies out defines and records that code as Copied, so that the hook is read
 * once every stretch is known; a rule's action is read to the end flex's own action scanner reads it to, which is not
 * where C would end it.
 */
namespace munch::tools::audit
{
/**
 * @brief Reads the definitions section, up to and over its `%%`, whose line names the scanner.
 * @param lines The cursor, at the file's first line.
 * @param spec The specification being filled.
 * @param settings The settings the `%option` lines resolve to, filled as they are read.
 * @param macros The macros the section's code defines, filled as it is read.
 * @param copied The stretches of code the section copies into the scanner, filled as they are read.
 * @throws Spec_error If the section never ends, a block is left open, or a definition has no pattern.
 */
void read_definitions(
        Lines& lines, Lexer_spec& spec, Settings& settings, Macros_t& macros, std::vector<Copied>& copied);

/**
 * @brief Reads the rules section, up to its `%%` or the end of the file.
 * @param lines The cursor, at the section's first line.
 * @param spec The specification being filled.
 * @param returning The forms besides `return` an action returns a token through.
 * @param macros The macros the section's code blocks define, added to.
 * @param copied The stretches of code the section copies into the scanner, added to.
 * @return The line the section ends on: its `%%`, or the file's last line when the file ends first.
 * @throws Spec_error If a rule is malformed, a comment stands at the margin, which flex refuses as an unrecognized
 *         rule, or a start-condition scope is never closed, which flex refuses as a parse error at the section's end.
 */
[[nodiscard]] std::size_t read_rules(
        Lines& lines, Lexer_spec& spec, const Returning_t& returning, Macros_t& macros, std::vector<Copied>& copied);

/**
 * @brief Gives every `|` action the token of the first rule below it that has an action of its own; flex takes a
 *        `|` and whatever follows it on the line, a comment usually, as that continuation. An `<<EOF>>` rule's action
 *        was read unchecked, since it runs where no match is; shared, it runs on the `|` rule's match, `a |` over
 *        `<<EOF>> { yymore(); }` returning one token for "ab" in flex 2.6.4, so it is checked here as any action is,
 *        at the line of the rule that runs it.
 * @param rules The rules, in file order.
 * @throws Spec_error If a `|` rule shares an `<<EOF>>` action that moves the match.
 */
void share_actions(std::vector<Lexer_spec::Rule>& rules);

/**
 * @brief Appends the default rule flex adds after the file's own once the section is read: one byte, `.|\n`, in
 *        every start condition and at the lowest priority, whose action `ECHO;` returns nothing, so that a byte no
 *        rule of the file's matches is a one-byte discarded token wherever the scanner stands.
 * @param spec The specification, its own rules read.
 * @param line The line the rules section ends on, which the rule is given, standing on none of its own.
 */
void add_default_rule(Lexer_spec& spec, std::size_t line);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_SECTIONS_HPP

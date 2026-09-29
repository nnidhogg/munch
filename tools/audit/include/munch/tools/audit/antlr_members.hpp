#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_MEMBERS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_MEMBERS_HPP

#include <cstddef>
#include <string_view>
#include <vector>

#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief What the lexer class ANTLR generates holds beyond the rules, refuse_lexer_class(): a superclass, and the
 *        `@members` code, Members_action, read as the target language reads it, refusing what stands in place of the
 *        runtime's own methods or runs when the lexer is built.
 */
namespace munch::tools::audit
{
/**
 * @brief One action whose code ANTLR writes into the lexer class it generates: its code and the line it opens on.
 */
struct Members_action
{
    /**
     * @brief The code, its braces included.
     */
    std::string_view code{};

    /**
     * @brief The line the code opens on, which a refusal of it names.
     */
    std::size_t line{};
};

/**
 * @brief Refuses a grammar whose lexer class holds more than its rules decide: a superclass the grammar names, members
 *        written for a target whose declarations this reading does not read, and members that stand in place of the
 *        runtime's own methods or run when the lexer is built.
 *
 * ANTLR writes the members into the lexer class it generates for the target language, and the reading reads the
 * declarations of three targets, Java, C++ and C#, which all write a method as a name, a parameter list and a body.
 * Another target writes one its own way, JavaScript assigning a function to an instance member among them, which is no
 * declaration this reading would find at all, so a grammar naming one is refused by name rather than read as though its
 * members declared nothing.
 * @param spec The scanner read, whose options name the target language and the superclass and whose line is the
 *        declaration's.
 * @param members The actions written into the lexer class, `members` and the C++ target's `declarations`.
 * @param actions_code The code of every named action, `header` among them, whose macros the members may use.
 * @throws Spec_error If the grammar sets superClass, its target is none of Java, C++ and C# while it has members, or
 *         the members define a method, an initializer block, an initializer that is more than a value or an accessor
 *         with a body.
 */
void refuse_lexer_class(
        const Lexer_spec& spec, const std::vector<Members_action>& members, std::string_view actions_code);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ANTLR_MEMBERS_HPP

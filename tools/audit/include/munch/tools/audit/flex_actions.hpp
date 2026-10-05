#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_ACTIONS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_ACTIONS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

/**
 * @brief What a flex action or hook does with the match, as far as its text decides: the stretches of code flex copies
 *        ahead of every action, Copied; the calls that move the bounds of a match or rerun it, stateful_use(); a
 *        `YY_USER_ACTION` that does what an action may not, User_action_use and user_action_use(); and the rest of what
 *        an action is refused for, refuse_action().
 *
 * The calls are refused by name, each with what it does to the tokens, since the token language has no place for a
 * match moved or rerun. An action is read as c_tokens() reads it, so a comment or a literal holds none of them, and the
 * macros the file defines are read for what they could put in an action out of the text's sight.
 */
namespace munch::tools::audit
{
/**
 * @brief A stretch of code flex copies into the scanner ahead of every action: a definitions block, an indented
 *        definitions line, the rules section's prologue or one of its code blocks. Every such stretch is read for the
 *        hook once the whole file's macros are known, since the generated scanner expands the hook under all of them,
 *        wherever they were defined.
 */
struct Copied
{
    /**
     * @brief The code.
     */
    std::string_view code{};

    /**
     * @brief The line the stretch begins at.
     */
    std::size_t first{};

    /**
     * @brief Which kind of stretch it is, as a refusal names it.
     */
    std::string_view what{};

    /**
     * @brief The path the stretch was read from as the include reader spells it, empty for the file audited, which the
     *        stretch's own includes are resolved beside.
     */
    std::string_view path{};
};

/**
 * @brief A `YY_USER_ACTION` that does what an action may not, as user_action_use() finds it.
 */
struct User_action_use
{
    /**
     * @brief What the refusal says after "the action".
     */
    std::string why{};

    /**
     * @brief How many lines into the stretch the directive stands.
     */
    std::size_t lines_in{};
};

/**
 * @brief Returns the first use in an action of one of flex's calls that move the bounds of the match or rerun it,
 *        `yymore()`, `REJECT`, `yyless()`, `unput()` and `input()` or `yyinput()`, which flex's C++ scanners name it,
 *        as the refusal states it: what the action does and what that does to the tokens.
 *
 * The action is read as c_tokens() reads it, a comment or a literal holding none of them. A call is a word whose next
 * token is a parenthesis, whatever blanks or comments stand between; the word standing otherwise, called through
 * parentheses, `(input)()`, taken as a pointer or declared anew, is named apart from a call, and what the action does
 * with it is out of sight, so it is refused as a use the reading cannot follow. A word whose previous token is `.` or
 * `->` is a member, a field or a function of the file's own named `input`, and none of them when read and not called,
 * `yylval.input = 1`. A member of `this`, `this->yyinput()` and `(*this).yyinput()`, is the word itself, the call as
 * flex's C++ scanners, whose `yyinput` is a member of the lexer class, write it. A member of the name on anything else,
 * called or parenthesised to be called, `(self->yyinput)()`, is the call too: an alias of `this`, `auto* self = this;
 * self->yyinput();`, is the same call under a name the text does not resolve, so `self->yyinput()` and `s.input()`
 * alike are refused by name. `REJECT` is a whole word in flex's spelling, capitals throughout, since flex takes
 * `reject` and `Reject` as names of the file's own. `BEGIN`, `yy_push_state`, `yy_pop_state` and `yy_top_state` change
 * the start condition and nothing else, which a token set per condition already stands for, so they are no concern
 * here.
 * @param action The action's text.
 * @param injecting_only Whether to name only the calls that push a byte onto the input or take one from it, which is
 *        what an end-of-input action, having no match to move, can still do.
 * @return What the refusal says after "the action", or std::nullopt when the action holds no such use.
 */
[[nodiscard]] std::optional<std::string> stateful_use(std::string_view action, bool injecting_only);

/**
 * @brief Returns the first match-moving call in a `YY_USER_ACTION` the definitions section's code defines, which flex
 *        runs before every rule's own action, so a call in it is a call in every action: under `#define YY_USER_ACTION
 *        input();` flex 2.6.4 takes "aab" through `a+` and `b` as the one token 7, the b consumed by the hook.
 *
 * The directive is found as C reads it, token by token as c_tokens() reads them, so a comment between its words and a
 * line splice inside one are no hiding place, and its replacement runs to the end of the line and on past a backslash
 * before the newline; a comment naming the macro defines nothing.
 * @param code A stretch of the definitions section's C.
 * @param macros The macros the file defines, whose opaque ones the hook may not use.
 * @param returning The forms besides `return` an action returns a token through, which the hook may not hold.
 * @return The use, or std::nullopt when no such hook is defined.
 */
[[nodiscard]] std::optional<User_action_use> user_action_use(
        std::string_view code, const Macros_t& macros, const Returning_t& returning);

/**
 * @brief Refuses a rule whose action holds what the reading cannot follow: a preprocessor directive, a return the text
 *        does not decide, flex's own `YY_BREAK`, or a use of a macro that could hide a call. A `|` action is the next
 *        rule's and is read there.
 * @param rule The rule.
 * @param macros The macros the file and the files it includes define.
 * @param returning The forms besides `return` an action returns a token through.
 * @throws Spec_error If the action is refused.
 */
void refuse_action(const Lexer_spec::Rule& rule, const Macros_t& macros, const Returning_t& returning);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_ACTIONS_HPP

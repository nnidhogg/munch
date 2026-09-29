#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ACTION_RETURN_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ACTION_RETURN_HPP

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief What a C action returns and how it leaves, as far as its text decides: returned(), the expression an action
 *        returns, and returns_undecided(), why the token it returns is out of sight, both over the names a scanner
 *        returns through besides `return`, Returning_t.
 *
 * Both read the action as c_tokens() reads it, with the bodies of the classes and the lambdas it declares left out,
 * since a `return` inside one returns from it and not from the action. The flex and re2c readers ask them of every
 * rule's action, and where the two generators differ, in what a `break`, a `goto` and the action's end do,
 * returns_undecided() is told which reading holds.
 */
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

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_ACTION_RETURN_HPP

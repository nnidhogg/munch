#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_ACTIONS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_ACTIONS_HPP

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/directives.hpp"
#include "munch/tools/audit/re2c_configuration.hpp"

/**
 * @brief What a re2c action does with the match, as far as its text decides: whether it leaves a scan pointer elsewhere
 *        than where the match ended, under the names the block's configurations give the pointers, whether it returns
 *        on every path or leaves by a jump the scan restarts at, restart_labels(), and what else it is refused for,
 *        refuse_action().
 *
 * An action is judged only once its block is read, since re2c applies a configuration to the whole block it stands in
 * and what the actions call the scan pointers is the block's last word on them; so a block keeps its actions as Action
 * until then. An action is read as c_tokens() reads it, a comment or a literal holding nothing, and a pointer spelled
 * through a macro, an index or parentheses is compared in one spelling with the configured one.
 */
namespace munch::tools::audit
{
/**
 * @brief An action of a block, kept until the block's configurations are all read: re2c applies a configuration to the
 *        whole block it stands in, wherever in the block it is written, so what the actions call the scan pointers is
 *        settled by the block and not by what stood above a rule.
 */
struct Action
{
    /**
     * @brief The action's code, a transition or a shortcut included as the rule wrote it.
     */
    std::string code{};

    /**
     * @brief The line to report the action at, its rule's.
     */
    std::size_t line{};

    /**
     * @brief How a refusal names the action: "the action ", "a setup rule moves " or "the entry rule <> moves ".
     */
    std::string what{};

    /**
     * @brief Whether the action is a rule's own, which falls into the next rule's unless it leaves, rather than a setup
     *        rule's or the entry rule's.
     */
    bool of_rule{};
};

/**
 * @brief Returns the labels the file's C code declares outside every block and before one, which a `goto` in an action
 *        restarts the scan by.
 *
 * A label is a name and a colon opening a statement, `loop:`; `case` and `default` labels and a `::` are none. A label
 * after the block, `done:` past the scanning loop, is left for, not restarted at; and a label before it restarts the
 * scan only where the code from the label to the block's opener runs into the block on every path, holding no return,
 * no jump and no other label, since `emit_token: return 9;` and `loop: if (finished) return 0;` before the block leave
 * the scan where a `goto` reaches them.
 * @param source The whole file.
 * @param opener The offset just past the block's opener, before which a label restarts the scan and after which it
 *        leaves it.
 * @param returning The forms besides `return` an action returns a token through, which leave the scan like a return.
 * @return The labels.
 */
[[nodiscard]] std::set<std::string, std::less<>> restart_labels(
        std::string_view source, std::size_t opener, const Returning_t& returning);

/**
 * @brief Refuses an action that moves a scan pointer, holds a directive, returns on some paths only or uses a macro
 *        that could hide what it does, the pointers named as the block's own configurations name them.
 * @param action The action.
 * @param pointers The names the block's configurations give the scan pointers.
 * @param macros The macros the C around the blocks defines, which an action may call.
 * @param returning The forms besides `return` an action returns a token through.
 * @param restarts The labels a `goto` in the action restarts the scan by.
 * @throws Spec_error If the action is refused, at its line.
 */
void refuse_action(
        const Action& action, const Pointers_t& pointers, const Macros_t& macros, const Returning_t& returning,
        const std::set<std::string, std::less<>>& restarts);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_ACTIONS_HPP

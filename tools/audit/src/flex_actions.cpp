#include "munch/tools/audit/flex_actions.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief One of flex's calls that moves the bounds of a match or reruns it, which the token language has no place for,
 *        and what it does.
 */
struct Stateful
{
    /**
     * @brief The name, `REJECT` in flex's own spelling and the rest as the macros are named.
     */
    std::string_view name{};

    /**
     * @brief Whether it is written as a call, a parenthesis after the name; `REJECT` stands alone.
     */
    bool call{};

    /**
     * @brief What it does to the tokens, for the refusal.
     */
    std::string_view does{};
};

/**
 * @brief The macro flex writes after every action, `break;` unless the file defines it.
 */
constexpr std::string_view break_macro{"YY_BREAK"};

/**
 * @brief flex's calls that move the bounds of a match or rerun it: what each does is what the refusal says.
 */
constexpr std::array stateful{
        Stateful{
                .name = "REJECT",
                .call = false,
                .does = "drops the match for the next rule's, so which rule matches is not the rules' longest match "
                        "and first rule"},
        Stateful{
                .name = "yymore",
                .call = true,
                .does = "appends the next match to this one, so the next token begins where this match did"},
        Stateful{
                .name = "yyless",
                .call = true,
                .does = "gives the end of the match back to be matched again, so the next token begins inside this "
                        "match"},
        Stateful{
                .name = "unput",
                .call = true,
                .does = "pushes a byte onto the input, so the next token is matched against bytes the input may not "
                        "hold"},
        Stateful{
                .name = "input",
                .call = true,
                .does = "consumes bytes no rule matched, so the next token begins past them"},
        Stateful{
                .name = "yyinput",
                .call = true,
                .does = "consumes bytes no rule matched, so the next token begins past them"},
        Stateful{
                .name = "yyterminate",
                .call = true,
                .does = "ends the scan with no token, so whether a match emits a token is out of sight"},
        Stateful{
                .name = "YY_FLUSH_BUFFER",
                .call = false,
                .does = "discards the buffer's remaining input, so the next token is not matched against the "
                        "input's next bytes"},
        Stateful{
                .name = "yy_flush_buffer",
                .call = true,
                .does = "discards a buffer's remaining input, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_init_buffer",
                .call = true,
                .does = "flushes and reinitializes a buffer, discarding its remaining input, so the next token is not "
                        "matched against the input's next bytes"},
        Stateful{
                .name = "yy_delete_buffer",
                .call = true,
                .does = "frees a buffer, and freeing the current one leaves the scan without its remaining input, so "
                        "the next token is not matched against the input's next bytes"},
        Stateful{
                .name = "yyrestart",
                .call = true,
                .does = "restarts the scan on another input, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_string",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_bytes",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_buffer",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_switch_to_buffer",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yypush_buffer_state",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yypop_buffer_state",
                .call = true,
                .does = "switches the scan back to an earlier buffer, so the next token is not matched against the "
                        "input's next bytes"}};

/**
 * @brief The words this reading gives meaning to in a flex action, which a macro's replacement may not hide: the calls
 *        that move or feed the match, and the one that ends the scan, the stateful calls' names.
 */
constexpr auto meaningful_words{[] {
    std::array<std::string_view, stateful.size()> words{};

    std::ranges::transform(stateful, words.begin(), &Stateful::name);

    return words;
}()};

/**
 * @brief Returns whether the `.` or `->` at an index names a member of `this`, `this->yyinput()`, `(this)->yyinput()`
 *        or `(*this).yyinput()`, which is the call as flex's C++ scanners write it; the parentheses and the dereference
 *        are stepped over.
 * @param tokens The action's tokens.
 * @param dot The index of the `.` or `->`.
 * @return True for a member of `this`.
 */
[[nodiscard]] bool is_member_of_this(const std::vector<C_token>& tokens, const std::size_t dot)
{
    if (dot > 0 && tokens[dot - 1].text == "this")
    {
        return true;
    }

    if (dot < 3 || tokens[dot - 1].text != ")" || tokens[dot - 2].text != "this")
    {
        return false;
    }

    const auto before{tokens[dot - 3].text == "*" ? dot - 3 : dot - 2};

    return before > 0 && tokens[before - 1].text == "(";
}

/**
 * @brief Returns what a `YY_USER_ACTION`'s replacement does that an action may not, as the refusal states it: the hook
 *        runs before the rule's own action, so a return in it returns before the action can, a `break` or a `continue`
 *        ends the rule's case without the action, and a `goto` leaves for somewhere out of sight; flex 2.6.4 under
 *        `#define YY_USER_ACTION return 9;` returns 9 for every match of `a+ return 7;`. A call that moves the match
 *        and a macro that could hide one are refused as they are in an action.
 * @param replacement The hook's replacement.
 * @param macros The macros the file defines.
 * @param returning The forms besides `return` an action returns a token through.
 * @return What the refusal says after "the action", or std::nullopt when the hook does nothing of the kind.
 */
[[nodiscard]] std::optional<std::string> replacement_use(
        const std::string_view replacement, const Macros_t& macros, const Returning_t& returning)
{
    static constexpr std::array<std::string_view, 5> jumps{"return", "break", "continue", "goto", break_macro};

    for (const auto& [at, end, text] : c_tokens(replacement))
    {
        const auto leaves{std::ranges::contains(jumps, text) || std::ranges::contains(returning, text)};

        if (leaves)
        {
            return std::format(
                    "holds `{}`, which ends the rule's case before its own action runs, so which token a match emits "
                    "is out of sight",
                    text);
        }
    }

    if (const auto use{stateful_use(replacement, false)})
    {
        return use;
    }

    return macro_use(replacement, macros, meaningful_words);
}

} // namespace

std::optional<std::string> stateful_use(const std::string_view action, const bool injecting_only)
{
    const auto tokens{c_tokens(action)};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        const auto& word{tokens[index].text};

        const auto entry{std::ranges::find(stateful, word, &Stateful::name)};

        if (entry == stateful.end())
        {
            continue;
        }

        const auto called{index + 1 < tokens.size() && tokens[index + 1].text == "("};

        // A member of the name on anything but `this` is the call when called or parenthesised to be called, and none
        // when read and not called.
        const auto through_member{
                index > 0 && (tokens[index - 1].text == "." || tokens[index - 1].text == "->") &&
                !is_member_of_this(tokens, index - 1)};

        const auto closed{index + 1 < tokens.size() && tokens[index + 1].text == ")"};

        const auto& [name, call, does]{*entry};

        // The calls that push a byte onto the input or take one from it, which change what is scanned next wherever
        // they stand; the others move a match, which an action without one cannot do.
        const auto injecting{name == "unput" || name == "input" || name == "yyinput"};

        if ((through_member && !called && !closed) || (injecting_only && !injecting))
        {
            continue;
        }

        if (!call)
        {
            return std::format("uses {}, which {}", name, does);
        }

        if (called)
        {
            return std::format("calls {}(), which {}", name, does);
        }

        return std::format(
                "names {} apart from a call, so what it does with it is out of sight; called, it {}", name, does);
    }

    return std::nullopt;
}

std::optional<User_action_use> user_action_use(
        const std::string_view code, const Macros_t& macros, const Returning_t& returning)
{
    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 2 < tokens.size(); ++at)
    {
        if (tokens[at].text != "#" || tokens[at + 1].text != "define")
        {
            continue;
        }

        const auto& name{tokens[at + 2].text};

        const auto line{lines_before(code, tokens[at].at)};

        // flex writes `YY_BREAK` after every action as well, a `break` by default, so a definition of it of the file's
        // own runs after every action too, and what it does there is out of this reading's sight.
        if (name == break_macro)
        {
            std::string why{
                    "is not the only hook the code defines: YY_BREAK is defined too, which flex writes after every "
                    "action, so what an action leaves is out of sight"};

            return User_action_use{.why = std::move(why), .lines_in = line};
        }

        if (name != "YY_USER_ACTION")
        {
            continue;
        }

        const auto from{tokens[at + 2].end};

        const auto end{directive_end(code, from)};

        const auto replacement{code.substr(from, end - from)};

        if (const auto use{replacement_use(replacement, macros, returning)})
        {
            return User_action_use{.why = *use, .lines_in = line};
        }
    }

    return std::nullopt;
}

void refuse_action(const Lexer_spec::Rule& rule, const Macros_t& macros, const Returning_t& returning)
{
    if (rule.action.starts_with('|'))
    {
        return;
    }

    const auto refuse{[&rule](const std::string_view use) { throw Spec_error{action_refusal(use), rule.line}; }};

    if (const auto use{directive_use(rule.action)})
    {
        refuse(*use);
    }

    if (const auto use{returns_undecided(rule.action, returning, true)})
    {
        refuse(*use);
    }

    // flex defines `YY_BREAK` as `break;` and writes it after every action; an action spelling it itself leaves the
    // rule's case on that path with the match discarded, as a `break` of its own would: flex 2.6.4 with `a+ { if
    // (yyleng == 1) YY_BREAK; return 7; }` before `b return 8;` returns 8 alone on "ab".
    const auto tokens{c_tokens(rule.action)};

    if (std::ranges::contains(tokens, break_macro, &C_token::text))
    {
        refuse("uses YY_BREAK, which flex defines as `break;`, so it leaves the rule's case on some path with the "
               "match discarded, and whether a match emits a token is out of sight");
    }

    if (const auto use{macro_use(rule.action, macros, meaningful_words)})
    {
        refuse(*use);
    }
}

} // namespace munch::tools::audit

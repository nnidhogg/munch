#include "munch/tools/audit/action_return.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <span>
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
 * @brief The statements of an action's own tokens and whether they return on every path through them, as
 *        returns_undecided() reads the action: a return, a block ending in one, an `if` whose both branches do, a
 *        labelled statement that does, or such a statement followed by the one jump a stored token allows.
 *
 * A loop, a `switch`, a `try` and an `if` without an `else` may fall through, and are out of sight rather than
 * followed.
 */
class Return_paths
{
public:
    /**
     * @brief A stretch of the tokens, from the index of its first through the index just past its last.
     */
    struct Range
    {
        /**
         * @brief The index of the stretch's first token.
         */
        std::size_t begin{};

        /**
         * @brief The index just past the stretch's last token.
         */
        std::size_t end{};
    };

    /**
     * @brief Binds the reading to an action's tokens.
     * @param tokens The action's own tokens, the bodies of its local classes and lambdas left out.
     * @param returning The names the reader gives meaning to as returns, as returned() takes them.
     * @param restarts The labels a `goto` restarts the scan by.
     */
    Return_paths(
            const std::vector<C_token>& tokens, const Returning_t& returning,
            const std::set<std::string, std::less<>>& restarts);

    /**
     * @brief Returns the statements of a stretch, each from its first token through the one before the next: one ends
     *        at a `;` or at a closing brace outside every brace of the stretch's own, unless an `else` follows, so that
     *        an `if` and its `else`, and a chain of them, are one statement.
     *
     * A stretch of nothing but `;`, `}` and `\\` is no statement, which is what flex passes on after an action's own
     * text, a stray close or a backslash ending its line.
     * @param range The stretch.
     * @return The statements in order.
     */
    [[nodiscard]] std::vector<Range> statements(Range range) const;

    /**
     * @brief Returns whether the last statement may follow the one before it as the one jump after the last return: a
     *        `break` after a stored token, `token = BUILD; break;`, which leaves the loop with the token stored, or any
     *        jump after a `return`, which is dead code; a `continue` or a restarting `goto` after a stored token
     *        rescans and drops the token, and is no such jump.
     * @param before The statement before the last.
     * @param last The last statement.
     * @return True when the last statement is that jump.
     */
    [[nodiscard]] bool is_trailing_jump(Range before, Range last) const;

    /**
     * @brief Returns whether a run of statements ends in the one jump after the last return, is_trailing_jump() of its
     *        last two.
     * @param statements The statements, in order.
     * @return True when there are two or more and the last is that jump.
     */
    [[nodiscard]] bool ends_in_trailing_jump(const std::vector<Range>& statements) const;

    /**
     * @brief Returns whether a statement is the one jump that may follow a stored token, `token = BUILD; break;`: a
     *        `break`, a `continue` or a `goto` to a restarting label; a `goto` to any other is never it, its label
     *        being out of sight.
     * @param statement The statement.
     * @return True for such a jump.
     */
    [[nodiscard]] bool is_jump(Range statement) const;

    /**
     * @brief Returns whether the last of a stretch's statements returns on every path through it, or the one before
     *        does and the last is a jump, `token = BUILD; break;`, which leaves the scanner with the token stored.
     * @param range The stretch.
     * @return True when the stretch ends by returning.
     */
    [[nodiscard]] bool ends_returning(Range range) const;

private:
    /**
     * @brief Returns whether a statement returns on every path through it: a return of the action's own; a block whose
     *        last statement does, dead code after a return counted as returning still; an `if` with an `else` whose
     *        both branches do; a labelled statement that does, `done: return 7;`.
     * @param range The statement.
     * @return True when it returns on every path.
     */
    [[nodiscard]] bool returns(Range range) const;

    /**
     * @brief Returns whether an `if` statement returns on every path through it: it has an `else`, and the branches the
     *        `else` outside every brace of theirs parts, the then-statement before it and the other after it, itself an
     *        `if` when the chain goes on, both return.
     * @param range The statement, its first token the `if`.
     * @return True when both branches return.
     */
    [[nodiscard]] bool if_returns(Range range) const;

    /**
     * @brief The action's own tokens.
     */
    const std::vector<C_token>& tokens_;

    /**
     * @brief The names besides `return` that return.
     */
    const Returning_t& returning_;

    /**
     * @brief The labels a `goto` restarts the scan by.
     */
    const std::set<std::string, std::less<>>& restarts_;
};

/**
 * @brief Returns the brace a `}` closes, found by matching the braces back from it.
 * @param tokens The tokens.
 * @param at The index of the `}`.
 * @return The index of the `{`, or zero when the matching reaches the first token.
 */
[[nodiscard]] std::size_t opening_brace(const std::vector<C_token>& tokens, const std::size_t at)
{
    auto open{at};

    for (auto depth{0};; --open)
    {
        depth += depth_step(tokens[open].text, "}", "{");

        if (depth == 0 || open == 0)
        {
            break;
        }
    }

    return open;
}

/**
 * @brief Returns the token before the template argument list a `>` closes: a template-id ends with `>`, so the name
 *        stands before the list and not beside what follows it, and `std::array<bool, 1>{true}` initializes as
 *        `Predicates{}` does, the `[` after either one subscripting rather than opening a capture list.
 *
 * An argument may be an expression with a comparison in it, `std::array<bool, (2 > 1)>`, whose signs are that
 * comparison and not the list's, so only the ones outside every group count.
 * @param tokens The tokens.
 * @param head The index of the `>`.
 * @return The index of the token before the list, or std::nullopt when the list does not close before the first token.
 */
[[nodiscard]] std::optional<std::size_t> before_template_arguments(const std::vector<C_token>& tokens, std::size_t head)
{
    auto angles{0};

    auto groups{0};

    for (; head > 0; --head)
    {
        const auto& text{tokens[head].text};

        // Read backwards, a closer deepens the group and an opener comes back out.
        const auto step{group_step(text, ")]}", "([{")};

        if (step != 0)
        {
            groups = std::max(groups + step, 0);
        }
        else if (groups == 0)
        {
            angles += depth_step(text, ">", "<");
        }

        if (groups == 0 && angles == 0)
        {
            break;
        }
    }

    if (angles != 0 || head == 0)
    {
        return std::nullopt;
    }

    return head - 1;
}

/**
 * @brief Returns whether the parenthesised group before a brace is a type, whose braced value ends an expression,
 *        rather than a condition, whose brace opens a block and ends none.
 *
 * Which it is the word before the group says: `if (q) { }` and the loops open a block, while anything else opens a
 * braced value, `decltype(flags){true}` and the compound literal `(int[]){1}` among them, whose `[` after the brace
 * subscripts. A group opening a statement stands after one of the words that open a block and after no other, and `if
 * constexpr (q)` and `if consteval` put a word between the `if` and the group and are conditions all the same.
 * @param tokens The tokens.
 * @param head The index of the group's `)`.
 * @return True for a type; false for a condition, or when the group opens at the first token.
 */
[[nodiscard]] bool is_type_group(const std::vector<C_token>& tokens, const std::size_t head)
{
    auto scan{head};

    for (auto groups{0}; scan > 0; --scan)
    {
        groups += depth_step(tokens[scan].text, ")", "(");

        if (groups == 0)
        {
            break;
        }
    }

    if (scan == 0)
    {
        return false;
    }

    const auto& before{tokens[scan - 1].text};

    const auto qualified{
            (before == "constexpr" || before == "consteval") && scan >= 2 && tokens[scan - 2].text == "if"};

    static constexpr std::array<std::string_view, 5> conditions{"if", "while", "for", "switch", "catch"};

    return !qualified && !std::ranges::contains(conditions, before);
}

/**
 * @brief Returns whether a token is a name or a number, which is what its first byte says: a letter, a digit or an
 *        underscore.
 * @param text The token's text, not empty.
 * @return True for a name or a number.
 */
[[nodiscard]] bool is_named(const std::string_view text)
{
    return is_name_byte(text.front());
}

/**
 * @brief Returns whether a `}` ends an expression, which depends on what it closes: a braced initializer's, whose `{`
 *        the type it initializes stands before, ends one, so the `[` of `Predicates{}[0]()` subscripts; a block's,
 *        whose `{` a parenthesis, a semicolon, another brace or a control keyword stands before, ends none, so the `[`
 *        of `if (q) { } [&]{ ... }();` opens a capture list.
 * @param tokens The tokens.
 * @param at The index of the `}`.
 * @return True when the brace closes a braced value.
 */
[[nodiscard]] bool closes_value(const std::vector<C_token>& tokens, const std::size_t at)
{
    const auto open{opening_brace(tokens, at)};

    if (open == 0)
    {
        return false;
    }

    // The token before that brace, which says which of the two it is, a template argument list stepped over.
    auto head{open - 1};

    if (tokens[head].text == ">")
    {
        const auto name{before_template_arguments(tokens, head)};

        if (!name)
        {
            return false;
        }

        head = *name;
    }

    if (tokens[head].text == ")")
    {
        return is_type_group(tokens, head);
    }

    const auto& before{tokens[head].text};

    // `if consteval { }` puts the word straight before the brace and opens a block all the same, and so do its
    // negations, `if !consteval` and `if not consteval`.
    if (before == "consteval" && head > 0)
    {
        const auto& prior{tokens[head - 1].text};

        if (prior == "if" || ((prior == "!" || prior == "not") && head > 1 && tokens[head - 2].text == "if"))
        {
            return false;
        }
    }

    static constexpr std::array<std::string_view, 13> blocks{"if",     "else", "for",      "while",  "do",
                                                             "switch", "try",  "catch",    "struct", "class",
                                                             "union",  "enum", "namespace"};

    return is_named(before) && !std::ranges::contains(blocks, before);
}

/**
 * @brief Returns whether a token is one an expression can end with: a name of the file's own, a number, a literal, a
 *        closing parenthesis or bracket, or a closing brace that closes_value() says ends one.
 *
 * A keyword of C's ends no expression, so the `[` of `return [](){ ... }()` opens a capture list where the `[` of
 * `h[i](x)` subscripts, and neither do the alternative spellings C++ gives the operators: `true and [] { ... }()` has
 * an operator before the capture list, not a value, so the `[` opens a lambda there as it does after `&&`.
 * @param tokens The tokens.
 * @param at The index of the token.
 * @return True when an expression can end with it.
 */
[[nodiscard]] bool ends_expression(const std::vector<C_token>& tokens, const std::size_t at)
{
    const auto& text{tokens[at].text};

    if (text == "}")
    {
        return closes_value(tokens, at);
    }

    const auto literal{text.front() == '"' || text.front() == '\''};

    static constexpr std::array<std::string_view, 23> keywords{
            "return",   "case",   "throw", "else",   "do",     "new",      "delete", "co_return",
            "co_yield", "sizeof", "and",   "or",     "not",    "xor",      "bitand", "bitor",
            "compl",    "and_eq", "or_eq", "xor_eq", "not_eq", "co_await", "alignof"};

    return (is_named(text) || literal || text == ")" || text == "]") && !std::ranges::contains(keywords, text);
}

/**
 * @brief Returns where the template parameter list after a capture list ends, when one stands there: its angle brackets
 *        are a group there and an operator anywhere after, `[](int x = (1 < 2)) { ... }` holding a comparison and not a
 *        list, so the list is stepped over before the body is looked for.
 *
 * A default argument of the list's own may compare too, `[]<bool b = (1 < 2)>() { ... }`, so the angle brackets inside
 * a group it opens are that comparison and not the list's, and the list closes outside every group.
 * @param tokens The tokens.
 * @param from The index just past the capture list.
 * @return The index just past the template parameter list, or from when none opens there.
 */
[[nodiscard]] std::size_t template_parameters_end(const std::vector<C_token>& tokens, std::size_t from)
{
    if (from >= tokens.size() || tokens[from].text != "<")
    {
        return from;
    }

    for (auto angles{0}, groups{0}; from < tokens.size(); ++from)
    {
        const auto& text{tokens[from].text};

        const auto step{group_step(text, "([{", ")]}")};

        if (step != 0)
        {
            groups = std::max(groups + step, 0);
        }
        else if (groups == 0)
        {
            angles += depth_step(text, "<", ">");
        }

        if (groups == 0 && angles == 0)
        {
            return from + 1;
        }
    }

    return from;
}

/**
 * @brief Returns where a requires expression inside a lambda's constraint ends: its parameter list and its requirement
 *        braces, each stepped over whole when it stands, so that neither is taken for the lambda's body.
 * @param tokens The tokens.
 * @param at The index of the expression's `requires`.
 * @return The index just past the expression.
 */
[[nodiscard]] std::size_t requires_expression_end(const std::vector<C_token>& tokens, const std::size_t at)
{
    auto scan{at + 1};

    static constexpr std::array bracket_pairs{
            std::pair{std::string_view{"("}, std::string_view{")"}},
            std::pair{std::string_view{"{"}, std::string_view{"}"}}};

    for (const auto& [opener, closer] : bracket_pairs)
    {
        if (scan >= tokens.size() || tokens[scan].text != opener)
        {
            continue;
        }

        for (auto groups{0}; scan < tokens.size(); ++scan)
        {
            groups += depth_step(tokens[scan].text, opener, closer);

            if (groups == 0)
            {
                ++scan;

                break;
            }
        }
    }

    return scan;
}

/**
 * @brief Returns the brace opening a lambda's body: the first one outside every group after the capture list and the
 *        template parameters, since a `(` or a `[` between the two opens a group a brace inside belongs to, `[](int x =
 *        int{7}) { ... }` being the case that says so.
 *
 * A `;` outside every group before it means the `[` opened no lambda at all. A requires clause stands between the
 * parameter list and the body, and its constraint may be a requires expression, whose own braces are not the body's:
 * `[]<class T>() requires requires { typename T::value_type; } { return 7; }` returns from the lambda in the second
 * group and not the first, so the clause is stepped over whole. The constraint is an expression like any other: names,
 * parenthesised expressions and requires expressions joined by `&&` and `||`, each requires expression stepped over
 * whole, so that `requires true && requires { ... } { ... }` reaches the second group as its body.
 * @param tokens The tokens.
 * @param from The index just past the template parameters, or past the capture list when there are none.
 * @return The index of the body's `{`, or std::nullopt when the bracket opened no lambda.
 */
[[nodiscard]] std::optional<std::size_t> body_open(const std::vector<C_token>& tokens, const std::size_t from)
{
    auto depth{0};

    // Whether a requires clause has been opened, so that the `requires` words after it are the constraint's own
    // requires expressions and not another clause.
    auto constraining{false};

    for (auto at{from}; at < tokens.size(); ++at)
    {
        const auto& text{tokens[at].text};

        // The first `requires` opens the clause; every later one opens a requires expression of its constraint.
        if (depth == 0 && text == "requires" && !constraining)
        {
            constraining = true;

            continue;
        }

        if (depth == 0 && text == "requires")
        {
            at = requires_expression_end(tokens, at) - 1;

            continue;
        }

        const auto step{group_step(text, "([", ")]")};

        if (step != 0)
        {
            depth = std::max(depth + step, 0);
        }
        else if (depth == 0 && text == ";")
        {
            return std::nullopt;
        }
        else if (depth == 0 && text == "{")
        {
            return at;
        }
    }

    return std::nullopt;
}

/**
 * @brief Erases a group of tokens, from its opener through its closer.
 * @param tokens The tokens.
 * @param open The index of the group's opener.
 * @param close The index of the group's closer.
 */
void erase_group(std::vector<C_token>& tokens, const std::size_t open, const std::size_t close)
{
    const auto group_begin{tokens.begin() + static_cast<std::ptrdiff_t>(open)};

    const auto group_end{tokens.begin() + static_cast<std::ptrdiff_t>(close) + 1};

    tokens.erase(group_begin, group_end);
}

/**
 * @brief Returns the tokens of an action with the body of every lambda declared in it left out, since a `return` in a
 *        lambda returns from the lambda and not from the action.
 *
 * A `[` opens a capture list where no expression ends before it, which would make it a subscript, and where it opens no
 * attribute's `[[`; the body is the first brace group after the list outside every group, a template parameter list and
 * a requires clause stepped over on the way, and a `;` before any such brace says the bracket opened no lambda.
 * @param tokens The action's tokens.
 * @return The tokens outside every lambda's body.
 */
[[nodiscard]] std::vector<C_token> outside_lambdas(std::vector<C_token> tokens)
{
    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        if (tokens[at].text != "[")
        {
            continue;
        }

        // An attribute opens with two brackets where a capture list opens with one, so `[[likely]] { ... }` opens no
        // lambda and the block after it is the function's own; the whole attribute is stepped over, wherever it stands,
        // since its inner bracket opens no capture list either and an expression may end before it.
        if (at + 1 < tokens.size() && tokens[at + 1].text == "[")
        {
            at = group_close(tokens, at, "[", "]");

            continue;
        }

        // A subscript's bracket, which an expression ends before.
        if (at > 0 && ends_expression(tokens, at - 1))
        {
            continue;
        }

        // The body is looked for past the capture list and the template parameters after it.
        const auto captures_close{group_close(tokens, at, "[", "]")};

        const auto from{template_parameters_end(tokens, captures_close + 1)};

        const auto open{body_open(tokens, from)};

        if (!open)
        {
            continue;
        }

        const auto close{group_close(tokens, *open, "{", "}")};

        if (close == tokens.size())
        {
            break;
        }

        erase_group(tokens, *open, close);
    }

    return tokens;
}

/**
 * @brief Returns the tokens of an action with the body of every class, struct or union declared in it left out, since a
 *        `return` in a method of one returns from that method and not from the action.
 *
 * `{ struct Local { int f() { return 7; } }; Local local; (void)local.f(); }` returns nothing from the rule, and the
 * scanner flex builds from it discards the match. The body is the brace group that follows the keyword and its name, so
 * the braced value of `struct holder h = {custom};`, which follows an `=`, is no body and stays.
 * @param tokens The action's tokens.
 * @return The tokens outside every such body.
 */
[[nodiscard]] std::vector<C_token> outside_local_types(std::vector<C_token> tokens)
{
    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        static constexpr std::array<std::string_view, 3> types{"struct", "class", "union"};

        if (!std::ranges::contains(types, tokens[at].text))
        {
            continue;
        }

        static constexpr std::array<std::string_view, 11> specifiers{"typedef", "static",   "const",    "constexpr",
                                                                     "inline",  "extern",   "volatile", "thread_local",
                                                                     "mutable", "register", "constinit"};

        static constexpr std::array<std::string_view, 4> statement_ends{";", "{", "}", ":"};

        // A definition begins a statement, so the keyword stands first or after a `;`, a brace or a label's colon, with
        // declaration specifiers allowed between, `static struct Helper { ... } h;`; the `class` of a template
        // parameter list, `[]<class T>()`, follows a `<` or a `,` and defines nothing.
        auto start{at};

        while (start > 0 && std::ranges::contains(specifiers, tokens[start - 1].text))
        {
            --start;
        }

        if (start > 0 && !std::ranges::contains(statement_ends, tokens[start - 1].text))
        {
            continue;
        }

        // The head runs from the keyword to the body's brace: a name, `final`, a base clause, an attribute or an
        // `alignas(...)` may stand between, their own groups stepped over, while an `=` or a `;` before any brace says
        // the keyword opened no class body at all, `struct holder h = {custom};` and `struct S;` among them.
        auto open{at + 1};

        for (auto groups{0}; open < tokens.size(); ++open)
        {
            const auto& piece{tokens[open].text};

            groups += group_step(piece, "([", ")]");

            if (groups == 0 && (piece == "{" || piece == "=" || piece == ";"))
            {
                break;
            }
        }

        if (open >= tokens.size() || tokens[open].text != "{")
        {
            continue;
        }

        const auto close{group_close(tokens, open, "{", "}")};

        if (close == tokens.size())
        {
            break;
        }

        erase_group(tokens, open, close);

        at = open - 1;
    }

    return tokens;
}

/**
 * @brief Returns whether a token returns: `return`, or one of the names the reader gives meaning to as returns.
 * @param token The token.
 * @param returning The names besides `return` that return.
 * @return True for a return.
 */
[[nodiscard]] bool is_return(const C_token& token, const Returning_t& returning)
{
    return token.text == "return" || std::ranges::contains(returning, token.text);
}

Return_paths::Return_paths(
        const std::vector<C_token>& tokens, const Returning_t& returning,
        const std::set<std::string, std::less<>>& restarts)
    : tokens_{tokens}, returning_{returning}, restarts_{restarts}
{}

std::vector<Return_paths::Range> Return_paths::statements(const Range range) const
{
    const auto is_filler{
            [](const C_token& token) { return token.text == ";" || token.text == "}" || token.text == "\\"; }};

    std::vector<Range> out{};

    auto depth{0};

    auto begin{range.begin};

    for (auto at{range.begin}; at < range.end; ++at)
    {
        const auto& text{tokens_[at].text};

        depth += depth_step(text, "{", "}");

        const auto continued{at + 1 < range.end && tokens_[at + 1].text == "else"};

        const auto ends{(text == ";" || text == "}") && depth <= 0 && !continued};

        if (!ends && at + 1 != range.end)
        {
            continue;
        }

        const auto statement{std::span{tokens_}.subspan(begin, at + 1 - begin)};

        const auto filler{std::ranges::all_of(statement, is_filler)};

        if (!filler)
        {
            out.push_back(Range{.begin = begin, .end = at + 1});
        }

        begin = at + 1;

        depth = std::max(depth, 0);
    }

    return out;
}

bool Return_paths::is_trailing_jump(const Range before, const Range last) const
{
    return is_jump(last) && returns(before) &&
           (tokens_[last.begin].text == "break" || tokens_[before.begin].text == "return");
}

bool Return_paths::ends_in_trailing_jump(const std::vector<Range>& statements) const
{
    return statements.size() >= 2 && is_trailing_jump(statements[statements.size() - 2], statements.back());
}

bool Return_paths::is_jump(const Range statement) const
{
    const auto& first{tokens_[statement.begin].text};

    return first == "break" || first == "continue" ||
           (first == "goto" && statement.end - statement.begin > 1 &&
            restarts_.contains(tokens_[statement.begin + 1].text));
}

bool Return_paths::ends_returning(const Range range) const
{
    auto own{statements(range)};

    if (ends_in_trailing_jump(own))
    {
        own.pop_back();
    }

    return !own.empty() && returns(own.back());
}

bool Return_paths::returns(const Range range) const
{
    if (range.begin >= range.end)
    {
        return false;
    }

    const auto& first{tokens_[range.begin].text};

    if (is_return(tokens_[range.begin], returning_))
    {
        return true;
    }

    if (first == "{")
    {
        return tokens_[range.end - 1].text == "}" && ends_returning({.begin = range.begin + 1, .end = range.end - 1});
    }

    if (first == "if")
    {
        return if_returns(range);
    }

    // A label, `done: return 7;`.
    if (range.end - range.begin > 2 && tokens_[range.begin + 1].text == ":")
    {
        return returns({.begin = range.begin + 2, .end = range.end});
    }

    return false;
}

bool Return_paths::if_returns(const Range range) const
{
    // Past the condition: the words of `if constexpr` and the parenthesised condition, or `if !consteval` and its kin,
    // which have none.
    auto at{range.begin + 1};

    while (at < range.end && tokens_[at].text != "(" && tokens_[at].text != "{")
    {
        ++at;
    }

    if (at < range.end && tokens_[at].text == "(")
    {
        at = group_close(tokens_, at, "(", ")") + 1;
    }

    const auto branches{statements({.begin = at, .end = range.end})};

    if (branches.size() != 1)
    {
        return false;
    }

    const auto [first, end]{branches.front()};

    auto depth{0};

    for (auto split{first}; split < end; ++split)
    {
        depth += depth_step(tokens_[split].text, "{", "}");

        if (depth == 0 && tokens_[split].text == "else")
        {
            return returns({.begin = first, .end = split}) && returns({.begin = split + 1, .end = end});
        }
    }

    return false;
}

/**
 * @brief Returns an action's own tokens: its tokens with the bodies of its local classes and of its lambdas left out,
 *        since a `return` in either returns from them and not from the action.
 * @param action The action's text.
 * @return The action's own tokens.
 */
[[nodiscard]] std::vector<C_token> own_tokens(const std::string_view action)
{
    auto all{c_tokens(action)};

    auto outside_types{outside_local_types(std::move(all))};

    return outside_lambdas(std::move(outside_types));
}

/**
 * @brief Returns what every return of the action's own returns, each read as returned() reads the first.
 * @param action The action's text.
 * @param tokens The action's own tokens.
 * @param returning The names besides `return` that return.
 * @return The text of each return's value in order, empty for one returned() reads no value of.
 */
[[nodiscard]] std::vector<std::string> returned_values(
        const std::string_view action, const std::vector<C_token>& tokens, const Returning_t& returning)
{
    std::vector<std::string> values{};

    for (auto at{tokens.begin()}; at != tokens.end(); ++at)
    {
        if (!is_return(*at, returning))
        {
            continue;
        }

        const auto rest{action.substr(at->at)};

        const auto value{returned(rest, returning)};

        values.push_back(value.value_or(std::string{}));

        at = std::ranges::find(at, tokens.end(), ";", &C_token::text);

        if (at == tokens.end())
        {
            break;
        }
    }

    return values;
}

/**
 * @brief Returns the action's own statements inside the braces that are the whole action when it has them, which is
 *        where the one jump allowed after the last return stands last.
 * @param paths The reading of the action's own tokens.
 * @param tokens The action's own tokens.
 * @return The statements.
 */
[[nodiscard]] std::vector<Return_paths::Range> unbraced_statements(
        const Return_paths& paths, const std::vector<C_token>& tokens)
{
    auto own{paths.statements({.begin = 0, .end = tokens.size()})};

    const auto one_block{[&tokens](const std::vector<Return_paths::Range>& statements) {
        if (statements.size() != 1)
        {
            return false;
        }

        const auto [begin, end]{statements.front()};

        return tokens[begin].text == "{" && tokens[end - 1].text == "}";
    }};

    while (one_block(own))
    {
        const auto [begin, end]{own.front()};

        own = paths.statements({.begin = begin + 1, .end = end - 1});
    }

    return own;
}

/**
 * @brief Returns why a jump outside the one allowed after the last return leaves what a match does out of sight, when
 *        one does.
 *
 * A jump anywhere but as the one after the last return ends the action on its path before any return: flex writes each
 * action as a case of a switch inside the scanning loop, so `break` and `continue` both leave it with the match
 * discarded, and re2c's actions stand in the loop the file wrote; `a+ { if (yyleng == 1) break; return 7; }` returns 8
 * alone on "ab" under flex 2.6.4. A `goto` leaves for a label out of the action's sight, `a+ { goto emit_token; }`
 * reaching a `return 8` in the next rule's action, so it is refused whether or not the action returns anywhere; a
 * `break` or a `continue` in an action returning nowhere discards on every path.
 * @param tokens The action's own tokens.
 * @param trailing The one jump allowed, an empty range at the tokens' end when there is none.
 * @param restarts The labels a `goto` restarts the scan by.
 * @param returns_somewhere Whether the action returns on some path.
 * @param break_discards Whether a `break` leaves the action with the match discarded rather than the scanner's loop.
 * @return The refusal, or std::nullopt when no such jump stands.
 */
[[nodiscard]] std::optional<std::string> jump_refusal(
        const std::vector<C_token>& tokens, const Return_paths::Range trailing,
        const std::set<std::string, std::less<>>& restarts, const bool returns_somewhere, const bool break_discards)
{
    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        const auto& text{tokens[at].text};

        if (at >= trailing.begin && at < trailing.end)
        {
            continue;
        }

        // A `goto` to a label that restarts the scan, standing before the block in the file, discards the match as a
        // `continue` does; any other label is out of sight.
        const auto restarting{text == "goto" && at + 1 < tokens.size() && restarts.contains(tokens[at + 1].text)};

        if (text == "goto" && !restarting)
        {
            return "leaves by `goto` on some path, for a label out of the action's sight, so whether a match emits "
                   "a token or is discarded is out of sight until every path returns";
        }

        if ((text == "break" || text == "continue" || restarting) && returns_somewhere)
        {
            const auto label{restarting ? std::format(" {}", tokens[at + 1].text) : std::string{}};

            return std::format(
                    "leaves by `{}{}` on some path before it returns, which the scanner takes as the action's end with "
                    "the match discarded, so whether a match emits a token or is discarded is out of sight until every "
                    "path returns",
                    text, label);
        }

        if (text == "break" && !break_discards)
        {
            return "leaves by `break` the loop the file wrote around the scanner, so whether the scan goes on past "
                   "the match is out of sight";
        }
    }

    return std::nullopt;
}

} // namespace

std::optional<std::string> returns_undecided(
        const std::string_view action, const Returning_t& returning, const bool break_discards,
        const std::set<std::string, std::less<>>& restarts, const bool chained)
{
    const auto tokens{own_tokens(action)};

    const Return_paths paths{tokens, returning, restarts};

    const auto values{returned_values(action, tokens, returning)};

    if (std::ranges::adjacent_find(values, std::ranges::not_equal_to{}) != values.end())
    {
        return "returns from more than one place and not the same token from each, so which token a match "
               "emits is out of sight until the action returns one token";
    }

    const auto own{unbraced_statements(paths, tokens)};

    const auto jumps_last{paths.ends_in_trailing_jump(own)};

    const auto trailing{jumps_last ? own.back() : Return_paths::Range{.begin = tokens.size(), .end = tokens.size()}};

    if (auto refusal{jump_refusal(tokens, trailing, restarts, !values.empty(), break_discards)})
    {
        return refusal;
    }

    if (values.empty())
    {
        // flex's `YY_BREAK` discards the match of an action returning nowhere; a chained re2c action returning nowhere
        // falls into the next rule's action unless it leaves by a jump, `continue;` or a restarting `goto`.
        const auto leaves_by_jump{!own.empty() && paths.is_jump(own.back())};

        if (chained && !leaves_by_jump)
        {
            return "ends without returning or leaving, and re2c writes the next rule's action right after it, so "
                   "what a match of this rule does is out of sight until the action leaves by a jump or a return";
        }

        return std::nullopt;
    }

    if (paths.ends_returning({.begin = 0, .end = tokens.size()}))
    {
        return std::nullopt;
    }

    return std::format(
            "returns {} on some path and ends without returning on another, so whether a match emits a token or is "
            "discarded is out of sight until the action ends by returning one",
            values.front());
}

std::optional<std::string> returned(const std::string_view action, const Returning_t& returning)
{
    const auto tokens{own_tokens(action)};

    // The text from one token through the one before another, spliced as the tokens are.
    const auto text{[action]<typename Iterator>(const Iterator from, const Iterator to) -> std::optional<std::string> {
        if (from >= to)
        {
            return std::nullopt;
        }

        const auto last{std::prev(to)};

        const auto stretch{action.substr(from->at, last->end - from->at)};

        auto [joined, place]{spliced(stretch)};

        return std::move(joined);
    }};

    const auto is_return_token{[&returning](const C_token& token) { return is_return(token, returning); }};

    const auto returns{std::ranges::find_if(tokens, is_return_token)};

    if (returns == tokens.end())
    {
        return std::nullopt;
    }

    const auto name{returns->text};

    const auto semicolon{std::ranges::find(returns, tokens.end(), ";", &C_token::text)};

    if (name == "return")
    {
        return text(std::next(returns), semicolon);
    }

    const auto next{std::next(returns)};

    if (next != tokens.end() && next->text == "=")
    {
        return text(std::next(next), semicolon);
    }

    if (next != tokens.end() && next->text == "(")
    {
        // The first argument: up to the comma or the close at depth one.
        auto depth{0};

        for (auto inner{next}; inner != tokens.end(); ++inner)
        {
            depth += depth_step(inner->text, "(", ")");

            const auto closed{inner->text == ")" && depth == 0};

            const auto first_ended{inner->text == "," && depth == 1};

            if (closed || first_ended)
            {
                return text(std::next(next), inner).value_or(std::string{name});
            }
        }
    }

    return std::string{name};
}

std::string action_refusal(const std::string_view use)
{
    return std::format("the action {}", use);
}

} // namespace munch::tools::audit

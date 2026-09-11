#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LEXER_SPEC_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_LEXER_SPEC_HPP

#include <cstddef>
#include <optional>
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
 * A flex file is one scanner; a re2c file holds one per block with rules. Rule order is the priority: flex and re2c
 * take the longest match and, among rules matching it, the first in the file, which is exactly the maximal-munch
 * scan with priority by rule index that the library runs. The code sections and the actions' bodies are carried as
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
     * @brief The file's options, each one entry, flex's `%option` words or re2c's configurations.
     */
    std::vector<std::string> options;

    /**
     * @brief The rules, in file order.
     */
    std::vector<Rule> rules;

    /**
     * @brief The line the rules begin on, counted from one: a flex file's `%%`, a re2c block's opener; what names a
     *        scanner among a file's.
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
 * @brief The expression a C action returns, when it returns one.
 *
 * What both readers ask of an action: a rule returning something produces a token, a rule returning nothing
 * discards its match, which is what whitespace and comment rules do. The earliest whole word among `return` and the
 * names given decides, the code around it not being read.
 * @param action The action's text.
 * @param returning The forms besides `return` that return, none unless given.
 * @return The expression between `return` and its `;`, or what a named form returns, trimmed; std::nullopt when the
 *         action returns nothing.
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
 * @brief Where a flex action ends: the first end of a line at which its braces balance, string and character
 *        literals and comments skipped, which is how flex reads one whether it opens with a brace or reaches one
 *        later on its line.
 * @param code The stretch of C the action opens.
 * @return The offset of that line's newline, or the stretch's size when it ends balanced; std::nullopt when a brace
 *         is left open.
 */
[[nodiscard]] std::optional<std::size_t> action_end(std::string_view code) noexcept;

/**
 * @brief Where the brace block opening a stretch of C closes, string and character literals and comments skipped,
 *        which is how re2c reads an action.
 * @param code The stretch, its first byte the opening brace.
 * @return The offset just past the closing brace, or std::nullopt when the stretch ends first.
 */
[[nodiscard]] std::optional<std::size_t> brace_close(std::string_view code) noexcept;

/**
 * @brief The token set a start condition scans with: every active rule is a token whose id and priority are its
 *        rule index, and the rules returning nothing are discarded.
 * @param spec The specification.
 * @param condition The condition, INITIAL for the default one.
 * @return The token set, its expressions parsed against the definitions.
 * @throws Spec_error If a rule's pattern is refused by regex::parse(), naming the rule's line and the reason, or the
 *         file asks for case-insensitive scanning throughout, which is not modelled.
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

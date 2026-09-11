#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_TOKEN_SET_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_TOKEN_SET_HPP

#include <cstddef>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"

namespace munch::tools::audit
{
/**
 * @brief One token of a set as the audit edits and compiles it: the pattern as nodes, the id the report names it
 *        by, its priority, and whether a parser ever sees it.
 */
struct Token_rule
{
    /**
     * @brief The pattern.
     */
    regex::Regex regex;

    /**
     * @brief The id, a rule index for a set read from a file.
     */
    std::size_t id;

    /**
     * @brief The priority, lower winning among rules matching the same longest lexeme.
     */
    std::size_t priority;

    /**
     * @brief Whether the token is discarded before a parser sees it, which is what the modulo certificate deletes.
     */
    bool discarded;
};

/**
 * @brief A token set held as patterns rather than as tables, so that it can be edited and compiled again.
 */
struct Token_set
{
    /**
     * @brief The rules, in the order the file gave them.
     */
    std::vector<Token_rule> rules;
};

/**
 * @brief Excludes one byte from every character set of a regex, in place, dropping the alternatives of a choice
 *        that cannot lose it.
 *
 * This is the one edit the cost analysis knows, and it is the edit behind every designed row of the split-points
 * study: a newline excluded from a comment's interior bounds the comment to a line, excluded from a string's
 * interior forbids raw newlines in strings, and excluded from a whitespace run splits the run at newlines. Call it
 * only where can_lose() holds; elsewhere the pattern would be left matching nothing.
 * @param regex The pattern, narrowed on return.
 * @param byte The byte.
 */
void exclude(regex::Regex& regex, unsigned char byte);

/**
 * @brief Compiles a token set.
 * @param set The token set.
 * @return The lexer, its discarded set declared.
 */
[[nodiscard]] core::Lexer compile(const Token_set& set);

/**
 * @brief Whether a regex can lose a byte: whether excluding it from every character set leaves a pattern that still
 *        matches something, which fails exactly where the byte is part of a fixed spelling the pattern needs.
 *
 * A character set can lose the byte while another member remains; a fixed spelling cannot lose a byte it holds; a
 * concatenation or repetition can lose it when its parts can; a choice can while one alternative can.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when exclude() would leave a pattern matching something.
 */
[[nodiscard]] bool can_lose(const regex::Regex& regex, unsigned char byte);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_TOKEN_SET_HPP

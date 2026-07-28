#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP

#include <vector>

namespace lexer::regex
{
struct Regex;

/**
 * @brief Regex node that matches one of several alternative regexes (alternation).
 *
 * Use the choice() combinator to construct.
 */
struct Choice
{
    /**
     * @brief The regexes matched as alternatives. Must hold at least one element.
     *
     * A vector is used because Regex is incomplete here, being the type this node is an alternative of.
     */
    std::vector<Regex> regexes;
};

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP

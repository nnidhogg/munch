#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP

#include <vector>

namespace lexer::regex
{
struct Regex;

/**
 * @brief Regex node that matches a sequence of regexes (concatenation).
 *
 * Use the concat() combinator to construct.
 */
struct Concat
{
    /**
     * @brief The regexes matched in sequence. Must hold at least one element.
     *
     * A vector is used because Regex is incomplete here, being the type this node is an alternative of.
     */
    std::vector<Regex> regexes;
};

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP

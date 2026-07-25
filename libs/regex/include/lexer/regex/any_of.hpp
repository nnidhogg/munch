#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP

#include "lexer/regex/set.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that matches any character from a given set.
 *
 * Use the any_of() combinator to construct.
 */
struct Any_of
{
    /**
     * @brief The set of characters matched by this node.
     */
    Set set;
};

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP

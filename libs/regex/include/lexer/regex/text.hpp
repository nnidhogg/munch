#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP

#include <string>

namespace lexer::regex
{
/**
 * @brief Regex node that matches a fixed sequence of characters (literal text).
 *
 * Use the text() combinator to construct.
 */
struct Text
{
    /**
     * @brief The literal text matched by this node.
     */
    std::string text;
};

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP

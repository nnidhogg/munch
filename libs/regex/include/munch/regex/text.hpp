#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_TEXT_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_TEXT_HPP

#include <string>

namespace munch::regex
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

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_TEXT_HPP

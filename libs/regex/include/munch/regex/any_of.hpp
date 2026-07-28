#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_ANY_OF_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_ANY_OF_HPP

#include "munch/regex/set.hpp"

namespace munch::regex
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

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_ANY_OF_HPP

#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_CHOICE_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_CHOICE_HPP

#include <vector>

namespace munch::regex
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

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_CHOICE_HPP

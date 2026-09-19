#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REPEAT_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REPEAT_HPP

#include <cstddef>
#include <variant>

#include "munch/regex/indirect.hpp"

namespace munch::regex
{
struct Regex;

/**
 * @brief Represents the Kleene star ('*') repetition, i.e. zero or more occurrences of the pattern.
 */
struct Kleene
{
};

/**
 * @brief Represents the Kleene plus ('+') repetition, i.e. one or more occurrences of the pattern.
 */
struct Plus
{
};

/**
 * @brief Represents an optional pattern ('?'), i.e. zero or one occurrence.
 */
struct Optional
{
};

/**
 * @brief Represents an exact repetition, i.e. exactly `count` occurrences.
 */
struct Exact
{
    /**
     * @brief The number of occurrences, exactly.
     */
    std::size_t count;
};

/**
 * @brief Represents a lower-bound repetition, i.e. at least `min` occurrences.
 */
struct At_least
{
    /**
     * @brief The fewest occurrences allowed.
     */
    std::size_t min;
};

/**
 * @brief Represents a bounded repetition, i.e. between `min` and `max` occurrences inclusive.
 */
struct Range
{
    /**
     * @brief The fewest occurrences allowed.
     */
    std::size_t min;

    /**
     * @brief The most occurrences allowed, inclusive.
     */
    std::size_t max;
};

/**
 * @brief Regex node that repeats a sub-pattern.
 *
 * Use the kleene(), plus(), optional(), exact(), at_least() or range() combinators to construct.
 */
struct Repeat
{
    /**
     * @brief The repetition kind applied to the sub-pattern.
     */
    using Kind_t = std::variant<Kleene, Plus, Optional, Exact, At_least, Range>;

    /**
     * @brief The repetition kind.
     */
    Kind_t kind;

    /**
     * @brief The repeated sub-pattern.
     */
    Indirect<Regex> regex;
};

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REPEAT_HPP

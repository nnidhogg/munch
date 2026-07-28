#ifndef MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REGEX_HPP
#define MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REGEX_HPP

#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/nfa/builder.hpp"
#include "munch/regex/any_of.hpp"
#include "munch/regex/choice.hpp"
#include "munch/regex/concat.hpp"
#include "munch/regex/repeat.hpp"
#include "munch/regex/text.hpp"

namespace munch::regex
{
/**
 * @brief A regex node.
 *
 * A regex is a sum of the node types it can be, held by value. Nodes carry no behaviour: operations over a regex are
 * free functions dispatching on the alternative, so a new operation can be added without touching the node types.
 */
struct Regex
{
    /**
     * @brief The node alternatives a regex can be.
     */
    using Node_t = std::variant<Any_of, Text, Concat, Choice, Repeat>;

    /**
     * @brief The node this regex holds.
     */
    Node_t node;
};

/**
 * @brief Converts a regex to an NFA builder.
 * @param regex The regex to convert.
 * @return NFA builder representing the regex.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex);

/**
 * @brief Converts an Any_of node to an NFA builder.
 * @param any_of The node to convert.
 * @return NFA builder representing the node.
 */
[[nodiscard]] nfa::Builder to_nfa(const Any_of& any_of);

/**
 * @brief Converts a Text node to an NFA builder.
 * @param text The node to convert.
 * @return NFA builder representing the node.
 */
[[nodiscard]] nfa::Builder to_nfa(const Text& text);

/**
 * @brief Converts a Concat node to an NFA builder.
 * @param concat The node to convert.
 * @return NFA builder representing the node.
 */
[[nodiscard]] nfa::Builder to_nfa(const Concat& concat);

/**
 * @brief Converts a Choice node to an NFA builder.
 * @param choice The node to convert.
 * @return NFA builder representing the node.
 */
[[nodiscard]] nfa::Builder to_nfa(const Choice& choice);

/**
 * @brief Converts a Repeat node to an NFA builder.
 * @param repeat The node to convert.
 * @return NFA builder representing the node.
 */
[[nodiscard]] nfa::Builder to_nfa(const Repeat& repeat);

/**
 * @brief Creates a regex matching any character from a set.
 * @tparam T Type convertible to Set.
 * @param chars The set of characters to match.
 * @return The created regex.
 */
template <typename T>
    requires std::is_constructible_v<Set, T>
[[nodiscard]] Regex any_of(T&& chars)
{
    return {.node = Any_of{.set = Set{std::forward<T>(chars)}}};
}

/**
 * @brief Creates a regex matching a fixed sequence of characters.
 * @tparam T Type a std::string can be list-initialized from, including a single character.
 * @param arg The string or character sequence to match.
 * @return The created regex.
 */
template <typename T>
[[nodiscard]] Regex text(T&& arg)
{
    return {.node = Text{.text = std::string{std::forward<T>(arg)}}};
}

/**
 * @brief Creates a regex matching one or more regexes in sequence.
 * @tparam Args Types convertible to Regex.
 * @param args The regexes to match in sequence.
 * @return The created regex.
 */
template <typename... Args>
    requires(sizeof...(Args) > 0)
[[nodiscard]] Regex concat(Args&&... args)
{
    std::vector<Regex> regexes;

    regexes.reserve(sizeof...(Args));

    (regexes.push_back(std::forward<Args>(args)), ...);

    return {.node = Concat{.regexes = std::move(regexes)}};
}

/**
 * @brief Creates a regex matching one of several alternatives.
 * @tparam Args Types convertible to Regex.
 * @param args The regexes to match as alternatives.
 * @return The created regex.
 */
template <typename... Args>
    requires(sizeof...(Args) > 0)
[[nodiscard]] Regex choice(Args&&... args)
{
    std::vector<Regex> regexes;

    regexes.reserve(sizeof...(Args));

    (regexes.push_back(std::forward<Args>(args)), ...);

    return {.node = Choice{.regexes = std::move(regexes)}};
}

/**
 * @brief Creates a Kleene star (zero or more) repetition regex.
 * @param regex The regex to repeat.
 * @return The created regex.
 */
[[nodiscard]] Regex kleene(Regex regex);

/**
 * @brief Creates a Kleene plus (one or more) repetition regex.
 * @param regex The regex to repeat.
 * @return The created regex.
 */
[[nodiscard]] Regex plus(Regex regex);

/**
 * @brief Creates an optional (zero or one) repetition regex.
 * @param regex The regex to make optional.
 * @return The created regex.
 */
[[nodiscard]] Regex optional(Regex regex);

/**
 * @brief Creates an exact repetition regex.
 * @param regex The regex to repeat.
 * @param count The exact number of repetitions.
 * @return The created regex.
 */
[[nodiscard]] Regex exact(Regex regex, std::size_t count);

/**
 * @brief Creates a lower-bound repetition regex.
 * @param regex The regex to repeat.
 * @param min The minimum number of repetitions.
 * @return The created regex.
 */
[[nodiscard]] Regex at_least(Regex regex, std::size_t min);

/**
 * @brief Creates a bounded repetition regex.
 * @param regex The regex to repeat.
 * @param min The minimum number of repetitions.
 * @param max The maximum number of repetitions.
 * @return The created regex.
 */
[[nodiscard]] Regex range(Regex regex, std::size_t min, std::size_t max);

} // namespace munch::regex

#endif // MUNCH_LIBS_REGEX_INCLUDE_MUNCH_REGEX_REGEX_HPP

#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP

#include <memory>

#include "lexer/regex/regex.hpp"
#include "lexer/regex/set.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that matches any character from a given set.
 *
 * Use the static create() method or the any_of() helper to construct.
 */
class Any_of final : public Regex
{
public:
    /**
     * @brief Creates an Any_of regex node from a set of characters.
     * @tparam T Type convertible to Set.
     * @param chars The set of characters to match.
     * @return Shared pointer to the created Any_of node.
     */
    template <typename T, typename = std::enable_if_t<std::is_constructible_v<Set, T>>>
    static std::shared_ptr<Any_of> create(T&& chars)
    {
        return std::shared_ptr<Any_of>(new Any_of(std::forward<T>(chars)));
    }

    Any_of(const Any_of&) = delete;
    Any_of& operator=(const Any_of&) = delete;

    /**
     * @brief Converts this regex node to an NFA builder.
     * @return NFA builder representing this regex.
     */
    [[nodiscard]] nfa::Builder to_nfa() const override;

private:
    template <typename T>
    explicit Any_of(T&& chars) : set_{std::forward<T>(chars)}
    {}

    Set set_;
};

/**
 * @brief Helper function to create an Any_of regex node from a set of characters.
 * @tparam T Type convertible to Set.
 * @param chars The set of characters to match.
 * @return Shared pointer to the created Regex node.
 */
template <typename T>
std::shared_ptr<const Regex> any_of(T&& chars)
{
    return Any_of::create(std::forward<T>(chars));
}

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_ANY_OF_HPP

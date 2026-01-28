#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP

#include <memory>
#include <string>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that matches a fixed sequence of characters (literal text).
 *
 * Use the static create() method or the text() helper to construct.
 */
class Text final : public Regex
{
public:
    /**
     * @brief Creates a Text regex node from a string or character sequence.
     * @tparam T Type convertible to std::string.
     * @param arg The string or character sequence to match.
     * @return Shared pointer to the created Text node.
     */
    template <typename T>
    static std::shared_ptr<Text> create(T&& arg)
    {
        return std::shared_ptr<Text>(new Text(std::forward<T>(arg)));
    }

    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;

    /**
     * @brief Converts this regex node to an NFA builder.
     * @return NFA builder representing this regex.
     */
    [[nodiscard]] nfa::Builder to_nfa() const override;

private:
    template <typename T>
    explicit Text(T&& arg) : text_{std::forward<T>(arg)}
    {}

    std::string text_;
};

/**
 * @brief Helper function to create a Text regex node from a string or character sequence.
 * @tparam T Type convertible to std::string.
 * @param arg The string or character sequence to match.
 * @return Shared pointer to the created Regex node.
 */
template <typename T>
std::shared_ptr<const Regex> text(T&& arg)
{
    return Text::create(std::forward<T>(arg));
}

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_TEXT_HPP

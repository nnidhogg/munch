#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP

#include <memory>
#include <vector>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that matches one of several alternative regexes (alternation).
 *
 * Use the static create() method or the choice() helper to construct.
 */
class Choice final : public Regex
{
public:
    /**
     * @brief Creates a Choice regex node from one or more regexes.
     * @tparam Args Types convertible to shared_ptr<const Regex>.
     * @param args The regexes to match as alternatives.
     * @return Shared pointer to the created Choice node.
     */
    template <typename... Args>
    static std::shared_ptr<Choice> create(Args&&... args)
    {
        return std::shared_ptr<Choice>(new Choice(std::forward<Args>(args)...));
    }

    Choice(const Choice&) = delete;
    Choice& operator=(const Choice&) = delete;

    /**
     * @brief Converts this regex node to an NFA builder.
     * @return NFA builder representing this regex.
     */
    [[nodiscard]] nfa::Builder to_nfa() const override;

private:
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    explicit Choice(Args&&... args) : regexes_{std::forward<Args>(args)...}
    {}

    std::vector<std::shared_ptr<const Regex>> regexes_;
};

/**
 * @brief Helper function to create a Choice regex node from one or more regexes.
 * @tparam Args Types convertible to shared_ptr<const Regex>.
 * @param args The regexes to match as alternatives.
 * @return Shared pointer to the created Regex node.
 */
template <typename... Args>
std::shared_ptr<const Regex> choice(Args&&... args)
{
    return Choice::create(std::forward<Args>(args)...);
}

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CHOICE_HPP

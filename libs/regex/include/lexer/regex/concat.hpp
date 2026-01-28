#ifndef LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP
#define LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP

#include <memory>
#include <vector>

#include "lexer/regex/regex.hpp"

namespace lexer::regex
{
/**
 * @brief Regex node that matches a sequence of regexes (concatenation).
 *
 * Use the static create() method or the concat() helper to construct.
 */
class Concat final : public Regex
{
public:
    /**
     * @brief Creates a Concat regex node from one or more regexes.
     * @tparam Args Types convertible to shared_ptr<const Regex>.
     * @param args The regexes to match in sequence.
     * @return Shared pointer to the created Concat node.
     */
    template <typename... Args>
    static std::shared_ptr<Concat> create(Args&&... args)
    {
        return std::shared_ptr<Concat>(new Concat(std::forward<Args>(args)...));
    }

    Concat(const Concat&) = delete;
    Concat& operator=(const Concat&) = delete;

    /**
     * @brief Converts this regex node to an NFA builder.
     * @return NFA builder representing this regex.
     */
    [[nodiscard]] nfa::Builder to_nfa() const override;

private:
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    explicit Concat(Args&&... args) : regexes_{std::forward<Args>(args)...}
    {}

    std::vector<std::shared_ptr<const Regex>> regexes_;
};

/**
 * @brief Helper function to create a Concat regex node from one or more regexes.
 * @tparam Args Types convertible to shared_ptr<const Regex>.
 * @param args The regexes to match in sequence.
 * @return Shared pointer to the created Regex node.
 */
template <typename... Args>
std::shared_ptr<const Regex> concat(Args&&... args)
{
    return Concat::create(std::forward<Args>(args)...);
}

} // namespace lexer::regex

#endif // LEXER_LIBS_REGEX_INCLUDE_LEXER_REGEX_CONCAT_HPP

#ifndef MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKEN_HPP
#define MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKEN_HPP

#include <string_view>

namespace munch::tools::tokenizer
{
/**
 * @brief Single token produced by a Tokenizer.
 *
 * The template parameter @p T is the token kind type (typically an enum) used by the underlying lexer.
 *
 * @tparam T Token kind type.
 */
template <typename T>
class Token
{
public:
    /**
     * @brief Construct a Token.
     * @param kind Semantic kind/value of the token (usually an enum value).
     * @param lexeme View into the original input corresponding to the token text.
     */
    Token(T kind, const std::string_view lexeme) noexcept : kind_{kind}, lexeme_{lexeme} {}

    /**
     * @brief Return the token kind.
     */
    [[nodiscard]] T kind() const noexcept { return kind_; }

    /**
     * @brief Return the token lexeme.
     *
     * The returned view always refers to the underlying input string owned by the Tokenizer that produced this token.
     */
    [[nodiscard]] std::string_view lexeme() const noexcept { return lexeme_; }

private:
    /**
     * @brief Semantic kind/value of the token.
     */
    T kind_;

    /**
     * @brief View into the original input corresponding to the token text.
     */
    std::string_view lexeme_;
};

} // namespace munch::tools::tokenizer

#endif // MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKEN_HPP

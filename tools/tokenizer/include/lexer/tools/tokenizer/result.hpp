#ifndef LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_RESULT_HPP
#define LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_RESULT_HPP

#include <utility>
#include <variant>

#include "lexer/tools/tokenizer/error.hpp"
#include "lexer/tools/tokenizer/token.hpp"

namespace lexer::tools::tokenizer
{
/**
 * @brief Marks the end of the input in a Result.
 */
struct End_of_input
{
};

/**
 * @brief The outcome of reading one token: a token, the end of the input, or a lexical error.
 *
 * The three outcomes are alternatives of one sum type rather than nested layers, so a caller tells them apart with a
 * single query, or handles them exhaustively with visit(). The constructors convert implicitly, letting each outcome
 * be returned as itself.
 * @tparam T The token kind type (enum or integral).
 */
template <typename T>
class Result
{
    /**
     * @brief The outcome alternatives a result can hold.
     */
    using Variant_t = std::variant<Token<T>, End_of_input, Error>;

public:
    /**
     * @brief Constructs a result holding a token.
     * @param token The matched token.
     */
    Result(Token<T> token) noexcept : variant_{std::move(token)} {}

    /**
     * @brief Constructs a result marking the end of the input.
     * @param end The end-of-input marker.
     */
    Result(End_of_input end) noexcept : variant_{end} {}

    /**
     * @brief Constructs a result holding a lexical error.
     * @param error The error describing the failure.
     */
    Result(Error error) noexcept : variant_{std::move(error)} {}

    /**
     * @brief Checks whether the result holds a token.
     * @return True if a token was matched.
     */
    [[nodiscard]] bool has_token() const noexcept { return std::holds_alternative<Token<T>>(variant_); }

    /**
     * @brief Checks whether the result holds a lexical error.
     * @return True if tokenization failed.
     */
    [[nodiscard]] bool has_error() const noexcept { return std::holds_alternative<Error>(variant_); }

    /**
     * @brief Checks whether the result marks the end of the input.
     * @return True if the input was exhausted.
     */
    [[nodiscard]] bool end_of_input() const noexcept { return std::holds_alternative<End_of_input>(variant_); }

    /**
     * @brief Returns the held token.
     * @return Reference to the token.
     * @throws std::bad_variant_access If the result does not hold a token.
     */
    [[nodiscard]] const Token<T>& token() const { return std::get<Token<T>>(variant_); }

    /**
     * @brief Returns the held error.
     * @return Reference to the error.
     * @throws std::bad_variant_access If the result does not hold an error.
     */
    [[nodiscard]] const Error& error() const { return std::get<Error>(variant_); }

    /**
     * @brief Applies a visitor to the held outcome, handling all three exhaustively.
     * @param visitor Callable invocable with Token<T>, End_of_input, and Error.
     * @return Whatever the visitor returns.
     */
    template <typename Visitor>
    decltype(auto) visit(Visitor&& visitor) const
    {
        return std::visit(std::forward<Visitor>(visitor), variant_);
    }

private:
    /**
     * @brief The outcome this result holds.
     */
    Variant_t variant_;
};

} // namespace lexer::tools::tokenizer

#endif // LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_RESULT_HPP

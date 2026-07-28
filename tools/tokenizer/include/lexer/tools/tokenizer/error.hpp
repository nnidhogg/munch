#ifndef LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_ERROR_HPP
#define LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_ERROR_HPP

#include <cstddef>
#include <string>

namespace lexer::tools::tokenizer
{
/**
 * @brief Describes a lexical error encountered while tokenizing input.
 */
class Error
{
public:
    /**
     * @brief Construct an Error.
     * @param message Human-readable description of the error.
     * @param position Byte offset in the input where the error occurred.
     */
    Error(std::string message, const std::size_t position) : message_{std::move(message)}, position_{position} {}

    /**
     * @brief Return the error message.
     */
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /**
     * @brief Return the error position (byte offset) in the input.
     */
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::string message_;

    std::size_t position_;
};

} // namespace lexer::tools::tokenizer

#endif // LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_ERROR_HPP

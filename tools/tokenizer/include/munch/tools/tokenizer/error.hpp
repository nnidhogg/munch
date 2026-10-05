#ifndef MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_ERROR_HPP
#define MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_ERROR_HPP

#include <cstddef>
#include <string>

namespace munch::tools::tokenizer
{
/**
 * @brief Describes a lexical error encountered while tokenizing input.
 */
class Error
{
public:
    /**
     * @brief Constructs an Error.
     * @param message Human-readable description of the error.
     * @param position Byte offset in the input where the error occurred.
     */
    Error(std::string message, std::size_t position);

    /**
     * @brief Returns the error message.
     * @return The human-readable description of the error.
     */
    [[nodiscard]] const std::string& message() const noexcept;

    /**
     * @brief Returns the error position (byte offset) in the input.
     * @return The byte offset in the input where the error occurred.
     */
    [[nodiscard]] std::size_t position() const noexcept;

private:
    /**
     * @brief Human-readable description of the error.
     */
    std::string message_;

    /**
     * @brief Byte offset in the input where the error occurred.
     */
    std::size_t position_;
};

} // namespace munch::tools::tokenizer

#endif // MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_ERROR_HPP

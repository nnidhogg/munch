#ifndef LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP
#define LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP

#include <algorithm>
#include <string>
#include <string_view>

#include "lexer/core/lexer.hpp"
#include "lexer/tools/tokenizer/result.hpp"

namespace lexer::tools::tokenizer
{
/**
 * @brief Wrapper that turns core::Lexer into a sequential token stream.
 *
 * Returns tokens in order as matched by the lexer without additional processing.
 *
 * @warning This class is not thread-safe. Concurrent calls to next() or load() on the same instance
 *          will result in undefined behavior.
 *
 * @warning The Token objects returned by next() contain a std::string_view that references the internal
 *          input buffer. These views become invalid if load() is called or if the Tokenizer is destroyed.
 *          If tokens need to outlive the Tokenizer or persist across load() calls, copy the lexeme to a
 *          std::string.
 */
class Tokenizer
{
public:
    /**
     * @brief Standard tokenizer result type.
     *
     * Holds a `Token<T>` on success, `End_of_input` when the input is exhausted, or an `Error` on failure.
     */
    template <typename T>
    using Result_t = Result<T>;

    /**
     * @brief Construct a tokenizer from a lexer.
     * @param lexer Lexer used to recognize tokens.
     */
    explicit Tokenizer(core::Lexer lexer) : lexer_{std::move(lexer)}, offset_{0} {}

    /**
     * @brief Construct a tokenizer from a lexer and an input string held in memory.
     * @param lexer Lexer used to recognize tokens.
     * @param input Input text to tokenize.
     */
    explicit Tokenizer(core::Lexer lexer, std::string input)
        : lexer_{std::move(lexer)}, input_{std::move(input)}, offset_{0}
    {}

    /**
     * @brief Replace the input text and reset tokenization state.
     */
    void load(std::string input)
    {
        input_ = std::move(input);

        offset_ = 0;
    }

    /**
     * @brief Reset the reading position to the beginning of the current input.
     */
    void reset() noexcept { offset_ = 0; }

    /**
     * @brief Move the reading position to the given byte offset, clamped to the end of the input.
     *
     * The escape hatch for tokens no automaton can recognize, such as C++ raw string literals: a driver reads the
     * prefix token, scans the remainder by hand, and seeks past it before reading on.
     */
    void seek(const std::size_t offset) noexcept { offset_ = std::min(offset, input_.size()); }

    /**
     * @brief Return the current byte offset in the input.
     *
     * Useful for error reporting and tracking tokenization progress.
     *
     * @return The current byte position in the input buffer.
     */
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    /**
     * @brief Return the next token.
     *
     * On success, returns a Token<T>; End_of_input indicates the input is exhausted.
     * On failure, returns an Error describing the lexical error at the current position.
     */
    template <typename T>
        requires(std::integral<T> || std::is_enum_v<T>)
    [[nodiscard]] Result_t<T> next()
    {
        if (offset_ >= input_.size())
        {
            return End_of_input{};
        }

        const auto view{std::string_view{input_}.substr(offset_)};

        const auto [token, consumed]{lexer_.tokenize<T>(view)};

        if (!token || consumed == 0)
        {
            return Error{"Unrecognized character at position " + std::to_string(offset_), offset_};
        }

        const auto lexeme{std::string_view{input_}.substr(offset_, consumed)};

        offset_ += consumed;

        return Token<T>{*token, lexeme};
    }

private:
    core::Lexer lexer_;

    std::string input_;

    std::size_t offset_;
};

} // namespace lexer::tools::tokenizer

#endif // LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP

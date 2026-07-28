#ifndef LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP
#define LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lexer/core/lexer.hpp"
#include "lexer/tools/tokenizer/result.hpp"

namespace lexer::tools::tokenizer
{
/**
 * @brief Wrapper that turns core::Lexer into a sequential token stream.
 *
 * Returns tokens in order as matched by the lexer without additional processing.
 *
 * A tokenizer may hold several lexers as modes over the same input, for languages whose tokenization is
 * context-dependent, such as header-names after `#include`. The driver switches modes explicitly with set_mode();
 * the tokenizer never switches on its own.
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
    explicit Tokenizer(core::Lexer lexer) : mode_{0}, offset_{0} { lexers_.push_back(std::move(lexer)); }

    /**
     * @brief Construct a tokenizer from a lexer and an input string held in memory.
     * @param lexer Lexer used to recognize tokens.
     * @param input Input text to tokenize.
     */
    explicit Tokenizer(core::Lexer lexer, std::string input) : mode_{0}, input_{std::move(input)}, offset_{0}
    {
        lexers_.push_back(std::move(lexer));
    }

    /**
     * @brief Construct a tokenizer from one lexer per mode.
     * @param lexers The lexers, indexed by mode; mode 0 starts active.
     * @throws std::invalid_argument If no lexer is given.
     */
    explicit Tokenizer(std::vector<core::Lexer> lexers) : mode_{0}, offset_{0}, lexers_{std::move(lexers)}
    {
        if (lexers_.empty())
        {
            throw std::invalid_argument("A tokenizer needs at least one lexer");
        }
    }

    /**
     * @brief Construct a tokenizer from one lexer per mode and an input string held in memory.
     * @param lexers The lexers, indexed by mode; mode 0 starts active.
     * @param input Input text to tokenize.
     * @throws std::invalid_argument If no lexer is given.
     */
    explicit Tokenizer(std::vector<core::Lexer> lexers, std::string input) : Tokenizer{std::move(lexers)}
    {
        input_ = std::move(input);
    }

    /**
     * @brief Replace the input text and reset tokenization state. The active mode is kept.
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
     * @brief Make the lexer of the given mode recognize the following tokens.
     * @tparam T The mode type (enum or integral).
     * @param mode The mode to activate, as passed to the constructor.
     * @throws std::out_of_range If no lexer was given for the mode.
     */
    template <typename T>
        requires(std::integral<T> || std::is_enum_v<T>)
    void set_mode(const T mode)
    {
        const auto index{static_cast<std::size_t>(mode)};

        if (index >= lexers_.size())
        {
            throw std::out_of_range("No lexer was given for mode " + std::to_string(index));
        }

        mode_ = index;
    }

    /**
     * @brief Return the active mode.
     * @return The mode whose lexer recognizes the following tokens.
     */
    [[nodiscard]] std::size_t mode() const noexcept { return mode_; }

    /**
     * @brief Return the current byte offset in the input.
     *
     * Useful for error reporting and tracking tokenization progress.
     *
     * @return The current byte position in the input buffer.
     */
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    /**
     * @brief Return the input text being tokenized.
     *
     * Lets a driver scan tokens by hand next to the automaton; see seek().
     *
     * @return View of the input buffer, invalidated by load() and destruction.
     */
    [[nodiscard]] std::string_view input() const noexcept { return input_; }

    /**
     * @brief Return the next token, recognized by the active mode's lexer.
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

        const auto [token, consumed]{lexers_[mode_].tokenize<T>(view)};

        if (!token || consumed == 0)
        {
            return Error{"Unrecognized character at position " + std::to_string(offset_), offset_};
        }

        const auto lexeme{std::string_view{input_}.substr(offset_, consumed)};

        offset_ += consumed;

        return Token<T>{*token, lexeme};
    }

private:
    /**
     * @brief The active mode, i.e. the index of the lexer recognizing tokens.
     */
    std::size_t mode_;

    /**
     * @brief The input text being tokenized.
     */
    std::string input_;

    /**
     * @brief The reading position as a byte offset into the input.
     */
    std::size_t offset_;

    /**
     * @brief The lexers, one per mode.
     */
    std::vector<core::Lexer> lexers_;
};

} // namespace lexer::tools::tokenizer

#endif // LEXER_TOOLS_TOKENIZER_INCLUDE_LEXER_TOOLS_TOKENIZER_TOKENIZER_HPP

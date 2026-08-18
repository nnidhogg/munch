#ifndef MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP
#define MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP

#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/tools/tokenizer/result.hpp"

namespace munch::tools::tokenizer
{
/**
 * @brief Wrapper that turns core::Lexer into a sequential token stream.
 *
 * Returns tokens in order as matched by the lexer without additional processing.
 *
 * A tokenizer may hold several lexers as modes over the same input, for languages whose tokenization is
 * context-dependent, such as header-names after `#include`. Constructed from lexers, the driver switches modes
 * explicitly with set_mode() and the tokenizer never switches on its own, which suits a parser that knows what is
 * coming. Constructed from a core::Mode_lexer, the grammar carries the switches instead: each token declares its
 * effect on a mode stack, so nested comments and string escapes need no bookkeeping from the driver. set_mode() and
 * mode() keep working either way, and depth() reports the nesting a stack has reached.
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
    explicit Tokenizer(core::Lexer lexer);

    /**
     * @brief Construct a tokenizer from a lexer and an input string held in memory.
     * @param lexer Lexer used to recognize tokens.
     * @param input Input text to tokenize.
     */
    explicit Tokenizer(core::Lexer lexer, std::string input);

    /**
     * @brief Construct a tokenizer from one lexer per mode.
     * @param lexers The lexers, indexed by mode; mode 0 starts active.
     * @throws std::invalid_argument If no lexer is given.
     */
    explicit Tokenizer(std::vector<core::Lexer> lexers);

    /**
     * @brief Construct a tokenizer from one lexer per mode and an input string held in memory.
     * @param lexers The lexers, indexed by mode; mode 0 starts active.
     * @param input Input text to tokenize.
     * @throws std::invalid_argument If no lexer is given.
     */
    explicit Tokenizer(std::vector<core::Lexer> lexers, std::string input);

    /**
     * @brief Construct a tokenizer whose grammar carries its own mode transitions.
     * @param lexer The mode lexer; its mode 0 starts active with an empty stack.
     */
    explicit Tokenizer(core::Mode_lexer lexer);

    /**
     * @brief Construct a tokenizer from a mode lexer and an input string held in memory.
     * @param lexer The mode lexer; its mode 0 starts active with an empty stack.
     * @param input Input text to tokenize.
     */
    explicit Tokenizer(core::Mode_lexer lexer, std::string input);

    /**
     * @brief Replace the input text and start over.
     *
     * Where a mode lexer drives the mode, it is scan state and returns to zero with the saved frames, since both
     * describe nesting in input that is being replaced: keeping either would scan a fresh buffer inside a half open
     * string literal it never entered. That holds however the current mode was reached, set_mode() included, since
     * nothing records which of the two chose it. Where the caller drives the mode with several lexers instead, it is
     * the caller's and is kept.
     */
    void load(std::string input);

    /**
     * @brief Reset the reading position to the beginning of the current input, and the mode with it.
     *
     * The mode a driven scan ended in belongs to the text it read, so rewinding the position rewinds the mode too,
     * including a mode set_mode() forced. Call set_mode() again after reset() to re-enter one deliberately.
     */
    void reset() noexcept;

    /**
     * @brief Move the reading position to the given byte offset, clamped to the end of the input.
     *
     * The escape hatch for tokens no automaton can recognize, such as C++ raw string literals: a driver reads the
     * prefix token, scans the remainder by hand, and seeks past it before reading on.
     */
    void seek(std::size_t offset) noexcept;

    /**
     * @brief Seeks to the next position the active mode's automaton certifies as a token start.
     *
     * The certified counterpart of the manual error loop: where seek() skips by whatever rule the driver
     * invents, recover() asks the active mode's lexer for its first certified byte or split window in evidence
     * order at or after the position past the current one, and moves there. The contract is complete-repair
     * invariance: in every completely tokenizable repair of the input before the answer's preserved evidence,
     * scanning resumes at a token start of the repaired segmentation. No tokenizable repair is promised to
     * exist, and the next read may error again. Consulting the active mode is a policy the flat theorems do
     * not upgrade to a modal guarantee, since a repair could reach the resume point in a different mode; a
     * forced or grammar-driven mode change is the driver's business exactly as for next(). When the search
     * finds no certificate ahead, the position does not move.
     * @return The number of bytes skipped from the current position, or std::nullopt when no certified start
     *         exists in the remaining input.
     */
    [[nodiscard]] std::optional<std::size_t> recover();

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

        const auto available{automatic_ ? automatic_->modes() : lexers_.size()};

        if (index >= available)
        {
            throw std::out_of_range("No lexer was given for mode " + std::to_string(index));
        }

        // Forcing a mode is still allowed when the grammar drives them, and is the recovery hatch after an error:
        // the saved frames are left alone, since the driver may well intend to return through them.
        stack_.current = index;

        mode_ = index;
    }

    /**
     * @brief Return the number of saved mode frames, i.e. how deeply nested the scan is.
     *
     * Always zero unless the tokenizer was constructed from a core::Mode_lexer, since only a grammar-carried
     * transition pushes. Together with mode() this says what a stopped scan was doing, which is what distinguishes
     * an unterminated string from an unrecognized byte in code.
     */
    [[nodiscard]] std::size_t depth() const noexcept;

    /**
     * @brief Return the active mode.
     * @return The mode whose lexer recognizes the following tokens.
     */
    [[nodiscard]] std::size_t mode() const noexcept;

    /**
     * @brief Return the current byte offset in the input.
     *
     * Useful for error reporting and tracking tokenization progress.
     *
     * @return The current byte position in the input buffer.
     */
    [[nodiscard]] std::size_t offset() const noexcept;

    /**
     * @brief Return the input text being tokenized.
     *
     * Lets a driver scan tokens by hand next to the automaton; see seek().
     *
     * @return View of the input buffer, invalidated by load() and destruction.
     */
    [[nodiscard]] std::string_view input() const noexcept;

    /**
     * @brief Return the next token, recognized by the active mode's lexer.
     *
     * On success, returns a Token<T>; End_of_input indicates the input is exhausted.
     * On failure, returns an Error describing the lexical error at the current position.
     *
     * An error does not advance the reading position: guessing a skip would invent tokens. Recovery is the
     * driver's choice of three: stop; seek() past the offending bytes by its own rule; or recover(), which asks
     * the active mode's automaton for the next certified token start. A loop that only tests end_of_input() and
     * ignores has_error() will not terminate.
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

        // The modal path advances the stack, so mode() follows it.
        const auto [token, consumed]{[&]() -> core::Lexer::Match<T> {
            if (automatic_)
            {
                const auto [matched, length]{automatic_->template tokenize<T>(view.cbegin(), view.cend(), stack_)};

                mode_ = stack_.current;

                return {.token = matched, .length = length};
            }

            return lexers_[mode_].template tokenize<T>(view);
        }()};

        if (!token)
        {
            return Error{"Unrecognized character at position " + std::to_string(offset_), offset_};
        }

        if (consumed == 0)
        {
            return Error{"Zero-width match at position " + std::to_string(offset_), offset_};
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
     * @brief The lexers, one per mode, when the driver switches modes itself. Empty otherwise.
     */
    std::vector<core::Lexer> lexers_;

    /**
     * @brief The mode lexer, when the grammar carries the transitions. Empty otherwise.
     */
    std::optional<core::Mode_lexer> automatic_;

    /**
     * @brief The mode and saved frames, advanced by each token's declared action.
     */
    core::Mode_stack stack_;
};

} // namespace munch::tools::tokenizer

#endif // MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP

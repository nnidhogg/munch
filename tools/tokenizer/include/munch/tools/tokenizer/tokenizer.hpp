#ifndef MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP
#define MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP

#include <optional>
#include <string>
#include <string_view>

#include "munch/common/concepts.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/tokenizer/result.hpp"

namespace munch::tools::tokenizer
{
/**
 * @brief Wrapper that turns core::Lexer into a sequential token stream.
 *
 * Returns tokens in order as matched by the lexer without additional processing.
 *
 * This is the flat tokenizer: one token set, no modes, no scan state beyond the reading position. A language whose
 * tokenization is context-dependent, such as header-names after `#include` or string interiors scanned as their own
 * tokens, wants Mode_tokenizer instead. Splitting the two is what lets each carry exactly the surface it supports:
 * there is no mode to read here, and lexer() reaches the compiled automaton, so a caller that wants the parallel
 * path of core::Lexer::chunk_boundaries() has it without a second scanner.
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
     * @brief Return the next token.
     *
     * On success, returns a Token<T>; End_of_input indicates the input is exhausted.
     * On failure, returns an Error describing the lexical error at the current position.
     *
     * An error does not advance the reading position: guessing a skip would invent tokens. Recovery is the
     * driver's choice of three: stop; seek() past the offending bytes by its own rule; or recover(), which asks
     * the automaton for the next certified token start. A loop that only tests end_of_input() and ignores
     * has_error() will not terminate.
     */
    template <common::concepts::Token_id T>
    [[nodiscard]] Result_t<T> next()
    {
        if (offset_ >= input_.size())
        {
            return End_of_input{};
        }

        const auto view{std::string_view{input_}.substr(offset_)};

        const auto [token, consumed]{lexer_.template tokenize<T>(view.cbegin(), view.cend())};

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

    /**
     * @brief Return the input text being tokenized.
     *
     * Lets a driver scan tokens by hand next to the automaton; see seek().
     *
     * @return View of the input buffer, invalidated by load() and destruction.
     */
    [[nodiscard]] std::string_view input() const noexcept;

    /**
     * @brief Return the current byte offset in the input.
     *
     * Useful for error reporting and tracking tokenization progress.
     *
     * @return The current byte position in the input buffer.
     */
    [[nodiscard]] std::size_t offset() const noexcept;

    /**
     * @brief Return the lexer recognizing the tokens.
     *
     * The whole reason this type is separate from Mode_tokenizer: a flat token set is what
     * core::Lexer::chunk_boundaries() and core::Lexer::tokenize_all_parallel() need, so a driver that wants to plan
     * a parallel scan of the same grammar reaches it here rather than keeping a second copy. A mode lexer has no
     * parallel entry point and Mode_tokenizer exposes none, which is why the accessor lives on this side only.
     *
     * @return The lexer, valid for the lifetime of this tokenizer.
     */
    [[nodiscard]] const core::Lexer& lexer() const noexcept;

    /**
     * @brief Replace the input text and start over.
     */
    void load(std::string input);

    /**
     * @brief Reset the reading position to the beginning of the current input.
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
     * @brief Seeks to the next position the automaton certifies as a token start.
     *
     * The certified counterpart of the manual error loop: where seek() skips by whatever rule the driver invents,
     * recover() asks the lexer for its first certified byte or split window in the order the walk meets them,
     * core::Lexer::next_certified_evidence()'s evidence order, at or after the position past the current one, and
     * moves there. The contract is that walk's complete-repair invariance: in every completely tokenizable repair of
     * the input before the answer's preserved evidence, scanning resumes at a token start of the repaired
     * segmentation. No tokenizable repair is promised to exist, and the next read may error again. When the search
     * finds no certificate ahead, the position does not move.
     * @return The number of bytes skipped from the current position, or std::nullopt when no certified byte and no
     *         certified window of two to four bytes lies ahead in the remaining input, the widths the search
     *         consults.
     */
    [[nodiscard]] std::optional<std::size_t> recover();

    /**
     * @brief recover(), with the supporting evidence returned: certified relative to the damaged suffix.
     *
     * The failure-anchored contract, named as such: the search starts one past the current position, which
     * after an error is the failure offset, and the answer carries core::Lexer::Certified_start's guarantee.
     * The scanner does not know the damage's true extent, so when the corruption reaches past the evidence,
     * the transfer to the intended input is forfeit; the returned interval is exactly what a caller needs to
     * check that condition against knowledge of its own.
     * @return The certified answer with its evidence interval, the position moved there, or std::nullopt.
     */
    [[nodiscard]] std::optional<core::Lexer::Certified_start> recover_from_failure();

    /**
     * @brief Recovery under a caller-supplied clean bound, so the answer transfers to the intended input.
     *
     * The clean-anchored contract: the search starts at the later of one past the current position and
     * clean_from, so the returned evidence begins at or after clean_from by construction. When the caller's
     * bound is truly at or past the damage's end, an editor's edit span or a transport frame's boundary, the
     * evidence lies in undamaged text, which settles the survival half of core::Lexer::Certified_start's
     * guarantee, that the evidence outlasted the damage, which the failure-anchored form cannot establish alone.
     * @param clean_from The caller's lower bound on undamaged text.
     * @return The certified answer with its evidence interval, the position moved there, or std::nullopt.
     */
    [[nodiscard]] std::optional<core::Lexer::Certified_start> recover_from_clean(std::size_t clean_from);

private:
    /**
     * @brief The input text being tokenized.
     */
    std::string input_;

    /**
     * @brief The reading position as a byte offset into the input.
     */
    std::size_t offset_;

    /**
     * @brief The compiled token set.
     */
    core::Lexer lexer_;
};

} // namespace munch::tools::tokenizer

#endif // MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_TOKENIZER_HPP

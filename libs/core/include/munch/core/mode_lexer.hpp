#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_LEXER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_LEXER_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/core/lexer.hpp"
#include "munch/core/mode.hpp"

namespace munch::core
{
/**
 * @brief A lexer whose token set depends on the scan's own history.
 *
 * One compiled Lexer per mode, selected by a Mode_stack the caller owns. This is what a front end needs for
 * separately tokenizing the interior of a string literal, string interpolation, and comments that nest. Be precise
 * about which of those a flat token set is denied: an ordinary escaped string literal is regular and a flat grammar
 * matches it whole, so modes buy the interior tokens rather than the literal. Arbitrarily nested comments are the
 * genuinely non-regular case, since counting to an unbounded depth is what one finite automaton cannot do.
 * A construct whose terminator is chosen per occurrence, such as a heredoc naming its own delimiter, is outside
 * this: a mode's token set is fixed at build time, so that needs the Tokenizer's hand-scanning hatch. Instances are
 * obtainable through Mode_builder::build().
 *
 * @par Why there is no parallel entry point
 * A byte cannot identify the mode where two or more modes admit it, so no byte certificate names a safe cut here;
 * use Lexer where the grammar admits a flat token set.
 */
class Mode_lexer
{
public:
    /**
     * @brief The result of one match attempt.
     * @tparam T The token type (enum or integral).
     */
    template <typename T>
    struct Match
    {
        std::optional<T> token{};

        std::size_t length{};

        bool operator==(const Match&) const = default;
    };

    /**
     * @brief A mode lexer whose modes never switch on their own: one compiled Lexer per mode and no actions.
     *
     * The caller-driven case, for a driver that switches modes itself: every token stays in its mode, so the stack
     * changes only when the caller sets its current mode. This is what lets one representation serve both ways of
     * getting context dependence. Grammar-carried transitions come through Mode_builder::build() instead.
     * @param lexers One Lexer per mode, indexed by mode; at least one.
     * @throws std::invalid_argument If no lexer is given.
     */
    explicit Mode_lexer(std::vector<Lexer> lexers);

    /**
     * @brief Matches one token in the stack's current mode and applies its action.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @param begin Iterator to the beginning of the remaining input.
     * @param end Iterator to the end of the input.
     * @param stack The mode stack, advanced by the matched token's action.
     * @return The match. An empty token means no pattern matched, or the token's pop found nothing saved; the
     *         stack is unchanged in both cases, so a caller can report the position without losing context.
     * @throws std::out_of_range If the stack's current mode is not a mode of this lexer, or a pop exposes a saved
     *         frame that is not.
     */
    template <common::concepts::Token_id T, common::concepts::Byte_iterator Iterator>
    [[nodiscard]] Match<T> tokenize(Iterator begin, Iterator end, Mode_stack& stack) const
    {
        check(stack.current);

        const auto [token, length]{lexers_[stack.current].tokenize<std::size_t>(begin, end)};

        if (!token)
        {
            return {};
        }

        // A zero-width match leaves the mode alone. The batch driver stops without reporting one at all, so the two
        // drivers differ on whether the token appears, and applying its action here would make them differ on the
        // mode as well. See docs/limits.md.
        if (length == 0)
        {
            return {.token = static_cast<T>(*token), .length = 0};
        }

        // A mode with no mode-changing token leaves the stack alone, so the lookup and the application are
        // skipped, the same test the batch driver makes once per mode change; this keeps the actionless
        // lexer of a caller-driven Tokenizer as cheap as the flat scan it replaces.
        if (!acting_[stack.current])
        {
            return {.token = static_cast<T>(*token), .length = length};
        }

        const auto action{action_of(stack.current, *token)};

        if (action.kind == Mode_action_kind::pop && !stack.saved.empty())
        {
            check(stack.saved.back());
        }

        if (!stack.apply(action))
        {
            return {};
        }

        return {.token = static_cast<T>(*token), .length = length};
    }

    /**
     * @brief Tokenizes the whole input, driving its own mode stack.
     *
     * Stays inside one mode's batch scan until a token carries any non-stay action, rather than re-entering the
     * scanner once per token. Any such action ends the pass, including a push whose target is the mode already
     * being scanned. Lexer::tokenize_all()'s sink may halt the scan by returning false, so a mode-changing
     * token ends the inner pass and the outer loop resumes in the new mode. Most tokens leave the mode alone, so
     * most of the input is scanned in the tight loop.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @tparam Sink Callable receiving each consumed token, its length, and the mode it matched in.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length, mode) for every consumed token, in input order.
     * @return The number of input elements tokenized; anything short of the input's size means the scan stopped at
     *         the returned offset: no token matched there, a zero-width token did, or a pop found nothing saved
     *         there; this form's sink cannot stop the scan.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator, typename Sink>
        requires std::invocable<Sink&, T, std::size_t, std::size_t>
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink) const
    {
        Mode_stack stack;

        return tokenize_all<T>(begin, end, std::move(sink), stack);
    }

    /**
     * @brief Tokenizes the whole input, reporting the mode and nesting depth the scan ended in.
     *
     * A short return says where a scan stopped but not what it was doing there, which for a modal grammar is most of
     * the diagnosis: an unterminated string and an unrecognized byte in code both stop, and only the mode
     * distinguishes them. The stack is left exactly as the scan left it, so `stack.current` names the mode and
     * `stack.saved.size()` the depth.
     * @param stack Receives the mode and saved frames at the stopping point; its incoming value starts the scan.
     * @throws std::out_of_range If the stack's current mode is not a mode of this lexer, or a pop exposes a saved
     *         frame that is not.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterator Iterator, typename Sink>
        requires std::invocable<Sink&, T, std::size_t, std::size_t>
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink, Mode_stack& stack) const
    {
        check(stack.current);

        std::size_t offset{0};

        const auto size{static_cast<std::size_t>(std::distance(begin, end))};

        while (offset < size)
        {
            const auto mode{stack.current};

            // A mode nothing can leave needs no halt condition, so its sink returns void and the pass runs to the end.
            if (!acting_[mode])
            {
                const auto whole{lexers_[mode].tokenize_all<std::size_t>(
                        begin + static_cast<std::ptrdiff_t>(offset), end,
                        [&](const std::size_t token, const std::size_t length) {
                            sink(static_cast<T>(token), length, mode);
                        })};

                return offset + whole;
            }

            // A refused token is never passed to the sink, but the scan counts it, so its length is subtracted.
            std::size_t refused{0};

            // Passes each token to the caller's sink and stops the inner pass at the first token carrying an action,
            // a push onto the mode already being scanned included, so the outer loop resumes in the new mode. A pop
            // with nothing saved is a lexing error: the token is refused, unreported, and its length recorded.
            const auto emit{[&](const std::size_t token, const std::size_t length, const std::uint64_t packed) {
                if (packed == 0)
                {
                    sink(static_cast<T>(token), length, mode);

                    return true;
                }

                const auto action{unpack(packed)};

                if (action.kind == Mode_action_kind::pop)
                {
                    if (stack.saved.empty())
                    {
                        refused = length;

                        return false;
                    }

                    check(stack.saved.back());
                }

                sink(static_cast<T>(token), length, mode);

                stack.apply(action);

                return false;
            }};

            const auto consumed{
                    lexers_[mode].tokenize_all<std::size_t>(begin + static_cast<std::ptrdiff_t>(offset), end, emit)};

            offset += consumed - refused;

            if (refused != 0 || consumed == 0)
            {
                return offset;
            }
        }

        return offset;
    }

    /**
     * @brief Tokenizes a container, driving its own mode stack.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterable Container, typename Sink>
        requires std::invocable<Sink&, T, std::size_t, std::size_t>
    std::size_t tokenize_all(const Container& container, Sink sink) const
    {
        return tokenize_all<T>(std::ranges::begin(container), std::ranges::end(container), std::move(sink));
    }

    /**
     * @brief Tokenizes a container, reporting the mode and nesting depth the scan ended in.
     * @throws std::out_of_range If the stack's current mode is not a mode of this lexer, or a pop exposes a saved
     *         frame that is not.
     */
    template <common::concepts::Token_id T, common::concepts::Random_access_byte_iterable Container, typename Sink>
        requires std::invocable<Sink&, T, std::size_t, std::size_t>
    std::size_t tokenize_all(const Container& container, Sink sink, Mode_stack& stack) const
    {
        return tokenize_all<T>(std::ranges::begin(container), std::ranges::end(container), std::move(sink), stack);
    }

    /**
     * @brief The number of modes the grammar declared.
     */
    [[nodiscard]] std::size_t modes() const noexcept { return lexers_.size(); }

    /**
     * @brief The lexer compiled for one mode, for callers wanting its diagnostics or its own certificate.
     *
     * The certificate a single mode reports is sound only for input known to be scanned entirely in that mode. It
     * says nothing about a cut in a multi-mode scan, where the mode at the cut is exactly what is unknown.
     * @param mode The mode to inspect.
     */
    [[nodiscard]] const Lexer& mode(const std::size_t mode) const { return lexers_.at(mode); }

private:
    friend class Mode_builder;

    /**
     * @brief One token that changes the mode, and the mode it was registered in.
     */
    struct Registered
    {
        std::size_t mode{0};

        std::size_t token{0};

        Packed_action_t action{0};
    };

    /**
     * @brief Constructs a mode lexer from one compiled Lexer per mode.
     * @param lexers One Lexer per mode, indexed by mode, each carrying its tokens' actions as payloads.
     * @param actions Each mode's mode-changing tokens and their actions, none of them stay.
     */
    Mode_lexer(std::vector<Lexer> lexers, std::vector<Registered> actions)
        : lexers_{std::move(lexers)}, actions_{std::move(actions)}, acting_(lexers_.size(), 0)
    {
        for (const auto& registered : actions_)
        {
            acting_[registered.mode] = 1;
        }
    }

    /**
     * @brief Rejects a mode index this lexer does not have.
     *
     * The caller owns the stack, so nothing stops it carrying a mode index from another lexer, or a saved frame
     * poisoned by hand. Both would be indexed unchecked, and the batch path would take an out-of-range mode straight
     * into its no-actions branch. Only the current mode is checked on entry, and a saved frame just before the pop
     * exposing it, since scanning every frame per call made a run of pushes quadratic in the nesting depth. The test
     * is inline because every scanned token makes it; the throw beside it is not.
     * @param mode The mode index to test.
     * @throws std::out_of_range If the index names no mode of this lexer.
     */
    void check(const std::size_t mode) const
    {
        if (mode >= lexers_.size())
        {
            reject(mode);
        }
    }

    /**
     * @brief The action registered for a token in a mode, defaulting to stay.
     *
     * Only the per-token entry point needs it; the batch driver reads each action from the matched token's payload.
     * Defined out of line to keep its loop out of callers that inline aggressively.
     */
    [[nodiscard]] Mode_action action_of(std::size_t mode, std::size_t token) const noexcept;

    /**
     * @brief Throws for a mode index no mode lexer has.
     *
     * Held apart from check() so that its test inlines into the scanning loop while this throw stays out of it, and
     * static because the message needs the index alone.
     * @param mode The index that names no mode.
     * @throws std::out_of_range Always.
     */
    [[noreturn]] static void reject(std::size_t mode);

    /**
     * @brief One compiled Lexer per mode, each carrying its tokens' actions as accepting-state payloads.
     */
    std::vector<Lexer> lexers_;

    /**
     * @brief Every mode-changing token in the grammar, read only by the per-token entry point.
     *
     * One flat list rather than a list per mode: a grammar has a handful of these in total, so the scan is short,
     * and the batch driver never reads them.
     */
    std::vector<Registered> actions_;

    /**
     * @brief Whether each mode has any such token, derived from actions_ at construction and tested per token on
     *        the single-token path and once per mode change on the batch one, so held apart from the lists
     *        themselves; a byte per mode rather than a bit, since the test sits on the hot path.
     */
    std::vector<std::uint8_t> acting_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_LEXER_HPP

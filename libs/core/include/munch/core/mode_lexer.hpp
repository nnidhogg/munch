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
 * string literals with escapes, interpolation, heredocs and nested comments, none of which a single flat token set
 * expresses. Instances are only constructible through Mode_builder::build().
 *
 * @par Why there is no parallel entry point
 * Lexer certifies split points: bytes safe to cut at whatever precedes them, which is what lets a worker start
 * mid-input without guessing. That argument needs the scan's whole state to be recoverable from the cut position,
 * and here it is not. A worker landing on a byte would have to recover the mode and the whole saved stack as well,
 * and no single byte carries that: the same byte is a quote inside code and a terminator inside a string, and the
 * stack depth a nested comment reached is unbounded, so no finite certificate can name it. The proved obstruction is
 * narrower than "never": a SINGLE BYTE cannot identify the mode where two or more of them admit every byte, which is
 * sufficient for a safe cut but not necessary. A multi-byte
 * window, a checkpoint from an earlier pass, or a stackless go_to-only mode set are outside it. Parallel
 * tokenization is absent rather than present and unsound; use Lexer where the grammar admits a flat token set.
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
     * @brief Matches one token in the stack's current mode and applies its action.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @param begin Iterator to the beginning of the remaining input.
     * @param end Iterator to the end of the input.
     * @param stack The mode stack, advanced by the matched token's action.
     * @return The match. An empty token means no pattern matched, or the token's pop found nothing saved; the
     *         stack is unchanged in both cases, so a caller can report the position without losing context.
     */
    template <typename T, common::concepts::Iterator Iterator>
        requires(std::integral<T> || std::is_enum_v<T>)
    [[nodiscard]] Match<T> tokenize(Iterator begin, Iterator end, Mode_stack& stack) const
    {
        verify(stack);

        const auto [token, length]{lexers_[stack.current].template tokenize<std::size_t>(begin, end)};

        if (!token)
        {
            return {};
        }

        if (!stack.apply(action_of(stack.current, *token)))
        {
            return {};
        }

        return {.token = static_cast<T>(*token), .length = length};
    }

    /**
     * @brief Tokenizes the whole input, driving its own mode stack.
     *
     * Stays inside one mode's batch scan for as long as the mode does not change, rather than re-entering the
     * scanner once per token. Lexer::tokenize_all()'s sink may halt the scan by returning false, so a mode-changing
     * token ends the inner pass and the outer loop resumes in the new mode. Most tokens leave the mode alone, so
     * most of the input is scanned in the tight loop; driving one token at a time costs 12 to 18 percent even for a
     * plain Lexer, and this recovers it.
     * @tparam T The token type (enum or integral).
     * @tparam Iterator The input iterator type.
     * @tparam Sink Callable receiving each matched token, its length, and the mode it matched in.
     * @param begin Iterator to the beginning of the input.
     * @param end Iterator to the end of the input.
     * @param sink Invoked as sink(token, length, mode) for every matched token, in input order.
     * @return The number of input elements tokenized; anything short of the input's size means no token matched at
     *         the returned offset, or a pop found nothing saved there.
     */
    template <typename T, std::random_access_iterator Iterator, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink, T, std::size_t, std::size_t>
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
     */
    template <typename T, std::random_access_iterator Iterator, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink, T, std::size_t, std::size_t>
    std::size_t tokenize_all(Iterator begin, Iterator end, Sink sink, Mode_stack& stack) const
    {
        verify(stack);

        std::size_t offset{0};

        const auto size{static_cast<std::size_t>(std::distance(begin, end))};

        while (offset < size)
        {
            const auto mode{stack.current};

            // A mode nothing can leave needs no halt condition, so its sink returns void.
            const auto& shape{dispatch_[mode]};

            if (shape.shape == Mode_dispatch::Shape::none)
            {
                const auto whole{lexers_[mode].template tokenize_all<std::size_t>(
                        begin + static_cast<std::ptrdiff_t>(offset), end,
                        [&sink, mode](const std::size_t token, const std::size_t length) {
                            sink(static_cast<T>(token), length, mode);
                        })};

                return offset + whole;
            }

            // A refused token is never passed to the sink, but the scan counts it, so its length is subtracted.
            std::size_t refused{0};

            // The shape is fixed for the whole of this pass, so the two lookups are separate instantiations rather
            // than a branch inside the sink. Testing it per token cost more than the lookup it selected.
            const auto scan{[&](auto&& resolve) {
                return lexers_[mode].template tokenize_all<std::size_t>(
                        begin + static_cast<std::ptrdiff_t>(offset), end,
                        [&sink, &stack, &refused, mode, resolve](const std::size_t token, const std::size_t length) {
                            const auto action{resolve(token)};

                            if (action.kind == Mode_action_kind::stay)
                            {
                                sink(static_cast<T>(token), length, mode);

                                return true;
                            }

                            if (action.kind == Mode_action_kind::pop && stack.saved.empty())
                            {
                                refused = length;

                                return false;
                            }

                            sink(static_cast<T>(token), length, mode);

                            stack.apply(action);

                            // Continuing on a same-mode action gains 11% at depth 16 and costs 5% at depth 1.
                            return false;
                        });
            }};

            const auto consumed{
                    shape.shape == Mode_dispatch::Shape::one ?
                            scan([single = shape.token, action = shape.action](const std::size_t token) {
                                return token == single ? action : Mode_action{};
                            }) :
                            scan([mask = shape.shape == Mode_dispatch::Shape::masked ? shape.mask : ~std::uint64_t{0},
                                  pairs = pairs_.data() + shape.first, count = shape.count](const std::size_t token) {
                                if (token < 64 && (mask >> token & 1U) == 0U)
                                {
                                    return Mode_action{};
                                }

                                for (std::size_t at{0}; at < count; ++at)
                                {
                                    if (pairs[at].first == token)
                                    {
                                        return pairs[at].second;
                                    }
                                }

                                return Mode_action{};
                            })};

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
    template <typename T, common::concepts::Random_access_iterable Container, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink, T, std::size_t, std::size_t>
    std::size_t tokenize_all(const Container& container, Sink sink) const
    {
        return tokenize_all<T>(std::ranges::begin(container), std::ranges::end(container), std::move(sink));
    }

    /**
     * @brief Tokenizes a container, reporting the mode and nesting depth the scan ended in.
     */
    template <typename T, common::concepts::Random_access_iterable Container, typename Sink>
        requires(std::integral<T> || std::is_enum_v<T>) && std::invocable<Sink, T, std::size_t, std::size_t>
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
     * @brief Rejects a stack naming a mode this lexer does not have.
     *
     * The caller owns the stack, so nothing stops it carrying a mode index from another lexer, or a saved frame
     * poisoned by hand. Both would be indexed unchecked, and the batch path would take an out-of-range mode straight
     * into its no-actions branch. Checked once per call rather than per token: build() has already validated every
     * target the driver itself can install, so only what the caller supplied can be wrong.
     * @throws std::out_of_range If the current mode or any saved frame is not a mode of this lexer.
     */
    void verify(const Mode_stack& stack) const
    {
        const auto bad{[this](const std::size_t mode) { return mode >= lexers_.size(); }};

        if (bad(stack.current) || std::ranges::any_of(stack.saved, bad))
        {
            throw std::out_of_range{"Mode_lexer: the mode stack names a mode this lexer does not have"};
        }
    }

    /**
     * @brief Constructs a mode lexer from one compiled Lexer per mode and a flat action table.
     * @param lexers One Lexer per mode, indexed by mode.
     * @param actions Actions at index mode * stride + token, or empty when no token changes the mode.
     * @param stride The row width of that table, one past the largest token ID carrying an action.
     */
    Mode_lexer(
            std::vector<Lexer> lexers, std::vector<Mode_dispatch> dispatch,
            std::vector<std::pair<std::size_t, Mode_action>> pairs)
        : lexers_{std::move(lexers)}, dispatch_{std::move(dispatch)}, pairs_{std::move(pairs)}
    {}

    /**
     * @brief The action registered for a token in a mode, defaulting to stay.
     *
     * One indexed load into a flat table rather than two into a vector of vectors. This runs once per token, so the
     * second indirection and its bounds check were measurable: most tokens leave the mode alone and the lookup is
     * pure overhead for them.
     */
    [[nodiscard]] Mode_action action_of(const std::size_t mode, const std::size_t token) const noexcept
    {
        if (mode >= dispatch_.size())
        {
            return {};
        }

        const auto& shape{dispatch_[mode]};

        if (shape.shape == Mode_dispatch::Shape::one)
        {
            return token == shape.token ? shape.action : Mode_action{};
        }

        if (shape.shape == Mode_dispatch::Shape::masked && (token >= 64 || (shape.mask >> token & 1U) == 0U))
        {
            return {};
        }

        for (std::size_t at{shape.first}; at < shape.first + shape.count; ++at)
        {
            if (pairs_[at].first == token)
            {
                return pairs_[at].second;
            }
        }

        return {};
    }

    /**
     * @brief One compiled Lexer per mode.
     */
    std::vector<Lexer> lexers_;

    /**
     * @brief How each mode's actions are looked up, one entry per mode.
     */
    std::vector<Mode_dispatch> dispatch_;

    /**
     * @brief Every mode's (token ID, action) pairs, in per-mode ranges named by Mode_dispatch.
     *
     * Sized by how many action tokens exist rather than by the largest token ID, so a sparse public enumeration
     * costs nothing.
     */
    std::vector<std::pair<std::size_t, Mode_action>> pairs_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_LEXER_HPP

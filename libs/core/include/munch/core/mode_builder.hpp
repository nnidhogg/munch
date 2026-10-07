#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP

#include <concepts>
#include <cstddef>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "munch/common/concepts.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/mode.hpp"
#include "munch/core/mode_lexer.hpp"
#include "munch/regex/regex.hpp"

namespace munch::core
{
/**
 * @brief Builds a Mode_lexer: one token set per mode, plus what each token does to the mode stack.
 *
 * Each mode compiles through the ordinary Builder, so determinization, minimization, longest match and priority
 * resolution are the same machinery a flat grammar uses, applied once per mode. Modes are dense indices starting at
 * zero, and mode 0 is where a scan begins.
 */
class Mode_builder
{
public:
    /**
     * @brief Diagnoses every mode, plus the faults only a modal grammar can have.
     *
     * Each mode is diagnosed by the ordinary Builder, so a token dead in one mode is reported against that mode rather
     * than against the grammar as a whole: a token can be legitimately dead in four modes and live in the fifth, which
     * a merged report would drown.
     */
    struct Mode_diagnostics
    {
        /**
         * @brief One Builder::Diagnostics per mode, indexed by mode.
         */
        std::vector<Builder::Diagnostics> per_mode{};

        /**
         * @brief Modes no token can reach from mode 0 with an initially empty stack, in ascending order, excluding mode
         *        0 itself where scanning starts.
         *
         * A mode nothing enters is a grammar fault the per-mode reports cannot see, since each of them is complete and
         * consistent on its own. The judgment is about the grammar's own transitions; a caller-seeded Mode_stack can
         * start a scan inside any mode regardless.
         */
        std::vector<std::size_t> unreachable_modes{};

        /**
         * @brief Modes with neither a live non-self push or go_to nor a live pop for which the default-start grammar
         *        can establish a frame naming another mode, in ascending order.
         *
         * The two exits are judged differently: a live non-self push or go_to makes a mode escapable by itself, even
         * when nothing reaches the mode, while pop escapability depends on the frames a scan from mode 0 with an
         * initially empty stack can establish. A caller-supplied frame may provide the missing return context, but only
         * where such a live pop exists; no frame helps a stay-only mode. Being inescapable is legitimate for a mode
         * meant to consume the rest of the input, and a mistake everywhere else, so it is reported rather than
         * rejected.
         */
        std::vector<std::size_t> inescapable_modes{};
    };

    /**
     * @brief Registers a token in one mode.
     * @tparam M The mode type (enum or integral).
     * @tparam T The token type (enum or integral).
     * @param mode The mode the pattern is legal in.
     * @param regex The regex pattern for the token.
     * @param token The token value.
     * @param priority The priority for resolving conflicts within this mode (lower is higher priority).
     * @param action What the token does to the mode stack once matched; tokens stay by default. A go_to or push target
     *        may name a mode not yet registered; build() checks it once every mode is known.
     * @throws std::invalid_argument If the action kind is not one of the four, if the mode or the token is negative or
     *         the largest representable index, or if this token was already registered in this mode with a different
     *         action.
     */
    template <common::concepts::Token_id M, common::concepts::Token_id T>
    void add_token(
            const M mode, const regex::Regex& regex, const T token, const std::size_t priority,
            const Mode_action action = {})
    {
        const auto index{as_index(mode, "mode")};

        const auto id{as_index(token, "token")};

        register_token(index, regex, id, priority, action);
    }

    /**
     * @brief Caps how many DFA states determinization may discover, applied to each mode separately.
     *
     * The cap is per mode rather than aggregate: a grammar with five modes may therefore discover up to five times the
     * limit in total, which a caller bounding untrusted input should account for.
     * @param limit The per-mode cap; zero, the default, means unlimited.
     */
    void set_state_limit(const std::size_t limit) noexcept { state_limit_ = limit; }

    /**
     * @brief Builds the mode lexer.
     * @return The constructed Mode_lexer.
     * @throws State_limit_error If any mode's determinization exceeds the cap.
     * @throws std::invalid_argument If no token was registered, a mode index was skipped, an action targets a mode that
     *         does not exist, or a token whose empty match would win carries an action.
     */
    [[nodiscard]] Mode_lexer build() const;

    /**
     * @brief Returns the number of modes: one more than the highest mode index tokens have been registered in.
     * @return The number of modes.
     */
    [[nodiscard]] std::size_t modes() const noexcept { return modes_.size(); }

    /**
     * @brief Diagnoses the registered grammar; see Mode_diagnostics.
     * @return The per-mode diagnostics, the unreachable modes and the inescapable modes.
     * @throws State_limit_error If any mode's determinization exceeds the cap.
     */
    [[nodiscard]] Mode_diagnostics diagnose() const;

private:
    /**
     * @brief Registers a token in a mode by index: the validation and the bookkeeping add_token() forwards to.
     * @param index The mode's index.
     * @param regex The token's pattern.
     * @param id The token's index.
     * @param priority The priority for resolving conflicts within the mode.
     * @param action What the token does to the mode stack once matched, normalized here.
     * @throws std::invalid_argument If the action kind is not one of the four, or if this token was already registered
     *         in this mode with a different action.
     */
    void register_token(
            std::size_t index, const regex::Regex& regex, std::size_t id, std::size_t priority, Mode_action action);

    /**
     * @brief Converts a caller's mode or token value to an index, rejecting what cannot survive the conversion.
     *
     * A negative value becomes an enormous unsigned one, and the `+ 1` used to size the per-mode rows then wraps to
     * zero, so the very next index is out of bounds on an empty vector. Caught here rather than discovered there.
     * @tparam V The caller's mode or token type, an enum or an integral type.
     * @param value The value to convert.
     * @param what What the value names, for the message of the rejection.
     * @return The index.
     * @throws std::invalid_argument If the value is negative or beyond what an index can hold.
     */
    template <typename V>
    [[nodiscard]] static std::size_t as_index(const V value, const std::string_view what)
    {
        // An enum converts through its underlying type.
        if constexpr (std::is_enum_v<V>)
        {
            return as_index(std::to_underlying(value), what);
        }

        if constexpr (std::is_signed_v<V>)
        {
            if (value < 0)
            {
                const auto message{std::format("Mode_builder::add_token: negative {}", what)};

                throw std::invalid_argument{message};
            }
        }

        const auto index{static_cast<std::size_t>(value)};

        // Sizing a row needs index + 1, so the largest representable value cannot be admitted either.
        if (index == std::numeric_limits<std::size_t>::max())
        {
            const auto message{std::format("Mode_builder::add_token: {} is not representable as an index", what)};

            throw std::invalid_argument{message};
        }

        return index;
    }

    /**
     * @brief One builder per mode, indexed by mode.
     */
    std::vector<Builder> modes_;

    /**
     * @brief The per-mode determinization cap; zero means unlimited.
     */
    std::size_t state_limit_{0};

    /**
     * @brief Every registered token and its normalized action, per mode.
     *
     * A list of what was registered rather than a row indexed by token ID, so a sparse numbering costs nothing; it also
     * answers whether a token was declared at all, which a table of non-stay actions cannot.
     */
    std::vector<std::vector<std::pair<std::size_t, Mode_action>>> registered_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP

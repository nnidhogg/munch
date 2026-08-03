#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

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
 * resolution are the same machinery a flat grammar uses, applied once per mode. Modes are dense indices starting
 * at zero, and mode 0 is where a scan begins.
 */
class Mode_builder
{
public:
    /**
     * @brief Registers a token in one mode.
     * @tparam M The mode type (enum or integral).
     * @tparam T The token type (enum or integral).
     * @param mode The mode the pattern is legal in.
     * @param regex The regex pattern for the token.
     * @param token The token value.
     * @param priority The priority for resolving conflicts within this mode (lower is higher priority).
     * @param action What the token does to the mode stack once matched; tokens stay by default. A go_to or push
     *        target may name a mode not yet registered; build() checks it once every mode is known.
     * @throws std::invalid_argument If this token was already registered in this mode with a different action.
     */
    template <typename M, typename T>
        requires(std::integral<M> || std::is_enum_v<M>) && (std::integral<T> || std::is_enum_v<T>)
    void add_token(
            const M mode, const regex::Regex& regex, const T token, const std::size_t priority,
            const Mode_action action = {})
    {
        const auto index{as_index(mode, "mode")};

        const auto id{as_index(token, "token")};

        // An action kind outside the enumeration reaches apply(), which rejects it, while the batch driver ignores
        // that rejection: the two drivers would disagree on the same input.
        switch (action.kind)
        {
        case Mode_action_kind::stay:
        case Mode_action_kind::go_to:
        case Mode_action_kind::push:
        case Mode_action_kind::pop:
            break;

        default:
            throw std::invalid_argument{"Mode_builder::add_token: the action kind is not one of the four"};
        }

        // A target is only meaningful for the two kinds that name one; stay and pop document it as ignored, so it is
        // normalized here rather than allowed to make two otherwise identical actions compare unequal. A go_to onto
        // the mode it was registered in is observably a stay, and saying so lets a mode whose only action is that one
        // take the driver's no-action path.
        const auto targeted{action.kind == Mode_action_kind::go_to || action.kind == Mode_action_kind::push};

        const auto self_go_to{action.kind == Mode_action_kind::go_to && action.target == index};

        const Mode_action normalized{
                .kind = self_go_to ? Mode_action_kind::stay : action.kind,
                .target = targeted && !self_go_to ? action.target : std::size_t{0}};

        // Patterns may share a token ID; conflicting actions for it cannot, since the scanner reports only the ID.
        // Checked before anything is resized or registered, so a caught exception leaves the builder as it was.
        if (index < registered_.size())
        {
            for (const auto& [declared, previous] : registered_[index])
            {
                if (declared == id && (previous.kind != normalized.kind || previous.target != normalized.target))
                {
                    throw std::invalid_argument{
                            "Mode_builder::add_token: token " + std::to_string(id) + " in mode " +
                            std::to_string(index) + " already carries a different action"};
                }
            }
        }

        if (index >= modes_.size())
        {
            modes_.resize(index + 1);

            registered_.resize(index + 1);

            populated_.resize(index + 1, false);
        }

        modes_[index].add_token(regex, id, priority);

        populated_[index] = true;

        if (!std::ranges::any_of(registered_[index], [id](const auto& pair) { return pair.first == id; }))
        {
            registered_[index].emplace_back(id, normalized);
        }
    }

    /**
     * @brief Caps how many DFA states determinization may discover, applied to each mode separately.
     *
     * The cap is per mode rather than aggregate: a grammar with five modes may therefore discover up to five times
     * the limit in total, which a caller bounding untrusted input should account for.
     * @param limit The per-mode cap; zero, the default, means unlimited.
     */
    void set_state_limit(const std::size_t limit) noexcept { state_limit_ = limit; }

    /**
     * @brief Builds the mode lexer.
     * @return The constructed Mode_lexer.
     * @throws State_limit_error If any mode's determinization exceeds the cap.
     * @throws std::invalid_argument If no token was registered, a mode index was skipped, or an action targets a
     *         mode that does not exist.
     */
    [[nodiscard]] Mode_lexer build() const;

    /**
     * @brief The number of modes tokens have been registered in.
     */
    [[nodiscard]] std::size_t modes() const noexcept { return modes_.size(); }

    /**
     * @brief Diagnoses every mode, plus the faults only a modal grammar can have.
     *
     * Each mode is diagnosed by the ordinary Builder, so a token dead in one mode is reported against that mode
     * rather than against the grammar as a whole: a token can be legitimately dead in four modes and live in the
     * fifth, which a merged report would drown.
     */
    struct Mode_diagnostics
    {
        /**
         * @brief One Builder::Diagnostics per mode, indexed by mode.
         */
        std::vector<Builder::Diagnostics> per_mode;

        /**
         * @brief Modes no token can reach, in ascending order, excluding mode 0 where scanning starts.
         *
         * A mode nothing enters is a grammar fault the per-mode reports cannot see, since each of them is complete
         * and consistent on its own.
         */
        std::vector<std::size_t> unreachable_modes;

        /**
         * @brief Modes carrying no token that leaves them, in ascending order.
         *
         * Entering one is a one-way trip. That is legitimate for a mode meant to consume the rest of the input, and
         * a mistake everywhere else, so it is reported rather than rejected.
         */
        std::vector<std::size_t> inescapable_modes;
    };

    /**
     * @brief Diagnoses the registered grammar; see Mode_diagnostics.
     * @throws State_limit_error If any mode's determinization exceeds the cap.
     */
    [[nodiscard]] Mode_diagnostics diagnose() const;

private:
    /**
     * @brief Converts a caller's mode or token value to an index, rejecting what cannot survive the conversion.
     *
     * A negative value becomes an enormous unsigned one, and the `+ 1` used to size the per-mode rows then wraps to
     * zero, so the very next index is out of bounds on an empty vector. Caught here rather than discovered there.
     */
    template <typename V>
    [[nodiscard]] static std::size_t as_index(const V value, const char* const what)
    {
        // An enum is dispatched through its underlying type rather than resolved with conditional_t, which would
        // instantiate underlying_type for the integral case where it does not exist.
        if constexpr (std::is_enum_v<V>)
        {
            return as_index(static_cast<std::underlying_type_t<V>>(value), what);
        }
        else
        {
            if constexpr (std::is_signed_v<V>)
            {
                if (value < 0)
                {
                    throw std::invalid_argument{std::string{"Mode_builder::add_token: negative "} + what};
                }
            }

            const auto index{static_cast<std::size_t>(value)};

            // Sizing a row needs index + 1, so the largest representable value cannot be admitted either.
            if (index == static_cast<std::size_t>(-1))
            {
                throw std::invalid_argument{
                        std::string{"Mode_builder::add_token: "} + what + " is not representable as an index"};
            }

            return index;
        }
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
     * @brief Whether each mode index received at least one token, so build() can reject a skipped mode rather
     *        than compile a lexer for it that matches nothing.
     */
    std::vector<bool> populated_;

    /**
     * @brief Every registered token and its normalized action, per mode.
     *
     * A list of what was actually registered rather than a row indexed by token ID: the row had to be sized to the
     * largest ID, so a grammar numbering a token 70000 allocated a megabyte of empty entries at construction. It
     * also answers whether a token was declared at all, which a table of non-stay actions cannot.
     */
    std::vector<std::vector<std::pair<std::size_t, Mode_action>>> registered_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_BUILDER_HPP

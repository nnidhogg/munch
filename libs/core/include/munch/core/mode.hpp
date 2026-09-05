#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace munch::core
{
/**
 * @brief What a token does to the mode stack once it has matched.
 *
 * A lexer mode is a separate token set, selected by the scan's own history: the tokens legal inside a string
 * literal are not the tokens legal outside one. The four actions are the whole vocabulary needed to express the
 * constructs that motivate modes, and nesting comes from the stack rather than from a fifth action.
 */
enum class Mode_action_kind : std::size_t
{
    /**
     * @brief Leaves the mode unchanged. The default, and what ordinary tokens do.
     */
    stay,

    /**
     * @brief Replaces the current mode, discarding nothing. A one-way switch with no return.
     */
    go_to,

    /**
     * @brief Pushes the current mode and enters the target, so a later pop returns here.
     */
    push,

    /**
     * @brief Returns to the mode the matching push saved. Popping an empty stack is a lexing error.
     */
    pop
};

/**
 * @brief A token's effect on the mode stack.
 */
struct Mode_action
{
    /**
     * @brief Which of the four effects to apply.
     */
    Mode_action_kind kind{Mode_action_kind::stay};

    /**
     * @brief The mode entered by go_to and push, ignored by stay and pop.
     */
    std::size_t target{0};
};

/**
 * @brief A Mode_action packed into one word, with stay as zero.
 *
 * Every token's action travels as the payload of its own accepting states, so the batch driver receives it as
 * accepting-state payload while the per-token driver looks it up by token ID. One word is what that channel carries,
 * and making stay zero lets the common case, a token that leaves the mode alone, be a test against zero.
 */
using Packed_action = std::uint64_t;

/**
 * @brief Packs an action, mapping every stay to zero whatever target it names.
 */
[[nodiscard]] constexpr Packed_action pack(const Mode_action& action) noexcept
{
    return action.kind == Mode_action_kind::stay ?
                   0 :
                   static_cast<Packed_action>(action.kind) | static_cast<Packed_action>(action.target) << 2U;
}

/**
 * @brief Unpacks an action, which both drivers do before applying it: the batch driver on every nonzero payload and
 *        the per-token driver on every registered action, a pop an empty stack goes on to refuse included.
 */
[[nodiscard]] constexpr Mode_action unpack(const Packed_action packed) noexcept
{
    return {.kind = static_cast<Mode_action_kind>(packed & 3U), .target = static_cast<std::size_t>(packed >> 2U)};
}

/**
 * @brief The scan position's mode, owned by the caller rather than by the lexer.
 *
 * Mode_lexer is const and holds no scan state, exactly as Lexer does, so the stack lives here and is threaded
 * through the calls. That keeps one compiled lexer usable from many threads, and keeps a mode-aware scan
 * resumable: a caller holding this value holds everything the next call needs.
 */
struct Mode_stack
{
    /**
     * @brief Equal when the current mode and every saved frame match.
     */
    bool operator==(const Mode_stack&) const = default;

    /**
     * @brief Applies an action, reporting whether it was legal.
     * @param action The matched token's action.
     * @return False if the action was a pop with nothing saved, in which case the stack is left unchanged.
     */
    bool apply(const Mode_action& action);

    /**
     * @brief The mode the next token is matched in.
     */
    std::size_t current{0};

    /**
     * @brief The modes push saved, innermost last.
     */
    std::vector<std::size_t> saved{};
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_HPP

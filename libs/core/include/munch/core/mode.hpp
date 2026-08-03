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
 * @brief How one mode's token actions are looked up, chosen per mode by how many there are.
 *
 * A dense row indexed by token ID answered every lookup with a dependent load, and its width was the largest token
 * ID registered, so one parser-style code near 2^16 sized the whole table. Almost every mode carries one or two
 * action tokens, which a comparison answers without touching memory at all.
 */
struct Mode_dispatch
{
    /**
     * @brief How many action tokens this mode has, which selects the lookup below.
     */
    enum class Shape : std::size_t
    {
        /**
         * @brief No token changes the mode; the driver scans it with a sink that cannot stop.
         */
        none,

        /**
         * @brief Exactly one; one comparison against a token ID held in a register.
         */
        one,

        /**
         * @brief Several, all with IDs under 64; a bit test rejects the common case before any search.
         */
        masked,

        /**
         * @brief Several, at least one ID too large to mask; the range is searched directly.
         */
        sparse
    };

    Shape shape{Shape::none};

    /**
     * @brief The single action token's ID, when shape is one.
     */
    std::size_t token{0};

    /**
     * @brief The single token's action, when shape is one. Held inline so the hit path loads nothing either.
     */
    Mode_action action{};

    /**
     * @brief A bit per token ID under 64, when shape is masked.
     */
    std::uint64_t mask{0};

    /**
     * @brief Where this mode's (token, action) pairs begin in the shared table, when shape is masked or sparse.
     */
    std::size_t first{0};

    /**
     * @brief How many pairs this mode has there.
     */
    std::size_t count{0};
};

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
     * @brief The mode the next token is matched in.
     */
    std::size_t current{0};

    /**
     * @brief The modes push saved, innermost last.
     */
    std::vector<std::size_t> saved{};

    /**
     * @brief Applies an action, reporting whether it was legal.
     * @param action The matched token's action.
     * @return False if the action was a pop with nothing saved, in which case the stack is left unchanged.
     */
    bool apply(const Mode_action& action)
    {
        switch (action.kind)
        {
        case Mode_action_kind::stay:
            return true;

        case Mode_action_kind::go_to:
            current = action.target;

            return true;

        case Mode_action_kind::push:
            saved.push_back(current);

            current = action.target;

            return true;

        case Mode_action_kind::pop:
            if (saved.empty())
            {
                return false;
            }

            current = saved.back();

            saved.pop_back();

            return true;
        }

        return false;
    }

    bool operator==(const Mode_stack&) const = default;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_MODE_HPP

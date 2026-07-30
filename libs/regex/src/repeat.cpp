#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
namespace
{
/**
 * @brief Wraps a single regex in the vector a Repeat node stores its sub-pattern in.
 */
/**
 * @brief Builds the NFA for a Kleene star (zero or more) repetition.
 * @param regex The sub-pattern to repeat.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_kleene(const Regex& regex)
{
    /**
     * Matches zero or more occurrences of a sub-pattern.
     *
     *       / <--------ε-------- \
     *      /                      \
     * ((S)) --ε--> ((regex)) --ε-->
     */
    auto S{to_nfa(regex).prepend_init_state()};

    std::ranges::for_each(
            S.accept_states(), [&S](const auto& pair) { S.add_epsilon_transition(pair.first, S.init_state()); });

    S.add_accept_state(S.init_state());

    return S;
}

/**
 * @brief Builds the NFA for a Kleene plus (one or more) repetition.
 * @param regex The sub-pattern to repeat.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_plus(const Regex& regex)
{
    /**
     * Matches one or more occurrences of a sub-pattern.
     *
     *     / <--------ε-------- \
     *    /                      \
     * (S) --ε--> ((regex)) --ε-->
     */
    auto S{to_nfa(regex).prepend_init_state()};

    std::ranges::for_each(
            S.accept_states(), [&S](const auto& pair) { S.add_epsilon_transition(pair.first, S.init_state()); });

    return S;
}

/**
 * @brief Builds the NFA for an optional (zero or one) repetition.
 * @param regex The sub-pattern to make optional.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_optional(const Regex& regex)
{
    /**
     * Matches zero or one occurrences of a sub-pattern.
     *
     * ((S)) --ε--> ((regex))
     */
    auto S{to_nfa(regex).prepend_init_state()};

    S.add_accept_state(S.init_state());

    return S;
}

/**
 * @brief Builds the NFA for an exact repetition.
 * @param regex The sub-pattern to repeat.
 * @param count The exact number of repetitions.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_exact(const Regex& regex, const std::size_t count)
{
    /**
     * Matches an exact number of occurrences of a sub-pattern.
     *
     * (S) --ε--> ... --ε--> ((regex n))
     */
    nfa::Builder S;

    S.add_accept_state(S.init_state());

    std::ranges::for_each(std::ranges::iota_view(static_cast<std::size_t>(0), count), [&regex, &S](auto) {
        S = S.append(to_nfa(regex));
    });

    return S;
}

/**
 * @brief Builds the NFA for a lower-bound repetition.
 * @param regex The sub-pattern to repeat.
 * @param min The minimum number of repetitions.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_at_least(const Regex& regex, const std::size_t min)
{
    /**
     * Matches a range of occurrences of a sub-pattern.
     *
     *                 / <-----ε----- \
     *                /                \
     * (S) --ε--> ... ((regex n)) --ε-->
     *
     * At least zero occurrences is the Kleene star; branching keeps the iota below well-formed, as its bound may not
     * lie before its start.
     */
    if (min == 0)
    {
        return to_kleene(regex);
    }

    nfa::Builder S;

    S.add_accept_state(S.init_state());

    std::ranges::for_each(
            std::views::iota(static_cast<std::size_t>(1), min), [&regex, &S](auto) { S = S.append(to_nfa(regex)); });

    auto F{to_nfa(regex)};

    std::ranges::for_each(std::views::keys(F.accept_states()), [&F](const auto state) {
        F.add_epsilon_transition(state, F.init_state());
    });

    return S.append(F);
}

/**
 * @brief Builds the NFA for a bounded repetition.
 * @param regex The sub-pattern to repeat.
 * @param min The minimum number of repetitions.
 * @param max The maximum number of repetitions.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_range(const Regex& regex, const std::size_t min, const std::size_t max)
{
    /**
     * Matches a range of occurrences of a sub-pattern.
     *
     * (S) --ε--> ... (regex n) --ε--> ... --ε--> ((regex m))
     *                         \          \                 /
     *                          \          \ ------ε-----> /
     *                           \                        /
     *                            \ ---------ε---------> /
     */
    nfa::Builder S;

    S.add_accept_state(S.init_state());

    std::ranges::for_each(
            std::views::iota(static_cast<std::size_t>(0), min), [&regex, &S](auto) { S = S.append(to_nfa(regex)); });

    nfa::Nfa::States_t pending;

    std::ranges::for_each(std::views::iota(min, max), [&regex, &S, &pending](auto) {
        std::ranges::copy(std::views::keys(S.accept_states()), std::inserter(pending, pending.end()));
        S = S.append(to_nfa(regex));
    });

    std::ranges::for_each(pending, [&S](const auto pending_state) {
        std::ranges::for_each(std::views::keys(S.accept_states()), [&S, pending_state](const auto accept_state) {
            S.add_epsilon_transition(pending_state, accept_state);
        });
    });

    return S;
}

} // namespace

nfa::Builder to_nfa(const Repeat& repeat)
{
    // The child needs no emptiness check: a Box always holds exactly one value.
    const auto& regex{*repeat.regex};

    return std::visit(
            [&regex]<typename T>(const T& kind) {
                if constexpr (std::is_same_v<T, Kleene>)
                {
                    return to_kleene(regex);
                }
                else if constexpr (std::is_same_v<T, Plus>)
                {
                    return to_plus(regex);
                }
                else if constexpr (std::is_same_v<T, Optional>)
                {
                    return to_optional(regex);
                }
                else if constexpr (std::is_same_v<T, Exact>)
                {
                    return to_exact(regex, kind.count);
                }
                else if constexpr (std::is_same_v<T, At_least>)
                {
                    return to_at_least(regex, kind.min);
                }
                else
                {
                    // Adding a repetition kind without handling it above is a compile error rather than a silent
                    // fall-through returning nothing.
                    static_assert(std::is_same_v<T, Range>, "Unhandled repetition kind");

                    return to_range(regex, kind.min, kind.max);
                }
            },
            repeat.kind);
}

Regex kleene(Regex regex)
{
    return {.node = Repeat{.kind = Kleene{}, .regex = Box{std::move(regex)}}};
}

Regex plus(Regex regex)
{
    return {.node = Repeat{.kind = Plus{}, .regex = Box{std::move(regex)}}};
}

Regex optional(Regex regex)
{
    return {.node = Repeat{.kind = Optional{}, .regex = Box{std::move(regex)}}};
}

Regex exact(Regex regex, const std::size_t count)
{
    return {.node = Repeat{.kind = Exact{.count = count}, .regex = Box{std::move(regex)}}};
}

Regex at_least(Regex regex, const std::size_t min)
{
    return {.node = Repeat{.kind = At_least{.min = min}, .regex = Box{std::move(regex)}}};
}

Regex range(Regex regex, const std::size_t min, const std::size_t max)
{
    if (max < min)
    {
        throw std::invalid_argument("A repetition range may not end before it starts");
    }

    return {.node = Repeat{.kind = Range{.min = min, .max = max}, .regex = Box{std::move(regex)}}};
}

} // namespace munch::regex

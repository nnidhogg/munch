#include <cstddef>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <variant>

#include "munch/regex/regex.hpp"

namespace munch::regex
{
namespace
{
/**
 * @brief Loops every accepting state of an automaton back to its initial state with an epsilon transition.
 * @param nfa The automaton.
 * @return The automaton with the loops added.
 */
[[nodiscard]] nfa::Builder loop_back(nfa::Builder nfa)
{
    for (const auto state : std::views::keys(nfa.accept_states()))
    {
        nfa.add_epsilon_transition(state, nfa.init_state());
    }

    return nfa;
}

/**
 * @brief Appends copies of a sub-pattern's automaton to an automaton, one after another.
 * @param nfa The automaton appended to.
 * @param regex The sub-pattern copied.
 * @param copies The number of copies.
 * @return The automaton followed by the copies.
 */
[[nodiscard]] nfa::Builder append_copies(nfa::Builder nfa, const Regex& regex, const std::size_t copies)
{
    for (std::size_t copy{0}; copy < copies; ++copy)
    {
        const auto body{to_nfa(regex)};

        nfa = nfa.append(body);
    }

    return nfa;
}

/**
 * @brief Builds the NFA for a Kleene star (zero or more) repetition.
 * @param regex The sub-pattern to repeat.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, Kleene)
{
    // Matches zero or more occurrences of a sub-pattern:
    //
    //         +-----------ε-----------+
    //         v                       |
    // ((nfa)) --ε--> ((regex)) --ε----+
    const auto body{to_nfa(regex)};

    const auto prepended{body.prepend_init_state()};

    auto nfa{loop_back(prepended)};

    nfa.add_accept_state(nfa.init_state());

    return nfa;
}

/**
 * @brief Builds the NFA for a Kleene plus (one or more) repetition.
 * @param regex The sub-pattern to repeat.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, Plus)
{
    // Matches one or more occurrences of a sub-pattern:
    //
    //       +-----------ε-----------+
    //       v                       |
    // (nfa) --ε--> ((regex)) --ε----+
    const auto body{to_nfa(regex)};

    const auto prepended{body.prepend_init_state()};

    return loop_back(prepended);
}

/**
 * @brief Builds the NFA for an optional (zero or one) repetition.
 * @param regex The sub-pattern to make optional.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, Optional)
{
    // Matches zero or one occurrences of a sub-pattern:
    //
    // ((nfa)) --ε--> ((regex))
    const auto body{to_nfa(regex)};

    auto nfa{body.prepend_init_state()};

    nfa.add_accept_state(nfa.init_state());

    return nfa;
}

/**
 * @brief Builds the NFA for an exact repetition.
 * @param regex The sub-pattern to repeat.
 * @param exact The repetition, carrying its count.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, const Exact& exact)
{
    // Matches an exact number of occurrences of a sub-pattern:
    //
    // (nfa) --ε--> ... --ε--> ((regex n))
    nfa::Builder nfa{};

    nfa.add_accept_state(nfa.init_state());

    return append_copies(nfa, regex, exact.count);
}

/**
 * @brief Builds the NFA for a lower-bound repetition.
 * @param regex The sub-pattern to repeat.
 * @param at_least The repetition, carrying its minimum.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, const At_least& at_least)
{
    // Matches at least `min` occurrences of a sub-pattern:
    //
    //                  +--------ε--------+
    //                  v                 |
    // (nfa) --ε--> ... ((regex n)) --ε---+
    //
    // At least zero occurrences is the Kleene star; otherwise min - 1 plain copies precede the looping one.
    if (at_least.min == 0)
    {
        return to_nfa(regex, Kleene{});
    }

    nfa::Builder start{};

    start.add_accept_state(start.init_state());

    const auto nfa{append_copies(start, regex, at_least.min - 1)};

    const auto body{to_nfa(regex)};

    const auto loop{loop_back(body)};

    return nfa.append(loop);
}

/**
 * @brief Builds the NFA for a bounded repetition.
 * @param regex The sub-pattern to repeat.
 * @param range The repetition, carrying its bounds.
 * @return NFA builder representing the repetition.
 */
[[nodiscard]] nfa::Builder to_nfa(const Regex& regex, const Range& range)
{
    // Matches a range of occurrences of a sub-pattern: the first `min` copies are required, and the state reached after
    // each further copy accepts, so the scan may stop at any count in the range:
    //
    // (nfa) --ε--> ... ((regex n)) --ε--> ... --ε--> ((regex m))
    //
    // The state reached after `k` copies is made an accepting state of its own, as the optional repetition makes its
    // skipped start one.
    nfa::Builder start{};

    start.add_accept_state(start.init_state());

    auto nfa{append_copies(start, regex, range.min)};

    nfa::Nfa::States_t pending{};

    for (std::size_t copy{range.min}; copy < range.max; ++copy)
    {
        const auto reached{std::views::keys(nfa.accept_states())};

        pending.insert(reached.begin(), reached.end());

        const auto body{to_nfa(regex)};

        nfa = nfa.append(body);
    }

    for (const auto pending_state : pending)
    {
        nfa.add_accept_state(pending_state);
    }

    return nfa;
}

} // namespace

nfa::Builder to_nfa(const Repeat& repeat)
{
    // The child needs no emptiness check: an Indirect always holds exactly one value.
    const auto& regex{*repeat.regex};

    const auto lower{[&regex](const auto& kind) { return to_nfa(regex, kind); }};

    return std::visit(lower, repeat.kind);
}

Regex kleene(Regex regex)
{
    return {.node = Repeat{.kind = Kleene{}, .regex = Indirect{std::move(regex)}}};
}

Regex plus(Regex regex)
{
    return {.node = Repeat{.kind = Plus{}, .regex = Indirect{std::move(regex)}}};
}

Regex optional(Regex regex)
{
    return {.node = Repeat{.kind = Optional{}, .regex = Indirect{std::move(regex)}}};
}

Regex exact(Regex regex, const std::size_t count)
{
    return {.node = Repeat{.kind = Exact{.count = count}, .regex = Indirect{std::move(regex)}}};
}

Regex at_least(Regex regex, const std::size_t min)
{
    return {.node = Repeat{.kind = At_least{.min = min}, .regex = Indirect{std::move(regex)}}};
}

Regex range(Regex regex, const std::size_t min, const std::size_t max)
{
    if (max < min)
    {
        throw std::invalid_argument{"A repetition range may not end before it starts"};
    }

    return {.node = Repeat{.kind = Range{.min = min, .max = max}, .regex = Indirect{std::move(regex)}}};
}

} // namespace munch::regex

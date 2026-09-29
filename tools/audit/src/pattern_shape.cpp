#include "munch/tools/audit/pattern_shape.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Returns the fixed count a repetition repeats its component, when it has one.
 * @param kind The repetition's kind.
 * @return The count, or std::nullopt for a count that varies.
 */
[[nodiscard]] std::optional<std::size_t> fixed_count(const regex::Repeat::Kind_t& kind)
{
    /**
     * @brief Returns the fixed count of a repetition of one kind.
     * @tparam Kind The kind's type.
     * @param counted The repetition's kind.
     * @return The count, or std::nullopt.
     */
    const auto count_of{[]<typename Kind>(const Kind& counted) -> std::optional<std::size_t> {
        if constexpr (std::is_same_v<Kind, regex::Exact>)
        {
            return counted.count;
        }
        else if constexpr (std::is_same_v<Kind, regex::Range>)
        {
            return counted.min == counted.max ? std::optional{counted.min} : std::nullopt;
        }
        else
        {
            return std::nullopt;
        }
    }};

    return std::visit(count_of, kind);
}

/**
 * @brief Returns the one word a component matches when it matches exactly one, whatever the spelling: a text, a class
 *        of one byte, a repetition of a fixed count of such a word, a repetition of exactly zero of anything, which is
 *        the empty word, a sequence of them, or a choice among spellings of the same one; none where the component
 *        matches two words or none.
 *
 * The shapes ask what a component matches, not how it is written, so that `\n`, `[\n]` and `\n{1}` are one terminator,
 * `"x"`, `[x]` and `x{1}` one opener and `[cd]{0}x` the terminator `x`, as they are one language to the scanner.
 * @param regex The component.
 * @return The word, or none.
 */
[[nodiscard]] std::optional<std::string> fixed_word(const regex::Regex& regex)
{
    /**
     * @brief Returns the one word a component of one kind matches.
     * @tparam Node The node's type.
     * @param node The component.
     * @return The word, or none.
     */
    const auto word_of{[]<typename Node>(const Node& node) -> std::optional<std::string> {
        if constexpr (std::is_same_v<Node, regex::Text>)
        {
            return node.text;
        }
        else if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            const auto& symbols{node.set.symbols()};

            if (symbols.size() != 1)
            {
                return std::nullopt;
            }

            return std::string{*symbols.begin()};
        }
        else if constexpr (std::is_same_v<Node, regex::Concat>)
        {
            std::string word{};

            for (const auto& part : node.regexes)
            {
                const auto fixed{fixed_word(part)};

                if (!fixed)
                {
                    return std::nullopt;
                }

                word += *fixed;
            }

            return word;
        }
        else if constexpr (std::is_same_v<Node, regex::Choice>)
        {
            std::optional<std::string> word{};

            for (const auto& part : node.regexes)
            {
                const auto fixed{fixed_word(part)};

                if (!fixed || (word && *word != *fixed))
                {
                    return std::nullopt;
                }

                word = fixed;
            }

            return word;
        }
        else
        {
            static_assert(std::is_same_v<Node, regex::Repeat>);

            // A fixed count repeats the word that many times, an exact zero the empty word whatever the component
            // matches; any other count repeats only the empty word into one word.
            const auto count{fixed_count(node.kind)};

            if (count == 0)
            {
                return std::string{};
            }

            const auto inner{fixed_word(*node.regex)};

            if (!inner)
            {
                return std::nullopt;
            }

            if (!count)
            {
                return inner->empty() ? inner : std::nullopt;
            }

            std::string word{};

            for (std::size_t n{0}; n < *count; ++n)
            {
                word += *inner;
            }

            return word;
        }
    }};

    return std::visit(word_of, regex.node);
}

/**
 * @brief Returns whether a pattern matches the empty word and no other.
 * @param regex The pattern.
 * @return True for the empty word alone.
 */
[[nodiscard]] bool is_empty_word(const regex::Regex& regex)
{
    const auto word{fixed_word(regex)};

    return word && word->empty();
}

/**
 * @brief Returns whether a pattern admits a byte anywhere: in a set, a text, or below, by what it matches rather than
 *        how it is written, so that a repetition of exactly zero, which matches the empty word alone, admits nothing
 *        whatever stands under it, and `[ab][x]{0}"x"` is the terminated shape `[ab]"x"` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool admits(const regex::Regex& regex, const unsigned char byte)
{
    /**
     * @brief Returns whether a part of the pattern admits the byte.
     * @param part The part.
     * @return True when it does.
     */
    const auto admits_byte{[byte](const regex::Regex& part) { return admits(part, byte); }};

    /**
     * @brief Returns whether a pattern of one kind admits the byte.
     * @tparam Node The node's type.
     * @param node The pattern.
     * @return True when it does.
     */
    const auto admitted{[byte, &admits_byte]<typename Node>(const Node& node) {
        if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            return node.set.symbols().contains(static_cast<char>(byte));
        }
        else if constexpr (std::is_same_v<Node, regex::Text>)
        {
            return node.text.contains(static_cast<char>(byte));
        }
        else if constexpr (std::is_same_v<Node, regex::Repeat>)
        {
            const auto none{fixed_count(node.kind) == 0};

            return !none && admits_byte(*node.regex);
        }
        else
        {
            return std::ranges::any_of(node.regexes, admits_byte);
        }
    }};

    return std::visit(admitted, regex.node);
}

/**
 * @brief Returns how many times a repetition must repeat its component at the least.
 * @param kind The repetition's kind.
 * @return The count.
 */
[[nodiscard]] std::size_t required_count(const regex::Repeat::Kind_t& kind)
{
    /**
     * @brief Returns the least count of a repetition of one kind.
     * @tparam Kind The kind's type.
     * @param counted The repetition's kind.
     * @return The count.
     */
    const auto least_of{[]<typename Kind>(const Kind& counted) -> std::size_t {
        if constexpr (std::is_same_v<Kind, regex::Plus>)
        {
            return 1;
        }
        else if constexpr (std::is_same_v<Kind, regex::Exact>)
        {
            return counted.count;
        }
        else if constexpr (std::is_same_v<Kind, regex::At_least> || std::is_same_v<Kind, regex::Range>)
        {
            return counted.min;
        }
        else
        {
            return 0;
        }
    }};

    return std::visit(least_of, kind);
}

/**
 * @brief Returns whether a pattern matches no word at all: a class of no byte, a sequence with such a part, a choice
 *        among such parts alone, or a repetition of one that must repeat it. No file syntax the readers accept spells
 *        one, an empty bracket being refused by the parser; a set assembled through the API may hold one, `any_of("")`.
 * @param regex The pattern.
 * @return True when no word matches.
 */
[[nodiscard]] bool matches_nothing(const regex::Regex& regex)
{
    /**
     * @brief Returns whether a pattern of one kind matches no word.
     * @tparam Node The node's type.
     * @param node The pattern.
     * @return True when none matches.
     */
    const auto nothing_of{[]<typename Node>(const Node& node) {
        if constexpr (std::is_same_v<Node, regex::Any_of>)
        {
            return node.set.symbols().empty();
        }
        else if constexpr (std::is_same_v<Node, regex::Text>)
        {
            return false;
        }
        else if constexpr (std::is_same_v<Node, regex::Concat>)
        {
            return std::ranges::any_of(node.regexes, matches_nothing);
        }
        else if constexpr (std::is_same_v<Node, regex::Choice>)
        {
            return std::ranges::all_of(node.regexes, matches_nothing);
        }
        else
        {
            static_assert(std::is_same_v<Node, regex::Repeat>);

            const auto required{required_count(node.kind)};

            return required > 0 && matches_nothing(*node.regex);
        }
    }};

    return std::visit(nothing_of, regex.node);
}

/**
 * @brief Returns a repetition as the scanner sees it: one repeating its component exactly once is that component, one
 *        of a component matching nothing matches nothing or only the empty word, and one of the empty word is the empty
 *        word.
 * @param repeat The repetition.
 * @return The pattern normalised.
 */
[[nodiscard]] regex::Regex normalized_repeat(const regex::Repeat& repeat)
{
    const auto& [kind, repeated]{repeat};

    const auto once{fixed_count(kind) == 1};

    if (once)
    {
        return normalized(*repeated);
    }

    // What a repetition repeats is normalised too, so `([ \t\n]{1})+` repeats the class `[ \t\n]` and is the run that
    // `[ \t\n]+` is, rather than a repetition of a sequence of one.
    auto inner{normalized(*repeated)};

    const auto inner_nothing{matches_nothing(inner)};

    // The repetition itself matches nothing when it must repeat a component matching nothing.
    const auto whole_nothing{required_count(kind) > 0 && matches_nothing(*repeated)};

    if (inner_nothing && whole_nothing)
    {
        return {.node = regex::Any_of{.set = {}}};
    }

    if (inner_nothing)
    {
        return {.node = regex::Text{.text = {}}};
    }

    // A repetition of the empty word is the empty word, however many times.
    if (is_empty_word(inner))
    {
        return {.node = regex::Text{.text = {}}};
    }

    return {.node = regex::Repeat{.kind = kind, .regex = regex::Indirect{std::move(inner)}}};
}

/**
 * @brief Returns a choice as the scanner sees it: the alternatives that match something, each normalised, a choice of
 *        none matching nothing and a choice of one being that one.
 * @param choice The choice.
 * @return The pattern normalised.
 */
[[nodiscard]] regex::Regex normalized_choice(const regex::Choice& choice)
{
    std::vector<regex::Regex> alternatives{};

    const auto& [choices]{choice};

    for (const auto& part : choices)
    {
        if (!matches_nothing(part))
        {
            alternatives.push_back(normalized(part));
        }
    }

    if (alternatives.empty())
    {
        return {.node = regex::Any_of{.set = {}}};
    }

    if (alternatives.size() == 1)
    {
        return std::move(alternatives.front());
    }

    return {.node = regex::Choice{.regexes = std::move(alternatives)}};
}

/**
 * @brief Returns a sequence as the scanner sees it: one with a part matching nothing matches nothing, its parts are
 *        normalised and the empty words among them dropped, a sequence of none is the empty word and one of one part
 *        that part.
 * @param concat The sequence.
 * @return The pattern normalised.
 */
[[nodiscard]] regex::Regex normalized_concat(const regex::Concat& concat)
{
    const auto& [sequence]{concat};

    if (std::ranges::any_of(sequence, matches_nothing))
    {
        return {.node = regex::Any_of{.set = {}}};
    }

    // Each part is normalised before it is asked whether it is the empty word, so that `(any_of(""))*`, which is the
    // empty word once normalised, is no part either; a sequence of no parts left is the empty word itself.
    std::vector<regex::Regex> parts{};

    for (const auto& part : sequence)
    {
        auto inner{normalized(part)};

        if (!is_empty_word(inner))
        {
            parts.push_back(std::move(inner));
        }
    }

    if (parts.empty())
    {
        return {.node = regex::Text{.text = {}}};
    }

    if (parts.size() == 1)
    {
        return std::move(parts.front());
    }

    return {.node = regex::Concat{.regexes = std::move(parts)}};
}

} // namespace

bool is_run(const regex::Regex& regex, const unsigned char byte)
{
    const auto whole{normalized(regex)};

    const auto parts{parts_of(whole)};

    const auto& node{parts.size() == 1 ? parts.front().node : whole.node};

    if (!std::holds_alternative<regex::Repeat>(node))
    {
        return false;
    }

    const auto& [kind, repeated]{std::get<regex::Repeat>(node)};

    if (!std::holds_alternative<regex::Kleene>(kind) && !std::holds_alternative<regex::Plus>(kind))
    {
        return false;
    }

    const auto& inner{(*repeated).node};

    if (!std::holds_alternative<regex::Any_of>(inner))
    {
        return false;
    }

    const auto& [set]{std::get<regex::Any_of>(inner)};

    return set.symbols().contains(static_cast<char>(byte));
}

bool is_terminated(const regex::Regex& regex, const unsigned char byte)
{
    const auto parts{parts_of(regex)};

    if (parts.size() < 2)
    {
        return false;
    }

    const auto last{fixed_word(parts.back())};

    if (last != std::string{static_cast<char>(byte)})
    {
        return false;
    }

    const auto before{parts | std::views::take(parts.size() - 1)};

    /**
     * @brief Returns whether a part of the pattern admits the byte.
     * @param part The part.
     * @return True when it does.
     */
    const auto admits_byte{[byte](const regex::Regex& part) { return admits(part, byte); }};

    return std::ranges::none_of(before, admits_byte);
}

bool is_delimited(const regex::Regex& regex, const unsigned char byte)
{
    const auto parts{parts_of(regex)};

    if (parts.size() < 2)
    {
        return false;
    }

    const auto opener{fixed_word(parts.front())};

    return opener && !opener->empty() && !opener->contains(static_cast<char>(byte));
}

std::vector<regex::Regex> parts_of(const regex::Regex& regex)
{
    auto whole{normalized(regex)};

    if (!std::holds_alternative<regex::Concat>(whole.node))
    {
        return {};
    }

    return std::move(std::get<regex::Concat>(whole.node).regexes);
}

regex::Regex normalized(const regex::Regex& regex)
{
    if (std::holds_alternative<regex::Repeat>(regex.node))
    {
        return normalized_repeat(std::get<regex::Repeat>(regex.node));
    }

    if (std::holds_alternative<regex::Choice>(regex.node))
    {
        return normalized_choice(std::get<regex::Choice>(regex.node));
    }

    if (!std::holds_alternative<regex::Concat>(regex.node))
    {
        return regex;
    }

    return normalized_concat(std::get<regex::Concat>(regex.node));
}

} // namespace munch::tools::audit

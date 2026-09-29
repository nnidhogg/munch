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
// Implements pattern_shape.hpp: the one word a component matches, whether a pattern admits a byte, and whether it
// matches nothing, are private to this unit.

/**
 * @brief The one word a component matches when it matches exactly one, whatever the spelling: a text, a class of one
 *        byte, a repetition of a fixed count of such a word, a repetition of exactly zero of anything, which is the
 *        empty word, a sequence of them, or a choice among spellings of the same one; none where the component
 *        matches two words or none.
 *
 * The shapes ask what a component matches, not how it is written, so that `\n`, `[\n]` and `\n{1}` are one terminator,
 * `"x"`, `[x]` and `x{1}` one opener and `[cd]{0}x` the terminator `x`, as they are one language to the scanner.
 * @param regex The component.
 * @return The word, or none.
 */
[[nodiscard]] std::optional<std::string> fixed_word(const regex::Regex& regex)
{
    return std::visit(
            []<typename Node>(const Node& node) -> std::optional<std::string> {
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

                    return std::string(1, *symbols.begin());
                }
                else if constexpr (std::is_same_v<Node, regex::Concat>)
                {
                    std::string word;

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
                    std::optional<std::string> word;

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

                    // A fixed count repeats the word that many times, an exact zero the empty word whatever the
                    // component matches; any other count repeats only the empty word into one word.
                    const auto count{std::visit(
                            []<typename Kind>(const Kind& kind) -> std::optional<std::size_t> {
                                if constexpr (std::is_same_v<Kind, regex::Exact>)
                                {
                                    return kind.count;
                                }
                                else if constexpr (std::is_same_v<Kind, regex::Range>)
                                {
                                    return kind.min == kind.max ? std::optional{kind.min} : std::nullopt;
                                }
                                else
                                {
                                    return std::nullopt;
                                }
                            },
                            node.kind)};

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

                    std::string word;

                    for (std::size_t n{0}; n < *count; ++n)
                    {
                        word += *inner;
                    }

                    return word;
                }
            },
            regex.node);
}

/**
 * @brief Whether a pattern admits a byte anywhere: in a set, a text, or below, by what it matches rather than how it
 *        is written, so that a repetition of exactly zero, which matches the empty word alone, admits nothing
 *        whatever stands under it, and `[ab][x]{0}"x"` is the terminated shape `[ab]"x"` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool admits(const regex::Regex& regex, const unsigned char byte)
{
    using namespace regex;

    return std::visit(
            [byte]<typename Node>(const Node& node) {
                if constexpr (std::is_same_v<Node, Any_of>)
                {
                    return node.set.symbols().contains(static_cast<char>(byte));
                }
                else if constexpr (std::is_same_v<Node, Text>)
                {
                    return node.text.contains(static_cast<char>(byte));
                }
                else if constexpr (std::is_same_v<Node, Repeat>)
                {
                    const auto none{std::visit(
                            []<typename Kind>(const Kind& kind) {
                                if constexpr (std::is_same_v<Kind, Exact>)
                                {
                                    return kind.count == 0;
                                }
                                else if constexpr (std::is_same_v<Kind, Range>)
                                {
                                    return kind.max == 0;
                                }
                                else
                                {
                                    return false;
                                }
                            },
                            node.kind)};

                    return !none && admits(*node.regex, byte);
                }
                else
                {
                    return std::ranges::any_of(node.regexes, [byte](const Regex& part) { return admits(part, byte); });
                }
            },
            regex.node);
}

/**
 * @brief Whether a pattern matches no word at all: a class of no byte, a sequence with such a part, a choice among
 *        such parts alone, or a repetition of one that must repeat it. No file syntax the readers accept spells one,
 *        an empty bracket being refused by the parser; a set assembled through the API may hold one, `any_of("")`.
 * @param regex The pattern.
 * @return True when no word matches.
 */
[[nodiscard]] bool matches_nothing(const regex::Regex& regex)
{
    return std::visit(
            []<typename Node>(const Node& node) {
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

                    const auto required{std::visit(
                            []<typename Kind>(const Kind& kind) -> std::size_t {
                                if constexpr (std::is_same_v<Kind, regex::Plus>)
                                {
                                    return 1;
                                }
                                else if constexpr (std::is_same_v<Kind, regex::Exact>)
                                {
                                    return kind.count;
                                }
                                else if constexpr (
                                        std::is_same_v<Kind, regex::At_least> || std::is_same_v<Kind, regex::Range>)
                                {
                                    return kind.min;
                                }
                                else
                                {
                                    return 0;
                                }
                            },
                            node.kind)};

                    return required > 0 && matches_nothing(*node.regex);
                }
            },
            regex.node);
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

    return std::holds_alternative<regex::Any_of>(inner) &&
           std::get<regex::Any_of>(inner).set.symbols().contains(static_cast<char>(byte));
}

bool is_terminated(const regex::Regex& regex, const unsigned char byte)
{
    const auto parts{parts_of(regex)};

    return parts.size() >= 2 && fixed_word(parts.back()) == std::string(1, static_cast<char>(byte)) &&
           std::ranges::none_of(parts | std::views::take(parts.size() - 1), [byte](const regex::Regex& part) {
               return admits(part, byte);
           });
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
        const auto& repeat{std::get<regex::Repeat>(regex.node)};

        const auto once{std::visit(
                []<typename Kind>(const Kind& kind) {
                    if constexpr (std::is_same_v<Kind, regex::Exact>)
                    {
                        return kind.count == 1;
                    }
                    else if constexpr (std::is_same_v<Kind, regex::Range>)
                    {
                        return kind.min == 1 && kind.max == 1;
                    }
                    else
                    {
                        return false;
                    }
                },
                repeat.kind)};

        if (once)
        {
            return normalized(*repeat.regex);
        }

        // What a repetition repeats is normalised too, so `([ \t\n]{1})+` repeats the class `[ \t\n]` and is the
        // run that `[ \t\n]+` is, rather than a repetition of a sequence of one.
        auto inner{normalized(*repeat.regex)};

        if (matches_nothing(inner))
        {
            return matches_nothing(regex) ? regex::Regex{.node = regex::Any_of{.set = {}}} :
                                            regex::Regex{.node = regex::Text{.text = {}}};
        }

        // A repetition of the empty word is the empty word, however many times.
        if (fixed_word(inner) == std::string{})
        {
            return {.node = regex::Text{.text = {}}};
        }

        return {.node = regex::Repeat{.kind = repeat.kind, .regex = regex::Indirect{std::move(inner)}}};
    }

    if (std::holds_alternative<regex::Choice>(regex.node))
    {
        std::vector<regex::Regex> alternatives;

        for (const auto& part : std::get<regex::Choice>(regex.node).regexes)
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

    if (!std::holds_alternative<regex::Concat>(regex.node))
    {
        return regex;
    }

    if (matches_nothing(regex))
    {
        return {.node = regex::Any_of{.set = {}}};
    }

    // Each part is normalised before it is asked whether it is the empty word, so that `(any_of(""))*`, which is the
    // empty word once normalised, is no part either; a sequence of no parts left is the empty word itself.
    std::vector<regex::Regex> parts;

    for (const auto& part : std::get<regex::Concat>(regex.node).regexes)
    {
        auto inner{normalized(part)};

        if (fixed_word(inner) != std::string{})
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

} // namespace munch::tools::audit

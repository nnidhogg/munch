#include "munch/tools/audit/price.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "munch/tools/audit/report.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The tokens the blame names for a byte, each once, in rule order: the order the set lists its rules, which
 *        for a set read from a file is the file's, whatever the ids are.
 * @param lexer The compiled set.
 * @param set The set it was compiled from, whose order is the rule order.
 * @param byte The byte.
 * @return The token ids.
 */
[[nodiscard]] std::vector<std::size_t> consumers(
        const core::Lexer& lexer, const Token_set& set, const unsigned char byte)
{
    std::vector<std::size_t> tokens;

    for (const auto& entry : blame(lexer))
    {
        if (entry.byte == byte && !std::ranges::contains(tokens, entry.token))
        {
            tokens.push_back(entry.token);
        }
    }

    // A token the set does not list, which none of the blamed ones is, would sort last.
    std::ranges::sort(tokens, {}, [&set](const std::size_t token) {
        return std::ranges::find(set.rules, token, &Token_rule::id) - set.rules.begin();
    });

    return tokens;
}

/**
 * @brief Whether some token of the compiled set matches the byte on its own.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @return True when a one-byte input of it tokenizes.
 */
[[nodiscard]] bool matched_alone(const core::Lexer& lexer, const unsigned char byte)
{
    const std::string input(1, static_cast<char>(byte));

    return lexer.tokenize<std::size_t>(input).length == 1;
}

/**
 * @brief Whether a pattern admits a byte anywhere: in a set, a text, or below.
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
                    return admits(*node.regex, byte);
                }
                else
                {
                    return std::ranges::any_of(node.regexes, [byte](const Regex& part) { return admits(part, byte); });
                }
            },
            regex.node);
}

/**
 * @brief The parts of a pattern's top-level sequence, the pattern itself as the one part when it is no sequence.
 * @param regex The pattern.
 * @return The parts.
 */
[[nodiscard]] const std::vector<regex::Regex>& parts_of(const regex::Regex& regex)
{
    static const std::vector<regex::Regex> none;

    const auto* concat{std::get_if<regex::Concat>(&regex.node)};

    return concat ? concat->regexes : none;
}

/**
 * @brief Whether a pattern is a run over a class the byte is in: one or more of a set, possibly the sole part of a
 *        sequence.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it is.
 */
[[nodiscard]] bool is_run(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    const auto& node{parts.size() == 1 ? parts.front().node : regex.node};

    const auto* repeat{std::get_if<regex::Repeat>(&node)};

    if (!repeat ||
        (!std::holds_alternative<regex::Kleene>(repeat->kind) && !std::holds_alternative<regex::Plus>(repeat->kind)))
    {
        return false;
    }

    const auto* set{std::get_if<regex::Any_of>(&(*repeat->regex).node)};

    return set && set->set.symbols().contains(static_cast<char>(byte));
}

/**
 * @brief The one word a component matches when it matches exactly one, whatever the spelling: a text, a class of one
 *        byte, a repetition of a fixed count of such a word, a sequence of them, or a choice among spellings of the
 *        same one; none where the component matches two words or none.
 *
 * The shapes ask what a component matches, not how it is written, so that `\n`, `[\n]` and `\n{1}` are one
 * terminator and `"x"`, `[x]` and `x{1}` one opener, as they are one language to the scanner.
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

                    const auto inner{fixed_word(*node.regex)};

                    if (!inner)
                    {
                        return std::nullopt;
                    }

                    // A fixed count repeats the word that many times; any other count repeats only the empty word
                    // into one word.
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
 * @brief Whether a sequence ends in the byte and holds it nowhere else: a body, then the one byte that terminates
 *        it.
 *
 * The last component has to be the terminator itself, the one byte and no other, because the edit this shape names
 * deletes that component whole: the last component of `[a]"xb"` admits the `x` while the token ends in `b`, and
 * the class of `[a][xb]` admits the `x` while the token may end in `b`, so calling either terminated described
 * leaving a terminator to the token after it while the edit evaluated deleted the token's own `b` along with it. A
 * pattern like that is no terminated shape here, and is priced as what it is. The terminator is read by what it
 * matches, so `[a]x{1}` is terminated as `[a]x` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_terminated(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    return parts.size() >= 2 && fixed_word(parts.back()) == std::string(1, static_cast<char>(byte)) &&
           std::ranges::none_of(parts | std::views::take(parts.size() - 1), [byte](const regex::Regex& part) {
               return admits(part, byte);
           });
}

/**
 * @brief Whether a sequence opens with a fixed word the byte is not in, followed by a body it is in.
 *
 * The opener is read by what it matches, as the terminator is, so `[x][ab]*` and `x{1}[ab]*` are delimited as
 * `"x"[ab]*` is.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool is_delimited(const regex::Regex& regex, const unsigned char byte)
{
    const auto& parts{parts_of(regex)};

    if (parts.size() < 2)
    {
        return false;
    }

    const auto opener{fixed_word(parts.front())};

    return opener && !opener->empty() && !opener->contains(static_cast<char>(byte));
}

/**
 * @brief The kinds of word a pattern matches, told apart by the fixed occurrences of a byte they hold: the
 *        occurrences a text spells or a class of the one byte matches, which no narrowing removes.
 *
 * A pattern's words are summarised as a set of these, held as bits, so that concatenation and repetition can be
 * computed over the sets and a pattern can be asked whether every word it matches is of one kind.
 */
namespace words
{
using Kinds = std::uint8_t;

/**
 * @brief The empty word.
 */
constexpr Kinds empty{1U};

/**
 * @brief A nonempty word with no fixed occurrence of the byte.
 */
constexpr Kinds plain{2U};

/**
 * @brief A word whose one fixed occurrence of the byte is its first byte.
 */
constexpr Kinds leading{4U};

/**
 * @brief A word with a fixed occurrence of the byte past its first byte.
 */
constexpr Kinds mid{8U};

/**
 * @brief The kind of the word one kind of word makes followed by another.
 * @param first The kind of the first word.
 * @param second The kind of the second.
 * @return The kind of their concatenation.
 */
[[nodiscard]] constexpr Kinds join(const Kinds first, const Kinds second)
{
    if (first == mid || second == mid)
    {
        return mid;
    }

    if (first == empty)
    {
        return second;
    }

    if (second == empty)
    {
        return first;
    }

    // A fixed first byte of the second word stands past the first byte of a nonempty first word.
    return second == leading ? mid : first;
}

/**
 * @brief The kinds of word every word of one set makes followed by every word of another.
 * @param first The kinds of the first words.
 * @param second The kinds of the second.
 * @return The kinds of their concatenations.
 */
[[nodiscard]] constexpr Kinds joined(const Kinds first, const Kinds second)
{
    Kinds out{0};

    for (const auto a : {empty, plain, leading, mid})
    {
        for (const auto b : {empty, plain, leading, mid})
        {
            if ((first & a) != 0 && (second & b) != 0)
            {
                out |= join(a, b);
            }
        }
    }

    return out;
}

/**
 * @brief The kinds of word a repetition of words of the given kinds makes, over the counts allowed.
 *
 * The kinds of n repetitions follow from the kinds of n - 1, and there are sixteen sets of kinds, so the sets from
 * the minimum count on repeat within sixteen steps; the union is complete at the first repeat, which is also where
 * an unbounded repetition stops.
 * @param kinds The kinds of the repeated words.
 * @param min The least count.
 * @param max The greatest count, or none for an unbounded repetition.
 * @return The kinds of the repetitions' words.
 */
[[nodiscard]] Kinds repeated(const Kinds kinds, const std::size_t min, const std::optional<std::size_t> max)
{
    Kinds out{0};

    Kinds count{empty};

    std::uint16_t seen{0};

    for (std::size_t n{0}; !max || n <= *max; ++n)
    {
        if (n >= min)
        {
            if ((seen & (1U << count)) != 0)
            {
                break;
            }

            seen |= static_cast<std::uint16_t>(1U << count);

            out |= count;
        }

        count = joined(count, kinds);
    }

    return out;
}

/**
 * @brief The kinds of word a pattern matches, for a byte.
 * @param regex The pattern.
 * @param byte The byte.
 * @return The kinds, at least one.
 */
[[nodiscard]] Kinds of(const regex::Regex& regex, const unsigned char byte)
{
    return std::visit(
            [byte]<typename Node>(const Node& node) -> Kinds {
                if constexpr (std::is_same_v<Node, regex::Any_of>)
                {
                    // A class of the one byte matches nothing else, so its occurrence is as fixed as a text's.
                    return node.set.symbols() == regex::Set::Symbols_t{static_cast<char>(byte)} ? leading : plain;
                }
                else if constexpr (std::is_same_v<Node, regex::Text>)
                {
                    if (node.text.empty())
                    {
                        return empty;
                    }

                    if (node.text.find(static_cast<char>(byte), 1) != std::string::npos)
                    {
                        return mid;
                    }

                    return node.text.front() == static_cast<char>(byte) ? leading : plain;
                }
                else if constexpr (std::is_same_v<Node, regex::Concat>)
                {
                    Kinds out{empty};

                    for (const auto& part : node.regexes)
                    {
                        out = joined(out, of(part, byte));
                    }

                    return out;
                }
                else if constexpr (std::is_same_v<Node, regex::Choice>)
                {
                    Kinds out{0};

                    for (const auto& part : node.regexes)
                    {
                        out |= of(part, byte);
                    }

                    return out;
                }
                else
                {
                    static_assert(std::is_same_v<Node, regex::Repeat>);

                    const auto inner{of(*node.regex, byte)};

                    return std::visit(
                            [inner]<typename Kind>(const Kind& kind) {
                                if constexpr (std::is_same_v<Kind, regex::Kleene>)
                                {
                                    return repeated(inner, 0, std::nullopt);
                                }
                                else if constexpr (std::is_same_v<Kind, regex::Plus>)
                                {
                                    return repeated(inner, 1, std::nullopt);
                                }
                                else if constexpr (std::is_same_v<Kind, regex::Optional>)
                                {
                                    return repeated(inner, 0, 1);
                                }
                                else if constexpr (std::is_same_v<Kind, regex::Exact>)
                                {
                                    return repeated(inner, kind.count, kind.count);
                                }
                                else if constexpr (std::is_same_v<Kind, regex::At_least>)
                                {
                                    return repeated(inner, kind.min, std::nullopt);
                                }
                                else
                                {
                                    static_assert(std::is_same_v<Kind, regex::Range>);

                                    return repeated(inner, kind.min, kind.max);
                                }
                            },
                            node.kind);
                }
            },
            regex.node);
}

} // namespace words

/**
 * @brief Whether every word of a pattern holds a fixed occurrence of the byte past its first byte, which is what
 *        makes a token that cannot lose the byte an obstruction rather than a token the narrowing decides nothing
 *        about.
 *
 * An edit that keeps any of the token's words, a class narrowed, an alternative dropped, a repetition run fewer
 * times, keeps that word's fixed occurrences, so when every word holds one past its first byte the token consumes
 * the byte mid-token in whatever narrowed form it keeps, and the byte cannot certify while the token stays: `[x]\n[x]`
 * and `\n{2}` are such tokens. Where some word holds the byte fixed only as its first byte, the occurrence the
 * initial state consumes, an edit this analysis does not make can keep that word and certify the byte: the class of
 * `\n[\nx]`, `[\n][\nx]`, `\n{1}[\nx]` or `(\n[xy])[\nx]` narrowed to `[x]`, or the second alternative of
 * `(\n[\nx]|\ny)` dropped and the first's class narrowed.
 * @param regex The pattern.
 * @param byte The byte.
 * @return True when it does.
 */
[[nodiscard]] bool fixed_mid_token(const regex::Regex& regex, const unsigned char byte)
{
    return words::of(regex, byte) == words::mid;
}

/**
 * @brief The bytes a compiled set certifies exactly.
 * @param lexer The compiled set.
 * @return The bytes, ascending.
 */
[[nodiscard]] std::vector<unsigned char> exact_bytes(const core::Lexer& lexer)
{
    std::vector<unsigned char> bytes;

    for (std::size_t value{0}; value < 256; ++value)
    {
        if (lexer.is_split_point(static_cast<char>(value)))
        {
            bytes.push_back(static_cast<unsigned char>(value));
        }
    }

    return bytes;
}

/**
 * @brief Where the byte stands over an edited set, against the bytes certified before any edit.
 * @param edited The set after the edit.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte priced.
 * @return The outcome.
 */
[[nodiscard]] Outcome outcome(
        const Token_set& edited, const std::vector<unsigned char>& before, const unsigned char byte)
{
    const auto lexer{compile(edited)};

    Outcome after{
            .exact = lexer.is_split_point(static_cast<char>(byte)),
            .modulo = lexer.is_split_point_ignoring(static_cast<char>(byte)),
            .gained = {}};

    std::ranges::set_difference(exact_bytes(lexer), before, std::back_inserter(after.gained));

    std::erase(after.gained, byte);

    return after;
}

/**
 * @brief The pattern a shape's edit leaves a token with: a terminated one its body, a delimited one its opener.
 * @param regex The token's pattern, of that shape for the byte.
 * @param shape The shape, terminated or delimited.
 * @return The pattern after the edit.
 */
[[nodiscard]] regex::Regex edited_by(const regex::Regex& regex, const Shape shape)
{
    const auto& parts{parts_of(regex)};

    if (shape == Shape::delimited)
    {
        return parts.front();
    }

    std::vector<regex::Regex> body{parts.begin(), parts.end() - 1};

    return body.size() == 1 ? std::move(body.front()) : regex::Regex{.node = regex::Concat{.regexes = std::move(body)}};
}

} // namespace

Shape shape_of(const regex::Regex& regex, const unsigned char byte)
{
    // A terminator or a body may spell the byte out, so the shapes with an edit of their own come before fixed.
    if (is_run(regex, byte))
    {
        return Shape::run;
    }

    if (is_terminated(regex, byte))
    {
        return Shape::terminated;
    }

    if (is_delimited(regex, byte))
    {
        return Shape::delimited;
    }

    return can_lose(regex, byte) ? Shape::other : Shape::fixed;
}

Pricing price(const Token_set& set, const unsigned char byte)
{
    auto edited{set};

    // Room for the one token the analysis may add, taken now: GCC 13 misreads the move a later reallocation would
    // make of a rule's pattern as a read of something uninitialized, and the build treats the warning as an error.
    edited.rules.reserve(set.rules.size() + 1);

    auto lexer{compile(edited)};

    const auto before{exact_bytes(lexer)};

    Pricing pricing{
            .byte = byte,
            .exact_before = lexer.is_split_point(static_cast<char>(byte)),
            .modulo_before = lexer.is_split_point_ignoring(static_cast<char>(byte)),
            .given = std::nullopt,
            .steps = {},
            .immovable = {},
            .undecided = {},
            .gained = {},
            .choices = {},
            .together = std::nullopt};

    if (pricing.exact_before)
    {
        return pricing;
    }

    // Ids and priorities for a token of the byte's own, past every existing one so nothing else moves.
    auto next_id{0UZ};

    auto next_priority{0UZ};

    for (const auto& rule : set.rules)
    {
        next_id = std::max(next_id, rule.id + 1);

        next_priority = std::max(next_priority, rule.priority + 1);
    }

    // A byte no token begins with is reported by neither certificate, every occurrence of it lying mid-token or
    // nowhere, and no narrowing changes that. It is given a token of its own before anything else, visible since no
    // token of the set says what it would be discarded as, and every edit below is made on the set holding it.
    const auto& simulator{lexer.simulator()};

    const auto from_start{simulator.step(simulator.init_state(), byte)};

    if (!from_start || !simulator.is_live(*from_start))
    {
        edited.rules.emplace_back(regex::text(std::string(1, static_cast<char>(byte))), next_id, next_priority, false);

        lexer = compile(edited);

        pricing.given = outcome(edited, before, byte);
    }

    // What each consuming token's shape offers on its own, before the uniform narrowing below edits anything, and
    // what every consumer taking its own shape's edit gives together.
    auto together{edited};

    together.rules.reserve(set.rules.size() + 1);

    auto offered{false};

    auto narrowed_discarded{false};

    for (const auto token : consumers(lexer, edited, byte))
    {
        const auto rule{std::ranges::find(set.rules, token, &Token_rule::id)};

        if (rule == set.rules.end())
        {
            continue;
        }

        for (const auto shape : {Shape::terminated, Shape::delimited})
        {
            if (shape == Shape::terminated ? is_terminated(rule->regex, byte) : is_delimited(rule->regex, byte))
            {
                auto alone{edited};

                std::ranges::find(alone.rules, token, &Token_rule::id)->regex = edited_by(rule->regex, shape);

                pricing.choices.push_back({.token = token, .shape = shape, .after = outcome(alone, before, byte)});
            }
        }

        auto& taken{*std::ranges::find(together.rules, token, &Token_rule::id)};

        switch (shape_of(rule->regex, byte))
        {
        case Shape::terminated:
        case Shape::delimited:
            taken.regex = edited_by(rule->regex, shape_of(rule->regex, byte));

            offered = true;

            break;
        case Shape::run:
        case Shape::other:
            exclude(taken.regex, byte);

            narrowed_discarded = narrowed_discarded || rule->discarded;

            break;
        case Shape::fixed:
            break;
        }
    }

    if (offered)
    {
        if (!matched_alone(compile(together), byte))
        {
            together.rules.emplace_back(
                    regex::text(std::string(1, static_cast<char>(byte))), next_id, next_priority, narrowed_discarded);
        }

        pricing.together = outcome(together, before, byte);
    }

    // The consumers are read from the recompiled tables again after every edit: narrowing one rule can leave the
    // byte to a rule that did not consume it before, a later rule whose match the narrowed one had won. The list
    // the blame gives before any edit is therefore not the list of rules that must change, and pricing from it
    // alone stopped at the first rule and called the byte unbought while an edit was still there to make.
    //
    // The token the byte may be given of its own is answered before the first step: it is none of the set's rules,
    // so it is no edit an author makes, no obstruction of theirs, and not a token the report can name.
    std::set<std::size_t> answered{next_id};

    while (true)
    {
        const auto remaining{consumers(lexer, edited, byte)};

        const auto next{std::ranges::find_if(
                remaining, [&answered](const std::size_t token) { return !answered.contains(token); })};

        // Every rule the byte is left to has been narrowed or found immovable: no edit of this analysis remains.
        if (next == remaining.end())
        {
            break;
        }

        const auto token{*next};

        answered.insert(token);

        const auto rule{std::ranges::find(edited.rules, token, &Token_rule::id)};

        if (rule == edited.rules.end())
        {
            continue;
        }

        if (!can_lose(rule->regex, byte))
        {
            // The narrowing is not applied to a rule that cannot lose the byte. The rule is an obstruction only
            // where every word it matches holds the byte fixed past its first byte; a rule with a word holding it
            // fixed only as its first byte, the initial state's occurrence, which begins a token, is reported as one
            // this analysis decides nothing about rather than as an impossibility.
            (fixed_mid_token(rule->regex, byte) ? pricing.immovable : pricing.undecided).push_back(token);

            continue;
        }

        const auto shape{shape_of(rule->regex, byte)};

        exclude(rule->regex, byte);

        lexer = compile(edited);

        Price_step step{
                .token = token,
                .shape = shape,
                .separated = false,
                .separated_discarded = false,
                .exact = false,
                .modulo = false};

        if (!matched_alone(lexer, byte))
        {
            edited.rules.emplace_back(
                    regex::text(std::string(1, static_cast<char>(byte))), next_id, next_priority, rule->discarded);

            step.separated = true;

            step.separated_discarded = rule->discarded;

            lexer = compile(edited);
        }

        step.exact = lexer.is_split_point(static_cast<char>(byte));

        step.modulo = lexer.is_split_point_ignoring(static_cast<char>(byte));

        pricing.steps.push_back(step);

        if (step.exact)
        {
            break;
        }
    }

    const auto after{exact_bytes(lexer)};

    std::ranges::set_difference(after, before, std::back_inserter(pricing.gained));

    std::erase(pricing.gained, byte);

    return pricing;
}

} // namespace munch::tools::audit

#include "munch/tools/audit/price.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/tools/audit/pattern_shape.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/word_kinds.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements price.hpp: the probes of a compiled set, the byte's own token, the shapes' choices and the narrowing steps
// are private to this unit.

/**
 * @brief What taking a rule's own shape's edit did to its copy in the combined set.
 */
struct Taken
{
    /**
     * @brief Whether the shape offered an edit, which the copy took.
     */
    bool offered{};

    /**
     * @brief Whether the copy was narrowed instead.
     */
    bool narrowed{};
};

/**
 * @brief The id and the priority a token of the byte's own is given: the smallest id no rule carries, since one past
 *        the highest is no id at all where a rule carries the largest std::size_t, and a priority past every rule's,
 *        so nothing else moves.
 */
struct Own_token
{
    /**
     * @brief The id.
     */
    std::size_t id{};

    /**
     * @brief The priority.
     */
    std::size_t priority{};
};

/**
 * @brief What the consuming tokens' shapes offer: one choice per token and shape, each edit on its own, and where the
 *        byte stands once every consumer takes its own shape's edit.
 */
struct Offers
{
    /**
     * @brief The choices, in the order the consumers were found.
     */
    std::vector<Choice> choices;

    /**
     * @brief Where the byte stands once every consumer takes its edit, absent when no shape offered one.
     */
    std::optional<Outcome> together;
};

/**
 * @brief The narrowing's answers to the consumers: the steps it took, and the tokens it could not take a step on.
 */
struct Answers
{
    /**
     * @brief The steps, in the order they were made.
     */
    std::vector<Price_step> steps;

    /**
     * @brief The tokens every word of which holds the byte fixed past its first byte.
     */
    std::vector<std::size_t> immovable;

    /**
     * @brief The tokens that cannot lose the byte while some word of them holds it fixed only as its first byte.
     */
    std::vector<std::size_t> undecided;
};

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
 * @brief The pattern a shape's edit leaves a token with: a terminated one its body, a delimited one its opener.
 * @param regex The token's pattern, of that shape for the byte.
 * @param shape The shape, terminated or delimited.
 * @return The pattern after the edit.
 */
[[nodiscard]] regex::Regex edited_by(const regex::Regex& regex, const Shape shape)
{
    const auto parts{parts_of(regex)};

    if (shape == Shape::delimited)
    {
        return parts.front();
    }

    std::vector<regex::Regex> body{parts.begin(), parts.end() - 1};

    return body.size() == 1 ? std::move(body.front()) : regex::Regex{.node = regex::Concat{.regexes = std::move(body)}};
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
 * @brief Takes the byte off a rule of the probe, so that the next round sees what the rule had won: narrowed where the
 *        rule can lose the byte, and by its own offered shape where it cannot, since a rule that spells the byte out is
 *        edited by its shape and not by narrowing. A rule with neither is left as it stands and goes on consuming the
 *        byte, which is what the steps report of it.
 * @param probed The probe's copy of the rule, edited.
 * @param rule The rule as the set gives it.
 * @param byte The byte.
 */
void take_from_probe(Token_rule& probed, const Token_rule& rule, const unsigned char byte)
{
    if (can_lose(probed.regex, byte))
    {
        exclude(probed.regex, byte);

        return;
    }

    if (const auto shape{shape_of(rule.regex, byte)}; shape == Shape::terminated || shape == Shape::delimited)
    {
        probed.regex = edited_by(rule.regex, shape);
    }
}

/**
 * @brief What a consuming rule's shapes offer on their own: for the terminated and the delimited shape the rule has,
 *        the edit the shape names applied to the rule alone, the rest of the set as it stands.
 * @param rule The rule.
 * @param edited The set before any narrowing.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte.
 * @return The choices, terminated first.
 */
[[nodiscard]] std::vector<Choice> choices_of(
        const Token_rule& rule, const Token_set& edited, const std::vector<unsigned char>& before,
        const unsigned char byte)
{
    std::vector<Choice> choices;

    for (const auto shape : {Shape::terminated, Shape::delimited})
    {
        if (shape == Shape::terminated ? !is_terminated(rule.regex, byte) : !is_delimited(rule.regex, byte))
        {
            continue;
        }

        auto alone{edited};

        std::ranges::find(alone.rules, rule.id, &Token_rule::id)->regex = edited_by(rule.regex, shape);

        choices.push_back({.token = rule.id, .shape = shape, .after = outcome(alone, before, byte)});
    }

    return choices;
}

/**
 * @brief Gives a rule's copy in the combined set the edit its shape names: a terminated one its body, a delimited one
 *        its opener, the rest narrowed as the steps narrow them. A rule that cannot lose the byte is left as it stands,
 *        since narrowing it would leave a choice with no regex in it, and the steps report such a rule as one no edit
 *        of this analysis decides, so editing it here would price an edit the report never offered.
 * @param taken The rule's copy in the combined set, edited.
 * @param rule The rule as the set gives it.
 * @param byte The byte.
 * @return Whether an edit of a shape was taken, and whether the copy was narrowed.
 */
[[nodiscard]] Taken take_shape(Token_rule& taken, const Token_rule& rule, const unsigned char byte)
{
    switch (shape_of(rule.regex, byte))
    {
    case Shape::terminated:
    case Shape::delimited:
        taken.regex = edited_by(rule.regex, shape_of(rule.regex, byte));

        return Taken{.offered = true, .narrowed = false};
    case Shape::run:
    case Shape::other:
        if (!can_lose(taken.regex, byte))
        {
            break;
        }

        exclude(taken.regex, byte);

        return Taken{.offered = false, .narrowed = true};
    case Shape::fixed:
        break;
    }

    return Taken{.offered = false, .narrowed = false};
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
 * @brief The byte's own token, as a rule of the set.
 * @param byte The byte.
 * @param own The id and the priority it takes.
 * @param discarded Whether it is discarded.
 * @return The rule.
 */
[[nodiscard]] Token_rule byte_rule(const unsigned char byte, const Own_token own, const bool discarded)
{
    return Token_rule{
            .regex = regex::text(std::string(1, static_cast<char>(byte))),
            .id = own.id,
            .priority = own.priority,
            .discarded = discarded};
}

/**
 * @brief The id and the priority a token of the byte's own takes in a set.
 * @param set The set.
 * @return The id and the priority.
 */
[[nodiscard]] Own_token own_token(const Token_set& set)
{
    std::vector<std::size_t> ids;

    std::ranges::transform(set.rules, std::back_inserter(ids), &Token_rule::id);

    std::ranges::sort(ids);

    auto next_id{0UZ};

    for (const auto id : ids)
    {
        if (id == next_id)
        {
            ++next_id;
        }
    }

    auto next_priority{0UZ};

    for (const auto& rule : set.rules)
    {
        next_priority = std::max(next_priority, rule.priority + 1);
    }

    return Own_token{.id = next_id, .priority = next_priority};
}

/**
 * @brief Whether a token of the compiled set begins with the byte: the initial state consumes it live.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @return True when one does.
 */
[[nodiscard]] bool begins_token(const core::Lexer& lexer, const unsigned char byte)
{
    const auto& simulator{lexer.simulator()};

    const auto from_start{simulator.step(simulator.init_state(), byte)};

    return from_start && simulator.is_live(*from_start);
}

/**
 * @brief What each consuming token's shape offers on its own, and what every consumer taking its own shape's edit gives
 *        together, before the uniform narrowing edits anything.
 *
 * A rule a narrowing uncovers has a shape of its own to offer too. The set each round looks in, the probe, has every
 * consumer answered so far taken off the byte, so that the rule the narrowing uncovers is the next one asked for its
 * shape; the probe gives a narrowed-away byte a token of its own as the steps do, since without a token beginning with
 * the byte no rule is blamed for consuming it.
 * @param set The set as given, whose rules the shapes are read from.
 * @param edited The set normalised, the byte's own token added when no token began with it.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte.
 * @param own The id and the priority the byte's own token takes.
 * @return The choices and the combined outcome.
 */
[[nodiscard]] Offers shape_choices(
        const Token_set& set, const Token_set& edited, const std::vector<unsigned char>& before,
        const unsigned char byte, const Own_token own)
{
    Offers offers;

    auto together{edited};

    together.rules.reserve(set.rules.size() + 1);

    auto offered{false};

    auto narrowed_discarded{false};

    std::set<std::size_t> shaped;

    auto probe{edited};

    auto probed{compile(probe)};

    for (;;)
    {
        auto fresh{false};

        for (const auto token : consumers(probed, probe, byte))
        {
            const auto rule{std::ranges::find(set.rules, token, &Token_rule::id)};

            if (rule == set.rules.end() || !shaped.insert(token).second)
            {
                continue;
            }

            fresh = true;

            take_from_probe(*std::ranges::find(probe.rules, token, &Token_rule::id), *rule, byte);

            std::ranges::move(choices_of(*rule, edited, before, byte), std::back_inserter(offers.choices));

            const auto [edit, narrowed]{
                    take_shape(*std::ranges::find(together.rules, token, &Token_rule::id), *rule, byte)};

            offered = offered || edit;

            narrowed_discarded = narrowed_discarded || (narrowed && rule->discarded);
        }

        if (!fresh)
        {
            break;
        }

        probed = compile(probe);

        if (!matched_alone(probed, byte))
        {
            probe.rules.push_back(byte_rule(byte, own, narrowed_discarded));

            probed = compile(probe);
        }
    }

    if (!offered)
    {
        return offers;
    }

    if (!matched_alone(compile(together), byte))
    {
        together.rules.push_back(byte_rule(byte, own, narrowed_discarded));
    }

    offers.together = outcome(together, before, byte);

    return offers;
}

/**
 * @brief Narrows the consumers one step at a time until the byte certifies or every token the byte is left to has
 *        been answered, by a step or by an obstruction, as price() narrows them.
 * @param edited The set, narrowed step by step.
 * @param lexer The set compiled, recompiled after every edit.
 * @param byte The byte.
 * @param own The id and the priority the byte's own token takes.
 * @return The steps and the tokens no step answers.
 */
[[nodiscard]] Answers narrowing_steps(
        Token_set& edited, core::Lexer& lexer, const unsigned char byte, const Own_token own)
{
    Answers answers;

    // The token the byte may be given of its own is answered before the first step: it is none of the set's rules, so
    // it is no edit an author makes, no obstruction of theirs, and not a token the report can name.
    std::set<std::size_t> answered{own.id};

    for (;;)
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
            // The narrowing is not applied to a rule that cannot lose the byte. The rule is an obstruction only where
            // every word it matches holds the byte fixed past its first byte; a rule with a word holding it fixed only
            // as its first byte, the initial state's occurrence, which begins a token, is reported as one this analysis
            // decides nothing about rather than as an impossibility.
            (fixed_mid_token(rule->regex, byte) ? answers.immovable : answers.undecided).push_back(token);

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
            const auto discarded{rule->discarded};

            edited.rules.push_back(byte_rule(byte, own, discarded));

            step.separated = true;

            step.separated_discarded = discarded;

            lexer = compile(edited);
        }

        step.exact = lexer.is_split_point(static_cast<char>(byte));

        step.modulo = lexer.is_split_point_ignoring(static_cast<char>(byte));

        answers.steps.push_back(step);

        if (step.exact)
        {
            break;
        }
    }

    return answers;
}

} // namespace

Pricing price(const Token_set& set, const unsigned char byte)
{
    auto edited{set};

    // Every pattern as the scanner sees it, the spellings that match the same words taken off, so that each shape
    // and each edit reads the one pattern whatever the set spelled: the choice of `[ab]\n` with `any_of("")` is
    // `[ab]\n`, terminated by the newline, and not a choice with an alternative reading as a word.
    for (auto& rule : edited.rules)
    {
        rule.regex = normalized(rule.regex);
    }

    // Room for the one token the analysis may add, taken now: GCC 13 misreads the move a later reallocation would make
    // of a rule's pattern as a read of something uninitialized, and the build treats the warning as an error.
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

    const auto own{own_token(set)};

    // A byte no token begins with is reported by neither certificate, every occurrence of it lying mid-token or
    // nowhere, and no narrowing changes that. It is given a token of its own before anything else, visible since no
    // token of the set says what it would be discarded as, and every edit below is made on the set holding it.
    if (!begins_token(lexer, byte))
    {
        edited.rules.push_back(byte_rule(byte, own, false));

        lexer = compile(edited);

        pricing.given = outcome(edited, before, byte);
    }

    auto [choices, together]{shape_choices(set, edited, before, byte, own)};

    pricing.choices = std::move(choices);

    pricing.together = std::move(together);

    auto [steps, immovable, undecided]{narrowing_steps(edited, lexer, byte, own)};

    pricing.steps = std::move(steps);

    pricing.immovable = std::move(immovable);

    pricing.undecided = std::move(undecided);

    std::ranges::set_difference(exact_bytes(lexer), before, std::back_inserter(pricing.gained));

    std::erase(pricing.gained, byte);

    return pricing;
}

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

} // namespace munch::tools::audit

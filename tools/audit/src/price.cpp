#include "munch/tools/audit/price.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "munch/regex/regex.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/pattern_shape.hpp"
#include "munch/tools/audit/report.hpp"
#include "munch/tools/audit/word_kinds.hpp"

namespace munch::tools::audit
{
namespace
{
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
 *        the highest is no id at all where a rule carries the largest std::size_t, and a priority past every rule's, so
 *        nothing else moves.
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
    std::vector<Choice> choices{};

    /**
     * @brief Where the byte stands once every consumer takes its edit, absent when no shape offered one.
     */
    std::optional<Outcome> together{};
};

/**
 * @brief The narrowing's answers to the consumers: the steps it took, and the tokens it could not take a step on.
 */
struct Answers
{
    /**
     * @brief The steps, in the order they were made.
     */
    std::vector<Price_step> steps{};

    /**
     * @brief The tokens every word of which holds the byte fixed past its first byte.
     */
    std::vector<std::size_t> immovable{};

    /**
     * @brief The tokens that cannot lose the byte while some word of them holds it fixed only as its first byte.
     */
    std::vector<std::size_t> undecided{};
};

/**
 * @brief Returns the bytes a compiled set certifies exactly.
 * @param lexer The compiled set.
 * @return The bytes, ascending.
 */
[[nodiscard]] std::vector<unsigned char> exact_bytes(const core::Lexer& lexer)
{
    const auto certifies{[&lexer](const std::size_t value) { return lexer.is_split_point(static_cast<char>(value)); }};

    std::vector<unsigned char> bytes{};

    for (const auto value : std::views::iota(0UZ, byte_values) | std::views::filter(certifies))
    {
        bytes.push_back(static_cast<unsigned char>(value));
    }

    return bytes;
}

/**
 * @brief Returns the bytes a compiled set certifies exactly that it did not before any edit, the byte priced left out.
 * @param lexer The compiled set.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte priced.
 * @return The bytes gained, ascending.
 */
[[nodiscard]] std::vector<unsigned char> gained_bytes(
        const core::Lexer& lexer, const std::vector<unsigned char>& before, const unsigned char byte)
{
    const auto certified{exact_bytes(lexer)};

    std::vector<unsigned char> gained{};

    std::ranges::set_difference(certified, before, std::back_inserter(gained));

    std::erase(gained, byte);

    return gained;
}

/**
 * @brief Returns the pattern a shape's edit leaves a token with: a terminated one its body, a delimited one its opener.
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

    if (body.size() == 1)
    {
        return std::move(body.front());
    }

    return {.node = regex::Concat{.regexes = std::move(body)}};
}

/**
 * @brief Returns where the byte stands over an edited set, against the bytes certified before any edit.
 * @param edited The set after the edit.
 * @param before The bytes certified exactly before any edit, ascending.
 * @param byte The byte priced.
 * @return The outcome.
 */
[[nodiscard]] Outcome outcome(
        const Token_set& edited, const std::vector<unsigned char>& before, const unsigned char byte)
{
    const auto lexer{compile(edited)};

    const auto exact{lexer.is_split_point(static_cast<char>(byte))};

    const auto modulo{lexer.is_split_point_ignoring(static_cast<char>(byte))};

    auto gained{gained_bytes(lexer, before, byte)};

    return {.exact = exact, .modulo = modulo, .gained = std::move(gained)};
}

/**
 * @brief Returns the tokens the blame names for a byte, each once, in rule order: the order the set lists its rules,
 *        which for a set read from a file is the file's, whatever the ids are.
 * @param lexer The compiled set.
 * @param set The set it was compiled from, whose order is the rule order.
 * @param byte The byte.
 * @return The token ids.
 */
[[nodiscard]] std::vector<std::size_t> consumers(
        const core::Lexer& lexer, const Token_set& set, const unsigned char byte)
{
    std::vector<std::size_t> tokens{};

    for (const auto& [blamed, token, after] : blame(lexer))
    {
        if (blamed == byte && !std::ranges::contains(tokens, token))
        {
            tokens.push_back(token);
        }
    }

    // A token the set does not list, which none of the blamed ones is, would sort last.
    const auto position{[&set](const std::size_t token) {
        return std::ranges::find(set.rules, token, &Token_rule::id) - set.rules.begin();
    }};

    std::ranges::sort(tokens, {}, position);

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
 * @brief Returns what a consuming rule's shapes offer on their own: for the terminated and the delimited shape the rule
 *        has, the edit the shape names applied to the rule alone, the rest of the set as it stands.
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
    std::vector<Choice> choices{};

    for (const auto shape : {Shape::terminated, Shape::delimited})
    {
        const auto has_shape{
                shape == Shape::terminated ? is_terminated(rule.regex, byte) : is_delimited(rule.regex, byte)};

        if (!has_shape)
        {
            continue;
        }

        auto alone{edited};

        const auto copy{std::ranges::find(alone.rules, rule.id, &Token_rule::id)};

        copy->regex = edited_by(rule.regex, shape);

        auto after{outcome(alone, before, byte)};

        choices.push_back({.token = rule.id, .shape = shape, .after = std::move(after)});
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
    const auto shape{shape_of(rule.regex, byte)};

    switch (shape)
    {
    case Shape::terminated:
    case Shape::delimited:
        taken.regex = edited_by(rule.regex, shape);

        return {.offered = true, .narrowed = false};
    case Shape::run:
    case Shape::other:
        if (!can_lose(taken.regex, byte))
        {
            break;
        }

        exclude(taken.regex, byte);

        return {.offered = false, .narrowed = true};
    case Shape::fixed:
        break;
    }

    return {.offered = false, .narrowed = false};
}

/**
 * @brief Returns whether some token of the compiled set matches the byte on its own.
 * @param lexer The compiled set.
 * @param byte The byte.
 * @return True when a one-byte input of it tokenizes.
 */
[[nodiscard]] bool matched_alone(const core::Lexer& lexer, const unsigned char byte)
{
    const std::string input{static_cast<char>(byte)};

    const auto [token, length]{lexer.tokenize<std::size_t>(input)};

    return length == 1;
}

/**
 * @brief Returns the byte's own token, as a rule of the set.
 * @param byte The byte.
 * @param own The id and the priority it takes.
 * @param discarded Whether it is discarded.
 * @return The rule.
 */
[[nodiscard]] Token_rule byte_rule(const unsigned char byte, const Own_token own, const bool discarded)
{
    const std::string spelled{static_cast<char>(byte)};

    return {.regex = regex::text(spelled), .id = own.id, .priority = own.priority, .discarded = discarded};
}

/**
 * @brief Returns the id and the priority a token of the byte's own takes in a set.
 * @param set The set.
 * @return The id and the priority.
 */
[[nodiscard]] Own_token own_token(const Token_set& set)
{
    std::vector<std::size_t> ids{};

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

    for (const auto& [rule_regex, id, priority, discarded] : set.rules)
    {
        next_priority = std::max(next_priority, priority + 1);
    }

    return {.id = next_id, .priority = next_priority};
}

/**
 * @brief Returns whether a token of the compiled set begins with the byte: the initial state consumes it live.
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
 * @brief Returns what each consuming token's shape offers on its own, and what every consumer taking its own shape's
 *        edit gives together, before the uniform narrowing edits anything.
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
    Offers offers{};

    auto together{edited};

    together.rules.reserve(set.rules.size() + 1);

    auto offered{false};

    auto narrowed_discarded{false};

    std::set<std::size_t> shaped{};

    auto probe{edited};

    auto probed{compile(probe)};

    // Takes the byte off the probe's copy of the rule, keeps its own choices and applies its shape's edit to the set.
    const auto shape_token{[&](const std::size_t token) {
        const auto rule{std::ranges::find(set.rules, token, &Token_rule::id)};

        if (rule == set.rules.end())
        {
            return false;
        }

        const auto [position, inserted]{shaped.insert(token)};

        if (!inserted)
        {
            return false;
        }

        auto& probed_rule{*std::ranges::find(probe.rules, token, &Token_rule::id)};

        take_from_probe(probed_rule, *rule, byte);

        auto choices{choices_of(*rule, edited, before, byte)};

        std::ranges::move(choices, std::back_inserter(offers.choices));

        auto& combined{*std::ranges::find(together.rules, token, &Token_rule::id)};

        const auto [edit, narrowed]{take_shape(combined, *rule, byte)};

        offered = offered || edit;

        narrowed_discarded = narrowed_discarded || (narrowed && rule->discarded);

        return true;
    }};

    for (;;)
    {
        auto fresh{false};

        for (const auto token : consumers(probed, probe, byte))
        {
            fresh = shape_token(token) || fresh;
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

    const auto compiled{compile(together)};

    if (!matched_alone(compiled, byte))
    {
        together.rules.push_back(byte_rule(byte, own, narrowed_discarded));
    }

    offers.together = outcome(together, before, byte);

    return offers;
}

/**
 * @brief Narrows the consumers one step at a time until the byte certifies or every token the byte is left to has been
 *        answered, by a step or by an obstruction, as price() narrows them.
 * @param edited The set, narrowed step by step.
 * @param lexer The set compiled, recompiled after every edit.
 * @param byte The byte.
 * @param own The id and the priority the byte's own token takes.
 * @return The steps and the tokens no step answers.
 */
[[nodiscard]] Answers narrowing_steps(
        Token_set& edited, core::Lexer& lexer, const unsigned char byte, const Own_token own)
{
    Answers answers{};

    // The token the byte may be given of its own is answered before the first step: it is none of the set's rules, so
    // it is no edit an author makes, no obstruction of theirs, and not a token the report can name.
    std::set answered{own.id};

    const auto is_open{[&answered](const std::size_t token) { return !answered.contains(token); }};

    for (;;)
    {
        const auto remaining{consumers(lexer, edited, byte)};

        const auto next{std::ranges::find_if(remaining, is_open)};

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
            // A rule that cannot lose the byte is not narrowed: it is an obstruction where every word it matches holds
            // the byte fixed past its first byte, and one with a word holding it fixed only as its first byte, which
            // begins a token, is one this analysis decides nothing about.
            auto& unanswered{fixed_mid_token(rule->regex, byte) ? answers.immovable : answers.undecided};

            unanswered.push_back(token);

            continue;
        }

        const auto shape{shape_of(rule->regex, byte)};

        exclude(rule->regex, byte);

        lexer = compile(edited);

        const auto separated{!matched_alone(lexer, byte)};

        // The rule is read before the byte's own rule is added, which may move the rules.
        const auto discarded{rule->discarded};

        if (separated)
        {
            edited.rules.push_back(byte_rule(byte, own, discarded));

            lexer = compile(edited);
        }

        const auto exact{lexer.is_split_point(static_cast<char>(byte))};

        const auto modulo{lexer.is_split_point_ignoring(static_cast<char>(byte))};

        answers.steps.push_back(
                {.token = token,
                 .shape = shape,
                 .separated = separated,
                 .separated_discarded = separated && discarded,
                 .exact = exact,
                 .modulo = modulo});

        if (exact)
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

    // Every pattern as the scanner sees it, the spellings that match the same words taken off, so that each shape and
    // each edit reads the one pattern whatever the set spelled: the choice of `[ab]\n` with `any_of("")` is `[ab]\n`,
    // terminated by the newline, and not a choice with an alternative reading as a word.
    for (auto& rule : edited.rules)
    {
        rule.regex = normalized(rule.regex);
    }

    // Room for the one token the analysis may add, taken now: GCC 13 misreads the move a later reallocation would make
    // of a rule's pattern as a read of something uninitialized, and the build treats the warning as an error.
    edited.rules.reserve(set.rules.size() + 1);

    auto lexer{compile(edited)};

    const auto before{exact_bytes(lexer)};

    const auto exact_before{lexer.is_split_point(static_cast<char>(byte))};

    const auto modulo_before{lexer.is_split_point_ignoring(static_cast<char>(byte))};

    if (exact_before)
    {
        return {.byte = byte,
                .exact_before = exact_before,
                .modulo_before = modulo_before,
                .given = std::nullopt,
                .steps = {},
                .immovable = {},
                .undecided = {},
                .gained = {},
                .choices = {},
                .together = std::nullopt};
    }

    const auto own{own_token(set)};

    std::optional<Outcome> given{};

    // A byte no token begins with is reported by neither certificate, every occurrence of it lying mid-token or
    // nowhere, and no narrowing changes that. It is given a token of its own before anything else, visible since no
    // token of the set says what it would be discarded as, and every edit below is made on the set holding it.
    if (!begins_token(lexer, byte))
    {
        edited.rules.push_back(byte_rule(byte, own, false));

        lexer = compile(edited);

        given = outcome(edited, before, byte);
    }

    auto [choices, together]{shape_choices(set, edited, before, byte, own)};

    auto [steps, immovable, undecided]{narrowing_steps(edited, lexer, byte, own)};

    auto gained{gained_bytes(lexer, before, byte)};

    return {.byte = byte,
            .exact_before = exact_before,
            .modulo_before = modulo_before,
            .given = std::move(given),
            .steps = std::move(steps),
            .immovable = std::move(immovable),
            .undecided = std::move(undecided),
            .gained = std::move(gained),
            .choices = std::move(choices),
            .together = std::move(together)};
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

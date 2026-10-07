/*
 * Asserts the validation figures of paper/split-points/split-points.tex, in the section deciding the relaxed condition.
 *
 * Two kinds of claim are made there and they deserve different treatment.
 *
 * The first two are properties: no symbol the relaxed condition admits ever fails to split, and no symbol the exact
 * condition admits is ever lost. Those are checked here over random token sets against an exhaustive oracle, every
 * string up to a bounded length on a three-symbol alphabet, so no symbol can be judged safe merely because a sampled
 * corpus never exercised it.
 *
 * The third is that the condition is conservative. A percentage from a random sweep says as much about the generator as
 * about the condition, so the sweep counts are asserted for reproducibility while the report leans on a named witness
 * instead: a specific small token set, written out below, where splitting is safe modulo the ignored set and the
 * condition still refuses. That is checkable by hand and does not move when the generator changes.
 */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <iterator>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"
#include "random.hpp"

namespace
{
using namespace figures;

/**
 * @brief The three symbols every token set and every string is drawn over.
 */
constexpr std::string_view alphabet{"abc"};

/**
 * @brief The most DFA states a swept token set may compile to; a set past it is skipped.
 */
constexpr std::size_t state_limit{400};

/**
 * @brief The token sets each sweep draws.
 */
constexpr std::size_t sweep_rounds{400};

/**
 * @brief The string length the published sweep is exhaustive to.
 */
constexpr std::size_t longer_bound{8};

/**
 * @brief The shorter string length the sweep is repeated at, to show the conservative count is bound-sensitive.
 */
constexpr std::size_t shorter_bound{6};

/**
 * @brief The string length the conservatism witness's oracle is exhaustive to.
 */
constexpr std::size_t witness_bound{6};

/**
 * @brief The fewest token kinds a swept token set draws.
 */
constexpr unsigned fewest_kinds{2};

/**
 * @brief How many kind counts a swept token set's size is drawn from, so a set holds two to four kinds.
 */
constexpr unsigned kind_spread{3};

/**
 * @brief The nesting depth each swept token's regex is drawn to.
 */
constexpr unsigned regex_depth{3};

/**
 * @brief The lowest priority a swept token draws.
 */
constexpr unsigned lowest_priority{1};

/**
 * @brief How many priorities a swept token's priority is drawn from, so it is one or two.
 */
constexpr unsigned priority_spread{2};

/**
 * @brief The fewest symbols a drawn literal holds.
 */
constexpr unsigned fewest_symbols{1};

/**
 * @brief How many lengths a drawn literal's length is drawn from, so it holds one or two symbols.
 */
constexpr unsigned symbol_spread{2};

/**
 * @brief Conservative pairs, each the round a token set was drawn in and a symbol.
 */
using Pairs_t = std::set<std::pair<std::size_t, char>>;

/**
 * @brief The oracle's verdict on one symbol.
 */
struct Verdict
{
    /**
     * @brief Whether some completely tokenizable string places the symbol after its first byte.
     */
    bool exercised{false};

    /**
     * @brief Whether every such cut splits the scan modulo the ignored kinds.
     */
    bool safe{true};
};

/**
 * @brief The counts of one sweep over the exercised symbols of every token set.
 */
struct Sweep
{
    /**
     * @brief The token sets within the state limit.
     */
    std::size_t token_sets{0};

    /**
     * @brief The symbols the report's condition admits.
     */
    std::size_t admitted{0};

    /**
     * @brief The admitted symbols the oracle finds unsafe.
     */
    std::size_t unsound{0};

    /**
     * @brief The symbols the exact certificate admits and the report's condition refuses.
     */
    std::size_t lost{0};

    /**
     * @brief The symbols the oracle finds safe and the report's condition refuses.
     */
    std::size_t conservative{0};

    /**
     * @brief The symbols the shipped predicate admits.
     */
    std::size_t shipped_admitted{0};

    /**
     * @brief The symbols the shipped predicate admits and the oracle finds unsafe.
     */
    std::size_t shipped_unsound{0};

    /**
     * @brief The symbols the report's condition admits and the shipped predicate refuses.
     */
    std::size_t shipped_lost{0};

    /**
     * @brief The (round, symbol) identity of every conservative pair, so a bound change must name what it reclassified.
     */
    Pairs_t conservative_pairs{};
};

/**
 * @brief Draws a random nonempty class over the alphabet, each symbol drawn in or out until one is in.
 *
 * Set exposes no emptiness query, so the count is tracked here: an empty class would register a token matching nothing,
 * which is not what this is sweeping.
 * @param random The stream.
 * @return The class.
 */
Set random_set(Random& random)
{
    Set set{};

    auto chosen{0U};

    do
    {
        for (const auto symbol : alphabet)
        {
            if (random.next(2) != 0)
            {
                set = set + symbol;

                ++chosen;
            }
        }
    } while (chosen == 0);

    return set;
}

/**
 * @brief Draws a random regex over the alphabet: at depth 0, or on one draw in three, a class or a literal of one or
 *        two symbols; otherwise a concatenation, a choice, a plus, an optional or a star of regexes one level
 *        shallower.
 * @param random The stream.
 * @param depth The deepest nesting left.
 * @return The regex.
 */
Regex random_regex(Random& random, const unsigned depth)
{
    if (depth == 0 || random.next(3) == 0)
    {
        if (random.next(2) == 0)
        {
            return any_of(random_set(random));
        }

        std::string literal{};

        for (auto count{fewest_symbols + random.next(symbol_spread)}; count > 0; --count)
        {
            const auto drawn{random.next(static_cast<unsigned>(alphabet.size()))};

            literal += alphabet[drawn];
        }

        return text(literal);
    }

    // Draws the left operand into a local before the right, so a seed yields one expression whichever order the
    // compiler evaluates arguments in.
    const auto operands{[&random, depth] {
        auto first{random_regex(random, depth - 1)};

        auto second{random_regex(random, depth - 1)};

        return std::pair{std::move(first), std::move(second)};
    }};

    switch (random.next(5))
    {
    case 0:
    {
        const auto [first, second]{operands()};

        return concat(first, second);
    }
    case 1:
    {
        const auto [first, second]{operands()};

        return choice(first, second);
    }
    case 2:
    {
        const auto operand{random_regex(random, depth - 1)};

        return plus(operand);
    }
    case 3:
    {
        const auto operand{random_regex(random, depth - 1)};

        return optional(operand);
    }
    default:
    {
        const auto operand{random_regex(random, depth - 1)};

        return kleene(operand);
    }
    }
}

/**
 * @brief Lists every string over the alphabet of length one to a bound, with no sampling, so no vacuous verdicts.
 * @param max_length The bound.
 * @return The strings, by length and within one length in alphabet order.
 */
std::vector<std::string> every_string(const std::size_t max_length)
{
    std::vector<std::string> corpus{};

    std::vector<std::string> frontier{""};

    for (std::size_t length{0}; length < max_length; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : frontier)
        {
            for (const auto symbol : alphabet)
            {
                next.push_back(prefix + symbol);
            }
        }

        corpus.insert(corpus.end(), next.begin(), next.end());

        frontier = std::move(next);
    }

    return corpus;
}

/**
 * @brief Runs the exhaustive oracle: every completely tokenizable string of the corpus cut before every byte but its
 *        first, each cut held to the serial scan modulo the ignored kinds; the cut before the first byte is a boundary
 *        of every scan and is exempted from every check.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param corpus The strings.
 * @return Per byte value, its verdict.
 */
std::vector<Verdict> oracle(
        const munch::core::Lexer& lexer, const Kinds_t& ignored, const std::vector<std::string>& corpus)
{
    std::vector<Verdict> verdicts(static_cast<std::size_t>(byte_values));

    for (const auto& text : corpus)
    {
        const auto [serial, consumed]{scan(lexer, text)};

        if (consumed != text.size())
        {
            continue;
        }

        for (std::size_t at{1}; at < text.size(); ++at)
        {
            auto& [exercised, safe]{verdicts[static_cast<unsigned char>(text[at])]};

            exercised = true;

            const auto agrees{cut_survives(lexer, ignored, text, serial, at)};

            safe = safe && agrees;
        }
    }

    return verdicts;
}

/**
 * @brief Evaluates the relaxed condition as the report states it on the compiled tables.
 *
 * The library's rule is stronger: the restart need only reach a state with the same future once ignored kinds are not
 * told apart, where the report's condition asks for the same state. The figures are the report's, so they are taken of
 * the report's condition, and the shipped predicate is checked against it separately: it admits at least as much and is
 * never unsound.
 * @param lexer The lexer.
 * @param ignored The ignored kinds.
 * @param symbol The symbol.
 * @return True when the report's condition admits the symbol.
 */
bool published_condition(const munch::core::Lexer& lexer, const Kinds_t& ignored, const unsigned char symbol)
{
    const auto& simulator{lexer.simulator()};

    const auto states{simulator.state_count()};

    const auto discarded{[&](const std::size_t state) {
        const auto token{simulator.accepted(state)};

        return token.has_value() && ignored.contains(token->id());
    }};

    std::vector<bool> reaches_kept(states, false);

    const auto reaches_a_kept_accept{[&](const std::size_t state) {
        if (simulator.is_accepting(state) && !discarded(state))
        {
            return true;
        }

        const auto steps_into_reaching{[&](const std::size_t byte) {
            const auto to{simulator.step(state, static_cast<unsigned char>(byte))};

            return to.has_value() && reaches_kept[*to];
        }};

        const auto bytes{std::views::iota(std::size_t{0}, static_cast<std::size_t>(byte_values))};

        return std::ranges::any_of(bytes, steps_into_reaching);
    }};

    for (bool changed{true}; changed;)
    {
        changed = false;

        for (std::size_t state{0}; state < states; ++state)
        {
            if (reaches_kept[state] || !reaches_a_kept_accept(state))
            {
                continue;
            }

            reaches_kept[state] = true;

            changed = true;
        }
    }

    const auto init{simulator.init_state()};

    const auto consumes{[&](const std::size_t state) {
        const auto to{simulator.step(state, symbol)};

        return to.has_value() && simulator.is_live(*to);
    }};

    for (std::size_t state{0}; state < states; ++state)
    {
        const auto consumes_live{simulator.is_live(state) && consumes(state)};

        const auto exempt_restart{state == init && !simulator.init_reentrant()};

        if (!consumes_live || exempt_restart)
        {
            continue;
        }

        const auto restarts_alike{simulator.step(state, symbol) == simulator.step(init, symbol)};

        if (!discarded(state) || reaches_kept[state] || !restarts_alike)
        {
            return false;
        }
    }

    return consumes(init);
}

/**
 * @brief Sweeps random token sets of two to four kinds, each kind ignored on one draw in two, over the oracle and both
 *        conditions; a token set past state_limit or with an empty language is skipped.
 * @param rounds The token sets drawn.
 * @param max_length The oracle's string length bound.
 * @return The counts.
 */
Sweep sweep(const std::size_t rounds, const std::size_t max_length)
{
    const auto corpus{every_string(max_length)};

    Random random{figure_seed};

    Sweep totals{};

    const auto tally_round{[&](const munch::core::Lexer& lexer, const Kinds_t& ignored, const std::size_t round) {
        const auto verdicts{oracle(lexer, ignored, corpus)};

        ++totals.token_sets;

        for (const auto symbol : alphabet)
        {
            const auto& [exercised, safe]{verdicts[static_cast<unsigned char>(symbol)]};

            if (!exercised)
            {
                continue;
            }

            const auto claim{published_condition(lexer, ignored, static_cast<unsigned char>(symbol))};

            const auto shipped{lexer.is_split_point_ignoring(symbol)};

            if (shipped)
            {
                ++totals.shipped_admitted;
            }

            if (shipped && !safe)
            {
                ++totals.shipped_unsound;
            }

            if (claim && !shipped)
            {
                ++totals.shipped_lost;
            }

            if (claim)
            {
                ++totals.admitted;
            }

            if (claim && !safe)
            {
                ++totals.unsound;
            }

            if (!claim && safe)
            {
                ++totals.conservative;

                totals.conservative_pairs.emplace(round, static_cast<char>(symbol));
            }

            if (lexer.is_split_point(symbol) && !claim)
            {
                ++totals.lost;
            }
        }
    }};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        munch::core::Builder builder{};

        const auto kinds{fewest_kinds + random.next(kind_spread)};

        Kinds_t ignored{};

        for (std::size_t kind{0}; kind < kinds; ++kind)
        {
            // Two draws in one argument list would be unsequenced, exactly as above.
            const auto pattern{random_regex(random, regex_depth)};

            const auto priority{lowest_priority + random.next(priority_spread)};

            builder.add_token(pattern, kind, priority);

            if (random.next(2) == 0)
            {
                ignored.insert(kind);
            }
        }

        builder.set_state_limit(state_limit);

        try
        {
            const auto lexer{build_ignoring(builder, ignored)};

            tally_round(lexer, ignored, round);
        }
        catch (const std::exception&)
        {
            // A state limit or an empty language throws, and neither is what this sweeps.
        }
    }

    return totals;
}

/**
 * @brief Checks a token set where splitting is safe modulo the ignored set and the condition still refuses.
 *
 * Two ignored tokens, \c ab* and \c b+, and one kept token \c c. Splitting at a 'b' inside an \c ab* token is always
 * safe modulo the ignored kinds: the left piece is a shorter \c ab* and the right is a \c b+, and both are discarded.
 * The condition refuses because the two scans do not reconverge. Advancing on 'b' from inside \c ab* stays in a state
 * accepting \c ab*, while advancing on 'b' from the initial state enters one accepting \c b+, and those accept
 * different tokens so minimization keeps them apart. Insisting on immediate reconvergence is what makes the test local,
 * and this is what it costs.
 * @return True when the oracle finds the cut at 'b' safe and exercised and the condition refuses it.
 */
bool conservatism_witness()
{
    munch::core::Builder builder{};

    builder.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), std::size_t{0}, 1);

    builder.add_token(plus(any_of(Set{'b'})), std::size_t{1}, 1);

    builder.add_token(text("c"), std::size_t{2}, 1);

    const Kinds_t ignored{0, 1};

    const auto lexer{build_ignoring(builder, ignored)};

    const auto strings{every_string(witness_bound)};

    const auto verdicts{oracle(lexer, ignored, strings)};

    const auto& [exercised, safe]{verdicts[static_cast<unsigned char>('b')]};

    return exercised && safe && !published_condition(lexer, ignored, 'b');
}

} // namespace

/**
 * @brief Asserts the validation figures: the sweep at two bounds, the reclassified pairs, the conservatism witness and
 *        the shipped predicate on the same sweep, and prints the verdict.
 * @return EXIT_SUCCESS when every figure agrees with the report, EXIT_FAILURE otherwise.
 */
int main()
{
    Figure_check check{};

    std::cout << std::format(
            "{} random token sets, exhaustive to length {} over a three-symbol alphabet\n", sweep_rounds, longer_bound);

    const auto eight{sweep(sweep_rounds, longer_bound)};

    check("token sets swept", eight.token_sets, sweep_rounds);

    check("symbols the condition admits", eight.admitted, std::size_t{265});

    check("admitted yet unsafe to split at", eight.unsound, std::size_t{0});

    check("admitted by the exact certificate yet refused by the relaxed one", eight.lost, std::size_t{0});

    check("safe to split at yet refused", eight.conservative, std::size_t{97});

    // Raising the bound reclassifies two pairs, which shows the count is bound-sensitive. It is the named witness
    // below, not this delta, that establishes the condition is genuinely conservative.
    const auto six{sweep(sweep_rounds, shorter_bound)};

    check("safe yet refused at the shorter bound", six.conservative, std::size_t{99});

    check("pairs reclassified by the longer bound", six.conservative - eight.conservative, std::size_t{2});

    // A counterexample within length six is one within length eight, so the longer bound can only remove pairs;
    // asserting the identities pins that no offsetting additions hide inside the aggregate difference of two.
    const auto contained{std::ranges::includes(six.conservative_pairs, eight.conservative_pairs)};

    check("every length-eight conservative pair is conservative at length six too", contained, true);

    Pairs_t reclassified{};

    std::ranges::set_difference(
            six.conservative_pairs, eight.conservative_pairs, std::inserter(reclassified, reclassified.begin()));

    const Pairs_t expected{{4, 'a'}, {319, 'c'}};

    check("the reclassified pairs are the two named ones", reclassified == expected, true);

    check("a named token set where splitting is safe and the condition refuses", conservatism_witness(), true);

    std::cout << "\nthe library's strengthened predicate on the same sweep, beyond what the report states\n";

    check("admitted by the report's condition yet refused by the library", eight.shipped_lost, std::size_t{0});

    check("admitted by the library yet unsafe to split at", eight.shipped_unsound, std::size_t{0});

    std::cout << std::format("  info admitted by the library: {}\n", eight.shipped_admitted);

    const auto failures{check.failures()};

    std::cout
            << (failures == 0 ? "\nthe validation figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

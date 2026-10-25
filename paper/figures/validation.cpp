/*
 * Asserts the validation figures of paper/split-points/split-points.tex, in the section deciding the relaxed condition.
 *
 * Three kinds of claim are made there and they deserve different treatment.
 *
 * The first two are properties: no symbol the relaxed condition admits ever fails to split, and no symbol the exact
 * condition admits is ever lost. Those are checked here over random token sets against an exhaustive oracle, every
 * string up to a bounded length on a three-symbol alphabet, so no symbol can be judged safe merely because a sampled
 * corpus never exercised it. The same two properties are checked of the stronger rule the library ships.
 *
 * The third is how conservative each rule is. A percentage from a random sweep says as much about the generator as
 * about the condition, so the sweep counts are asserted for reproducibility while the report leans on named token sets
 * instead, written out below and checkable by hand: one the published condition refuses and the shipped rule admits,
 * two more where the shipped rule gains for each of its two changes, two both rules refuse although splitting is safe,
 * one that is unsafe because the restarted scan swallows a later kept token, and one that holds the exact decision's
 * output letters to the kinds' full width. What settles safety for the pairs the oracle cannot settle is the exact
 * decision of modulo.hpp, which decides every exercised pair and replays every negative answer through the scanner; its
 * answers are checked against the oracle wherever the oracle has one.
 */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <iterator>
#include <ostream>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "modulo.hpp"
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
 * @brief The string length the named token sets' oracle is exhaustive to.
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
     * @brief The symbol and token set pairs the oracle exercised.
     */
    std::size_t exercised{0};

    /**
     * @brief The symbols the published condition admits.
     */
    std::size_t admitted{0};

    /**
     * @brief The admitted symbols the oracle finds unsafe.
     */
    std::size_t unsound{0};

    /**
     * @brief The symbols the exact certificate admits and the published condition refuses.
     */
    std::size_t lost{0};

    /**
     * @brief The symbols the oracle finds safe and the published condition refuses.
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
     * @brief The symbols the published condition admits and the shipped predicate refuses.
     */
    std::size_t shipped_lost{0};

    /**
     * @brief The pairs the exact decision finds safe.
     */
    std::size_t exact_safe{0};

    /**
     * @brief The pairs the exact decision left undecided within its budget.
     */
    std::size_t exact_undecided{0};

    /**
     * @brief The pairs where the exact decision and the oracle disagree.
     */
    std::size_t exact_disagreeing{0};

    /**
     * @brief The pairs the exact decision finds safe and the published condition refuses.
     */
    std::size_t published_refused_safe{0};

    /**
     * @brief The pairs the exact decision finds safe and the shipped predicate refuses.
     */
    std::size_t shipped_refused_safe{0};

    /**
     * @brief The pairs the oracle finds safe, the published condition refuses and the exact decision refutes.
     */
    std::size_t conservative_refuted{0};

    /**
     * @brief The length of the longest witness the exact decision replayed.
     */
    std::size_t longest_witness{0};

    /**
     * @brief The longest witness's input.
     */
    std::string longest_witness_text{};

    /**
     * @brief The longest witness's cut.
     */
    std::size_t longest_witness_cut{0};

    /**
     * @brief The (round, symbol) identity of every conservative pair, so a bound change must name what it reclassified.
     */
    Pairs_t conservative_pairs{};
};

/**
 * @brief What the four judges say of one symbol of one named token set, so that each named example asserts all of them.
 */
struct Named
{
    /**
     * @brief Orders judgements member by member, so that a named example compares against its expected judgements.
     */
    auto operator<=>(const Named&) const = default;

    /**
     * @brief Whether the published condition admits the symbol.
     */
    bool published{false};

    /**
     * @brief Whether the shipped rule admits it.
     */
    bool shipped{false};

    /**
     * @brief Whether the exact decision finds it safe.
     */
    bool exact_safe{false};

    /**
     * @brief Whether the oracle exercises it and finds it safe.
     */
    bool oracle_safe{false};
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
 * @brief Lists every string over some letters of length one to a bound, with no sampling, so no vacuous verdicts.
 * @param letters The letters.
 * @param max_length The bound.
 * @return The strings, by length and within one length in the letters' order.
 */
std::vector<std::string> every_string(const std::string_view letters, const std::size_t max_length)
{
    std::vector<std::string> corpus{};

    std::vector<std::string> frontier{""};

    for (std::size_t length{0}; length < max_length; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& prefix : frontier)
        {
            for (const auto symbol : letters)
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
 * @brief Sweeps random token sets of two to four kinds, each kind ignored on one draw in two, over the oracle, both
 *        conditions and the exact decision; a token set past state_limit or with an empty language is skipped.
 * @param rounds The token sets drawn.
 * @param max_length The oracle's string length bound.
 * @return The counts.
 */
Sweep sweep(const std::size_t rounds, const std::size_t max_length)
{
    const auto corpus{every_string(alphabet, max_length)};

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

            ++totals.exercised;

            const auto claim{published_condition(lexer, ignored, static_cast<unsigned char>(symbol))};

            const auto shipped{lexer.is_split_point_ignoring(symbol)};

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

            const auto exact{decide_exactly(lexer, ignored, static_cast<unsigned char>(symbol))};

            if (exact.outcome == Exact_verdict::Outcome::budget)
            {
                ++totals.exact_undecided;

                continue;
            }

            const auto exact_safe{exact.outcome == Exact_verdict::Outcome::safe};

            const auto witness_length{exact.text.size()};

            // The oracle refutes with a string of at most max_length bytes, so the decision may not answer safe where
            // the oracle refutes, and a replayed witness is a refutation the oracle misses only for its length.
            const auto safe_where_refuted{exact_safe && !safe};

            const auto refuted_within_reach{!exact_safe && safe && witness_length <= max_length};

            if (safe_where_refuted || refuted_within_reach)
            {
                ++totals.exact_disagreeing;
            }

            if (exact_safe)
            {
                ++totals.exact_safe;
            }

            if (exact_safe && !claim)
            {
                ++totals.published_refused_safe;
            }

            if (exact_safe && !shipped)
            {
                ++totals.shipped_refused_safe;
            }

            if (!exact_safe && !claim && safe)
            {
                ++totals.conservative_refuted;
            }

            if (exact_safe || witness_length <= totals.longest_witness)
            {
                continue;
            }

            totals.longest_witness = witness_length;

            totals.longest_witness_text = exact.text;

            totals.longest_witness_cut = exact.cut;
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
 * @brief Prints what the four judges say of one symbol.
 * @param out The stream.
 * @param named The judgements.
 * @return The stream.
 */
std::ostream& operator<<(std::ostream& out, const Named& named)
{
    const auto& [published, shipped, exact_safe, oracle_safe]{named};

    return out << std::format(
                   "published {}, shipped {}, exact {}, oracle {}", published, shipped, exact_safe, oracle_safe);
}

/**
 * @brief Judges one symbol of a named token set by the published condition, the shipped rule, the exact decision and
 *        the oracle over every string of the token set's letters up to witness_bound.
 * @param builder The builder holding the token set.
 * @param ignored The ignored kinds.
 * @param letters The letters the oracle's strings are drawn over.
 * @param symbol The symbol.
 * @return The four judgements.
 */
Named judge(munch::core::Builder& builder, const Kinds_t& ignored, const std::string_view letters, const char symbol)
{
    const auto lexer{build_ignoring(builder, ignored)};

    const auto strings{every_string(letters, witness_bound)};

    const auto verdicts{oracle(lexer, ignored, strings)};

    const auto byte{static_cast<unsigned char>(symbol)};

    const auto& [exercised, safe]{verdicts[byte]};

    const auto published{published_condition(lexer, ignored, byte)};

    const auto shipped{lexer.is_split_point_ignoring(symbol)};

    const auto exact{decide_exactly(lexer, ignored, byte)};

    const auto exact_safe{exact.outcome == Exact_verdict::Outcome::safe};

    return {.published = published, .shipped = shipped, .exact_safe = exact_safe, .oracle_safe = exercised && safe};
}

/**
 * @brief Judges the report's witness that the published condition is conservative, the example of what the shipped rule
 *        gains.
 *
 * Two ignored tokens, \c ab* and \c b+, and one kept token \c c. Splitting at a 'b' inside an \c ab* token is always
 * safe modulo the ignored kinds: the left piece is a shorter \c ab* and the right is a \c b+, and both are discarded.
 * The published condition refuses because the two scans do not reconverge on one state: advancing on 'b' from inside
 * \c ab* stays in a state accepting \c ab*, while advancing on 'b' from the initial state enters one accepting \c b+,
 * and those accept different tokens so minimization keeps them apart. Once the two ignored kinds share a colour the two
 * states have the same future, which is what the shipped rule asks, so it admits 'b'.
 * @return The four judgements at 'b'.
 */
Named gain_witness()
{
    munch::core::Builder builder{};

    builder.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), std::size_t{0}, 1);

    builder.add_token(plus(any_of(Set{'b'})), std::size_t{1}, 1);

    builder.add_token(text("c"), std::size_t{2}, 1);

    return judge(builder, {0, 1}, "abc", 'b');
}

/**
 * @brief Judges a token set that coalescing the discarded kinds admits: ignored \c a and \c ab of one kind and \c b of
 *        another, beside a kept \c k.
 *
 * After 'a', consuming 'b' reaches the state accepting \c ab, from the initial state it reaches the one accepting \c b,
 * and the two accept different kinds, so the published condition refuses; with both kinds one colour the two states are
 * equivalent, and every other clause holds.
 * @return The four judgements at 'b'.
 */
Named coalescing_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(text("a"), text("ab")), std::size_t{0}, 1);

    builder.add_token(text("b"), std::size_t{1}, 1);

    builder.add_token(text("k"), std::size_t{2}, 1);

    return judge(builder, {0, 1}, "abk", 'b');
}

/**
 * @brief Judges a token set that testing the discarded future past the cut, rather than at the state before it, admits:
 *        ignored \c a, \c b, \c c and \c ab, beside a kept \c ac.
 *
 * The state after 'a' can still reach the kept \c ac, so the published condition refuses 'b'; past the 'b' only the
 * discarded \c ab remains, which is all the severed token can become, and the shipped rule asks only that.
 * @return The four judgements at 'b'.
 */
Named edge_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(choice(text("a"), text("b")), choice(text("c"), text("ab"))), std::size_t{0}, 1);

    builder.add_token(text("ac"), std::size_t{1}, 1);

    return judge(builder, {0}, "abc", 'b');
}

/**
 * @brief Judges a token set both rules refuse although every cut before 'b' is safe: ignored \c a, \c b, \c c and
 *        \c abc, beside kept \c k and \c kk.
 *
 * Every input tokenizes completely, kept output comes only from runs of 'k', and no cut before 'b' touches such a run.
 * Yet after 'a', consuming 'b' reaches the non-accepting prefix of \c abc, while from the initial state it reaches the
 * state accepting \c b, and the two differ already on the empty continuation. Only the exact decision admits it.
 * @return The four judgements at 'b'.
 */
Named incompleteness_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(choice(text("a"), text("b")), choice(text("c"), text("abc"))), std::size_t{0}, 1);

    builder.add_token(choice(text("k"), text("kk")), std::size_t{1}, 1);

    return judge(builder, {0}, "abck", 'b');
}

/**
 * @brief Judges a token set showing by witness that testing each severed token on its own is not enough: ignored \c a,
 *        \c ab, \c b and \c bc beside a kept \c c.
 *
 * Every token containing a 'b' can be cut there on its own, yet \c abc scans as \c ab then \c c while the chunks \c a
 * and \c bc are both discarded, so the kept \c c vanishes with both chunks complete.
 * @return The four judgements at 'b'.
 */
Named swallowed_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(choice(text("a"), text("ab")), choice(text("b"), text("bc"))), std::size_t{0}, 1);

    builder.add_token(text("c"), std::size_t{1}, 1);

    return judge(builder, {0}, "abc", 'b');
}

/**
 * @brief Judges a token set whose observable is the kept kinds and lengths, not where they came from: ignored \c a,
 *        \c ab, \c b and \c d beside kept \c bc and \c cd of one kind.
 *
 * Every cut before 'b' is safe, although on \c abcd the kept token moves from \c cd at offset two to \c bc at offset
 * one. Both local rules refuse it on their clause 2: restarted at 'b' the scan can still reach the kept \c bc, while
 * past \c ab the interrupted one can reach nothing, so the two states are neither equal nor equivalent. Only the exact
 * decision admits it.
 * @return The four judgements at 'b'.
 */
Named moved_origin_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(choice(text("a"), text("ab")), choice(text("b"), text("d"))), std::size_t{0}, 1);

    builder.add_token(choice(text("bc"), text("cd")), std::size_t{1}, 1);

    return judge(builder, {0}, "abcd", 'b');
}

/**
 * @brief Judges the moved-origin token set with \c bc and \c cd kept as two kinds, 0 and 2^32, beside ignored \c a,
 *        \c ab, \c b and \c d of kind 1.
 *
 * On \c abcd the serial scan keeps \c cd while the chunks \c a and \c bcd keep \c bc, so the kept kind changes and the
 * cut before 'b' is unsafe. The two kinds agree in their low 32 bits, so this holds the output letters to the kinds'
 * full width.
 * @return The four judgements at 'b'.
 */
Named wide_kinds_witness()
{
    munch::core::Builder builder{};

    builder.add_token(choice(choice(text("a"), text("ab")), choice(text("b"), text("d"))), std::size_t{1}, 1);

    builder.add_token(text("bc"), std::size_t{0}, 1);

    builder.add_token(text("cd"), std::size_t{1} << 32U, 1);

    return judge(builder, {1}, "abcd", 'b');
}

} // namespace

/**
 * @brief Asserts the validation figures: the sweep at two bounds, the reclassified pairs, the shipped predicate and the
 *        exact decision on the same sweep and the named token sets, and prints the verdict.
 * @return EXIT_SUCCESS when every figure agrees with the report, EXIT_FAILURE otherwise.
 */
int main()
{
    Figure_check check{};

    std::cout << std::format(
            "{} random token sets, exhaustive to length {} over a three-symbol alphabet\n", sweep_rounds, longer_bound);

    const auto eight{sweep(sweep_rounds, longer_bound)};

    check("token sets swept", eight.token_sets, sweep_rounds);

    check("symbol and token set pairs exercised", eight.exercised, std::size_t{1125});

    check("symbols the published condition admits", eight.admitted, std::size_t{265});

    check("admitted yet unsafe to split at", eight.unsound, std::size_t{0});

    check("admitted by the exact certificate yet refused by the relaxed one", eight.lost, std::size_t{0});

    check("safe to split at through length eight yet refused", eight.conservative, std::size_t{97});

    // Raising the bound reclassifies two pairs, which shows the count is bound-sensitive. It is the exact decision
    // below, not this delta, that settles the pairs the oracle leaves open.
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

    std::cout << "\nthe rule the library ships, on the same sweep\n";

    check("symbols the shipped rule admits", eight.shipped_admitted, std::size_t{306});

    check("admitted by the shipped rule yet unsafe to split at", eight.shipped_unsound, std::size_t{0});

    check("admitted by the published condition yet refused by the shipped rule", eight.shipped_lost, std::size_t{0});

    std::cout << "\nthe exact decision, on every exercised pair\n";

    check("pairs the decision left undecided", eight.exact_undecided, std::size_t{0});

    check("pairs where the decision and the oracle disagree", eight.exact_disagreeing, std::size_t{0});

    check("pairs safe to split at, exactly", eight.exact_safe, std::size_t{360});

    check("of the 97 refused pairs the oracle left open, safe", eight.published_refused_safe, std::size_t{95});

    check("of the 97, refuted by a witness longer than eight", eight.conservative_refuted, std::size_t{2});

    check("safe pairs the shipped rule refuses", eight.shipped_refused_safe, std::size_t{54});

    // The report quotes the witnesses' length, so it is asserted: the longest is nine bytes, one past the oracle.
    check("bytes in the longest witness", eight.longest_witness, std::size_t{9});

    std::cout << std::format(
            "  info longest witness: {} cut at {}\n", eight.longest_witness_text, eight.longest_witness_cut);

    std::cout << "\nthe named token sets, each judged by both rules, the exact decision and the oracle to length six\n";

    check("ab*, b+ ignored and c kept, at b", gain_witness(),
          Named{.published = false, .shipped = true, .exact_safe = true, .oracle_safe = true});

    check("a, ab and b ignored in two kinds and k kept, at b", coalescing_witness(),
          Named{.published = false, .shipped = true, .exact_safe = true, .oracle_safe = true});

    check("a, b, c, ab ignored and ac kept, at b", edge_witness(),
          Named{.published = false, .shipped = true, .exact_safe = true, .oracle_safe = true});

    check("a, b, c, abc ignored and k, kk kept, at b", incompleteness_witness(),
          Named{.published = false, .shipped = false, .exact_safe = true, .oracle_safe = true});

    check("a, ab, b, bc ignored and c kept, at b", swallowed_witness(),
          Named{.published = false, .shipped = false, .exact_safe = false, .oracle_safe = false});

    check("a, ab, b, d ignored and bc, cd kept in one kind, at b", moved_origin_witness(),
          Named{.published = false, .shipped = false, .exact_safe = true, .oracle_safe = true});

    check("a, ab, b, d ignored and bc, cd kept as kinds 0 and 2^32, at b", wide_kinds_witness(),
          Named{.published = false, .shipped = false, .exact_safe = false, .oracle_safe = false});

    const auto failures{check.failures()};

    std::cout
            << (failures == 0 ? "\nthe validation figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

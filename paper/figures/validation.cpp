/*
 * Asserts the validation figures of paper/split-points/split-points.tex, in the section deciding the relaxed condition.
 *
 * Three kinds of claim are made there and they deserve different treatment.
 *
 * The first two are properties: no symbol the relaxed condition admits ever fails to split, and no symbol the exact
 * condition admits is ever lost. Those are checked here over random token sets against an EXHAUSTIVE oracle, every
 * string up to a bounded length on a three-symbol alphabet, so no symbol can be judged safe merely because a
 * sampled corpus never exercised it. The same two properties are checked of the stronger rule the library ships.
 *
 * The third is how conservative each rule is. A percentage from a random sweep says as much about the generator as
 * about the condition, so the sweep counts are asserted for reproducibility while the report leans on named token
 * sets instead, written out below and checkable by hand: one the published condition refuses and the shipped rule
 * admits, two more where the shipped rule gains for each of its two changes, two both rules refuse although splitting
 * is safe, and one that is unsafe because the restarted scan swallows a later kept token. What settles safety for the
 * pairs the oracle cannot settle is the exact decision of modulo.hpp, which decides every exercised pair and replays
 * every negative answer through the scanner; its answers are checked against the oracle wherever the oracle has one.
 */

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "modulo.hpp"
#include "munch/core/builder.hpp"
#include "munch/core/lexer.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
{
using namespace munch::regex;

using Kinds = std::set<std::size_t>;

constexpr std::string_view alphabet{"abc"};

using Stream = std::vector<std::pair<std::size_t, std::size_t>>;

class Random
{
public:
    explicit Random(const unsigned seed) : seed_{seed} {}

    unsigned next(const unsigned bound)
    {
        seed_ = seed_ * 1664525U + 1013904223U;

        return (seed_ >> 8U) % bound;
    }

private:
    unsigned seed_;
};

// Set exposes no emptiness query, so the count is tracked here: an empty class would register a token matching
// nothing, which is not what this is sweeping.
Set random_set(Random& random)
{
    Set set;

    for (auto chosen{0U}; chosen == 0;)
    {
        for (const auto symbol : alphabet)
        {
            if (random.next(2) != 0)
            {
                set = set + symbol;

                ++chosen;
            }
        }
    }

    return set;
}

Regex random_regex(Random& random, const unsigned depth)
{
    if (depth == 0 || random.next(3) == 0)
    {
        if (random.next(2) == 0)
        {
            return any_of(random_set(random));
        }

        std::string literal;

        for (auto count{1U + random.next(2)}; count > 0; --count)
        {
            literal += alphabet[random.next(static_cast<unsigned>(alphabet.size()))];
        }

        return text(literal);
    }

    // The two-operand cases bind their operands to locals first. Argument evaluation order is unspecified in C++, so
    // concat(random_regex(...), random_regex(...)) draws from the generator in whichever order the compiler chooses,
    // and GCC and Clang choose differently: the same seed then builds different token sets and every count below
    // becomes compiler-dependent. That is not hypothetical, it is what this program reported before the fix.
    switch (random.next(5))
    {
    case 0:
    {
        const auto first{random_regex(random, depth - 1)};

        const auto second{random_regex(random, depth - 1)};

        return concat(first, second);
    }
    case 1:
    {
        const auto first{random_regex(random, depth - 1)};

        const auto second{random_regex(random, depth - 1)};

        return choice(first, second);
    }
    case 2:
        return plus(random_regex(random, depth - 1));
    case 3:
        return optional(random_regex(random, depth - 1));
    default:
        return kleene(random_regex(random, depth - 1));
    }
}

// Every string of length one to max_length over the letters. This is the point: no sampling, so no vacuous verdicts.
std::vector<std::string> every_string(const std::string_view letters, const std::size_t max_length)
{
    std::vector<std::string> corpus, frontier{""};

    for (std::size_t length{0}; length < max_length; ++length)
    {
        std::vector<std::string> next;

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

Stream scan(const munch::core::Lexer& lexer, const std::string& text, std::size_t& consumed)
{
    Stream stream;

    consumed = lexer.tokenize_all<std::size_t>(
            text, [&stream](const std::size_t kind, const std::size_t length) { stream.emplace_back(kind, length); });

    return stream;
}

Stream without(const Stream& stream, const Kinds& ignored)
{
    Stream kept;

    for (const auto& token : stream)
    {
        if (!ignored.contains(token.first))
        {
            kept.push_back(token);
        }
    }

    return kept;
}

struct Verdict
{
    bool exercised{false};
    bool safe{true};
};

std::vector<Verdict> oracle(
        const munch::core::Lexer& lexer, const Kinds& ignored, const std::vector<std::string>& corpus)
{
    std::vector<Verdict> verdicts(256);

    for (const auto& text : corpus)
    {
        std::size_t consumed{0};

        const auto serial{scan(lexer, text, consumed)};

        if (consumed != text.size())
        {
            continue;
        }

        // The cut before the first byte is a boundary of every scan and is exempted from every check.
        for (std::size_t at{1}; at < text.size(); ++at)
        {
            auto& verdict{verdicts[static_cast<unsigned char>(text[at])]};

            verdict.exercised = true;

            std::size_t left_used{0}, right_used{0};

            const auto left{scan(lexer, text.substr(0, at), left_used)};

            const auto right{scan(lexer, text.substr(at), right_used)};

            auto agrees{left_used == at && right_used == text.size() - at};

            if (agrees)
            {
                Stream spliced{left};

                spliced.insert(spliced.end(), right.begin(), right.end());

                agrees = without(spliced, ignored) == without(serial, ignored);
            }

            verdict.safe = verdict.safe && agrees;
        }
    }

    return verdicts;
}

struct Sweep
{
    std::size_t token_sets{0};
    std::size_t exercised{0};

    // The published condition against the bounded oracle.
    std::size_t admitted{0};
    std::size_t unsound{0};
    std::size_t lost{0};
    std::size_t conservative{0};

    // The shipped rule against the same oracle and against the published condition.
    std::size_t shipped_admitted{0};
    std::size_t shipped_unsound{0};
    std::size_t shipped_lost{0};

    // The exact decision: what it settles that the oracle cannot, and where it must agree with the oracle.
    std::size_t exact_safe{0};
    std::size_t exact_undecided{0};
    std::size_t exact_disagreeing{0};
    std::size_t published_refused_safe{0};
    std::size_t shipped_refused_safe{0};
    std::size_t conservative_refuted{0};
    std::size_t longest_witness{0};
    std::string longest_witness_text;
    std::size_t longest_witness_cut{0};

    // The (round, symbol) identity of every conservative pair, so a bound change must name what it reclassified.
    std::set<std::pair<std::size_t, char>> conservative_pairs;
};

Sweep sweep(const std::size_t rounds, const std::size_t max_length)
{
    const auto corpus{every_string(alphabet, max_length)};

    Random random{20260801U};

    Sweep totals;

    for (std::size_t round{0}; round < rounds; ++round)
    {
        munch::core::Builder builder;

        const auto kinds{2U + random.next(3)};

        Kinds ignored;

        for (std::size_t kind{0}; kind < kinds; ++kind)
        {
            // Two draws in one argument list would be unsequenced, exactly as above.
            const auto pattern{random_regex(random, 3)};

            const auto priority{1 + random.next(2)};

            builder.add_token(pattern, kind, priority);

            if (random.next(2) == 0)
            {
                ignored.insert(kind);
            }
        }

        builder.set_state_limit(400);

        try
        {
            builder.set_ignored_tokens(std::vector<std::size_t>{ignored.begin(), ignored.end()});

            const auto lexer{builder.build()};

            const auto verdicts{oracle(lexer, ignored, corpus)};

            ++totals.token_sets;

            for (const auto symbol : alphabet)
            {
                const auto& verdict{verdicts[static_cast<unsigned char>(symbol)]};

                if (!verdict.exercised)
                {
                    continue;
                }

                ++totals.exercised;

                const auto claim{figures::published_condition(lexer, ignored, static_cast<unsigned char>(symbol))};

                const auto shipped{lexer.is_split_point_ignoring(symbol)};

                totals.admitted += claim ? 1 : 0;

                totals.unsound += claim && !verdict.safe ? 1 : 0;

                totals.conservative += !claim && verdict.safe ? 1 : 0;

                if (!claim && verdict.safe)
                {
                    totals.conservative_pairs.emplace(round, static_cast<char>(symbol));
                }

                totals.lost += lexer.is_split_point(symbol) && !claim ? 1 : 0;

                totals.shipped_admitted += shipped ? 1 : 0;

                totals.shipped_unsound += shipped && !verdict.safe ? 1 : 0;

                totals.shipped_lost += claim && !shipped ? 1 : 0;

                const auto exact{figures::decide_exactly(lexer, ignored, static_cast<unsigned char>(symbol))};

                if (exact.outcome == figures::Exact_verdict::Outcome::budget)
                {
                    ++totals.exact_undecided;

                    continue;
                }

                const auto safe{exact.outcome == figures::Exact_verdict::Outcome::safe};

                // The oracle refutes with a string of at most max_length bytes, so the decision may not answer safe
                // where the oracle refutes, and a replayed witness is a refutation the oracle can have missed only
                // for its length.
                totals.exact_disagreeing +=
                        (safe && !verdict.safe) || (!safe && verdict.safe && exact.text.size() <= max_length) ? 1 : 0;

                totals.exact_safe += safe ? 1 : 0;

                totals.published_refused_safe += safe && !claim ? 1 : 0;

                totals.shipped_refused_safe += safe && !shipped ? 1 : 0;

                totals.conservative_refuted += !safe && !claim && verdict.safe ? 1 : 0;

                if (!safe && exact.text.size() > totals.longest_witness)
                {
                    totals.longest_witness = exact.text.size();
                    totals.longest_witness_text = exact.text;
                    totals.longest_witness_cut = exact.cut;
                }
            }
        }
        catch (const std::exception&)
        {
            continue; // state limit or an empty language; neither is what this sweeps
        }
    }

    return totals;
}

// What the three judges say of one symbol of one named token set, so that each named example asserts all of them.
struct Named
{
    bool published{false};
    bool shipped{false};
    bool exact_safe{false};
    bool oracle_safe{false};

    auto operator<=>(const Named&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Named& named)
{
    return out << "published " << named.published << ", shipped " << named.shipped << ", exact " << named.exact_safe
               << ", oracle " << named.oracle_safe;
}

Named judge(munch::core::Builder& builder, const Kinds& ignored, const std::string_view letters, const char symbol)
{
    builder.set_ignored_tokens(std::vector<std::size_t>{ignored.begin(), ignored.end()});

    const auto lexer{builder.build()};

    const auto verdicts{oracle(lexer, ignored, every_string(letters, 6))};

    const auto& verdict{verdicts[static_cast<unsigned char>(symbol)]};

    const auto exact{figures::decide_exactly(lexer, ignored, static_cast<unsigned char>(symbol))};

    return {.published = figures::published_condition(lexer, ignored, static_cast<unsigned char>(symbol)),
            .shipped = lexer.is_split_point_ignoring(symbol),
            .exact_safe = exact.outcome == figures::Exact_verdict::Outcome::safe,
            .oracle_safe = verdict.exercised && verdict.safe};
}

/**
 * @brief The report's witness that the published condition is conservative, now the example of what the shipped
 *        rule gains.
 *
 * Two ignored tokens, \c ab* and \c b+, and one kept token \c c. Splitting at a 'b' inside an \c ab* token is
 * always safe modulo the ignored kinds: the left piece is a shorter \c ab* and the right is a \c b+, and both are
 * discarded. The published condition refuses because the two scans do not reconverge on one state: advancing on 'b'
 * from inside \c ab* stays in a state accepting \c ab*, while advancing on 'b' from the initial state enters one
 * accepting \c b+, and those accept different tokens so minimization keeps them apart. Once the two ignored kinds
 * share a colour the two states have the same future, which is what the shipped rule asks, so it admits 'b'.
 */
Named gain_witness()
{
    munch::core::Builder builder;

    builder.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), std::size_t{0}, 1);
    builder.add_token(plus(any_of(Set{'b'})), std::size_t{1}, 1);
    builder.add_token(text("c"), std::size_t{2}, 1);

    return judge(builder, {0, 1}, "abc", 'b');
}

/**
 * @brief Coalescing the discarded kinds is what admits this one: ignored \c a and \c ab of one kind and \c b of
 *        another, beside a kept \c k. After 'a', consuming 'b' reaches the state accepting \c ab, from the initial
 *        state it reaches the one accepting \c b, and the two accept different kinds, so the published condition
 *        refuses; with both kinds one colour the two states are equivalent, and every other clause holds.
 */
Named coalescing_witness()
{
    munch::core::Builder builder;

    builder.add_token(choice(text("a"), text("ab")), std::size_t{0}, 1);
    builder.add_token(text("b"), std::size_t{1}, 1);
    builder.add_token(text("k"), std::size_t{2}, 1);

    return judge(builder, {0, 1}, "abk", 'b');
}

/**
 * @brief Testing the discarded future past the cut rather than at the state before it is what admits this one:
 *        ignored \c a, \c b, \c c and \c ab, beside a kept \c ac. The state after 'a' can still reach the kept
 *        \c ac, so the published condition refuses 'b'; past the 'b' only the discarded \c ab remains, which is all
 *        the severed token can become, and the shipped rule asks only that.
 */
Named edge_witness()
{
    munch::core::Builder builder;

    builder.add_token(choice(choice(text("a"), text("b")), choice(text("c"), text("ab"))), std::size_t{0}, 1);
    builder.add_token(text("ac"), std::size_t{1}, 1);

    return judge(builder, {0}, "abc", 'b');
}

/**
 * @brief Both rules refuse this one although every cut before 'b' is safe: ignored \c a, \c b, \c c and \c abc,
 *        beside kept \c k and \c kk. Every input tokenizes completely, kept output comes only from runs of 'k', and no
 *        cut before 'b' touches such a run. Yet after 'a', consuming 'b' reaches the non-accepting prefix of
 *        \c abc, while from the initial state it reaches the state accepting \c b, and the two differ already on the
 *        empty continuation. Only the exact decision admits it.
 */
Named incompleteness_witness()
{
    munch::core::Builder builder;

    builder.add_token(choice(choice(text("a"), text("b")), choice(text("c"), text("abc"))), std::size_t{0}, 1);
    builder.add_token(choice(text("k"), text("kk")), std::size_t{1}, 1);

    return judge(builder, {0}, "abck", 'b');
}

/**
 * @brief Testing each severed token on its own is not enough, which the exact decision shows by witness: ignored
 *        \c a, \c ab, \c b and \c bc beside a kept \c c. Every token containing a 'b' can be cut there on its own,
 *        yet \c abc scans as \c ab then \c c while the chunks \c a and \c bc are both discarded, so the kept
 *        \c c vanishes with both chunks complete.
 */
Named swallowed_witness()
{
    munch::core::Builder builder;

    builder.add_token(choice(choice(text("a"), text("ab")), choice(text("b"), text("bc"))), std::size_t{0}, 1);
    builder.add_token(text("c"), std::size_t{1}, 1);

    return judge(builder, {0}, "abc", 'b');
}

/**
 * @brief The observable is the kept kinds and lengths, not where they came from: ignored \c a, \c ab, \c b and
 *        \c d beside kept \c bc and \c cd of one kind. Every cut before 'b' is safe, although on \c abcd the kept
 *        token moves from \c cd at offset two to \c bc at offset one. Both local rules refuse it on their clause 2:
 *        restarted at 'b' the scan can still reach the kept \c bc, while past \c ab the interrupted one can reach
 *        nothing, so the two states are neither equal nor equivalent. Only the exact decision admits it.
 */
Named moved_origin_witness()
{
    munch::core::Builder builder;

    builder.add_token(choice(choice(text("a"), text("ab")), choice(text("b"), text("d"))), std::size_t{0}, 1);
    builder.add_token(choice(text("bc"), text("cd")), std::size_t{1}, 1);

    return judge(builder, {0}, "abcd", 'b');
}

} // namespace

int main()
{
    int failures{0};

    const auto check{[&failures](const std::string& what, const auto actual, const auto expected) {
        const auto agrees{actual == expected};

        std::cout << (agrees ? "  ok   " : "  FAIL ") << what << ": " << actual << '\n';

        if (!agrees)
        {
            std::cout << "         report says " << expected << '\n';

            failures += 1;
        }
    }};

    std::cout << "400 random token sets, exhaustive to length 8 over a three-symbol alphabet\n";

    const auto eight{sweep(400, 8)};

    check("token sets swept", eight.token_sets, std::size_t{400});

    check("symbol and token set pairs exercised", eight.exercised, std::size_t{1125});

    check("symbols the published condition admits", eight.admitted, std::size_t{265});

    check("admitted yet unsafe to split at", eight.unsound, std::size_t{0});

    check("admitted by the exact certificate yet refused by the relaxed one", eight.lost, std::size_t{0});

    check("safe to split at through length eight yet refused", eight.conservative, std::size_t{97});

    // Raising the bound reclassifies two pairs, which shows the count is bound-sensitive. It is the exact decision
    // below, not this delta, that settles the pairs the oracle leaves open.
    const auto six{sweep(400, 6)};

    check("safe yet refused at the shorter bound", six.conservative, std::size_t{99});

    check("pairs reclassified by the longer bound", six.conservative - eight.conservative, std::size_t{2});

    // A counterexample within length six is one within length eight, so the longer bound can only remove pairs;
    // asserting the identities pins that no offsetting additions hide inside the aggregate difference of two.
    const auto contained{std::ranges::includes(six.conservative_pairs, eight.conservative_pairs)};

    check("every length-eight conservative pair is conservative at length six too", contained ? 1U : 0U, 1U);

    std::set<std::pair<std::size_t, char>> reclassified;

    std::ranges::set_difference(
            six.conservative_pairs, eight.conservative_pairs, std::inserter(reclassified, reclassified.begin()));

    const std::set<std::pair<std::size_t, char>> expected{{4, 'a'}, {319, 'c'}};

    check("the reclassified pairs are the two named ones", reclassified == expected ? 1U : 0U, 1U);

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

    std::cout << "  info longest witness: " << eight.longest_witness_text << " cut at " << eight.longest_witness_cut
              << '\n';

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

    std::cout
            << (failures == 0 ? "\nthe validation figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? 0 : 1;
}

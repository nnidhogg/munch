/*
 * Asserts the validation figures of paper/split-points.tex, in the section deciding the relaxed condition.
 *
 * Two kinds of claim are made there and they deserve different treatment.
 *
 * The first two are properties: no symbol the relaxed condition admits ever fails to split, and no symbol the exact
 * condition admits is ever lost. Those are checked here over random token sets against an EXHAUSTIVE oracle, every
 * string up to a bounded length on a three-symbol alphabet, so no symbol can be judged safe merely because a sampled
 * corpus never exercised it.
 *
 * The third is that the condition is conservative. A percentage from a random sweep says as much about the generator
 * as about the condition, so the sweep counts are asserted for reproducibility while the report leans on a named
 * witness instead: a specific small token set, written out below, where splitting is safe modulo the ignored set and
 * the condition still refuses. That is checkable by hand and does not move when the generator changes.
 */

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

// Every string of length one to max_length. This is the point: no sampling, so no vacuous verdicts.
std::vector<std::string> every_string(const std::size_t max_length)
{
    std::vector<std::string> corpus, frontier{""};

    for (std::size_t length{0}; length < max_length; ++length)
    {
        std::vector<std::string> next;

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

        // exempted the cut before the final byte from every check.
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
    std::size_t admitted{0};
    std::size_t unsound{0};
    std::size_t lost{0};
    std::size_t conservative{0};

    // The (round, symbol) identity of every conservative pair, so a bound change must name what it reclassified.
    std::set<std::pair<std::size_t, char>> conservative_pairs;
};

Sweep sweep(const std::size_t rounds, const std::size_t max_length)
{
    const auto corpus{every_string(max_length)};

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

                const auto claim{lexer.is_split_point_ignoring(symbol)};

                totals.admitted += claim ? 1 : 0;

                totals.unsound += claim && !verdict.safe ? 1 : 0;

                totals.conservative += !claim && verdict.safe ? 1 : 0;

                if (!claim && verdict.safe)
                {
                    totals.conservative_pairs.emplace(round, static_cast<char>(symbol));
                }

                totals.lost += lexer.is_split_point(symbol) && !claim ? 1 : 0;
            }
        }
        catch (const std::exception&)
        {
            continue; // state limit or an empty language; neither is what this sweeps
        }
    }

    return totals;
}

/**
 * @brief A token set where splitting is safe modulo the ignored set and the condition still refuses.
 *
 * Two ignored tokens, \c ab* and \c b+, and one kept token \c c. Splitting at a 'b' inside an \c ab* token is
 * always safe modulo the ignored kinds: the left piece is a shorter \c ab* and the right is a \c b+, and both are
 * discarded. The condition refuses because the two scans do not reconverge. Advancing on 'b' from inside \c ab*
 * stays in a state accepting \c ab*, while advancing on 'b' from the initial state enters one accepting \c b+, and
 * those accept different tokens so minimization keeps them apart. Insisting on immediate reconvergence is what makes
 * the test local, and this is what it costs.
 */
bool conservatism_witness()
{
    munch::core::Builder builder;

    builder.add_token(concat(text("a"), kleene(any_of(Set{'b'}))), std::size_t{0}, 1);
    builder.add_token(plus(any_of(Set{'b'})), std::size_t{1}, 1);
    builder.add_token(text("c"), std::size_t{2}, 1);
    builder.set_ignored_tokens(std::vector<std::size_t>{0, 1});

    const auto lexer{builder.build()};

    const Kinds ignored{0, 1};

    const auto verdicts{oracle(lexer, ignored, every_string(6))};

    const auto& verdict{verdicts[static_cast<unsigned char>('b')]};

    return verdict.exercised && verdict.safe && !lexer.is_split_point_ignoring('b');
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

    check("symbols the condition admits", eight.admitted, std::size_t{265});

    check("admitted yet unsafe to split at", eight.unsound, std::size_t{0});

    check("admitted by the exact certificate yet refused by the relaxed one", eight.lost, std::size_t{0});

    check("safe to split at yet refused", eight.conservative, std::size_t{97});

    // Raising the bound reclassifies two pairs, which shows the count is bound-sensitive. It is the named witness
    // below, not this delta, that establishes the condition is genuinely conservative.
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

    check("a named token set where splitting is safe and the condition refuses", conservatism_witness(), true);

    std::cout
            << (failures == 0 ? "\nthe validation figures reproduce the report\n" :
                                "\nfigures disagreeing with the report\n");

    return failures == 0 ? 0 : 1;
}

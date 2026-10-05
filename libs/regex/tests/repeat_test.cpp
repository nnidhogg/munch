#include "munch/regex/repeat.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/nfa/simulator.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::regex;

/**
 * @brief A match the NFA simulator reports.
 */
using Match = Simulator::Match;

/**
 * @brief The match of a scan that matched nothing.
 */
const Match no_match{.token = std::nullopt, .length = 0};

TEST(Repeat_test, A_kleene_star_matches_every_run_including_the_empty_one)
{
    const auto a{text('a')};

    const auto regex{kleene(a)};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "baa"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), (Match{.token = token, .length = 0}));
}

TEST(Repeat_test, A_plus_matches_one_or_more)
{
    const auto a{text('a')};

    const auto regex{plus(a)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), (Match{.token = token, .length = 1}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "baa"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "baaa"), no_match);
}

TEST(Repeat_test, An_optional_matches_zero_or_one)
{
    const auto a{text('a')};

    const auto regex{optional(a)};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ab"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 0}));
}

TEST(Repeat_test, An_exact_repetition_matches_only_its_count)
{
    const auto a{text('a')};

    const auto regex{exact(a, 3)};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "aa"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "ba"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "baaa"), no_match);
}

TEST(Repeat_test, An_at_least_repetition_matches_its_count_and_more)
{
    const auto a{text('a')};

    const auto regex{at_least(a, 3)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), (Match{.token = token, .length = 5}));
    EXPECT_EQ(Simulator::run(nfa, "aaaaaa"), (Match{.token = token, .length = 6}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "aa"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "aab"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "baaa"), no_match);
}

TEST(Repeat_test, At_least_zero_repetitions_is_the_kleene_star)
{
    const auto a{text('a')};

    const auto regex{at_least(a, 0)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "a"), (Match{.token = token, .length = 1}));
    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));

    EXPECT_EQ(Simulator::run(nfa, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(nfa, "ba"), (Match{.token = token, .length = 0}));
}

TEST(Repeat_test, A_range_ending_before_it_starts_throws)
{
    EXPECT_THROW(std::ignore = range(text('a'), 3, 2), std::invalid_argument);
}

TEST(Repeat_test, A_range_repetition_matches_every_count_within_it)
{
    const auto a{text('a')};

    const auto regex{range(a, 2, 4)};

    const Token token{6, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(nfa, "aa"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), (Match{.token = token, .length = 4}));

    EXPECT_EQ(Simulator::run(nfa, ""), no_match);
    EXPECT_EQ(Simulator::run(nfa, "a"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "b"), no_match);
    EXPECT_EQ(Simulator::run(nfa, "baaa"), no_match);
}

TEST(Repeat_test, A_range_over_a_body_that_ends_in_a_repetition_admits_no_suffix_of_that_body)
{
    // The state a range reaches after each optional copy accepts on its own, so `(ab+){0,1}` admits no suffix of its
    // body: "b" is refused, which only a body whose last atom repeats shows.
    const auto body{concat(text('a'), plus(text('b')))};

    const Token token{7, 1};

    const auto once{to_nfa(range(body, 0, 1)).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(once, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(once, "bb"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(once, ""), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(once, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(once, "abb"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(once, "abab"), (Match{.token = token, .length = 2}));

    const auto twice{to_nfa(range(body, 0, 2)).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(twice, "b"), (Match{.token = token, .length = 0}));
    EXPECT_EQ(Simulator::run(twice, "abab"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(twice, "ababab"), (Match{.token = token, .length = 4}));

    // The same body with a required copy before the optional one, where the suffix could only be admitted after the
    // first copy had matched.
    const auto bounded{to_nfa(range(body, 1, 2)).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(bounded, "b"), no_match);
    EXPECT_EQ(Simulator::run(bounded, "ab"), (Match{.token = token, .length = 2}));
    EXPECT_EQ(Simulator::run(bounded, "abb"), (Match{.token = token, .length = 3}));
    EXPECT_EQ(Simulator::run(bounded, "abab"), (Match{.token = token, .length = 4}));
}

TEST(Repeat_test, Every_repetition_agrees_with_its_expansion_on_every_body_and_input_enumerated)
{
    // A range between two counts is the choice of the concatenations of each count; an exact repetition is the
    // concatenation of that many copies; at least n is n copies then any number; a plus is a copy then any number; an
    // optional is nothing or a copy; a star is nothing or a plus. Each is compared with what it must equal under
    // longest match, over bodies whose last atom repeats, is optional or is nullable, and over every input over three
    // bytes up to a length.
    const auto a{text('a')};

    const auto b{text('b')};

    const auto c{text('c')};

    const std::vector<Regex> bodies{
            a,
            concat(a, b),
            plus(a),
            concat(a, plus(b)),
            concat(a, optional(b)),
            choice(a, b),
            kleene(concat(a, b)),
            concat(kleene(a), b),
            choice(a, concat(a, b)),
            concat(optional(b), a),
            optional(a),
            kleene(a),
            range(concat(a, optional(b)), 0, 2),
            kleene(concat(plus(a), b)),
            choice(concat(a, plus(b)), c)};

    std::vector<std::string> inputs{""};

    std::vector<std::string> layer{""};

    constexpr std::size_t longest_input{5};

    for (std::size_t length{1}; length <= longest_input; ++length)
    {
        std::vector<std::string> next{};

        for (const auto& shorter : layer)
        {
            for (const char byte : {'a', 'b', 'c'})
            {
                next.push_back(shorter + byte);
            }
        }

        inputs.insert(inputs.end(), next.begin(), next.end());

        layer = std::move(next);
    }

    const auto power{[](const Regex& body, const std::size_t count) {
        if (count == 0)
        {
            return exact(body, 0);
        }

        auto out{body};

        for (std::size_t copy{1}; copy < count; ++copy)
        {
            out = concat(out, body);
        }

        return out;
    }};

    const auto any_count{[&power](const Regex& body, const std::size_t fewest, const std::size_t most) {
        auto out{power(body, fewest)};

        for (std::size_t count{fewest + 1}; count <= most; ++count)
        {
            out = choice(out, power(body, count));
        }

        return out;
    }};

    const Token token{1, 1};

    const auto agree{[&](const Regex& left, const Regex& right, const std::string& name) {
        const auto one{to_nfa(left).set_accept_token(token).build()};

        const auto other{to_nfa(right).set_accept_token(token).build()};

        for (const auto& input : inputs)
        {
            EXPECT_EQ(Simulator::run(one, input), Simulator::run(other, input)) << name << R"( on ")" << input << '"';
        }
    }};

    for (std::size_t which{0}; which < bodies.size(); ++which)
    {
        const auto& body{bodies[which]};

        const auto name{std::format("body {}", which)};

        constexpr std::size_t most_required{2};

        constexpr std::size_t most_copies{3};

        for (std::size_t fewest{0}; fewest <= most_required; ++fewest)
        {
            for (std::size_t most{fewest}; most <= most_copies; ++most)
            {
                agree(range(body, fewest, most), any_count(body, fewest, most), std::format("{} range", name));
            }
        }

        for (std::size_t count{0}; count <= most_copies; ++count)
        {
            agree(exact(body, count), power(body, count), std::format("{} exact", name));

            agree(at_least(body, count), concat(power(body, count), kleene(body)), std::format("{} at least", name));
        }

        agree(plus(body), concat(body, kleene(body)), std::format("{} plus", name));

        agree(optional(body), choice(exact(body, 0), body), std::format("{} optional", name));

        agree(kleene(body), choice(exact(body, 0), plus(body)), std::format("{} kleene", name));

        agree(range(range(body, 0, 1), 1, 2), any_count(range(body, 0, 1), 1, 2), std::format("{} nested range", name));

        agree(concat(c, range(body, 0, 2), c), concat(c, any_count(body, 0, 2), c),
              std::format("{} range inside", name));
    }
}

TEST(Repeat_test, Copies_are_independent_values)
{
    // The Indirect gives Repeat value semantics: copying a pattern deep-copies its child, so both the original and the
    // copy lower to working automata, and an empty Repeat is not representable at all.
    const auto original{plus(text("ab"))};

    const auto copy{original};

    const Token token{1, 1};

    const auto original_nfa{to_nfa(original).set_accept_token(token).build()};

    const auto copied_nfa{to_nfa(copy).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(original_nfa, "abab"), (Match{.token = token, .length = 4}));
    EXPECT_EQ(Simulator::run(copied_nfa, "abab"), (Match{.token = token, .length = 4}));
}

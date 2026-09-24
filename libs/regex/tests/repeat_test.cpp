#include "munch/regex/repeat.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

#include "munch/nfa/simulator.hpp"
#include "munch/nfa/tools/graphviz.hpp"
#include "munch/regex/regex.hpp"

using namespace munch::nfa;
using namespace munch::nfa::tools;
using namespace munch::regex;

class Repeat_test : public testing::Test
{
protected:
    void write_dot(const auto& nfa, const std::string& name) const
    {
        Graphviz::to_file(nfa, debug_path_ / (name + ".dot"));
    }

private:
    std::filesystem::path debug_path_{std::string(SOURCE_DIR) + "/debug/"};
};

TEST_F(Repeat_test, Kleene_star)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{kleene(a)};

    const Token token{1, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(token, 0));
}

TEST_F(Repeat_test, Plus)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{plus(a)};

    const Token token{2, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "ababa"), Match(token, 1));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, Optional)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{optional(a)};

    const Token token{3, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ab"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
}

TEST_F(Repeat_test, Exact_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{exact(a, 3)};

    const Token token{4, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 3)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Match(token, 5));
    EXPECT_EQ(Simulator::run(nfa, "aaaaaa"), Match(token, 6));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, At_least_zero_repetitions_is_the_kleene_star)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{at_least(a, 0)};

    const Token token{5, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "a"), Match(token, 1));
    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));

    EXPECT_EQ(Simulator::run(nfa, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(nfa, "ba"), Match(token, 0));
}

TEST_F(Repeat_test, Range_ending_before_it_starts_throws)
{
    EXPECT_THROW((void)range(text('a'), 3, 2), std::invalid_argument);
}

TEST_F(Repeat_test, Range_repetition)
{
    using namespace testing;

    const auto a{text('a')};

    const auto regex{range(a, 2, 4)};

    const Token token{6, 1};

    const auto nfa{to_nfa(regex).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(nfa, "aa"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaa"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(nfa, "aaaa"), Match(token, 4));
    EXPECT_EQ(Simulator::run(nfa, "aaab"), Match(token, 3));
    EXPECT_EQ(Simulator::run(nfa, "aaaaa"), Match(token, 4));

    EXPECT_EQ(Simulator::run(nfa, ""), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "a"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(nfa, "baaa"), Match(std::nullopt, 0));
}

TEST_F(Repeat_test, A_range_over_a_body_that_ends_in_a_repetition_admits_no_suffix_of_that_body)
{
    using namespace testing;

    // The state a range reaches after each optional copy accepts on its own. Joining it to the accepting state of
    // a later copy instead would accept the same words and more: that state lies inside the body and carries the
    // body's own outgoing transitions, so a scan arriving there by that join could go on consuming through them.
    // Under `(ab+){0,1}` the machine then admitted "b", a suffix of the body and no word of the pattern, and the
    // suite had no case over a body whose last atom repeats, which is what shows it.
    const auto body{concat(text('a'), plus(text('b')))};

    const Token token{7, 1};

    const auto once{to_nfa(range(body, 0, 1)).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(once, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(once, "bb"), Match(token, 0));
    EXPECT_EQ(Simulator::run(once, ""), Match(token, 0));
    EXPECT_EQ(Simulator::run(once, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(once, "abb"), Match(token, 3));
    EXPECT_EQ(Simulator::run(once, "abab"), Match(token, 2));

    const auto twice{to_nfa(range(body, 0, 2)).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(twice, "b"), Match(token, 0));
    EXPECT_EQ(Simulator::run(twice, "abab"), Match(token, 4));
    EXPECT_EQ(Simulator::run(twice, "ababab"), Match(token, 4));

    // The same body with a required copy before the optional one, where the suffix could only be admitted after
    // the first copy had matched.
    const auto bounded{to_nfa(range(body, 1, 2)).set_accept_token(token).build()};

    EXPECT_EQ(Simulator::run(bounded, "b"), Match(std::nullopt, 0));
    EXPECT_EQ(Simulator::run(bounded, "ab"), Match(token, 2));
    EXPECT_EQ(Simulator::run(bounded, "abb"), Match(token, 3));
    EXPECT_EQ(Simulator::run(bounded, "abab"), Match(token, 4));
}

TEST_F(Repeat_test, Every_repetition_agrees_with_its_expansion_on_every_body_and_input_enumerated)
{
    using namespace testing;

    // A range between two counts is the choice of the concatenations of each count; an exact repetition is the
    // concatenation of that many copies; at least n is n copies then any number; a plus is a copy then any number;
    // an optional is nothing or a copy; a star is nothing or a plus. Each is compared with what it must equal under
    // longest match, over bodies whose last atom repeats, is optional or is nullable, and over every input over
    // three bytes up to a length, since the range's own defect stood for a year in a suite whose bodies were one
    // byte long. Against the old construction this case reports thousands of mismatches.
    const auto a{text('a')}, b{text('b')}, c{text('c')};

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

    for (std::size_t length{1}; length <= 5; ++length)
    {
        std::vector<std::string> next;

        for (const auto& shorter : inputs)
        {
            if (shorter.size() == length - 1)
            {
                for (const char byte : {'a', 'b', 'c'})
                {
                    next.push_back(shorter + byte);
                }
            }
        }

        inputs.insert(inputs.end(), next.begin(), next.end());
    }

    const auto power{[](const Regex& body, const std::size_t n) {
        auto out{exact(body, 0)};

        for (std::size_t i{0}; i < n; ++i)
        {
            out = i == 0 ? body : concat(out, body);
        }

        return out;
    }};

    const auto any_count{[&power](const Regex& body, const std::size_t lo, const std::size_t hi) {
        auto out{power(body, lo)};

        for (std::size_t k{lo + 1}; k <= hi; ++k)
        {
            out = choice(out, power(body, k));
        }

        return out;
    }};

    const Token token{1, 1};

    const auto agree{[&](const Regex& left, const Regex& right, const std::string& name) {
        const auto one{to_nfa(left).set_accept_token(token).build()};

        const auto other{to_nfa(right).set_accept_token(token).build()};

        for (const auto& input : inputs)
        {
            EXPECT_EQ(Simulator::run(one, input), Simulator::run(other, input)) << name << " on \"" << input << '"';
        }
    }};

    for (std::size_t which{0}; which < bodies.size(); ++which)
    {
        const auto& body{bodies[which]};

        const auto name{"body " + std::to_string(which)};

        for (std::size_t lo{0}; lo <= 2; ++lo)
        {
            for (std::size_t hi{lo}; hi <= 3; ++hi)
            {
                agree(range(body, lo, hi), any_count(body, lo, hi), name + " range");
            }
        }

        for (std::size_t n{0}; n <= 3; ++n)
        {
            agree(exact(body, n), power(body, n), name + " exact");

            agree(at_least(body, n), concat(power(body, n), kleene(body)), name + " at least");
        }

        agree(plus(body), concat(body, kleene(body)), name + " plus");

        agree(optional(body), choice(exact(body, 0), body), name + " optional");

        agree(kleene(body), choice(exact(body, 0), plus(body)), name + " kleene");

        agree(range(range(body, 0, 1), 1, 2), any_count(range(body, 0, 1), 1, 2), name + " nested range");

        agree(concat(c, range(body, 0, 2), c), concat(c, any_count(body, 0, 2), c), name + " range inside");
    }
}

TEST_F(Repeat_test, Copies_are_independent_values)
{
    // The Indirect gives Repeat value semantics: copying a pattern deep-copies its child, so both the original and the
    // copy lower to working automata, and an empty Repeat is no longer representable at all.
    const auto original{plus(text("ab"))};

    const auto copy{original};

    const Token token{1, 1};

    const auto original_nfa{to_nfa(original).set_accept_token(token).build()};

    const auto copied_nfa{to_nfa(copy).set_accept_token(token).build()};

    using Match = Simulator::Match;

    EXPECT_EQ(Simulator::run(original_nfa, "abab"), Match(token, 4));
    EXPECT_EQ(Simulator::run(copied_nfa, "abab"), Match(token, 4));
}

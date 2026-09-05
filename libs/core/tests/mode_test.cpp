#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "munch/core/mode_builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

using namespace munch;
using namespace munch::core;
using namespace munch::regex;

namespace
{
enum class Mode : std::size_t
{
    code,
    string,
    comment
};

enum class Tok : std::size_t
{
    identifier,
    quote,
    text,
    escape,
    comment_open,
    comment_close,
    comment_text,
    space
};

using Stream_t = std::vector<std::tuple<Tok, std::size_t, std::size_t>>;

/**
 * @brief A grammar with the three constructs a flat token set cannot express: strings with escapes, and comments
 *        that nest.
 */
Mode_lexer build()
{
    Mode_builder builder;

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 2);
    builder.add_token(Mode::code, any_of(Set{' '}), Tok::space, 2);
    builder.add_token(
            Mode::code, text("\""), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});
    builder.add_token(
            Mode::code, text("/*"), Tok::comment_open, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::comment)});

    // Inside a string the same quote byte terminates rather than opens, which is the whole point of a mode.
    builder.add_token(Mode::string, text("\""), Tok::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::string, concat(text("\\"), any_of(Set::all())), Tok::escape, 1);
    builder.add_token(Mode::string, plus(any_of(Set::all() - '"' - '\\')), Tok::text, 2);

    // Nesting comes from the stack: an inner open pushes again, and each close pops one level.
    builder.add_token(
            Mode::comment, text("/*"), Tok::comment_open, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::comment)});
    builder.add_token(Mode::comment, text("*/"), Tok::comment_close, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, any_of(Set::all()), Tok::comment_text, 2);

    return builder.build();
}

Stream_t scan(const Mode_lexer& lexer, const std::string& input, std::size_t& consumed)
{
    Stream_t stream;

    consumed = lexer.tokenize_all<Tok>(
            input, [&stream](const Tok token, const std::size_t length, const std::size_t mode) {
                stream.emplace_back(token, length, mode);
            });

    return stream;
}
} // namespace

// One viability probe per public Mode_lexer overload, for the same reason the flat lexer's suite keeps one per
// overload: a combined requires-expression proves only that at least one call rejects a type.
template <typename Iterator>
concept Mode_single_through = requires(const Mode_lexer& lexer, const Iterator& iterator, Mode_stack& stack) {
    lexer.template tokenize<int>(iterator, iterator, stack);
};

template <typename Iterator>
concept Mode_full_through = requires(const Mode_lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all<int>(iterator, iterator, [](int, std::size_t, std::size_t) {});
};

template <typename Iterator>
concept Mode_full_through_with_stack = requires(const Mode_lexer& lexer, const Iterator& iterator, Mode_stack& stack) {
    lexer.template tokenize_all<int>(iterator, iterator, [](int, std::size_t, std::size_t) {}, stack);
};

template <typename Container>
concept Mode_full_over = requires(const Mode_lexer& lexer, const Container& container) {
    lexer.template tokenize_all<int>(container, [](int, std::size_t, std::size_t) {});
};

template <typename Container>
concept Mode_full_over_with_stack = requires(const Mode_lexer& lexer, const Container& container, Mode_stack& stack) {
    lexer.template tokenize_all<int>(container, [](int, std::size_t, std::size_t) {}, stack);
};

TEST(Mode, Every_entry_point_holds_the_byte_domain_and_scans_std_byte_like_char)
{
    using Good_iterator = std::string_view::iterator;
    using Bad_iterator = std::vector<double>::const_iterator;

    static_assert(Mode_single_through<Good_iterator> && !Mode_single_through<Bad_iterator>);
    static_assert(Mode_full_through<Good_iterator> && !Mode_full_through<Bad_iterator>);
    static_assert(Mode_full_through_with_stack<Good_iterator> && !Mode_full_through_with_stack<Bad_iterator>);
    static_assert(Mode_full_over<std::string> && !Mode_full_over<std::vector<double>>);
    static_assert(!Mode_full_over<std::vector<std::string>>);
    static_assert(Mode_full_over_with_stack<std::string> && !Mode_full_over_with_stack<std::vector<double>>);

    // Viability never instantiates a body, so std::byte runs through the modal scan as well, and the answers
    // must match the same input spelled as char.
    const auto lexer{build()};

    const std::string text{R"(ab "x\"y" /* c /* d */ e */ f)"};

    std::vector<std::byte> bytes;

    for (const char symbol : text)
    {
        bytes.push_back(static_cast<std::byte>(symbol));
    }

    std::size_t consumed{0};

    const auto expected{scan(lexer, text, consumed)};

    ASSERT_EQ(consumed, text.size());

    Stream_t from_bytes;

    EXPECT_EQ(
            consumed, lexer.tokenize_all<Tok>(
                              bytes, [&from_bytes](const Tok token, const std::size_t length, const std::size_t mode) {
                                  from_bytes.emplace_back(token, length, mode);
                              }));

    EXPECT_EQ(from_bytes, expected);

    Stream_t through_iterators;

    EXPECT_EQ(
            consumed, lexer.tokenize_all<Tok>(
                              bytes.begin(), bytes.end(),
                              [&through_iterators](const Tok token, const std::size_t length, const std::size_t mode) {
                                  through_iterators.emplace_back(token, length, mode);
                              }));

    EXPECT_EQ(through_iterators, expected);
}

TEST(Mode, Same_byte_means_different_tokens_in_different_modes)
{
    const auto lexer{build()};

    std::size_t consumed{0};

    const auto stream{scan(lexer, R"(ab "cd" ef)", consumed)};

    EXPECT_EQ(consumed, 10U);

    const Stream_t expected{{Tok::identifier, 2, 0}, {Tok::space, 1, 0}, {Tok::quote, 1, 0},     {Tok::text, 2, 1},
                            {Tok::quote, 1, 1},      {Tok::space, 1, 0}, {Tok::identifier, 2, 0}};

    EXPECT_EQ(stream, expected);
}

TEST(Mode, Escapes_keep_the_string_open)
{
    const auto lexer{build()};

    std::size_t consumed{0};

    // The escaped quote must not terminate: a flat grammar has no way to say so.
    const auto stream{scan(lexer, R"("a\"b" c)", consumed)};

    EXPECT_EQ(consumed, 8U);

    ASSERT_EQ(stream.size(), 7U);

    EXPECT_EQ(std::get<0>(stream[2]), Tok::escape);

    EXPECT_EQ(std::get<0>(stream[4]), Tok::quote);

    // The final identifier is back in code mode, so the string really closed.
    EXPECT_EQ(std::get<2>(stream.back()), 0U);
}

TEST(Mode, Comments_nest_through_the_stack)
{
    const auto lexer{build()};

    std::size_t consumed{0};

    const auto stream{scan(lexer, "a /* x /* y */ z */ b", consumed)};

    EXPECT_EQ(consumed, 21U);

    // Only the outermost close returns to code mode; the inner one drops a level and stays inside.
    EXPECT_EQ(std::get<2>(stream.back()), 0U);

    std::size_t closes{0};

    for (const auto& [token, length, mode] : stream)
    {
        closes += token == Tok::comment_close ? 1 : 0;
    }

    EXPECT_EQ(closes, 2U);
}

TEST(Mode, Unbalanced_close_stops_the_scan)
{
    const auto lexer{build()};

    // Started in string mode with nothing saved, so the closing quote's pop finds no frame: the text before it is
    // delivered, the scan stops at the quote with a short consumed length, and the stack stays where it stood.
    Mode_stack stack;

    stack.current = static_cast<std::size_t>(Mode::string);

    Stream_t stream;

    const std::string input{"a\"b"};

    const auto consumed{lexer.tokenize_all<Tok>(
            input.begin(), input.end(),
            [&stream](const Tok token, const std::size_t length, const std::size_t mode) {
                stream.emplace_back(token, length, mode);
            },
            stack)};

    EXPECT_EQ(consumed, 1U);

    ASSERT_EQ(stream.size(), 1U);

    EXPECT_EQ(std::get<0>(stream.front()), Tok::text);

    EXPECT_EQ(stack.current, static_cast<std::size_t>(Mode::string));

    EXPECT_TRUE(stack.saved.empty());

    // The bare stack refuses the same pop.
    Mode_stack bare;

    EXPECT_FALSE(bare.apply({.kind = Mode_action_kind::pop}));

    EXPECT_EQ(bare.current, 0U);
}

TEST(Mode, Unterminated_string_consumes_but_stays_in_string_mode)
{
    const auto lexer{build()};

    std::size_t consumed{0};

    const auto stream{scan(lexer, R"("abc)", consumed)};

    EXPECT_EQ(consumed, 4U);

    EXPECT_EQ(std::get<2>(stream.back()), static_cast<std::size_t>(Mode::string));
}

TEST(Mode, A_skipped_mode_index_is_rejected)
{
    Mode_builder builder;

    builder.add_token(std::size_t{0}, text("a"), Tok::identifier, 1);

    builder.add_token(std::size_t{2}, text("b"), Tok::identifier, 1);

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode, An_empty_grammar_is_rejected)
{
    const Mode_builder builder;

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode, Per_mode_lexers_remain_inspectable)
{
    const auto lexer{build()};

    EXPECT_EQ(lexer.modes(), 3U);

    // A single mode still certifies its own split points. That answer is sound only for input known to be scanned
    // entirely in that mode, which is why Mode_lexer exposes no parallel entry point of its own.
    EXPECT_TRUE(lexer.mode(static_cast<std::size_t>(Mode::code)).is_split_point(' '));

    EXPECT_FALSE(lexer.mode(static_cast<std::size_t>(Mode::string)).is_split_point(' '));
}

TEST(Mode, The_two_drivers_agree_on_the_token_stream)
{
    // tokenize_all() stays inside one mode's batch scan until an action fires, rather than re-entering the scanner
    // once per token, and it reads each action from the matched token's payload where the per-token entry point
    // searches for it. Two different paths, so they are pinned to the same stream. Their throughput ratio is a
    // benchmark row, munch_benchmark_modes, not an assertion here: a wall-clock bound in a unit test varies with the
    // machine, the compiler and the sanitizers.
    const auto lexer{build()};

    std::string input;

    while (input.size() < (1U << 16))
    {
        input += "ab \"cd ef\" gh /* x */ ij ";
    }

    std::vector<std::pair<Tok, std::size_t>> batch;

    lexer.tokenize_all<Tok>(input, [&batch](const Tok token, const std::size_t length, std::size_t) {
        batch.emplace_back(token, length);
    });

    std::vector<std::pair<Tok, std::size_t>> per_token;

    Mode_stack stack;

    for (std::size_t offset{0}; offset < input.size();)
    {
        const auto match{
                lexer.tokenize<Tok>(input.cbegin() + static_cast<std::ptrdiff_t>(offset), input.cend(), stack)};

        if (!match.token || match.length == 0)
        {
            break;
        }

        per_token.emplace_back(*match.token, match.length);

        offset += match.length;
    }

    EXPECT_FALSE(batch.empty());

    EXPECT_EQ(batch, per_token);
}

TEST(Mode, A_stopped_scan_reports_the_mode_and_depth_it_stopped_in)
{
    const auto lexer{build()};

    // An unterminated string and an unrecognized byte in code both stop the scan. Only the mode tells them apart,
    // which is the whole reason the stack is exposed.
    Mode_stack unterminated;

    // std::string, not the literal: a string literal is a char array whose NUL terminator is part of the range, and
    // both of these grammars accept it as content.
    const auto in_string{
            lexer.tokenize_all<Tok>(std::string{R"("abc)"}, [](Tok, std::size_t, std::size_t) {}, unterminated)};

    EXPECT_EQ(in_string, 4U);

    EXPECT_EQ(unterminated.current, static_cast<std::size_t>(Mode::string));

    EXPECT_EQ(unterminated.saved.size(), 1U);

    Mode_stack nested;

    const auto in_comment{
            lexer.tokenize_all<Tok>(std::string{"a /* x /* y"}, [](Tok, std::size_t, std::size_t) {}, nested)};

    EXPECT_EQ(in_comment, 11U);

    EXPECT_EQ(nested.current, static_cast<std::size_t>(Mode::comment));

    // Two opens, no closes, so the depth names how many comments are still open.
    EXPECT_EQ(nested.saved.size(), 2U);
}

TEST(Mode, Diagnose_reports_faults_only_a_modal_grammar_can_have)
{
    Mode_builder builder;

    builder.add_token(Mode::code, text("a"), Tok::identifier, 1);
    builder.add_token(
            Mode::code, text("\""), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    // string can be entered and left; comment can be entered by nothing and left by nothing.
    builder.add_token(Mode::string, text("\""), Tok::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, text("z"), Tok::comment_text, 1);

    const auto report{builder.diagnose()};

    ASSERT_EQ(report.per_mode.size(), 3U);

    EXPECT_EQ(report.unreachable_modes, std::vector<std::size_t>{static_cast<std::size_t>(Mode::comment)});

    // code never leaves itself except by pushing, which counts; comment has no exit at all.
    EXPECT_EQ(report.inescapable_modes, std::vector<std::size_t>{static_cast<std::size_t>(Mode::comment)});
}

TEST(Mode, Diagnose_is_quiet_on_a_sound_grammar)
{
    Mode_builder builder;

    builder.add_token(Mode::code, text("a"), Tok::identifier, 1);
    builder.add_token(
            Mode::code, text("\""), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});
    builder.add_token(Mode::string, text("\""), Tok::quote, 1, {.kind = Mode_action_kind::pop});

    const auto report{builder.diagnose()};

    EXPECT_TRUE(report.unreachable_modes.empty());

    EXPECT_TRUE(report.inescapable_modes.empty());
}

namespace
{
/**
 * @brief A deterministic pseudo-random modal grammar over a three-symbol alphabet.
 *
 * Hand-picked grammars agree with the driver that was written alongside them. Random ones do not, which is what
 * makes them worth running: the alphabet is kept tiny so inputs collide with the grammar often enough to exercise
 * mode changes rather than failing at the first byte.
 */
Mode_lexer random_mode_grammar(unsigned& seed)
{
    const auto next{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    const auto modes{2 + next() % 3};

    Mode_builder builder;

    for (std::size_t mode{0}; mode < modes; ++mode)
    {
        // Every mode gets one single-byte token that changes the mode. Without it most random grammars fail at the
        // first byte, streams stay one or two tokens long, and the driver under test never sees a mode change.
        builder.add_token(
                mode, text(std::string(1, static_cast<char>('a' + mode % 3))), std::size_t{0}, 1,
                Mode_action{
                        .kind = mode % 2 == 0 ? Mode_action_kind::push : Mode_action_kind::pop,
                        .target = (mode + 1) % modes});

        const auto tokens{2 + next() % 3};

        for (std::size_t token{1}; token < tokens + 1; ++token)
        {
            constexpr const char* atoms[]{"a", "b", "c", "ab", "bc", "ca", "abc"};

            const auto pattern{text(atoms[next() % std::size(atoms)])};

            Mode_action action{};

            switch (next() % 5)
            {
            case 0:
                action = {.kind = Mode_action_kind::push, .target = next() % modes};
                break;

            case 1:
                action = {.kind = Mode_action_kind::pop};
                break;

            case 2:
                action = {.kind = Mode_action_kind::go_to, .target = next() % modes};
                break;

            default:
                break;
            }

            builder.add_token(mode, pattern, token, 1 + token % 2, action);
        }
    }

    return builder.build();
}

/**
 * @brief Drives a mode lexer one token at a time, the shape the batch driver replaced.
 */
Stream_t drive_per_token(const Mode_lexer& lexer, const std::string& input, std::size_t& consumed, Mode_stack& stack)
{
    Stream_t stream;

    consumed = 0;

    while (consumed < input.size())
    {
        const auto mode{stack.current};

        const auto match{
                lexer.tokenize<Tok>(input.cbegin() + static_cast<std::ptrdiff_t>(consumed), input.cend(), stack)};

        if (!match.token || match.length == 0)
        {
            break;
        }

        stream.emplace_back(*match.token, match.length, mode);

        consumed += match.length;
    }

    return stream;
}
} // namespace

TEST(Mode, The_two_drivers_agree_on_random_grammars_and_inputs)
{
    unsigned seed{20260803};

    const auto next{[&seed] { return seed = seed * 1664525U + 1013904223U, seed >> 16U; }};

    std::size_t checked{0};

    std::size_t with_switches{0};

    for (int round{0}; round < 300; ++round)
    {
        const auto lexer{random_mode_grammar(seed)};

        std::string input;

        for (auto length{8 + next() % 60}; input.size() < length;)
        {
            input += static_cast<char>('a' + next() % 3);
        }

        Stream_t batched;

        Mode_stack batch_stack;

        const auto batch_consumed{lexer.tokenize_all<Tok>(
                input,
                [&batched](const Tok token, const std::size_t length, const std::size_t mode) {
                    batched.emplace_back(token, length, mode);
                },
                batch_stack)};

        std::size_t single_consumed{0};

        Mode_stack single_stack;

        const auto single{drive_per_token(lexer, input, single_consumed, single_stack)};

        ASSERT_EQ(batch_consumed, single_consumed) << "round " << round << ", input \"" << input << '"';

        ASSERT_EQ(batched, single) << "round " << round << ", input \"" << input << '"';

        ASSERT_EQ(batch_stack, single_stack) << "round " << round << ", input \"" << input << '"';

        ++checked;

        for (std::size_t at{1}; at < batched.size(); ++at)
        {
            if (std::get<2>(batched[at]) != std::get<2>(batched[at - 1]))
            {
                ++with_switches;

                break;
            }
        }
    }

    EXPECT_EQ(checked, 300U);

    // Measured at 82 of 300. A run where few grammars changed mode would prove little about a driver whose entire
    // purpose is handling changes, and an earlier version of this generator managed only 19.
    EXPECT_GT(with_switches, 60U) << "the random grammars stopped exercising mode changes";
}

TEST(Mode, An_action_targeting_a_mode_that_does_not_exist_is_rejected)
{
    Mode_builder builder;

    builder.add_token(Mode::code, text("x"), Tok::identifier, 1, {.kind = Mode_action_kind::go_to, .target = 7});

    // Left unchecked this reaches Mode_stack::apply, which sets current without a bound, and the next token indexes
    // the per-mode lexer vector out of range.
    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode, A_target_may_name_a_mode_registered_later)
{
    Mode_builder builder;

    // Forward references are legitimate: the target is checked at build(), once every mode is known.
    builder.add_token(
            Mode::code, text("\""), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    builder.add_token(Mode::string, text("\""), Tok::quote, 1, {.kind = Mode_action_kind::pop});

    EXPECT_NO_THROW(std::ignore = builder.build());
}

TEST(Mode, Two_conflicting_actions_for_one_token_are_rejected)
{
    Mode_builder builder;

    builder.add_token(
            Mode::code, text("x"), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    // The scanner reports the token ID, not which pattern matched, so it could not choose between these. Silently
    // keeping the last one made the push simply not happen.
    EXPECT_THROW(
            builder.add_token(Mode::code, text("y"), Tok::quote, 1, {.kind = Mode_action_kind::pop}),
            std::invalid_argument);
}

TEST(Mode, Two_patterns_may_share_a_token_when_the_action_agrees)
{
    Mode_builder builder;

    // Sharing an ID is fine; only a conflicting action is not.
    builder.add_token(Mode::code, text("x"), Tok::identifier, 1);

    EXPECT_NO_THROW(builder.add_token(Mode::code, text("y"), Tok::identifier, 1));

    builder.add_token(
            Mode::code, text("p"), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    EXPECT_NO_THROW(builder.add_token(
            Mode::code, text("q"), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)}));

    builder.add_token(Mode::string, text("z"), Tok::text, 1);

    EXPECT_NO_THROW(std::ignore = builder.build());
}

TEST(Mode, A_caller_supplied_stack_naming_a_missing_mode_is_rejected)
{
    const auto lexer{build()};

    const std::string input{"ab"};

    Mode_stack bad;

    bad.current = 9;

    EXPECT_THROW(std::ignore = lexer.tokenize<Tok>(input.cbegin(), input.cend(), bad), std::out_of_range);

    // The batch path would otherwise carry the out-of-range mode into its no-actions branch and index there.
    EXPECT_THROW(
            std::ignore = lexer.tokenize_all<Tok>(input, [](Tok, std::size_t, std::size_t) {}, bad), std::out_of_range);

    // A saved frame is checked when a pop is about to expose it rather than on entry, so a scan that never pops
    // runs to completion beside one, and the same scan with a closing quote is rejected.
    Mode_stack poisoned;

    poisoned.saved.push_back(9);

    EXPECT_NO_THROW(std::ignore = lexer.tokenize_all<Tok>(input, [](Tok, std::size_t, std::size_t) {}, poisoned));

    Mode_stack popping;

    popping.current = static_cast<std::size_t>(Mode::string);

    popping.saved.push_back(9);

    const std::string closing{"a\""};

    EXPECT_THROW(
            std::ignore = lexer.tokenize_all<Tok>(
                    closing, [](Tok, std::size_t, std::size_t) {}, popping),
            std::out_of_range);

    Mode_stack per_token;

    per_token.current = static_cast<std::size_t>(Mode::string);

    per_token.saved.push_back(9);

    EXPECT_THROW(std::ignore = lexer.tokenize<Tok>(closing.cbegin() + 1, closing.cend(), per_token), std::out_of_range);
}

TEST(Mode, A_pop_escapes_only_a_mode_something_pushes_into)
{
    Mode_builder builder;

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 2);

    // Entered by go_to, so nothing ever saves a frame for its pop to return to. The pop can fire only on a frame the
    // caller supplied, which the grammar does not establish, so the mode is inescapable by this grammar alone.
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Tok::quote, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::string)});

    builder.add_token(Mode::string, any_of(Set::all()), Tok::text, 2);
    builder.add_token(Mode::string, any_of(Set{'"'}), Tok::quote, 1, {.kind = Mode_action_kind::pop});

    const auto diagnostics{builder.diagnose()};

    EXPECT_EQ(diagnostics.inescapable_modes, std::vector<std::size_t>{static_cast<std::size_t>(Mode::string)});
}

TEST(Mode, A_go_to_carries_the_frame_that_a_later_pop_returns_to)
{
    Mode_builder builder;

    // 0 pushes into 1, 1 goes to 2, 2 pops. The scan succeeds, because go_to keeps the frame the push left, so
    // mode 2 escapes even though nothing pushes into it directly.
    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 2);
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    builder.add_token(
            Mode::string, any_of(Set{'#'}), Tok::escape, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::comment)});
    builder.add_token(Mode::string, any_of(Set::all()), Tok::text, 2);

    builder.add_token(Mode::comment, any_of(Set{'!'}), Tok::comment_close, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, any_of(Set::all()), Tok::comment_text, 2);

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(diagnostics.inescapable_modes.empty());
}

TEST(Mode, A_self_push_does_not_make_a_pop_an_escape)
{
    Mode_builder builder;

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 2);
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Tok::quote, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::string)});

    // Entered by go_to, so every frame the pop can ever expose comes from the mode's own self-push and names the
    // mode itself: each pop returns exactly where it started, and no scan from mode 0 ever leaves again.
    builder.add_token(
            Mode::string, any_of(Set{'('}), Tok::escape, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});
    builder.add_token(Mode::string, any_of(Set{')'}), Tok::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::string, plus(any_of(Set::alpha())), Tok::text, 2);

    const auto diagnostics{builder.diagnose()};

    EXPECT_EQ(diagnostics.inescapable_modes, std::vector<std::size_t>{static_cast<std::size_t>(Mode::string)});
}

TEST(Mode, A_dead_token_grants_neither_reachability_nor_escape)
{
    Mode_builder builder;

    // The identifier fully shadows the push at every input, so the push can never fire: diagnose() must not let it
    // enter its target or count as leaving, or the report describes a grammar the scanner does not run.
    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 1);
    builder.add_token(
            Mode::code, plus(any_of(Set::alpha())), Tok::text, 2,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});
    builder.add_token(Mode::string, any_of(Set{'"'}), Tok::quote, 1, {.kind = Mode_action_kind::pop});

    const auto diagnostics{builder.diagnose()};

    EXPECT_TRUE(std::ranges::contains(diagnostics.per_mode[0].dead_tokens, static_cast<std::size_t>(Tok::text)));
    EXPECT_EQ(diagnostics.unreachable_modes, std::vector<std::size_t>{static_cast<std::size_t>(Mode::string)});

    const std::vector<std::size_t> both{static_cast<std::size_t>(Mode::code), static_cast<std::size_t>(Mode::string)};

    EXPECT_EQ(diagnostics.inescapable_modes, both);
}

TEST(Mode, A_pop_with_nothing_saved_is_refused_before_the_sink_in_the_batch_driver)
{
    const auto lexer{build()};

    // Seeded directly into the string mode with nothing saved, so the closing quote's pop must refuse. The batch
    // scan counts a stopping token as consumed, so the driver has to hold that length back and fire no sink.
    Mode_stack stack{.current = static_cast<std::size_t>(Mode::string)};

    const std::string input{"\""};

    auto calls{0};

    const auto consumed{lexer.tokenize_all<Tok>(
            input.begin(), input.end(), [&calls](const Tok, const std::size_t, const std::size_t) { ++calls; }, stack)};

    EXPECT_EQ(consumed, 0U);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(stack.current, static_cast<std::size_t>(Mode::string));
    EXPECT_TRUE(stack.saved.empty());
}

TEST(Mode, A_go_to_onto_its_own_mode_is_a_stay)
{
    Mode_builder builder;

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Tok::identifier, 2);

    // Observably a stay: the mode does not change, so both drivers must emit what a stay would.
    builder.add_token(
            Mode::code, any_of(Set{' '}), Tok::space, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::code)});

    const auto lexer{builder.build()};

    const std::string input{"ab cd ef"};

    std::vector<std::pair<Tok, std::size_t>> batch;

    Mode_stack stack;

    const auto consumed{lexer.tokenize_all<Tok>(
            input,
            [&batch](const Tok token, const std::size_t length, std::size_t) { batch.emplace_back(token, length); },
            stack)};

    EXPECT_EQ(consumed, input.size());

    EXPECT_EQ(stack.current, static_cast<std::size_t>(Mode::code));

    EXPECT_TRUE(stack.saved.empty());

    std::vector<std::pair<Tok, std::size_t>> per_token;

    Mode_stack walking;

    for (std::size_t offset{0}; offset < input.size();)
    {
        const auto [token, length]{lexer.tokenize<Tok>(input.cbegin() + offset, input.cend(), walking)};

        ASSERT_TRUE(token.has_value());

        per_token.emplace_back(*token, length);

        offset += length;
    }

    EXPECT_EQ(batch, per_token);

    EXPECT_EQ(walking.current, stack.current);
}

TEST(Mode, A_nullable_token_carrying_an_action_is_rejected)
{
    Mode_builder builder;

    // Matches the empty string, so the batch driver would stop without reporting it while the per-token driver
    // returns it with length zero. An action neither applies is not something a caller can rely on.
    builder.add_token(
            Mode::code, kleene(any_of(Set{'a'})), Tok::identifier, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::string)});
    builder.add_token(Mode::string, any_of(Set::all()), Tok::text, 2);

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode, A_nullable_token_without_an_action_still_builds)
{
    Mode_builder builder;

    // Legal, and the two drivers still report it differently: only the action is refused, not the pattern.
    builder.add_token(Mode::code, kleene(any_of(Set{'a'})), Tok::identifier, 1);

    const auto lexer{builder.build()};

    const std::string input{"b"};

    Mode_stack per_token;

    const auto [token, length]{lexer.tokenize<Tok>(input.cbegin(), input.cend(), per_token)};

    EXPECT_EQ(token, Tok::identifier);

    EXPECT_EQ(length, 0U);

    Mode_stack batch;

    EXPECT_EQ(lexer.tokenize_all<Tok>(input, [](Tok, std::size_t, std::size_t) {}, batch), 0U);

    EXPECT_EQ(per_token.current, batch.current);
}

TEST(Mode, An_action_kind_outside_the_enumeration_is_rejected)
{
    Mode_builder builder;

    // Accepted, the per-token driver refused the token and the batch driver emitted it: the same input tokenized
    // two different ways depending on which entry point was used.
    EXPECT_THROW(
            builder.add_token(Mode::code, text("x"), Tok::identifier, 1, {.kind = static_cast<Mode_action_kind>(999)}),
            std::invalid_argument);
}

TEST(Mode, A_negative_or_unrepresentable_id_is_rejected)
{
    Mode_builder builder;

    // -1 becomes SIZE_MAX, and the id + 1 that sizes the row wraps to zero, so the next index is out of bounds on an
    // empty vector. This crashed before it was checked.
    EXPECT_THROW(builder.add_token(std::size_t{0}, text("x"), -1, 1), std::invalid_argument);

    EXPECT_THROW(builder.add_token(-1, text("y"), std::size_t{0}, 1), std::invalid_argument);
}

TEST(Mode, A_rejected_registration_leaves_the_grammar_untouched)
{
    Mode_builder builder;

    builder.add_token(
            Mode::code, text("x"), Tok::quote, 1,
            {.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(Mode::string)});

    EXPECT_THROW(
            builder.add_token(Mode::code, text("y"), Tok::quote, 1, {.kind = Mode_action_kind::pop}),
            std::invalid_argument);

    builder.add_token(Mode::string, text("z"), Tok::text, 1, {.kind = Mode_action_kind::pop});

    const auto lexer{builder.build()};

    // If the rejected pattern had been registered anyway it would inherit the push, so "y" would enter string mode.
    Mode_stack stack;

    const std::string input{"y"};

    const auto match{lexer.tokenize<Tok>(input.cbegin(), input.cend(), stack)};

    EXPECT_FALSE(match.token) << "the rejected pattern was registered after all";
}

TEST(Mode, A_target_is_ignored_where_the_kind_does_not_use_one)
{
    Mode_builder builder;

    builder.add_token(Mode::code, text("x"), Tok::identifier, 1, {.kind = Mode_action_kind::stay, .target = 3});

    // stay and pop document target as ignored, so these agree and must not read as a conflict.
    EXPECT_NO_THROW(builder.add_token(
            Mode::code, text("y"), Tok::identifier, 1, {.kind = Mode_action_kind::stay, .target = 7}));
}

TEST(Mode, Reachability_is_a_walk_from_mode_zero_not_merely_being_named)
{
    Mode_builder builder;

    builder.add_token(Mode::code, text("a"), Tok::identifier, 1);

    // comment is named only by string, and nothing reaches string from code. Marking every target as entered would
    // call both reachable; a walk from mode 0 reports both.
    builder.add_token(
            Mode::string, text("b"), Tok::text, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::comment)});

    builder.add_token(Mode::comment, text("c"), Tok::comment_text, 1);

    const auto report{builder.diagnose()};

    const std::vector<std::size_t> expected{
            static_cast<std::size_t>(Mode::string), static_cast<std::size_t>(Mode::comment)};

    EXPECT_EQ(report.unreachable_modes, expected);
}

TEST(Mode, Sparse_token_ids_cost_only_what_was_registered)
{
    Mode_builder builder;

    // A row indexed by token value, in the builder as well as at runtime, made one parser-style code size the whole
    // table per mode. Both are lists of what was registered now, so these are as cheap as small ones.
    builder.add_token(Mode::code, text("a"), std::size_t{0}, 1);

    builder.add_token(
            Mode::code, text("b"), std::size_t{70000}, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::string)});

    builder.add_token(
            Mode::string, text("c"), std::size_t{70001}, 1,
            {.kind = Mode_action_kind::go_to, .target = static_cast<std::size_t>(Mode::code)});

    const auto lexer{builder.build()};

    std::size_t tokens{0};

    const std::string input{"abcabc"};

    const auto consumed{
            lexer.tokenize_all<std::size_t>(input, [&tokens](std::size_t, std::size_t, std::size_t) { ++tokens; })};

    EXPECT_EQ(consumed, input.size());

    EXPECT_EQ(tokens, 6U);
}

TEST(Mode, The_two_drivers_agree_for_every_action_count_and_token_base)
{
    // The batch driver reads each action from the matched token's payload and the per-token one searches for it, so
    // they are separate code needing to agree whatever a mode's action count. The input drives every go_to letter
    // once, returns through y each time, and ends on z in mode 0, so both drivers consume it whole.
    for (const std::size_t actions : {1U, 3U})
    {
        for (const std::size_t base : {std::size_t{0}, std::size_t{100}})
        {
            Mode_builder builder;

            builder.add_token(std::size_t{0}, text("z"), base, 2);

            for (std::size_t extra{0}; extra < actions; ++extra)
            {
                builder.add_token(
                        std::size_t{0}, text(std::string(1, static_cast<char>('a' + extra))), base + extra + 1, 1,
                        {.kind = Mode_action_kind::go_to, .target = 1});
            }

            builder.add_token(std::size_t{1}, text("y"), base + 90, 1, {.kind = Mode_action_kind::go_to, .target = 0});

            const auto lexer{builder.build()};

            std::string input;

            std::vector<std::pair<std::size_t, std::size_t>> expected;

            for (std::size_t extra{0}; extra < actions; ++extra)
            {
                input += static_cast<char>('a' + extra);
                input += 'y';
                expected.emplace_back(base + extra + 1, std::size_t{0});
                expected.emplace_back(base + 90, std::size_t{1});
            }

            input += 'z';
            expected.emplace_back(base, std::size_t{0});

            std::vector<std::pair<std::size_t, std::size_t>> batch;

            Mode_stack batch_stack;

            const auto batch_consumed{lexer.tokenize_all<std::size_t>(
                    input.cbegin(), input.cend(),
                    [&batch](const std::size_t token, std::size_t, const std::size_t mode) {
                        batch.emplace_back(token, mode);
                    },
                    batch_stack)};

            std::vector<std::pair<std::size_t, std::size_t>> single;

            Mode_stack stack;

            std::size_t at{0};

            while (at < input.size())
            {
                const auto mode{stack.current};

                const auto match{lexer.tokenize<std::size_t>(
                        input.cbegin() + static_cast<std::ptrdiff_t>(at), input.cend(), stack)};

                if (!match.token || match.length == 0)
                {
                    break;
                }

                single.emplace_back(*match.token, mode);

                at += match.length;
            }

            const auto label{"actions=" + std::to_string(actions) + " base=" + std::to_string(base)};

            EXPECT_EQ(batch_consumed, input.size()) << label;

            EXPECT_EQ(at, input.size()) << label;

            EXPECT_EQ(batch, expected) << label;

            EXPECT_EQ(single, expected) << label;

            EXPECT_EQ(batch_stack.current, 0U) << label;

            EXPECT_EQ(stack.current, 0U) << label;

            EXPECT_TRUE(batch_stack.saved.empty() && stack.saved.empty()) << label;
        }
    }
}

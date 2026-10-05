#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "munch/core/mode_builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

using namespace munch::core;
using namespace munch::regex;

namespace
{
/**
 * @brief The modes of the test grammar.
 */
enum class Mode : std::size_t
{
    code,
    string,
    comment
};

/**
 * @brief The tokens of the test grammars.
 */
enum class Token_kind : std::uint8_t
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

/**
 * @brief A scanned token stream: each token with its length and the mode it matched in.
 */
using Stream_t = std::vector<std::tuple<Token_kind, std::size_t, std::size_t>>;

/**
 * @brief A token stream collected from a scan, each token with its length and the mode it matched in, and the bytes the
 *        scan consumed.
 * @tparam T The token type.
 */
template <typename T>
struct Scanned
{
    /**
     * @brief The tokens in input order, each with its length and the mode it matched in.
     */
    std::vector<std::tuple<T, std::size_t, std::size_t>> stream{};

    /**
     * @brief The number of input bytes the scan consumed.
     */
    std::size_t consumed{};
};

/**
 * @brief Ignores every token of a scan.
 * @tparam T The token type.
 */
constexpr auto ignore_token{[]<typename T>(const T, const std::size_t, const std::size_t) {}};

/**
 * @brief The multiplier of the tests' linear congruential generator.
 */
constexpr unsigned lcg_multiplier{1664525U};

/**
 * @brief The increment of the tests' linear congruential generator.
 */
constexpr unsigned lcg_increment{1013904223U};

/**
 * @brief The random grammars the driver agreement test draws.
 */
constexpr std::size_t rounds{300};

/**
 * @brief Returns the token of one entry of a stream.
 * @param emitted The stream entry.
 * @return The token.
 */
Token_kind token_of(const Stream_t::value_type& emitted)
{
    const auto& [token, length, mode]{emitted};

    return token;
}

/**
 * @brief Returns the mode one entry of a stream matched in.
 * @param emitted The stream entry.
 * @return The mode.
 */
std::size_t mode_of(const Stream_t::value_type& emitted)
{
    const auto& [token, length, mode]{emitted};

    return mode;
}

/**
 * @brief Builds a grammar with the three constructs a flat token set cannot express: strings with escapes, and comments
 *        that nest.
 * @return The compiled mode lexer.
 */
Mode_lexer build()
{
    Mode_builder builder{};

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 2);
    builder.add_token(Mode::code, any_of(Set{' '}), Token_kind::space, 2);
    builder.add_token(
            Mode::code, text(R"(")"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});
    builder.add_token(
            Mode::code, text("/*"), Token_kind::comment_open, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::comment)});

    // Inside a string the same quote byte terminates rather than opens, which is the whole point of a mode.
    builder.add_token(Mode::string, text(R"(")"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::string, concat(text(R"(\)"), any_of(Set::all())), Token_kind::escape, 1);
    builder.add_token(Mode::string, plus(any_of(Set::all() - '"' - '\\')), Token_kind::text, 2);

    // Nesting comes from the stack: an inner open pushes again, and each close pops one level.
    builder.add_token(
            Mode::comment, text("/*"), Token_kind::comment_open, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::comment)});
    builder.add_token(Mode::comment, text("*/"), Token_kind::comment_close, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, any_of(Set::all()), Token_kind::comment_text, 2);

    return builder.build();
}

/**
 * @brief Returns a sink that appends every token, its length and its mode to a stream.
 * @tparam T The token type.
 * @param stream The stream appended to.
 * @return The sink.
 */
template <typename T = Token_kind>
auto collect_into(std::vector<std::tuple<T, std::size_t, std::size_t>>& stream)
{
    return [&stream](const T token, const std::size_t length, const std::size_t mode) {
        stream.emplace_back(token, length, mode);
    };
}

/**
 * @brief Scans an input with the batch driver, collecting the stream.
 * @param lexer The mode lexer.
 * @param input The input to scan.
 * @return The token stream and the bytes the scan consumed.
 */
Scanned<Token_kind> scan(const Mode_lexer& lexer, const std::string& input)
{
    Scanned<Token_kind> scanned{};

    scanned.consumed = lexer.tokenize_all<Token_kind>(input, collect_into(scanned.stream));

    return scanned;
}

/**
 * @brief Advances the tests' linear congruential generator and draws a value.
 * @param seed The generator state, advanced by the draw.
 * @return The new state's bits from the sixteenth up.
 */
unsigned next_draw(unsigned& seed)
{
    seed = seed * lcg_multiplier + lcg_increment;

    return seed >> 16U;
}

/**
 * @brief Builds a deterministic pseudo-random modal grammar over a three-symbol alphabet.
 *
 * Hand-picked grammars share the assumptions of the driver they were written with; random ones do not, which is what
 * makes them worth running: the alphabet is kept tiny so inputs collide with the grammar often enough to exercise mode
 * changes rather than failing at the first byte.
 * @param seed The generator state, advanced by every draw.
 * @return The compiled mode lexer.
 */
Mode_lexer random_mode_grammar(unsigned& seed)
{
    const auto modes{2 + next_draw(seed) % 3};

    Mode_builder builder{};

    for (std::size_t mode{0}; mode < modes; ++mode)
    {
        // Every mode gets one single-byte token that changes the mode. Without it most random grammars fail at the
        // first byte, streams stay one or two tokens long, and the driver under test never sees a mode change.
        const auto symbol{static_cast<char>('a' + mode % 3)};

        const auto kind{mode % 2 == 0 ? Mode_action_kind::push : Mode_action_kind::pop};

        builder.add_token(
                mode, text(symbol), std::size_t{0}, 1, Mode_action{.kind = kind, .target = (mode + 1) % modes});

        const auto tokens{2 + next_draw(seed) % 3};

        for (std::size_t token{1}; token < tokens + 1; ++token)
        {
            constexpr std::array<std::string_view, 7> atoms{"a", "b", "c", "ab", "bc", "ca", "abc"};

            const auto pattern{text(atoms[next_draw(seed) % atoms.size()])};

            Mode_action action{};

            switch (next_draw(seed) % 5)
            {
            case 0:
                action = {.kind = Mode_action_kind::push, .target = next_draw(seed) % modes};
                break;

            case 1:
                action = {.kind = Mode_action_kind::pop};
                break;

            case 2:
                action = {.kind = Mode_action_kind::go_to, .target = next_draw(seed) % modes};
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
 * @brief Drives a mode lexer one tokenize() call per token, stopping where no token or a zero-width one matches.
 * @tparam T The token type the lexer reports.
 * @param lexer The mode lexer.
 * @param input The input to scan.
 * @param stack The mode stack the scan starts from and leaves where it stopped.
 * @return The token stream and the bytes consumed up to where the scan stopped.
 */
template <typename T = Token_kind>
Scanned<T> drive_per_token(const Mode_lexer& lexer, const std::string& input, Mode_stack& stack)
{
    Scanned<T> scanned{};

    while (scanned.consumed < input.size())
    {
        const auto mode{stack.current};

        const auto resume{input.cbegin() + static_cast<std::ptrdiff_t>(scanned.consumed)};

        const auto [token, length]{lexer.tokenize<T>(resume, input.cend(), stack)};

        if (!token || length == 0)
        {
            break;
        }

        scanned.stream.emplace_back(*token, length, mode);

        scanned.consumed += length;
    }

    return scanned;
}

/**
 * @brief Whether tokenize() over an iterator pair with a caller's stack accepts the iterator type.
 *
 * One viability probe per public Mode_lexer overload, for the same reason the flat lexer's suite keeps one per
 * overload: a combined requires-expression proves only that at least one call rejects a type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Mode_single_through = requires(const Mode_lexer& lexer, const Iterator& iterator, Mode_stack& stack) {
    lexer.template tokenize<int>(iterator, iterator, stack);
};

/**
 * @brief Whether tokenize_all() over an iterator pair accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Mode_full_through = requires(const Mode_lexer& lexer, const Iterator& iterator) {
    lexer.template tokenize_all<int>(iterator, iterator, ignore_token);
};

/**
 * @brief Whether tokenize_all() over an iterator pair with a caller's stack accepts the iterator type.
 * @tparam Iterator The iterator type probed.
 */
template <typename Iterator>
concept Mode_full_through_with_stack = requires(const Mode_lexer& lexer, const Iterator& iterator, Mode_stack& stack) {
    lexer.template tokenize_all<int>(iterator, iterator, ignore_token, stack);
};

/**
 * @brief Whether tokenize_all() over a container accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Mode_full_over = requires(const Mode_lexer& lexer, const Container& container) {
    lexer.template tokenize_all<int>(container, ignore_token);
};

/**
 * @brief Whether tokenize_all() over a container with a caller's stack accepts the container type.
 * @tparam Container The container type probed.
 */
template <typename Container>
concept Mode_full_over_with_stack = requires(const Mode_lexer& lexer, const Container& container, Mode_stack& stack) {
    lexer.template tokenize_all<int>(container, ignore_token, stack);
};

} // namespace

TEST(Mode_test, Every_entry_point_holds_the_byte_domain_and_scans_std_byte_like_char)
{
    using Good_iterator = std::string_view::iterator;
    using Bad_iterator = std::vector<double>::const_iterator;

    static_assert(Mode_single_through<Good_iterator> && !Mode_single_through<Bad_iterator>);
    static_assert(Mode_full_through<Good_iterator> && !Mode_full_through<Bad_iterator>);
    static_assert(Mode_full_through_with_stack<Good_iterator> && !Mode_full_through_with_stack<Bad_iterator>);
    static_assert(Mode_full_over<std::string> && !Mode_full_over<std::vector<double>>);
    static_assert(!Mode_full_over<std::vector<std::string>>);
    static_assert(Mode_full_over_with_stack<std::string> && !Mode_full_over_with_stack<std::vector<double>>);

    // Viability never instantiates a body, so std::byte runs through the modal scan as well, and the answers must match
    // the same input spelled as char.
    const auto lexer{build()};

    const std::string text{R"(ab "x\"y" /* c /* d */ e */ f)"};

    std::vector<std::byte> bytes{};

    const auto as_byte{[](const char symbol) { return static_cast<std::byte>(symbol); }};

    std::ranges::transform(text, std::back_inserter(bytes), as_byte);

    const auto [expected, consumed]{scan(lexer, text)};

    ASSERT_EQ(consumed, text.size());

    Stream_t from_bytes{};

    const auto bytes_consumed{lexer.tokenize_all<Token_kind>(bytes, collect_into(from_bytes))};

    EXPECT_EQ(consumed, bytes_consumed);

    EXPECT_EQ(from_bytes, expected);

    Stream_t through_iterators{};

    const auto iterators_consumed{
            lexer.tokenize_all<Token_kind>(bytes.begin(), bytes.end(), collect_into(through_iterators))};

    EXPECT_EQ(consumed, iterators_consumed);

    EXPECT_EQ(through_iterators, expected);
}

TEST(Mode_test, Same_byte_means_different_tokens_in_different_modes)
{
    const auto lexer{build()};

    const auto [stream, consumed]{scan(lexer, R"(ab "cd" ef)")};

    EXPECT_EQ(consumed, 10U);

    const Stream_t expected{{Token_kind::identifier, 2, 0}, {Token_kind::space, 1, 0}, {Token_kind::quote, 1, 0},
                            {Token_kind::text, 2, 1},       {Token_kind::quote, 1, 1}, {Token_kind::space, 1, 0},
                            {Token_kind::identifier, 2, 0}};

    EXPECT_EQ(stream, expected);
}

TEST(Mode_test, Escapes_keep_the_string_open)
{
    const auto lexer{build()};

    // The escaped quote must not terminate: a flat grammar has no way to say so.
    const auto [stream, consumed]{scan(lexer, R"("a\"b" c)")};

    EXPECT_EQ(consumed, 8U);

    ASSERT_EQ(stream.size(), 7U);

    EXPECT_EQ(token_of(stream[2]), Token_kind::escape);

    EXPECT_EQ(token_of(stream[4]), Token_kind::quote);

    // The final identifier is back in code mode, so the string really closed.
    EXPECT_EQ(mode_of(stream.back()), 0U);
}

TEST(Mode_test, Comments_nest_through_the_stack)
{
    const auto lexer{build()};

    const auto [stream, consumed]{scan(lexer, "a /* x /* y */ z */ b")};

    EXPECT_EQ(consumed, 21U);

    // Only the outermost close returns to code mode; the inner one drops a level and stays inside.
    EXPECT_EQ(mode_of(stream.back()), 0U);

    const auto closes{std::ranges::count(stream, Token_kind::comment_close, token_of)};

    EXPECT_EQ(closes, 2);
}

TEST(Mode_test, Unbalanced_close_stops_the_scan)
{
    const auto lexer{build()};

    // Started in string mode with nothing saved, so the closing quote's pop finds no frame: the text before it is
    // delivered, the scan stops at the quote with a short consumed length, and the stack stays where it stood.
    Mode_stack stack{};

    stack.current = std::to_underlying(Mode::string);

    Stream_t stream{};

    const std::string input{R"(a"b)"};

    const auto consumed{lexer.tokenize_all<Token_kind>(input.begin(), input.end(), collect_into(stream), stack)};

    EXPECT_EQ(consumed, 1U);

    ASSERT_EQ(stream.size(), 1U);

    EXPECT_EQ(token_of(stream.front()), Token_kind::text);

    EXPECT_EQ(stack.current, std::to_underlying(Mode::string));

    EXPECT_TRUE(stack.saved.empty());

    // The bare stack refuses the same pop.
    Mode_stack bare{};

    EXPECT_FALSE(bare.apply({.kind = Mode_action_kind::pop}));

    EXPECT_EQ(bare.current, 0U);
}

TEST(Mode_test, Unterminated_string_consumes_but_stays_in_string_mode)
{
    const auto lexer{build()};

    const auto [stream, consumed]{scan(lexer, R"("abc)")};

    EXPECT_EQ(consumed, 4U);

    EXPECT_EQ(mode_of(stream.back()), std::to_underlying(Mode::string));
}

TEST(Mode_test, A_skipped_mode_index_is_rejected)
{
    Mode_builder builder{};

    builder.add_token(std::size_t{0}, text("a"), Token_kind::identifier, 1);

    builder.add_token(std::size_t{2}, text("b"), Token_kind::identifier, 1);

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode_test, An_empty_grammar_is_rejected)
{
    const Mode_builder builder{};

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode_test, Per_mode_lexers_remain_inspectable)
{
    const auto lexer{build()};

    EXPECT_EQ(lexer.modes(), 3U);

    // A single mode still certifies its own split points. That answer is sound only for input known to be scanned
    // entirely in that mode, which is why Mode_lexer exposes no parallel entry point of its own.
    EXPECT_TRUE(lexer.mode(std::to_underlying(Mode::code)).is_split_point(' '));

    EXPECT_FALSE(lexer.mode(std::to_underlying(Mode::string)).is_split_point(' '));
}

TEST(Mode_test, The_two_drivers_agree_on_the_token_stream)
{
    // tokenize_all() stays inside one mode's batch scan until an action fires and reads each action from the matched
    // token's payload, where the per-token entry point searches for it; the two paths are pinned to the same stream.
    const auto lexer{build()};

    std::string input{};

    while (input.size() < (1U << 16))
    {
        input += R"(ab "cd ef" gh /* x */ ij )";
    }

    const auto [batch, batch_consumed]{scan(lexer, input)};

    Mode_stack stack{};

    const auto [per_token, walked]{drive_per_token(lexer, input, stack)};

    EXPECT_FALSE(batch.empty());

    EXPECT_EQ(batch, per_token);
}

TEST(Mode_test, A_stopped_scan_reports_the_mode_and_depth_it_stopped_in)
{
    const auto lexer{build()};

    // An unterminated string and an unrecognized byte in code both stop the scan. Only the mode tells them apart, which
    // is the whole reason the stack is exposed.
    Mode_stack unterminated{};

    // Scans an unterminated string held in a std::string, so the literal's NUL terminator is not part of the input.
    const auto in_string{lexer.tokenize_all<Token_kind>(std::string{R"("abc)"}, ignore_token, unterminated)};

    EXPECT_EQ(in_string, 4U);

    EXPECT_EQ(unterminated.current, std::to_underlying(Mode::string));

    EXPECT_EQ(unterminated.saved.size(), 1U);

    Mode_stack nested{};

    const auto in_comment{lexer.tokenize_all<Token_kind>(std::string{"a /* x /* y"}, ignore_token, nested)};

    EXPECT_EQ(in_comment, 11U);

    EXPECT_EQ(nested.current, std::to_underlying(Mode::comment));

    // Two opens, no closes, so the depth names how many comments are still open.
    EXPECT_EQ(nested.saved.size(), 2U);
}

TEST(Mode_test, Diagnose_reports_faults_only_a_modal_grammar_can_have)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, text("a"), Token_kind::identifier, 1);
    builder.add_token(
            Mode::code, text(R"(")"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    // string can be entered and left; comment can be entered by nothing and left by nothing.
    builder.add_token(Mode::string, text(R"(")"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, text("z"), Token_kind::comment_text, 1);

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    ASSERT_EQ(per_mode.size(), 3U);

    EXPECT_EQ(unreachable_modes, std::vector<std::size_t>{std::to_underlying(Mode::comment)});

    // code never leaves itself except by pushing, which counts; comment has no exit at all.
    EXPECT_EQ(inescapable_modes, std::vector<std::size_t>{std::to_underlying(Mode::comment)});
}

TEST(Mode_test, Diagnose_is_quiet_on_a_sound_grammar)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, text("a"), Token_kind::identifier, 1);
    builder.add_token(
            Mode::code, text(R"(")"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});
    builder.add_token(Mode::string, text(R"(")"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    EXPECT_TRUE(unreachable_modes.empty());

    EXPECT_TRUE(inescapable_modes.empty());
}

TEST(Mode_test, The_two_drivers_agree_on_random_grammars_and_inputs)
{
    unsigned seed{20260803};

    std::size_t checked{0};

    std::size_t with_switches{0};

    for (std::size_t round{0}; round < rounds; ++round)
    {
        const auto lexer{random_mode_grammar(seed)};

        std::string input{};

        for (auto length{8 + next_draw(seed) % 60}; input.size() < length;)
        {
            input += static_cast<char>('a' + next_draw(seed) % 3);
        }

        Stream_t batched{};

        Mode_stack batch_stack{};

        const auto batch_consumed{lexer.tokenize_all<Token_kind>(input, collect_into(batched), batch_stack)};

        Mode_stack single_stack{};

        const auto [single, single_consumed]{drive_per_token(lexer, input, single_stack)};

        const auto label{std::format(R"(round {}, input "{}")", round, input)};

        ASSERT_EQ(batch_consumed, single_consumed) << label;

        ASSERT_EQ(batched, single) << label;

        ASSERT_EQ(batch_stack, single_stack) << label;

        ++checked;

        const auto first_switch{std::ranges::adjacent_find(batched, std::ranges::not_equal_to{}, mode_of)};

        if (first_switch != batched.end())
        {
            ++with_switches;
        }
    }

    EXPECT_EQ(checked, rounds);

    // Measured at 82 of 300. A run where few grammars changed mode would prove little about a driver whose entire
    // purpose is handling changes.
    EXPECT_GT(with_switches, 60U) << "the random grammars stopped exercising mode changes";
}

TEST(Mode_test, An_action_targeting_a_mode_that_does_not_exist_is_rejected)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, text("x"), Token_kind::identifier, 1, {.kind = Mode_action_kind::go_to, .target = 7});

    // Mode 7 has no lexer, and build() refuses the target before any scan can index the per-mode lexer vector with it.
    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode_test, A_target_may_name_a_mode_registered_later)
{
    Mode_builder builder{};

    // Forward references are legitimate: the target is checked at build(), once every mode is known.
    builder.add_token(
            Mode::code, text(R"(")"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    builder.add_token(Mode::string, text(R"(")"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});

    EXPECT_NO_THROW(std::ignore = builder.build());
}

TEST(Mode_test, Two_conflicting_actions_for_one_token_are_rejected)
{
    Mode_builder builder{};

    builder.add_token(
            Mode::code, text("x"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    // The scanner reports the token ID, not which pattern matched, so it could not choose between these, and keeping
    // either one would drop the other's action silently.
    EXPECT_THROW(
            builder.add_token(Mode::code, text("y"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop}),
            std::invalid_argument);
}

TEST(Mode_test, Two_patterns_may_share_a_token_when_the_action_agrees)
{
    Mode_builder builder{};

    // Sharing an ID is fine; only a conflicting action is not.
    builder.add_token(Mode::code, text("x"), Token_kind::identifier, 1);

    EXPECT_NO_THROW(builder.add_token(Mode::code, text("y"), Token_kind::identifier, 1));

    builder.add_token(
            Mode::code, text("p"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    EXPECT_NO_THROW(builder.add_token(
            Mode::code, text("q"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)}));

    builder.add_token(Mode::string, text("z"), Token_kind::text, 1);

    EXPECT_NO_THROW(std::ignore = builder.build());
}

TEST(Mode_test, A_caller_supplied_stack_naming_a_missing_mode_is_rejected)
{
    const auto lexer{build()};

    const std::string input{"ab"};

    Mode_stack bad{};

    bad.current = 9;

    EXPECT_THROW(std::ignore = lexer.tokenize<Token_kind>(input.cbegin(), input.cend(), bad), std::out_of_range);

    // The batch path refuses the out-of-range mode before its no-actions branch indexes with it.
    EXPECT_THROW(std::ignore = lexer.tokenize_all<Token_kind>(input, ignore_token, bad), std::out_of_range);

    // A saved frame is checked when a pop is about to expose it rather than on entry, so a scan that never pops runs to
    // completion beside one, and the same scan with a closing quote is rejected.
    Mode_stack poisoned{};

    poisoned.saved.push_back(9);

    EXPECT_NO_THROW(std::ignore = lexer.tokenize_all<Token_kind>(input, ignore_token, poisoned));

    Mode_stack popping{};

    popping.current = std::to_underlying(Mode::string);

    popping.saved.push_back(9);

    const std::string closing{R"(a")"};

    EXPECT_THROW(std::ignore = lexer.tokenize_all<Token_kind>(closing, ignore_token, popping), std::out_of_range);

    Mode_stack per_token{};

    per_token.current = std::to_underlying(Mode::string);

    per_token.saved.push_back(9);

    EXPECT_THROW(
            std::ignore = lexer.tokenize<Token_kind>(closing.cbegin() + 1, closing.cend(), per_token),
            std::out_of_range);
}

TEST(Mode_test, A_pop_escapes_only_a_mode_something_pushes_into)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 2);

    // Entered by go_to, so nothing ever saves a frame for its pop to return to. The pop can fire only on a frame the
    // caller supplied, which the grammar does not establish, so the mode is inescapable by this grammar alone.
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Token_kind::quote, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::string)});

    builder.add_token(Mode::string, any_of(Set::all()), Token_kind::text, 2);
    builder.add_token(Mode::string, any_of(Set{'"'}), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    EXPECT_EQ(inescapable_modes, std::vector<std::size_t>{std::to_underlying(Mode::string)});
}

TEST(Mode_test, A_go_to_carries_the_frame_that_a_later_pop_returns_to)
{
    Mode_builder builder{};

    // 0 pushes into 1, 1 goes to 2, 2 pops. The scan succeeds, because go_to keeps the frame the push left, so mode 2
    // escapes even though nothing pushes into it directly.
    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 2);
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    builder.add_token(
            Mode::string, any_of(Set{'#'}), Token_kind::escape, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::comment)});
    builder.add_token(Mode::string, any_of(Set::all()), Token_kind::text, 2);

    builder.add_token(Mode::comment, any_of(Set{'!'}), Token_kind::comment_close, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::comment, any_of(Set::all()), Token_kind::comment_text, 2);

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    EXPECT_TRUE(inescapable_modes.empty());
}

TEST(Mode_test, A_self_push_does_not_make_a_pop_an_escape)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 2);
    builder.add_token(
            Mode::code, any_of(Set{'"'}), Token_kind::quote, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::string)});

    // Entered by go_to, so every frame the pop can ever expose comes from the mode's own self-push and names the mode
    // itself: each pop returns exactly where it started, and no scan from mode 0 ever leaves again.
    builder.add_token(
            Mode::string, any_of(Set{'('}), Token_kind::escape, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});
    builder.add_token(Mode::string, any_of(Set{')'}), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});
    builder.add_token(Mode::string, plus(any_of(Set::alpha())), Token_kind::text, 2);

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    EXPECT_EQ(inescapable_modes, std::vector<std::size_t>{std::to_underlying(Mode::string)});
}

TEST(Mode_test, A_dead_token_grants_neither_reachability_nor_escape)
{
    Mode_builder builder{};

    // The identifier fully shadows the push at every input, so the push can never fire: diagnose() must not let it
    // enter its target or count as leaving, or the report describes a grammar the scanner does not run.
    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 1);
    builder.add_token(
            Mode::code, plus(any_of(Set::alpha())), Token_kind::text, 2,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});
    builder.add_token(Mode::string, any_of(Set{'"'}), Token_kind::quote, 1, {.kind = Mode_action_kind::pop});

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    EXPECT_TRUE(std::ranges::contains(per_mode[0].dead_tokens, std::to_underlying(Token_kind::text)));
    EXPECT_EQ(unreachable_modes, std::vector<std::size_t>{std::to_underlying(Mode::string)});

    const std::vector<std::size_t> both{std::to_underlying(Mode::code), std::to_underlying(Mode::string)};

    EXPECT_EQ(inescapable_modes, both);
}

TEST(Mode_test, A_pop_with_nothing_saved_is_refused_before_the_sink_in_the_batch_driver)
{
    const auto lexer{build()};

    // Seeded directly into the string mode with nothing saved, so the closing quote's pop must refuse. The batch scan
    // counts a stopping token as consumed, so the driver has to hold that length back and fire no sink.
    Mode_stack stack{.current = std::to_underlying(Mode::string)};

    const std::string input{R"(")"};

    std::size_t calls{0};

    const auto count_call{[&calls](const Token_kind, const std::size_t, const std::size_t) { ++calls; }};

    const auto consumed{lexer.tokenize_all<Token_kind>(input.begin(), input.end(), count_call, stack)};

    EXPECT_EQ(consumed, 0U);
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(stack.current, std::to_underlying(Mode::string));
    EXPECT_TRUE(stack.saved.empty());
}

TEST(Mode_test, A_go_to_onto_its_own_mode_is_a_stay)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, plus(any_of(Set::alpha())), Token_kind::identifier, 2);

    // Observably a stay: the mode does not change, so both drivers must emit what a stay would.
    builder.add_token(
            Mode::code, any_of(Set{' '}), Token_kind::space, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::code)});

    const auto lexer{builder.build()};

    const std::string input{"ab cd ef"};

    Stream_t batch{};

    Mode_stack stack{};

    const auto consumed{lexer.tokenize_all<Token_kind>(input, collect_into(batch), stack)};

    EXPECT_EQ(consumed, input.size());

    EXPECT_EQ(stack.current, std::to_underlying(Mode::code));

    EXPECT_TRUE(stack.saved.empty());

    Mode_stack walking{};

    const auto [per_token, walked]{drive_per_token(lexer, input, walking)};

    ASSERT_EQ(walked, input.size());

    EXPECT_EQ(batch, per_token);

    EXPECT_EQ(walking.current, stack.current);
}

TEST(Mode_test, The_winning_nullable_token_carrying_an_action_is_rejected)
{
    Mode_builder builder{};

    // Matches the empty string, so the batch driver would stop without reporting it while the per-token driver returns
    // it with length zero. An action neither applies is not something a caller can rely on.
    builder.add_token(
            Mode::code, kleene(any_of(Set{'a'})), Token_kind::identifier, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::string)});
    builder.add_token(Mode::string, any_of(Set::all()), Token_kind::text, 2);

    EXPECT_THROW(std::ignore = builder.build(), std::invalid_argument);
}

TEST(Mode_test, A_nullable_token_without_an_action_still_builds)
{
    Mode_builder builder{};

    // Legal, and the two drivers still report it differently: only the action is refused, not the pattern.
    builder.add_token(Mode::code, kleene(any_of(Set{'a'})), Token_kind::identifier, 1);

    const auto lexer{builder.build()};

    const std::string input{"b"};

    Mode_stack per_token{};

    const auto [token, length]{lexer.tokenize<Token_kind>(input.cbegin(), input.cend(), per_token)};

    EXPECT_EQ(token, Token_kind::identifier);

    EXPECT_EQ(length, 0U);

    Mode_stack batch{};

    const auto consumed{lexer.tokenize_all<Token_kind>(input, ignore_token, batch)};

    EXPECT_EQ(consumed, 0U);

    EXPECT_EQ(per_token.current, batch.current);
}

TEST(Mode_test, An_action_kind_outside_the_enumeration_is_rejected)
{
    Mode_builder builder{};

    // add_token() refuses a kind outside the enumeration, so both drivers only ever see the kinds they agree on.
    EXPECT_THROW(
            builder.add_token(
                    Mode::code, text("x"), Token_kind::identifier, 1, {.kind = static_cast<Mode_action_kind>(999)}),
            std::invalid_argument);
}

TEST(Mode_test, A_negative_or_unrepresentable_id_is_rejected)
{
    Mode_builder builder{};

    // A signed -1 is refused for being negative, before the conversion that would turn it into the unsigned maximum.
    EXPECT_THROW(builder.add_token(std::size_t{0}, text("x"), -1, 1), std::invalid_argument);

    EXPECT_THROW(builder.add_token(-1, text("y"), std::size_t{0}, 1), std::invalid_argument);

    // The unsigned maximum is not negative, and it is refused because the row it sizes, id + 1, does not fit in a
    // std::size_t.
    constexpr auto unrepresentable{std::numeric_limits<std::size_t>::max()};

    EXPECT_THROW(builder.add_token(std::size_t{0}, text("z"), unrepresentable, 1), std::invalid_argument);

    EXPECT_THROW(builder.add_token(unrepresentable, text("w"), std::size_t{0}, 1), std::invalid_argument);
}

TEST(Mode_test, A_rejected_registration_leaves_the_grammar_untouched)
{
    Mode_builder builder{};

    builder.add_token(
            Mode::code, text("x"), Token_kind::quote, 1,
            {.kind = Mode_action_kind::push, .target = std::to_underlying(Mode::string)});

    EXPECT_THROW(
            builder.add_token(Mode::code, text("y"), Token_kind::quote, 1, {.kind = Mode_action_kind::pop}),
            std::invalid_argument);

    builder.add_token(Mode::string, text("z"), Token_kind::text, 1, {.kind = Mode_action_kind::pop});

    const auto lexer{builder.build()};

    // The rejected pattern is not registered, so "y" matches nothing in code mode.
    Mode_stack stack{};

    const std::string input{"y"};

    const auto [token, length]{lexer.tokenize<Token_kind>(input.cbegin(), input.cend(), stack)};

    EXPECT_FALSE(token) << "the rejected pattern was registered after all";
}

TEST(Mode_test, A_target_is_ignored_where_the_kind_does_not_use_one)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, text("x"), Token_kind::identifier, 1, {.kind = Mode_action_kind::stay, .target = 3});

    // stay and pop document target as ignored, so these agree and must not read as a conflict.
    EXPECT_NO_THROW(builder.add_token(
            Mode::code, text("y"), Token_kind::identifier, 1, {.kind = Mode_action_kind::stay, .target = 7}));
}

TEST(Mode_test, Reachability_is_a_walk_from_mode_zero_not_merely_being_named)
{
    Mode_builder builder{};

    builder.add_token(Mode::code, text("a"), Token_kind::identifier, 1);

    // comment is named only by string, and nothing reaches string from code, so the walk from mode 0 reports both as
    // unreachable, comment although string names it.
    builder.add_token(
            Mode::string, text("b"), Token_kind::text, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::comment)});

    builder.add_token(Mode::comment, text("c"), Token_kind::comment_text, 1);

    const auto [per_mode, unreachable_modes, inescapable_modes]{builder.diagnose()};

    const std::vector<std::size_t> expected{std::to_underlying(Mode::string), std::to_underlying(Mode::comment)};

    EXPECT_EQ(unreachable_modes, expected);
}

TEST(Mode_test, Far_apart_token_ids_match_and_carry_their_actions)
{
    Mode_builder builder{};

    // The builder and the runtime both keep lists of what was registered rather than rows indexed by token value, so an
    // id far from the others matches and acts as a small one does.
    builder.add_token(Mode::code, text("a"), std::size_t{0}, 1);

    builder.add_token(
            Mode::code, text("b"), std::size_t{70000}, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::string)});

    builder.add_token(
            Mode::string, text("c"), std::size_t{70001}, 1,
            {.kind = Mode_action_kind::go_to, .target = std::to_underlying(Mode::code)});

    const auto lexer{builder.build()};

    std::size_t tokens{0};

    const std::string input{"abcabc"};

    const auto count_token{[&tokens](const std::size_t, const std::size_t, const std::size_t) { ++tokens; }};

    const auto consumed{lexer.tokenize_all<std::size_t>(input, count_token)};

    EXPECT_EQ(consumed, input.size());

    EXPECT_EQ(tokens, 6U);
}

TEST(Mode_test, The_two_drivers_agree_for_action_counts_to_three_at_dense_and_sparse_token_ids)
{
    // The batch driver reads each action from the matched token's payload and the per-token one searches for it, so
    // they are separate code needing to agree whatever a mode's action count. The input drives every go_to letter once,
    // returns through y each time, and ends on z in mode 0, so both drivers consume it whole. Zero actions is the count
    // that leaves mode 0 with nothing to act on, which is the batch driver's separate no-action pass.

    const auto check{[](const std::size_t actions, const std::size_t base) {
        Mode_builder builder{};

        builder.add_token(std::size_t{0}, text("z"), base, 2);

        for (std::size_t extra{0}; extra < actions; ++extra)
        {
            const auto symbol{static_cast<char>('a' + extra)};

            builder.add_token(
                    std::size_t{0}, text(symbol), base + extra + 1, 1, {.kind = Mode_action_kind::go_to, .target = 1});
        }

        builder.add_token(std::size_t{1}, text("y"), base + 90, 1, {.kind = Mode_action_kind::go_to, .target = 0});

        const auto lexer{builder.build()};

        std::string input{};

        std::vector<std::pair<std::size_t, std::size_t>> expected{};

        for (std::size_t extra{0}; extra < actions; ++extra)
        {
            input += static_cast<char>('a' + extra);

            input += 'y';

            expected.emplace_back(base + extra + 1, std::size_t{0});

            expected.emplace_back(base + 90, std::size_t{1});
        }

        input += 'z';

        expected.emplace_back(base, std::size_t{0});

        std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> batch_stream{};

        Mode_stack batch_stack{};

        const auto batch_consumed{
                lexer.tokenize_all<std::size_t>(input.cbegin(), input.cend(), collect_into(batch_stream), batch_stack)};

        Mode_stack stack{};

        const auto [walked, at]{drive_per_token<std::size_t>(lexer, input, stack)};

        const auto token_and_mode{[](const std::tuple<std::size_t, std::size_t, std::size_t>& entry) {
            const auto& [token, length, mode]{entry};

            return std::pair{token, mode};
        }};

        auto batch_view{batch_stream | std::views::transform(token_and_mode)};

        const std::vector<std::pair<std::size_t, std::size_t>> batch{batch_view.begin(), batch_view.end()};

        auto single_view{walked | std::views::transform(token_and_mode)};

        const std::vector<std::pair<std::size_t, std::size_t>> single{single_view.begin(), single_view.end()};

        const auto label{std::format("actions={} base={}", actions, base)};

        EXPECT_EQ(batch_consumed, input.size()) << label;

        EXPECT_EQ(at, input.size()) << label;

        EXPECT_EQ(batch, expected) << label;

        EXPECT_EQ(single, expected) << label;

        EXPECT_EQ(batch_stack.current, 0U) << label;

        EXPECT_EQ(stack.current, 0U) << label;

        EXPECT_TRUE(batch_stack.saved.empty() && stack.saved.empty()) << label;
    }};

    for (const std::size_t actions : {0U, 1U, 2U, 3U})
    {
        for (const std::size_t base : {std::size_t{0}, std::size_t{100}})
        {
            check(actions, base);
        }
    }
}

TEST(Mode_test, Unpack_inverts_pack_on_every_stored_kind_and_only_a_stay_packs_to_zero)
{
    // The three kinds the drivers store, each at target zero, at a small target and at one wider than the two kind bits
    // leave in a 32-bit word, so the round trip is pinned at the width the channel carries; a stay packs to zero
    // whatever target it names, which is why no stored action can be mistaken for one.
    for (const auto kind : {Mode_action_kind::go_to, Mode_action_kind::push, Mode_action_kind::pop})
    {
        for (const std::size_t target : {std::size_t{0}, std::size_t{5}, std::size_t{1} << 40U})
        {
            const Mode_action action{.kind = kind, .target = target};

            const auto packed{pack(action)};

            EXPECT_NE(packed, 0U);

            const auto [unpacked_kind, unpacked_target]{unpack(packed)};

            EXPECT_EQ(unpacked_kind, kind);
            EXPECT_EQ(unpacked_target, target);
        }
    }

    EXPECT_EQ(pack(Mode_action{.kind = Mode_action_kind::stay, .target = 0}), 0U);
    EXPECT_EQ(pack(Mode_action{.kind = Mode_action_kind::stay, .target = 5}), 0U);
}

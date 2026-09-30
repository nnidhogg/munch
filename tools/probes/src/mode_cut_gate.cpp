// Asserts the measurement behind the single-byte mode-cut obstruction.
//
// For a cut at a byte to need no context reconstruction, this route asks three things to hold at once. They are
// sufficient for a context-free cut, not necessary: two modes emitting the same tokens onward would also admit one
// without either being identified, so what follows is an obstruction rather than an impossibility.
//
//   1. the byte begins a token no matter which mode the scan is in, or the worker cannot know it is between tokens;
//   2. the byte determines the mode, since knowing you are between tokens is useless without knowing which token set
//      applies;
//   3. the byte determines the mode stack, which no byte can do once nesting is unbounded.
//
// Condition 1 is satisfiable and eleven bytes satisfy it here. Condition 2 is not: every byte begins a token in at
// least two modes, because string, character and comment bodies each admit the whole alphabet, so no byte rules any
// of them out. That closes this route for single-byte cuts in any grammar with two such modes. It does not close
// every route, and a product-state argument over what the modes emit onward is what would settle the general case.
//
// The counts below are asserted, not only printed: they are the measurement behind docs/limits.md's statement that
// Mode_lexer exposes no parallel entry point, and a moved count fails the probe, as the certificate figures under
// paper/figures are compared rather than displayed.

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "munch/core/mode_builder.hpp"
#include "munch/regex/regex.hpp"
#include "munch/regex/set.hpp"

namespace
{
using namespace munch;
using namespace munch::core;
using namespace munch::regex;

enum class Mode : std::size_t
{
    code,
    string,
    character,
    line_comment,
    block_comment
};

enum class Token : std::size_t
{
    whitespace,
    identifier,
    number,
    op,
    punctuation,
    quote,
    apostrophe,
    escape,
    content,
    open_line,
    open_block,
    close_block,
    newline
};

/**
 * @brief One mode's name and the exact certificate size the report publishes for it.
 */
struct Expectation
{
    /**
     * @brief The mode's name as the report prints it.
     */
    std::string_view name;

    /**
     * @brief The number of bytes the report publishes in the mode's exact certificate.
     */
    std::size_t certified;
};

constexpr std::array kModes{
        Expectation{.name = "code", .certified = 13}, Expectation{.name = "string", .certified = 249},
        Expectation{.name = "character", .certified = 249}, Expectation{.name = "line_comment", .certified = 256},
        Expectation{.name = "block_comment", .certified = 255}};

constexpr std::size_t kCertifiedEverywhere{11};

constexpr std::size_t kStartableInExactlyOneMode{0};

constexpr std::size_t kStartableInEveryMode{79};

/**
 * @brief Builds a five-mode C-like grammar (code, string, character, line comment, block comment) in the
 *        split-friendliest formulation available to it.
 *
 * Content is matched one byte at a time rather than as a run, since a run token de-certifies its own bytes, and
 * escapes name their permitted characters instead of admitting any byte. This is the formulation most favourable to
 * finding a safe cut.
 *
 * @return The five-mode lexer.
 */
Mode_lexer build()
{
    Mode_builder builder{};

    const auto push{[](const Mode mode) {
        return Mode_action{.kind = Mode_action_kind::push, .target = static_cast<std::size_t>(mode)};
    }};

    constexpr Mode_action pop{.kind = Mode_action_kind::pop};

    const auto escapes{any_of(Set{'n', 't', 'r', '\\', '"', '\'', '0'})};

    builder.add_token(Mode::code, plus(any_of(Set{' ', '\t', '\n'})), Token::whitespace, 2);
    builder.add_token(
            Mode::code, concat(any_of(Set::alpha() + '_'), kleene(any_of(Set::alphanum() + '_'))), Token::identifier,
            2);
    builder.add_token(Mode::code, plus(any_of(Set::digits())), Token::number, 2);
    builder.add_token(
            Mode::code,
            choice(text("=="), text("!="), text("<="), text(">="), text("+"), text("-"), text("="), text("<"),
                   text(">")),
            Token::op, 2);
    builder.add_token(Mode::code, any_of(Set{'(', ')', '{', '}', ';', ','}), Token::punctuation, 2);
    builder.add_token(Mode::code, text("\""), Token::quote, 1, push(Mode::string));
    builder.add_token(Mode::code, text("'"), Token::apostrophe, 1, push(Mode::character));
    builder.add_token(Mode::code, text("//"), Token::open_line, 1, push(Mode::line_comment));
    builder.add_token(Mode::code, text("/*"), Token::open_block, 1, push(Mode::block_comment));

    builder.add_token(Mode::string, text("\""), Token::quote, 1, pop);
    builder.add_token(Mode::string, concat(text("\\"), escapes), Token::escape, 1);
    builder.add_token(Mode::string, any_of(Set::all() - '"' - '\\'), Token::content, 2);

    builder.add_token(Mode::character, text("'"), Token::apostrophe, 1, pop);
    builder.add_token(Mode::character, concat(text("\\"), escapes), Token::escape, 1);
    builder.add_token(Mode::character, any_of(Set::all() - '\'' - '\\'), Token::content, 2);

    builder.add_token(Mode::line_comment, text("\n"), Token::newline, 1, pop);
    builder.add_token(Mode::line_comment, any_of(Set::all() - '\n'), Token::content, 2);

    builder.add_token(Mode::block_comment, text("*/"), Token::close_block, 1, pop);
    builder.add_token(Mode::block_comment, any_of(Set::all()), Token::content, 2);

    return builder.build();
}

/**
 * @brief Prints a count against what the report publishes, marking a disagreement.
 * @param what The count's label, padded to 34 columns.
 * @param measured The count the shipped predicates give.
 * @param published The count the report publishes.
 * @return True when the two counts agree.
 */
bool agrees(const std::string_view what, const std::size_t measured, const std::size_t published)
{
    const auto ok{measured == published};

    std::printf("  %-34s %3zu%s\n", std::string{what}.c_str(), measured, ok ? "" : "  <- REPORT SAYS OTHERWISE");

    return ok;
}

/**
 * @brief Counts the bytes in one mode's exact certificate.
 * @param lexer The mode lexer.
 * @param mode The mode's index, below lexer.modes().
 * @return The number of bytes, of 256, that are split points in the mode.
 */
std::size_t certified_in(const Mode_lexer& lexer, const std::size_t mode)
{
    std::size_t certified{0};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        certified += lexer.mode(mode).is_split_point(static_cast<char>(symbol)) ? 1 : 0;
    }

    return certified;
}

/**
 * @brief The per-byte census of the three conditions over every mode.
 */
struct Census
{
    /**
     * @brief The bytes certified in every mode.
     */
    std::size_t everywhere{0};

    /**
     * @brief The bytes that begin a one-byte token in exactly one mode.
     */
    std::size_t unique_starter{0};

    /**
     * @brief The bytes that begin a one-byte token in every mode.
     */
    std::size_t universal_starter{0};
};

/**
 * @brief Counts, over all 256 bytes, those certified in every mode and those beginning a one-byte token in exactly one
 *        mode and in every mode.
 * @param lexer The mode lexer.
 * @return The three counts.
 */
Census census_of(const Mode_lexer& lexer)
{
    const auto modes{lexer.modes()};

    Census census{};

    for (int symbol{0}; symbol < 256; ++symbol)
    {
        const std::string one(1, static_cast<char>(symbol));

        std::size_t certifying_modes{0};

        std::size_t starting_modes{0};

        for (std::size_t mode{0}; mode < modes; ++mode)
        {
            certifying_modes += lexer.mode(mode).is_split_point(static_cast<char>(symbol)) ? 1 : 0;

            starting_modes += lexer.mode(mode).tokenize<std::size_t>(one.begin(), one.end()).token ? 1 : 0;
        }

        census.everywhere += certifying_modes == modes ? 1 : 0;

        census.unique_starter += starting_modes == 1 ? 1 : 0;

        census.universal_starter += starting_modes == modes ? 1 : 0;
    }

    return census;
}
} // namespace

/**
 * @brief Prints each mode's exact certificate and the census of the three conditions against the published figures.
 * @return 0 when every figure agrees, 1 otherwise.
 */
int main()
{
    const auto lexer{build()};

    const auto modes{lexer.modes()};

    auto ok{modes == kModes.size()};

    std::printf("per-mode exact certificates, of 256\n");

    for (std::size_t mode{0}; mode < modes && mode < kModes.size(); ++mode)
    {
        ok = agrees(kModes[mode].name, certified_in(lexer, mode), kModes[mode].certified) && ok;
    }

    const auto census{census_of(lexer)};

    std::printf("\nthe three conditions\n");

    ok = agrees("certified in every mode", census.everywhere, kCertifiedEverywhere) && ok;

    ok = agrees("begins a token in exactly one mode", census.unique_starter, kStartableInExactlyOneMode) && ok;

    ok = agrees("begins a token in every mode", census.universal_starter, kStartableInEveryMode) && ok;

    std::printf(
            "\n%s\n", ok ? "Condition 1 holds and condition 2 cannot: no byte determines the mode, so this route to "
                           "a context-free single-byte cut is closed. Identifying the mode is sufficient, not "
                           "necessary, so other routes are not ruled out." :
                           "A published number moved. Re-derive the report before trusting it.");

    return ok ? 0 : 1;
}

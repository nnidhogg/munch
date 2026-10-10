#include "munch/tools/audit/flex_pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief What opens a POSIX class inside a bracket, `[:alpha:]`.
 */
constexpr std::string_view class_opener{"[:"};

/**
 * @brief What opens a negated POSIX class, `[:^alpha:]`.
 */
constexpr std::string_view negated_class_opener{"[:^"};

/**
 * @brief What closes a POSIX class.
 */
constexpr std::string_view class_closer{":]"};

/**
 * @brief What opens a flag group, `(?i:...)`.
 */
constexpr std::string_view group_opener{"(?"};

/**
 * @brief What opens a hex escape, `\x41`, whose digits follow it.
 */
constexpr std::string_view hex_escape_opener{R"(\x)"};

/**
 * @brief The escaped letter flex reads as itself and the pattern parser does not, `\u{61}` being a code point to the
 *        parser: the letter alone is the same byte to both.
 */
constexpr char code_point_escape{'u'};

/**
 * @brief The base an octal escape writes its number in.
 */
constexpr int octal_base{8};

/**
 * @brief Where the scanner of a rule's pattern stands, which decides what a `]` is and what ends the pattern.
 */
enum class Pattern_at : std::uint8_t
{
    /**
     * @brief Outside a quote and a bracket, where a blank ends the pattern and the action begins.
     */
    pattern,

    /**
     * @brief Inside a `"..."` literal, where a blank is a byte of it.
     */
    quoted,

    /**
     * @brief Just past a bracket's `[`, where a `]` is a member and a `^` negates the bracket.
     */
    opened,

    /**
     * @brief Just past a bracket's `[^`, where a `]` is still a member and a further `^` is one.
     */
    negated,

    /**
     * @brief Inside a bracket past its first member, where a `]` closes it.
     */
    bracket
};

/**
 * @brief Returns the length of the POSIX class standing at an index inside a bracket: `[:`, an optional negating `^`,
 *        one or more letters and `:]`, which is the only shape flex lexes as a class, its CCL_EXPR.
 *
 * A `[:` of any other shape leaves the `[` an ordinary member, which is how flex reads `[[:al]pha:]` as the bracket
 * `[[:al]` and the text `pha:]` after it, and `[[:alpha]]` as the bracket `[[:alpha]` and a literal `]`.
 * @param line The rule line.
 * @param at The index of the `[`.
 * @return The class's length, or zero when no class stands there.
 */
[[nodiscard]] std::size_t class_length(const std::string_view line, const std::size_t at) noexcept
{
    const auto rest{line.substr(at)};

    if (!rest.starts_with(class_opener))
    {
        return 0;
    }

    auto past{at + (rest.starts_with(negated_class_opener) ? negated_class_opener.size() : class_opener.size())};

    const auto name{past};

    while (past < line.size() && is_letter(static_cast<unsigned char>(line[past])))
    {
        ++past;
    }

    return past > name && line.substr(past).starts_with(class_closer) ? past + class_closer.size() - at : 0;
}

/**
 * @brief Returns where the scanner of a pattern stands after the byte at an index, as flex lexes a pattern: an escape
 *        carries the byte after it and uses up a bracket's first position; a quote opens text only outside a bracket,
 *        where inside one it is a member, and the text runs to the next quote; a `[` outside opens a bracket, a `^`
 *        just past its `[` negates it, and a `]` past its first member closes it; a `[:class:]` inside one is one token
 *        whose `]` is no close.
 * @param state Where the scanner stands before the byte.
 * @param text The pattern or rule line.
 * @param at The index of the byte; left at the last byte the step takes, the escaped byte or the class's `]`.
 * @return Where the scanner stands after it.
 */
[[nodiscard]] Pattern_at next_pattern_at(const Pattern_at state, const std::string_view text, std::size_t& at) noexcept
{
    const auto byte{text[at]};

    if (byte == '\\')
    {
        ++at;

        return state == Pattern_at::opened || state == Pattern_at::negated ? Pattern_at::bracket : state;
    }

    if (state == Pattern_at::quoted)
    {
        return byte == '"' ? Pattern_at::pattern : state;
    }

    if (state == Pattern_at::pattern)
    {
        if (byte == '"')
        {
            return Pattern_at::quoted;
        }

        return byte == '[' ? Pattern_at::opened : state;
    }

    if (byte == ']' && state == Pattern_at::bracket)
    {
        return Pattern_at::pattern;
    }

    if (byte == '^' && state == Pattern_at::opened)
    {
        return Pattern_at::negated;
    }

    if (const auto length{class_length(text, at)}; length > 0)
    {
        at += length - 1;
    }

    return Pattern_at::bracket;
}

/**
 * @brief Returns whether a byte is a digit in the base an escape writes its number in.
 * @param byte The byte.
 * @param base hex_base or octal_base.
 * @return True for a hex digit in the hex base, and for 0 to 7 in the octal base.
 */
[[nodiscard]] constexpr bool is_digit_in(const char byte, const int base) noexcept
{
    return base == hex_base ? is_hex_digit(byte) : is_octal_digit(byte);
}

/**
 * @brief Returns a byte spelled as a hex escape, which is text whatever the file's encoding.
 * @param byte The byte.
 * @return `\x` and the byte's two hex digits, in capitals.
 */
[[nodiscard]] std::string hex_spelling(const unsigned char byte)
{
    return std::format(R"(\x{:02X})", byte);
}

} // namespace

std::size_t pattern_length(const std::string_view line, const std::size_t number)
{
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < line.size(); ++at)
    {
        if (state == Pattern_at::pattern && (line[at] == ' ' || line[at] == '\t'))
        {
            return at;
        }

        state = next_pattern_at(state, line, at);
    }

    if (state != Pattern_at::pattern)
    {
        const auto left_open{
                state == Pattern_at::quoted ? "a quote is left open in the pattern" :
                                              "a bracket is left open in the pattern"};

        throw Spec_error{left_open, number};
    }

    return line.size();
}

std::optional<std::string> negated_class(const std::string_view pattern)
{
    // A quote opens text only outside a bracket, where inside one it is a member, `[^"[:^print:]]` holding the quote
    // and then the class.
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        const auto negating{
                state != Pattern_at::pattern && state != Pattern_at::quoted &&
                pattern.substr(at).starts_with(negated_class_opener)};

        if (const auto length{negating ? class_length(pattern, at) : 0UZ}; length > 0)
        {
            return std::string{pattern.substr(at, length)};
        }

        state = next_pattern_at(state, pattern, at);
    }

    return std::nullopt;
}

std::string negated_class_refusal(const std::string_view negated)
{
    return std::format(
            "holds {}, which flex fills under the locale it runs under, dropping the bytes that locale counts in the "
            "class beside the ASCII ones, which the file does not decide",
            negated);
}

std::optional<std::string> folding_group(const std::string_view pattern)
{
    // Flags apply in order, a `-` turning off every flag after it, so `(?i-i:` and `(?-ii:` fold nothing.
    const auto folding_opening{[pattern](const std::size_t at) -> std::optional<std::string> {
        auto on{true};

        auto folds{false};

        for (auto flag{at + group_opener.size()}; flag < pattern.size(); ++flag)
        {
            const auto letter{pattern[flag]};

            if (letter == ':' && !folds)
            {
                return std::nullopt;
            }

            if (letter == ':')
            {
                return std::string{pattern.substr(at, flag - at + 1)};
            }

            if (letter == '-')
            {
                on = false;

                continue;
            }

            if (letter == 'i')
            {
                folds = on;

                continue;
            }

            if (letter != 's' && letter != 'x' && letter != 'r')
            {
                return std::nullopt;
            }
        }

        return std::nullopt;
    }};

    // A group opens outside a quote and a bracket alone: `"(?i:"` is text and `[(?i:]` members, as flex lexes them.
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        if (state != Pattern_at::pattern || !pattern.substr(at).starts_with(group_opener))
        {
            state = next_pattern_at(state, pattern, at);

            continue;
        }

        if (auto opening{folding_opening(at)})
        {
            return opening;
        }
    }

    return std::nullopt;
}

std::optional<std::string> beyond_ascii(const std::string_view pattern)
{
    // The escape as written when the byte it names is beyond ASCII; `at` moves to its last digit.
    const auto numbered{
            [pattern](std::size_t& at, const std::size_t first, const int base) -> std::optional<std::string> {
                auto past{first};

                // The digits stand up to the third byte after the backslash, `\x41` or `\101`.
                static constexpr std::size_t longest_escape{4};

                while (past < pattern.size() && past < at + longest_escape && is_digit_in(pattern[past], base))
                {
                    ++past;
                }

                const auto digits{pattern.substr(first, past - first)};

                const auto escape{pattern.substr(at, past - at)};

                at = past - 1;

                if (digits.empty())
                {
                    return std::nullopt;
                }

                const auto value{std::stoul(std::string{digits}, nullptr, base)};

                if (value > last_ascii)
                {
                    return std::string{escape};
                }

                return std::nullopt;
            }};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        // A raw byte is spelled in hex, so that the refusal is text whatever the file's encoding.
        if (const auto raw{static_cast<unsigned char>(pattern[at])}; raw > last_ascii)
        {
            return hex_spelling(raw);
        }

        if (pattern[at] != '\\' || at + 1 >= pattern.size())
        {
            continue;
        }

        // flex takes `\x` with one or two hex digits and an octal escape of one to three digits.
        if (pattern.substr(at).starts_with(hex_escape_opener))
        {
            if (auto escape{numbered(at, at + hex_escape_opener.size(), hex_base)})
            {
                return escape;
            }
        }
        else if (is_digit_in(pattern[at + 1], octal_base))
        {
            if (auto escape{numbered(at, at + 1, octal_base)})
            {
                return escape;
            }
        }
        else
        {
            // Any other escape carries the byte after it as itself, a raw byte beyond ASCII included.
            ++at;

            if (const auto escaped{static_cast<unsigned char>(pattern[at])}; escaped > last_ascii)
            {
                return hex_spelling(escaped);
            }
        }
    }

    return std::nullopt;
}

std::string expression_of(const std::string_view pattern)
{
    std::string expression{};

    expression.reserve(pattern.size());

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        const auto escape{pattern[at] == '\\' && at + 1 < pattern.size()};

        // The escape and the byte it carries are copied as one, so that an escaped backslash escapes nothing after it.
        if (escape && pattern[at + 1] != code_point_escape)
        {
            expression.push_back(pattern[at]);

            ++at;
        }
        else if (escape)
        {
            ++at;
        }

        expression.push_back(pattern[at]);
    }

    return expression;
}

} // namespace munch::tools::audit

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
// Implements flex_pattern.hpp: the states of a pattern the scans walk, the step between them and the length of a POSIX
// class inside a bracket are private to this unit.

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
 * @brief The length of the POSIX class standing at an index inside a bracket: `[:`, an optional negating `^`, one or
 *        more letters and `:]`, which is the only shape flex lexes as a class, its CCL_EXPR.
 *
 * A `[:` of any other shape leaves the `[` an ordinary member, which is how flex reads `[[:al]pha:]` as the bracket
 * `[[:al]` and the text `pha:]` after it, and `[[:alpha]]` as the bracket `[[:alpha]` and a literal `]`.
 * @param line The rule line.
 * @param at The index of the `[`.
 * @return The class's length, or zero when no class stands there.
 */
[[nodiscard]] std::size_t class_length(const std::string_view line, const std::size_t at) noexcept
{
    if (!line.substr(at).starts_with("[:"))
    {
        return 0;
    }

    auto past{at + (line.substr(at).starts_with("[:^") ? 3U : 2U)};

    const auto name{past};

    while (past < line.size() && is_letter(static_cast<unsigned char>(line[past])))
    {
        ++past;
    }

    return past > name && line.substr(past).starts_with(":]") ? past + 2 - at : 0;
}

/**
 * @brief Where the scanner of a pattern stands after the byte at an index, as flex lexes a pattern: an escape carries
 *        the byte after it and uses up a bracket's first position; a quote opens text only outside a bracket, where
 *        inside one it is a member, and the text runs to the next quote; a `[` outside opens a bracket, a `^` just past
 *        its `[` negates it, and a `]` past its first member closes it; a `[:class:]` inside one is one token whose `]`
 *        is no close.
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
        return byte == '"' ? Pattern_at::quoted : byte == '[' ? Pattern_at::opened : state;
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
        throw Spec_error{
                state == Pattern_at::quoted ? "a quote is left open in the pattern" :
                                              "a bracket is left open in the pattern",
                number};
    }

    return line.size();
}

std::optional<std::string> negated_class(const std::string_view pattern) noexcept
{
    // A quote opens text only outside a bracket, where inside one it is a member, `[^"[:^print:]]` holding the quote
    // and then the class.
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        const auto negating{
                state != Pattern_at::pattern && state != Pattern_at::quoted && pattern.substr(at).starts_with("[:^")};

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
    return "holds " + std::string{negated} +
           ", which flex fills under the locale it runs under, dropping the bytes that locale counts in the class "
           "beside the ASCII ones, which the file does not decide";
}

std::optional<std::string> folding_group(const std::string_view pattern) noexcept
{
    // A group opens outside a quote and a bracket alone: `"(?i:"` is text and `[(?i:]` members, as flex lexes them.
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        if (state != Pattern_at::pattern || !pattern.substr(at).starts_with("(?"))
        {
            state = next_pattern_at(state, pattern, at);

            continue;
        }

        // The flags apply in order, as flex applies them, a `-` turning off every flag after it: `(?i-i:` and `(?-ii:`
        // fold nothing, flex 2.6.4 taking neither over the other case.
        auto on{true};

        auto folds{false};

        for (auto flag{at + 2}; flag < pattern.size(); ++flag)
        {
            if (pattern[flag] == ':')
            {
                if (folds)
                {
                    return std::string{pattern.substr(at, flag - at + 1)};
                }

                break;
            }

            if (pattern[flag] == '-')
            {
                on = false;
            }
            else if (pattern[flag] == 'i')
            {
                folds = on;
            }
            else if (pattern[flag] != 's' && pattern[flag] != 'x' && pattern[flag] != 'r')
            {
                break;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> beyond_ascii(const std::string_view pattern) noexcept
{
    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        // A raw byte is spelled in hex, so that the refusal is text whatever the file's encoding.
        if (static_cast<unsigned char>(pattern[at]) >= 0x80)
        {
            return std::format("\\x{:02X}", static_cast<unsigned char>(pattern[at]));
        }

        if (pattern[at] != '\\' || at + 1 >= pattern.size())
        {
            continue;
        }

        // flex takes `\x` with one or two hex digits and an octal escape of one to three digits.
        if (pattern[at + 1] == 'x')
        {
            std::size_t past{at + 2};

            while (past < pattern.size() && past < at + 4 && is_hex_digit(pattern[past]))
            {
                ++past;
            }

            if (past > at + 2 && std::stoul(std::string{pattern.substr(at + 2, past - at - 2)}, nullptr, 16) >= 0x80)
            {
                return std::string{pattern.substr(at, past - at)};
            }

            at = past - 1;
        }
        else if (pattern[at + 1] >= '0' && pattern[at + 1] <= '7')
        {
            std::size_t past{at + 1};

            while (past < pattern.size() && past < at + 4 && pattern[past] >= '0' && pattern[past] <= '7')
            {
                ++past;
            }

            if (std::stoul(std::string{pattern.substr(at + 1, past - at - 1)}, nullptr, 8) >= 0x80)
            {
                return std::string{pattern.substr(at, past - at)};
            }

            at = past - 1;
        }
        else
        {
            // Any other escape carries the byte after it as itself, a raw byte beyond ASCII included.
            ++at;

            if (static_cast<unsigned char>(pattern[at]) >= 0x80)
            {
                return std::format("\\x{:02X}", static_cast<unsigned char>(pattern[at]));
            }
        }
    }

    return std::nullopt;
}

} // namespace munch::tools::audit

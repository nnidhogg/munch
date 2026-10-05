#include "munch/tools/audit/c_tokens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The member access through a pointer, the one punctuator of two bytes a token is read as.
 */
constexpr std::string_view arrow{"->"};

/**
 * @brief How many bytes the marker of a universal character name takes, `\u` or `\U`, before its hexadecimal digits.
 */
constexpr std::size_t universal_marker_length{2};

/**
 * @brief What a byte of a stretch of C stands in as spliced() reads it: code, a string or character literal, a line
 *        comment or a block comment.
 *
 * A raw string opens in code alone, so the `R` of `"R"` or of a comment's text opens none, and its content is what it
 * says, splices and all: the language exempts it from the joining of lines. Everything else is joined at a splice, a
 * literal, a comment and code alike, as the compiler joins every line before it reads them.
 */
enum class Inside : std::uint8_t
{
    /**
     * @brief Outside every literal and comment.
     */
    code,

    /**
     * @brief Inside a string or character literal, from its opening quote through its closing one.
     */
    literal,

    /**
     * @brief Inside a `//` comment, up to the newline ending it.
     */
    line_comment,

    /**
     * @brief Inside a block comment, from its slash-star through its star-slash.
     */
    block_comment
};

/**
 * @brief Returns the index just past the raw string literal opening at an index, when one does: an `R` after an
 *        optional `u8`, `u`, `U` or `L`, then `"`, a delimiter of up to sixteen bytes, `(`, the body, `)`, the same
 *        delimiter and `"`, as C++ reads one. Flex's C++ scanners hold C++ actions, so the word `R` followed by a quote
 *        is the prefix and not a name of the file's own; one left open runs to the end.
 * @param code The stretch of C.
 * @param at The index, of a byte that is no whitespace.
 * @return The index past the literal, or std::nullopt when none opens here.
 */
[[nodiscard]] std::optional<std::size_t> raw_string_end(const std::string_view code, const std::size_t at)
{
    // A raw string's prefix is a word of its own, so one standing after a word's byte opens none.
    if (at > 0 && is_word_byte(code[at - 1]))
    {
        return std::nullopt;
    }

    const auto rest{code.substr(at)};

    static constexpr std::string_view marker{"R\""};

    const auto opens{[rest](const std::string_view encoding) {
        return rest.starts_with(std::format("{}{}", encoding, marker));
    }};

    static constexpr std::array<std::string_view, 5> encodings{"u8", "u", "U", "L", ""};

    const auto encoding{std::ranges::find_if(encodings, opens)};

    if (encoding == encodings.end())
    {
        return std::nullopt;
    }

    const auto prefix{encoding->size() + marker.size()};

    const auto open{code.find('(', at + prefix)};

    if (open == std::string_view::npos)
    {
        return code.size();
    }

    const auto delimiter{code.substr(at + prefix, open - at - prefix)};

    const auto closer{std::format("){}\"", delimiter)};

    const auto close{code.find(closer, open + 1)};

    return close == std::string_view::npos ? code.size() : close + closer.size();
}

/**
 * @brief Returns whether a universal character name, `\u` or `\U` and its hexadecimal digits, opens at an index; it is
 *        part of the identifier around it, so that `return\u03B1` is a name of the file's own and no `return`.
 * @param code The stretch of C.
 * @param at The index.
 * @return True when one opens here.
 */
[[nodiscard]] bool at_universal_name(const std::string_view code, const std::size_t at) noexcept
{
    return at + 1 < code.size() && code[at] == '\\' && (code[at + 1] == 'u' || code[at + 1] == 'U');
}

/**
 * @brief Returns the index just past the universal character name opening at an index: its `\u` or `\U` and the
 *        hexadecimal digits after it.
 * @param code The stretch of C.
 * @param at The index, where at_universal_name() holds.
 * @return The index past the name.
 */
[[nodiscard]] std::size_t past_universal_name(const std::string_view code, const std::size_t at) noexcept
{
    auto end{at + universal_marker_length};

    while (end < code.size() && is_hex_digit(code[end]))
    {
        ++end;
    }

    return end;
}

/**
 * @brief Returns the index past the number that begins at an index, which runs as the preprocessor's pp-number does:
 *        digits, letters, dots, a digit separator's apostrophe before a digit or a letter, and a sign after an
 *        exponent's letter, so `1'000` is one token and its apostrophe opens no character literal, and `0'0'0` is the
 *        one index 0.
 * @param code The stretch of C.
 * @param at The index of the number's first byte, a digit or a dot before one.
 * @return The index past the number.
 */
[[nodiscard]] std::size_t number_end(const std::string_view code, const std::size_t at)
{
    auto end{at + 1};

    while (end < code.size())
    {
        const auto byte{code[end]};

        const auto next{end + 1 < code.size() ? code[end + 1] : '\0'};

        const auto separator{byte == '\'' && std::isalnum(static_cast<unsigned char>(next)) != 0};

        const auto exponent{std::string_view{"eEpP"}.contains(byte) && (next == '+' || next == '-')};

        if (exponent)
        {
            end += 2;

            continue;
        }

        if (is_word_byte(byte) || byte == '.' || separator)
        {
            end += separator ? 2 : 1;

            continue;
        }

        break;
    }

    return end;
}

/**
 * @brief Returns the index just past the token or the comment opening at an index of a stretch of C, as c_tokens()
 *        reads them: past a literal's closing quote, an escaped byte carried, past a comment's end, past the last byte
 *        of a word, and past the one or two bytes of a punctuator; a literal or a comment left open runs to the end.
 * @param code The stretch of C.
 * @param at The index, of a byte that is no whitespace.
 * @return The index to resume at.
 */
[[nodiscard]] std::size_t token_end(const std::string_view code, const std::size_t at)
{
    const auto rest{code.substr(at)};

    if (const auto raw{raw_string_end(code, at)})
    {
        return *raw;
    }

    if (rest.starts_with(line_comment_opener))
    {
        const auto newline{code.find('\n', at)};

        return std::min(newline, code.size());
    }

    if (rest.starts_with(comment_opener))
    {
        const auto close{code.find(comment_closer, at + comment_opener.size())};

        return close == std::string_view::npos ? code.size() : close + comment_closer.size();
    }

    if (rest.front() == '"' || rest.front() == '\'')
    {
        auto close{at + 1};

        while (close < code.size() && code[close] != rest.front())
        {
            close += code[close] == '\\' ? 2 : 1;
        }

        return std::min(close + 1, code.size());
    }

    if (is_digit(rest.front()) || (rest.front() == '.' && rest.size() > 1 && is_digit(rest[1])))
    {
        return number_end(code, at);
    }

    if (is_word_byte(rest.front()) || at_universal_name(code, at))
    {
        auto end{at};

        while (end < code.size() && (is_word_byte(code[end]) || at_universal_name(code, end)))
        {
            end = at_universal_name(code, end) ? past_universal_name(code, end) : end + 1;
        }

        return end;
    }

    return at + (rest.starts_with(arrow) ? arrow.size() : 1);
}

/**
 * @brief Returns the tokens of a text as token_end() reads them, whitespace and comments left out, each placed by its
 *        offsets in that text.
 * @param text The text.
 * @return The tokens in order.
 */
[[nodiscard]] std::vector<C_token> tokens_in(const std::string_view text)
{
    std::vector<C_token> tokens{};

    for (std::size_t at{0}; at < text.size();)
    {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0)
        {
            ++at;

            continue;
        }

        const auto end{token_end(text, at)};

        const auto rest{text.substr(at)};

        const auto comment{rest.starts_with(line_comment_opener) || rest.starts_with(comment_opener)};

        if (!comment)
        {
            tokens.push_back({.at = at, .end = end, .text = std::string{text.substr(at, end - at)}});
        }

        at = end;
    }

    return tokens;
}

/**
 * @brief Returns what the byte after the one at hand stands in, given what the byte at hand does there: code opens a
 *        literal at a quote and a comment at two slashes or at a slash before a star; a literal ends at its quote, or
 *        at the line's end when it is left open; a line comment ends at the newline and a block comment at its
 *        star-slash.
 * @param inside What the byte at hand stands in.
 * @param rest The stretch from the byte at hand on, not empty.
 * @param quote The quote the open literal began with, when one is open.
 * @return What the next byte stands in.
 */
[[nodiscard]] Inside next_inside(const Inside inside, const std::string_view rest, const char quote) noexcept
{
    switch (inside)
    {
    case Inside::code:
        if (rest.front() == '"' || rest.front() == '\'')
        {
            return Inside::literal;
        }

        if (rest.starts_with(line_comment_opener))
        {
            return Inside::line_comment;
        }

        return rest.starts_with(comment_opener) ? Inside::block_comment : Inside::code;

    case Inside::literal:
        return rest.front() == quote || rest.front() == '\n' ? Inside::code : Inside::literal;

    case Inside::line_comment:
        return rest.front() == '\n' ? Inside::code : Inside::line_comment;

    case Inside::block_comment:
        return rest.starts_with(comment_closer) ? Inside::code : Inside::block_comment;
    }

    return inside;
}

/**
 * @brief Returns the newline a backslash at an index joins its line to the next at, as gcc joins lines: blanks may
 *        stand between the backslash and the line's end, as gcc allows them with a warning, and a carriage return
 *        before the newline.
 *
 * The code a scanner carries is compiled by the compiler and not by the standard, so a splice is read the way the
 * compiler reads it: `ret\` with a space after it, then `urn 7;` on the next line, is one `return`.
 * @param code The stretch of C.
 * @param at The index of the backslash.
 * @return The index of the newline, or std::nullopt when the backslash joins no line.
 */
[[nodiscard]] std::optional<std::size_t> spliced_newline(const std::string_view code, const std::size_t at)
{
    const auto past_blanks{code.find_first_not_of(" \t", at + 1)};

    auto newline{std::min(past_blanks, code.size())};

    newline += newline < code.size() && code[newline] == '\r' ? 1 : 0;

    return newline < code.size() && code[newline] == '\n' ? std::optional{newline} : std::nullopt;
}

/**
 * @brief Returns where an ordinary literal opening at an index of a directive ends, as the preprocessor reads it: its
 *        escapes carry their byte and its splices join, the lines joined before any escape is read, so a backslash
 *        before a backslash ending the line escapes the next line's first byte; one left open ends with the line.
 * @param code The stretch of C the directive stands in.
 * @param scan The index of the literal's opening quote.
 * @return The index of its closing quote, of the newline ending the line it is left open on, or the stretch's size.
 */
[[nodiscard]] std::size_t literal_end_spliced(const std::string_view code, std::size_t scan)
{
    const auto quote{code[scan]};

    for (++scan; scan < code.size() && code[scan] != quote; ++scan)
    {
        if (code[scan] == '\n')
        {
            return scan;
        }

        if (code[scan] != '\\')
        {
            continue;
        }

        if (const auto past{spliced_newline(code, scan)})
        {
            scan = *past;

            continue;
        }

        ++scan;

        const auto joined{scan < code.size() && code[scan] == '\\' ? spliced_newline(code, scan) : std::nullopt};

        if (joined)
        {
            scan = *joined + 1;
        }
    }

    return scan;
}

} // namespace

std::vector<C_token> c_tokens(const std::string_view code)
{
    const auto [text, place]{spliced(code)};

    auto tokens{tokens_in(text)};

    for (auto& [begin, end, spelled] : tokens)
    {
        begin = place[begin];

        end = place[end - 1] + 1;
    }

    return tokens;
}

std::vector<C_token> java_tokens(const std::string_view code)
{
    std::string text{code};

    std::ranges::replace(text, '\r', '\n');

    return tokens_in(text);
}

std::optional<std::size_t> brace_close(const std::string_view code)
{
    int depth{0};

    for (const auto& [at, end, text] : c_tokens(code))
    {
        const auto step{depth_step(text, "{", "}")};

        depth += step;

        if (step < 0 && depth == 0)
        {
            return end;
        }
    }

    return std::nullopt;
}

Spliced spliced(const std::string_view code)
{
    Spliced result{};

    const auto copy{[&result, code](const std::size_t at) {
        result.text.push_back(code[at]);

        result.place.push_back(at);
    }};

    auto inside{Inside::code};

    char quote{};

    for (std::size_t at{0}; at < code.size(); ++at)
    {
        if (const auto raw{inside == Inside::code ? raw_string_end(code, at) : std::nullopt})
        {
            for (; at < *raw; ++at)
            {
                copy(at);
            }

            --at;

            continue;
        }

        if (const auto newline{code[at] == '\\' ? spliced_newline(code, at) : std::nullopt})
        {
            at = *newline;

            continue;
        }

        // An escape inside a literal carries its byte, so an escaped quote closes nothing; the byte it carries is the
        // one after any splice, since the compiler joins the lines before it reads an escape, and so a backslash before
        // a backslash ending the line, `"x\\` and a newline, escapes the next line's first byte.
        if (code[at] == '\\' && inside == Inside::literal && at + 1 < code.size())
        {
            copy(at);

            const auto joined{code[at + 1] == '\\' ? spliced_newline(code, at + 1) : std::nullopt};

            const auto escaped{joined ? *joined + 1 : at + 1};

            if (escaped < code.size())
            {
                copy(escaped);
            }

            at = escaped;

            continue;
        }

        const auto next{next_inside(inside, code.substr(at), quote)};

        if (inside == Inside::code && next == Inside::literal)
        {
            quote = code[at];
        }

        // A comment's opening and closing are two bytes each, taken together so that the star of `/*/` closes nothing.
        if ((inside == Inside::code && next == Inside::block_comment) ||
            (inside == Inside::block_comment && next == Inside::code))
        {
            copy(at);

            ++at;
        }

        inside = next;

        copy(at);
    }

    return result;
}

std::size_t directive_end(const std::string_view code, const std::size_t from)
{
    // Whether the scan stands in a line comment, which runs with the directive to its end.
    auto commented{false};

    for (auto scan{from}; scan < code.size(); ++scan)
    {
        // A raw string's newlines are its own and end no directive, and neither do a block comment's, which is one
        // blank to the preprocessor however many lines it spans.
        if (const auto raw{commented ? std::nullopt : raw_string_end(code, scan)})
        {
            scan = *raw - 1;

            continue;
        }

        if (!commented && code.substr(scan).starts_with(comment_opener))
        {
            const auto close{code.find(comment_closer, scan + comment_opener.size())};

            scan = close == std::string_view::npos ? code.size() : close + comment_closer.size() - 1;

            continue;
        }

        // A line comment runs to the line's end, a splice carrying it on with the directive; nothing inside it opens a
        // literal, a comment or a raw string.
        if (!commented && code.substr(scan).starts_with(line_comment_opener))
        {
            commented = true;

            ++scan;

            continue;
        }

        // The `R` inside `"R"` opens no raw string.
        if (!commented && (code[scan] == '"' || code[scan] == '\''))
        {
            scan = literal_end_spliced(code, scan);

            if (scan < code.size() && code[scan] == '\n')
            {
                return scan;
            }

            continue;
        }

        if (const auto past{code[scan] == '\\' ? spliced_newline(code, scan) : std::nullopt})
        {
            scan = *past;

            continue;
        }

        if (code[scan] == '\n')
        {
            return scan;
        }
    }

    return code.size();
}

} // namespace munch::tools::audit

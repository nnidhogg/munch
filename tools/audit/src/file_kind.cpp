#include "munch/tools/audit/file_kind.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/antlr_cursor.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements file_kind.hpp: the scans for a grammar declaration, a derive naming Logos and a re2c opener, and what
// hides one, are private to this unit.

/**
 * @brief The bytes a blank is, between the pieces of a grammar declaration or an attribute.
 */
constexpr std::string_view blanks{" \t\r\n"};

/**
 * @brief Where a block comment opening at an index ends: at its star-slash, Rust's nesting read so that an inner
 *        slash-star takes another star-slash to close.
 * @param source The file's text.
 * @param at The index of the comment's slash-star.
 * @param rust Whether the text is read as Rust, whose block comments nest.
 * @return The index just past the close, or the text's size when the comment never closes.
 */
[[nodiscard]] std::size_t block_comment_end(const std::string_view source, const std::size_t at, const bool rust)
{
    auto depth{1};

    for (auto scan{at + 2}; scan + 1 < source.size();)
    {
        if (rust && source.substr(scan).starts_with("/*"))
        {
            ++depth;

            scan += 2;

            continue;
        }

        if (!source.substr(scan).starts_with("*/"))
        {
            ++scan;

            continue;
        }

        --depth;

        scan += 2;

        if (depth == 0)
        {
            return scan;
        }
    }

    return source.size();
}

/**
 * @brief Where a character literal opening at an index ends: at its closing quote, an escaped byte carried. Rust's
 *        `'` opens a literal only when one byte or an escape closes it, since `'static` is a lifetime and not a
 *        literal that swallows the text after it.
 * @param source The file's text.
 * @param at The index of the literal's quote.
 * @param rust Whether the text is read as Rust.
 * @return The index just past the literal, the index past the quote for a Rust lifetime, or the text's size when the
 *         literal never closes.
 */
[[nodiscard]] std::size_t character_end(const std::string_view source, const std::size_t at, const bool rust)
{
    const auto rest{source.substr(at)};

    const auto shut{rest.starts_with("'\\") ? rest.find('\'', 3) : rest.find('\'', 2)};

    if (rust && (shut == std::string_view::npos || shut > 4))
    {
        return at + 1;
    }

    return shut == std::string_view::npos ? source.size() : at + shut + 1;
}

/**
 * @brief Where a quoted string opening at an index ends: at its closing quote, an escaped byte carried, or at the
 *        line's end, where a string left open stops hiding what follows.
 * @param source The file's text.
 * @param at The index of the string's quote.
 * @return The index just past the string, or the text's size.
 */
[[nodiscard]] std::size_t string_end(const std::string_view source, const std::size_t at)
{
    auto end{at + 1};

    for (; end < source.size() && source[end] != '"' && source[end] != '\n'; ++end)
    {
        end += source[end] == '\\' ? 1 : 0;
    }

    return std::min(end + 1, source.size());
}

/**
 * @brief Where C++'s raw string opening at an index ends, `R"d(...)d"` with an encoding prefix allowed before the
 *        `R`: at the delimiter that closes it, no escape read, so that an embedded quote closes nothing.
 *
 * C++ writes the delimiter without blanks, parentheses or a backslash, and in sixteen bytes at most, so a quote that
 * opens none of that opens no raw string.
 * @param source The file's text.
 * @param at The index of the prefix.
 * @return The index just past the string, the text's size when it never closes, or std::nullopt when no raw string
 *         opens here.
 */
[[nodiscard]] std::optional<std::size_t> cpp_raw_string_end(const std::string_view source, const std::size_t at)
{
    const auto rest{source.substr(at)};

    const auto raw{[&]() -> std::size_t {
        for (const std::string_view mark : {"u8R\"", "uR\"", "UR\"", "LR\"", "R\""})
        {
            if (rest.starts_with(mark))
            {
                return mark.size() - 1;
            }
        }

        return 0UZ;
    }()};

    if (raw == 0)
    {
        return std::nullopt;
    }

    const auto open{rest.find('(', raw + 1)};

    const auto delimiter{open == std::string_view::npos ? std::string_view{} : rest.substr(raw + 1, open - raw - 1)};

    if (open == std::string_view::npos || delimiter.size() > 16 ||
        delimiter.find_first_of(" \t\r\n()\\") != std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto shut{std::string{')'} + std::string{delimiter} + '"'};

    const auto close{source.find(shut, at + open)};

    return close == std::string_view::npos ? source.size() : close + shut.size();
}

/**
 * @brief Where Rust's raw string opening at an index ends, `r#"..."#` or the byte string `br#"..."#`: at the quote
 *        followed by as many hashes as opened it, no escape read.
 * @param source The file's text.
 * @param at The index of the prefix.
 * @return The index just past the string, the text's size when it never closes, or std::nullopt when no raw string
 *         opens here.
 */
[[nodiscard]] std::optional<std::size_t> rust_raw_string_end(const std::string_view source, const std::size_t at)
{
    const auto rest{source.substr(at)};

    const auto prefix{rest.starts_with("br") ? 2UZ : rest.starts_with('r') ? 1UZ : 0UZ};

    if (prefix == 0)
    {
        return std::nullopt;
    }

    const auto hashes{rest.find_first_not_of('#', prefix)};

    if (hashes == std::string_view::npos || rest[hashes] != '"')
    {
        return std::nullopt;
    }

    for (auto close{source.find('"', at + hashes + 1)}; close != std::string_view::npos;
         close = source.find('"', close + 1))
    {
        const auto after{source.substr(close + 1, hashes - prefix)};

        if (after.size() == hashes - prefix && after.find_first_not_of('#') == std::string_view::npos)
        {
            return close + 1 + after.size();
        }
    }

    return source.size();
}

/**
 * @brief Where a comment or a literal opening at an index ends, read as the named language reads it: a block comment
 *        at its close, a line comment at the line's end, or at a carriage return where the language ends one there, a
 *        quoted literal at its closing quote and a raw string at the delimiter that closes it.
 *
 * The scan is its own rather than c_tokens()' or Rust_cursor's: it sees the comments c_tokens() drops, a re2c opener
 * being one; it refuses no literal or comment left open, which Rust_cursor refuses; and it reads C++'s raw strings in
 * either language, Rust's in Rust.
 * @param source The file's text.
 * @param at The index.
 * @param rust Whether the text is read as Rust rather than as C.
 * @return The index just past what opens there, or std::nullopt when no comment or literal opens there.
 */
[[nodiscard]] std::optional<std::size_t> hiding_end(
        const std::string_view source, const std::size_t at, const bool rust)
{
    const auto rest{source.substr(at)};

    if (rest.starts_with("/*"))
    {
        return block_comment_end(source, at, rust);
    }

    if (rest.starts_with("//"))
    {
        return std::min(source.find_first_of(rust ? "\n" : "\n\r", at), source.size());
    }

    // A single quote or a raw string's prefix after a name byte opens nothing: `1'000` is C++'s digit separator.
    const auto after_name{at > 0 && is_name_byte(source[at - 1])};

    if (rest.starts_with('\'') && !after_name)
    {
        return character_end(source, at, rust);
    }

    if (rest.starts_with('"'))
    {
        return string_end(source, at);
    }

    if (after_name)
    {
        return std::nullopt;
    }

    if (const auto end{cpp_raw_string_end(source, at)})
    {
        return end;
    }

    return rust ? rust_raw_string_end(source, at) : std::nullopt;
}

/**
 * @brief The index past the blanks and comments after an index, the text read as Rust, for the pieces of an
 *        attribute, which Rust lets stand apart.
 * @param source The file's text.
 * @param from The index.
 * @return The index of the next byte that is neither, or the text's size.
 */
[[nodiscard]] std::size_t past(const std::string_view source, std::size_t from)
{
    for (from = std::min(source.find_first_not_of(blanks, from), source.size());
         from < source.size() && (source.substr(from).starts_with("//") || source.substr(from).starts_with("/*"));
         from = std::min(source.find_first_not_of(blanks, *hiding_end(source, from, true)), source.size()))
    {
    }

    return from;
}

/**
 * @brief Where a derive's list closes: at its own closing parenthesis, the ones inside a comment or a string of it
 *        passed over, so that the list names what Rust reads it to name.
 * @param source The file's text.
 * @param open The index the list opens at when one does.
 * @return The index of the closing parenthesis, or npos when no list opens there or it never closes.
 */
[[nodiscard]] std::size_t list_close(const std::string_view source, const std::size_t open)
{
    if (open >= source.size() || source[open] != '(')
    {
        return std::string_view::npos;
    }

    // `#[derive(/* ) */ Logos)]` names Logos: the comment's parenthesis closes nothing.
    for (auto scan{open}, depth{0UZ}; scan < source.size();)
    {
        if (const auto end{hiding_end(source, scan, true)}; end && *end > scan)
        {
            scan = *end;

            continue;
        }

        depth += source[scan] == '(' ? 1 : source[scan] == ')' ? -1 : 0;

        if (source[scan] == ')' && depth == 0)
        {
            return scan;
        }

        ++scan;
    }

    return std::string_view::npos;
}

/**
 * @brief Whether a derive's list names Logos as the last segment of one of its paths.
 * @param list The list, from its opening parenthesis.
 * @return True when it does.
 */
[[nodiscard]] bool names_logos(const std::string_view list)
{
    for (auto found{list.find("Logos")}; found != std::string_view::npos; found = list.find("Logos", found + 5))
    {
        const auto after{found + 5 < list.size() ? list[found + 5] : ' '};

        if (!is_name_byte(list[found - 1]) && !is_name_byte(after))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Whether the attribute opening at an index derives Logos: `#`, `[`, the word `derive` or the `cfg_attr` that
 *        applies one, and its list, with blanks and comments allowed between every two of them, and Logos the last
 *        segment of one of the list's paths. Whether a `cfg_attr` predicate holds is the reader's reading and not this
 *        choice of reader: a Rust file is read by the Rust reader either way, which then says what the derive it
 *        applies scans.
 * @param source The file's text.
 * @param at The index.
 * @return True when it does.
 */
[[nodiscard]] bool derives_logos_at(const std::string_view source, const std::size_t at)
{
    if (source[at] != '#')
    {
        return false;
    }

    auto open{past(source, at + 1)};

    if (open >= source.size() || source[open] != '[')
    {
        return false;
    }

    open = past(source, open + 1);

    const auto applies{source.substr(open).starts_with("cfg_attr")};

    if (!applies && !source.substr(open).starts_with("derive"))
    {
        return false;
    }

    open = past(source, open + (applies ? 8 : 6));

    const auto close{list_close(source, open)};

    const auto list{close == std::string_view::npos ? std::string_view{} : source.substr(open, close - open)};

    // A `cfg_attr` that applies no derive names no scanner, whatever else its list holds.
    return (!applies || list.contains("derive")) && names_logos(list);
}

/**
 * @brief Whether the text's first item is a grammar declaration, `grammar`, `lexer grammar` or `parser grammar`, each
 *        word followed by a blank or a comment so that `grammarx` is none, the blanks, comments and byte order marks
 *        before and between them stepped over as ANTLR's lexer steps over them.
 * @param source The file's text.
 * @return True when it is.
 */
[[nodiscard]] bool declares_grammar(const std::string_view source)
{
    Antlr_cursor cursor{source};

    const auto separated{[&cursor] {
        return (cursor.peek() && blanks.contains(*cursor.peek())) || cursor.at("//") || cursor.at("/*");
    }};

    try
    {
        cursor.skip_blanks();

        auto word{cursor.identifier()};

        if ((word == "lexer" || word == "parser") && separated())
        {
            cursor.skip_blanks();

            word = cursor.identifier();
        }

        return word == "grammar" && separated();
    }
    catch (const Spec_error&)
    {
        // A block comment left open runs to the end, and no declaration follows it.
        return false;
    }
}

/**
 * @brief Whether a derive anywhere in the text names Logos, the text read as Rust.
 * @param source The file's text.
 * @return True when one does.
 */
[[nodiscard]] bool derives_logos(const std::string_view source)
{
    for (std::size_t scan{0}; scan < source.size();)
    {
        if (const auto end{hiding_end(source, scan, true)})
        {
            scan = *end > scan ? *end : scan + 1;

            continue;
        }

        if (derives_logos_at(source, scan))
        {
            return true;
        }

        ++scan;
    }

    return false;
}

/**
 * @brief Whether the text opens a re2c block, the text read as C, which is what a re2c block lives in.
 * @param source The file's text.
 * @return True when an opener stands outside every comment and literal.
 */
[[nodiscard]] bool opens_re2c_block(const std::string_view source)
{
    for (std::size_t scan{0}; scan < source.size();)
    {
        const auto rest{source.substr(scan)};

        if (rest.starts_with("/*!re2c") || rest.starts_with("/*!rules:re2c") || rest.starts_with("/*!local:re2c") ||
            rest.starts_with("/*!use:re2c"))
        {
            return true;
        }

        const auto end{hiding_end(source, scan, false)};

        scan = end && *end > scan ? *end : scan + 1;
    }

    return false;
}

} // namespace

Kind kind_of(const std::string_view source) noexcept
{
    if (declares_grammar(source))
    {
        return Kind::antlr;
    }

    if (derives_logos(source))
    {
        return Kind::logos;
    }

    return opens_re2c_block(source) ? Kind::re2c : Kind::flex;
}

} // namespace munch::tools::audit

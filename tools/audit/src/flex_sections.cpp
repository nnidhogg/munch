#include "munch/tools/audit/flex_sections.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/flex_pattern.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief One open start-condition scope, `<s>{` through the line opening with `}`.
 */
struct Scope
{
    /**
     * @brief How many names the scope stack held before this scope opened, which closing it restores.
     */
    std::size_t names{};

    /**
     * @brief The line the scope opened on, named when the section ends with it still open.
     */
    std::size_t line{};
};

/**
 * @brief A rule line as it stands past its start-condition prefix.
 */
struct Rule_start
{
    /**
     * @brief The start conditions the prefix names, none when no prefix stands.
     */
    std::vector<std::string> conditions{};

    /**
     * @brief The rule's text past the prefix, its pattern first.
     */
    std::string_view text{};

    /**
     * @brief Where the text begins in the cursor's line, which the action's offset is counted from.
     */
    std::size_t line_at{};

    /**
     * @brief The line the rule stands on.
     */
    std::size_t number{};
};

/**
 * @brief The refusal of a quote a rule's action leaves open at the end of its line, where its braces balance.
 */
constexpr std::string_view open_literal{
        "a quote is left open at the end of the action's line, where flex ends the action inside the literal and never "
        "closes the code it emits for it, so that the m4 it runs stops with an end of file in string"};

/**
 * @brief The refusal of an action's comment left open at the end of the file.
 */
constexpr std::string_view open_comment{
        "the action's comment is never closed, which flex refuses as an end of file inside an action"};

/**
 * @brief What opens a code block, `%{`.
 */
constexpr std::string_view block_opener{"%{"};

/**
 * @brief What closes a code block, `%}`.
 */
constexpr std::string_view block_closer{"%}"};

/**
 * @brief What parts the sections, `%%` at the margin.
 */
constexpr std::string_view section_delimiter{"%%"};

/**
 * @brief How a refusal names a stretch of the definitions section's code.
 */
constexpr std::string_view definitions_code{"the definitions define"};

/**
 * @brief A line splice, a backslash before the newline.
 */
constexpr std::string_view splice{"\\\n"};

/**
 * @brief Returns whether a line is a section delimiter, as flex lexes one: `%%` at the margin, and after it anything at
 *        all, a comment, text or blanks, which flex drops with the rest of the line; an indented `%%` is no delimiter.
 * @param line The line, untrimmed.
 * @return True for a delimiter.
 */
[[nodiscard]] bool is_delimiter(const std::string_view line) noexcept
{
    return line.starts_with(section_delimiter);
}

/**
 * @brief Records a stretch of code flex copies into the scanner: the macros it defines are taken now and the hook it
 *        defines is read once every stretch is, since a helper the hook calls may be defined in a later block and the
 *        generated scanner expands the hook where it runs, under every definition the file leaves.
 * @param code The stretch.
 * @param first The line it begins on.
 * @param what Which kind of stretch it is, as a refusal names it: "the definitions define".
 * @param macros The macros the file defines, added to.
 * @param copied The stretches of copied code, the stretch appended.
 */
void copy_code(
        const std::string_view code, const std::size_t first, const std::string_view what, Macros_t& macros,
        std::vector<Copied>& copied)
{
    take_macros(code, macros);

    copied.push_back({.code = code, .first = first, .what = what, .path = {}});
}

/**
 * @brief Returns the text the cursor has moved over since it stood where a view of the rest began.
 * @param before The rest of the text as it was, from the line the cursor stood at.
 * @param lines The cursor, moved on since.
 * @return The lines between, from the first one's first byte to the current line's first.
 */
[[nodiscard]] std::string_view passed(const std::string_view before, const Lines& lines) noexcept
{
    const auto after{lines.rest()};

    return before.substr(0, before.size() - after.size());
}

/**
 * @brief Copies a block flex copies into the scanner, from the line the cursor stands at through the line its skip
 *        leaves it at.
 * @tparam Skip The type of the skip.
 * @param lines The cursor, at the block's first line; left where the skip leaves it.
 * @param skip What moves the cursor to the block's last line.
 * @param what Which kind of stretch it is, as a refusal names it.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the block appended.
 * @throws Spec_error If the skip finds the block never closed.
 */
template <std::invocable Skip>
void copy_block(
        Lines& lines, const Skip& skip, const std::string_view what, Macros_t& macros, std::vector<Copied>& copied)
{
    const auto before{lines.rest()};

    const auto first{lines.number()};

    skip();

    copy_code(passed(before, lines), first, what, macros, copied);
}

/**
 * @brief Returns where a line's text begins past its leading blanks, as trimmed() drops them.
 * @param line The line.
 * @return The offset of its first byte that is no blank, or its size when every byte is one.
 */
[[nodiscard]] std::size_t indentation(const std::string_view line) noexcept
{
    const auto first{line.find_first_not_of(line_blanks)};

    return first == std::string_view::npos ? line.size() : first;
}

/**
 * @brief Reads a `%{` code block when one opens on the line, to the line holding its `%}`.
 * @param lines The cursor, at the line; left at the block's last line.
 * @param text The line, trimmed.
 * @param what Which kind of stretch the block is, as a refusal names it.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the block appended.
 * @return Whether the line opens such a block.
 * @throws Spec_error If the block is never closed.
 */
[[nodiscard]] bool take_code_block(
        Lines& lines, const std::string_view text, const std::string_view what, Macros_t& macros,
        std::vector<Copied>& copied)
{
    if (!text.starts_with(block_opener))
    {
        return false;
    }

    const auto to_close{[&lines] { lines.skip_to(block_closer, "a %{ code block"); }};

    copy_block(lines, to_close, what, macros, copied);

    return true;
}

/**
 * @brief Reads a `%top` block when one opens on the line, to the line that is its `}`.
 * @param lines The cursor, at the line; left at the block's last line.
 * @param text The line, trimmed.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the block appended.
 * @return Whether the line opens such a block.
 * @throws Spec_error If the block is never closed.
 */
[[nodiscard]] bool take_top_block(
        Lines& lines, const std::string_view text, Macros_t& macros, std::vector<Copied>& copied)
{
    static constexpr std::string_view top{"%top"};

    if (!text.starts_with(top) || !trimmed(text.substr(top.size())).starts_with('{'))
    {
        return false;
    }

    const auto through_close{[&lines] { lines.skip_through("}", "a %top block"); }};

    copy_block(lines, through_close, definitions_code, macros, copied);

    return true;
}

/**
 * @brief Skips a comment of the definitions section when one opens on the line, to the line holding its star-slash.
 * @param lines The cursor, at the line; left at the comment's last line.
 * @param text The line, trimmed.
 * @return Whether the line opens a comment.
 */
[[nodiscard]] bool skip_definitions_comment(Lines& lines, const std::string_view text)
{
    if (!text.starts_with(comment_opener))
    {
        return false;
    }

    while (lines.more() && !lines.current().contains(comment_closer))
    {
        lines.advance();
    }

    return true;
}

/**
 * @brief Returns whether a line is code or blank rather than a declaration: it begins with a blank or is empty.
 * @param line The line.
 * @return True for an indented or empty line.
 */
[[nodiscard]] bool is_code(const std::string_view line) noexcept
{
    return line.empty() || line.front() == ' ' || line.front() == '\t' || trimmed(line).empty();
}

/**
 * @brief Returns whether a line continues on the next, its last byte a backslash, the line's own ending disregarded.
 * @param line The line, with or without a carriage return at its end.
 * @return True when the line is spliced to the next.
 */
[[nodiscard]] bool continues(std::string_view line) noexcept
{
    if (line.ends_with('\r'))
    {
        line.remove_suffix(1);
    }

    while (line.ends_with(' ') || line.ends_with('\t'))
    {
        line.remove_suffix(1);
    }

    return line.ends_with('\\');
}

/**
 * @brief Returns the line under the cursor with the lines it continues on: a directive spliced over several lines is
 *        one line to the compiler that reads it, flex copying the lines through as they stand and a backslash at a
 *        line's end joining it to the next before any of it means anything.
 * @param lines The cursor, at the line; left at the last line the code continues on.
 * @return The code, from the line's first byte through the last line's end, its newline excluded.
 */
[[nodiscard]] std::string_view continued_code(Lines& lines)
{
    const auto before{lines.rest()};

    while (continues(lines.current()) && lines.more())
    {
        lines.advance();
    }

    const auto last{lines.current()};

    return before.substr(0, passed(before, lines).size() + last.size());
}

/**
 * @brief Reads an indented or blank line of the definitions section as code, with the lines it continues on.
 * @param lines The cursor, at the line; left at the last line the code continues on.
 * @param line The line, untrimmed.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the line's appended.
 * @return Whether the line is code.
 */
[[nodiscard]] bool take_code_line(
        Lines& lines, const std::string_view line, Macros_t& macros, std::vector<Copied>& copied)
{
    if (!is_code(line))
    {
        return false;
    }

    const auto first{lines.number()};

    const auto code{continued_code(lines)};

    copy_code(code, first, definitions_code, macros, copied);

    return true;
}

/**
 * @brief Returns the words of a line after its first, split on blanks outside double quotes.
 *
 * flex lexes a quoted value on an `%option` line as one token whatever blanks it holds, and the word after the closing
 * quote as the next token whether or not a blank parts them, so the value stays in the word that names it:
 * `header-file="a caseless.h"` is one word, neither `caseless.h"` nor anything else inside the quotes is an option of
 * its own, and `header-file="a b.h"caseless` is that word and then `caseless`.
 * @param line The line.
 * @param number The line number, for the error.
 * @return The words.
 * @throws Spec_error If a quote is left open, which flex refuses as an unrecognized option.
 */
[[nodiscard]] std::vector<std::string> words_after_first(const std::string_view line, const std::size_t number)
{
    std::vector<std::string> words{};

    auto rest{trimmed(line)};

    const auto blank{rest.find_first_of(" \t")};

    rest.remove_prefix(std::min(blank, rest.size()));

    std::string word{};

    auto quoted{false};

    for (const auto byte : rest)
    {
        if (quoted)
        {
            word.push_back(byte);

            if (byte == '"')
            {
                quoted = false;

                words.push_back(std::exchange(word, {}));
            }

            continue;
        }

        if (byte == ' ' || byte == '\t')
        {
            if (!word.empty())
            {
                words.push_back(std::exchange(word, {}));
            }

            continue;
        }

        quoted = byte == '"';

        word.push_back(byte);
    }

    if (quoted)
    {
        throw Spec_error{"a quote is left open on the line, which flex refuses", number};
    }

    if (!word.empty())
    {
        words.push_back(std::move(word));
    }

    return words;
}

/**
 * @brief Reads an `%option` line, each word into the settings and the file's options in order.
 * @param lines The cursor, at the line.
 * @param text The line, trimmed.
 * @param spec The specification being filled.
 * @param settings The settings the words resolve to.
 * @return Whether the line is an `%option` line.
 * @throws Spec_error If a quote is left open on the line.
 */
[[nodiscard]] bool take_options(const Lines& lines, const std::string_view text, Lexer_spec& spec, Settings& settings)
{
    if (!text.starts_with("%option"))
    {
        return false;
    }

    for (auto& word : words_after_first(text, lines.number()))
    {
        take_option(settings, word, lines.number());

        spec.options.push_back(std::move(word));
    }

    return true;
}

/**
 * @brief Reads a `%s` or `%x` line, each name a start condition, inclusive or exclusive as the line says.
 * @param lines The cursor, at the line.
 * @param text The line, trimmed.
 * @param spec The specification being filled.
 * @return Whether the line declares start conditions.
 * @throws Spec_error If a quote is left open on the line.
 */
[[nodiscard]] bool take_conditions(const Lines& lines, const std::string_view text, Lexer_spec& spec)
{
    if (text.size() < 2 || text[0] != '%' || !std::string_view{"sSxX"}.contains(text[1]))
    {
        return false;
    }

    const auto exclusive{text[1] == 'x' || text[1] == 'X'};

    for (auto& name : words_after_first(text, lines.number()))
    {
        spec.conditions.push_back({.name = std::move(name), .exclusive = exclusive});
    }

    return true;
}

/**
 * @brief Returns whether a name is one flex accepts for a definition or a start condition.
 * @param name The candidate.
 * @return True for a letter or underscore followed by letters, digits, underscores and hyphens.
 */
[[nodiscard]] bool is_name(const std::string_view name) noexcept
{
    if (!starts_name(name))
    {
        return false;
    }

    const auto continues_name{[](const char byte) { return is_name_byte(byte) || byte == '-'; }};

    return std::ranges::all_of(name, continues_name);
}

/**
 * @brief Reads a definition: a name at the margin, blanks, and the pattern to the end of the line, a comment there
 *        included, since flex takes the definition to the line's end and reads such a comment as part of it.
 * @param lines The cursor, at the line.
 * @param text The line, trimmed.
 * @param spec The specification being filled.
 * @throws Spec_error If the name is none flex accepts, the pattern is missing, or it holds a negated class.
 */
void take_definition(const Lines& lines, const std::string_view text, Lexer_spec& spec)
{
    const auto split{text.find_first_of(" \t")};

    const auto name{text.substr(0, split)};

    if (!is_name(name))
    {
        throw Spec_error{std::format("a definition name was expected at the margin, got '{}'", name), lines.number()};
    }

    const auto pattern{split == std::string_view::npos ? std::string_view{} : trimmed(text.substr(split))};

    if (pattern.empty())
    {
        throw Spec_error{std::format("definition '{}' has no pattern", name), lines.number()};
    }

    if (const auto negated{negated_class(pattern)})
    {
        throw Spec_error{std::format("the definition '{}' {}", name, negated_class_refusal(*negated)), lines.number()};
    }

    spec.definitions.insert_or_assign(std::string{name}, expression_of(pattern));
}

/**
 * @brief Reads a `%{` code block of the rules section when one opens on the line, leaving the cursor past the line
 *        holding its `%}`: the block is copied into the scanner as the definitions' blocks are, ahead of every action,
 *        so a YY_USER_ACTION defined there is the same hook and refused by the same name.
 * @param lines The cursor, at the line.
 * @param text The line, trimmed.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the block appended.
 * @return Whether the line opens such a block.
 * @throws Spec_error If the block is never closed.
 */
[[nodiscard]] bool take_rules_code_block(
        Lines& lines, const std::string_view text, Macros_t& macros, std::vector<Copied>& copied)
{
    if (!take_code_block(lines, text, "the rules' code block defines", macros, copied))
    {
        return false;
    }

    lines.advance();

    return true;
}

/**
 * @brief Returns where a string or character literal of a flex action ends, as flex's action scanner reads one: at its
 *        closing quote or at the end of its line, whichever comes first. A backslash before a newline splices the
 *        lines; any other carries the byte after it, with whatever splices stand between, and a newline after those
 *        ends the literal as it ends any other.
 * @param code The stretch of C the action opens.
 * @param at The index of the literal's opening quote.
 * @return The index of the closing quote, of the newline ending the literal's line, or the stretch's size.
 */
[[nodiscard]] std::size_t literal_end(const std::string_view code, const std::size_t at)
{
    const auto quote{code[at]};

    auto close{at + 1};

    while (close < code.size() && code[close] != quote && code[close] != '\n')
    {
        if (code[close] != '\\')
        {
            ++close;

            continue;
        }

        if (code.substr(close).starts_with(splice))
        {
            close += splice.size();

            continue;
        }

        ++close;

        while (code.substr(close).starts_with(splice))
        {
            close += splice.size();
        }

        if (close < code.size() && code[close] != '\n')
        {
            ++close;
        }
    }

    return close;
}

/**
 * @brief Returns where a flex action ends, as flex 2.6.4's action scanner reads one: at the first end of a line at
 *        which its braces balance, whether it opens with a brace or reaches one later on its line, a stray close
 *        counting below zero.
 *
 * What hides a brace is what that scanner has a state for. A block comment runs to its star-slash over any number of
 * lines. A string or character literal runs to its closing quote or to the end of its line, whichever comes first, an
 * escape carrying the byte after it and a backslash before the newline carrying the literal on to the next line, as C
 * splices lines; a literal the line's end closes leaves the action open where a brace is, and where none is, flex ends
 * a rule's action there in the literal's state and never closes the code it emits for it, so that the m4 it runs stops
 * with an end of file in string, which is refused by name, while a scope's opener or close line, whose code flex copies
 * out as no rule's action, ends there like any other line. There is no state for a `//` comment, so a brace after one
 * on the line counts and a quote there opens a literal. An action opening with `%{` is a code block instead, read in a
 * state of its own with no comments or literals, which runs to the end of the first line holding `%}`.
 * @param code The stretch of C the action opens, from its first byte to the end of the file.
 * @param number The line the action begins on, for the refusals.
 * @param rule Whether the code is a rule's action rather than the code on a scope's opener or close line.
 * @return The offset of the newline ending the action, or the stretch's size when the file ends it balanced.
 * @throws Spec_error If a brace, a comment or a `%{` block is left open at the end of the file, which flex refuses as
 *         an end of file inside an action, or a rule's action leaves a literal open at the end of a line where its
 *         braces balance.
 */
[[nodiscard]] std::size_t action_end(const std::string_view code, const std::size_t number, const bool rule)
{
    if (code.starts_with(block_opener))
    {
        const auto close{code.find(block_closer)};

        if (close == std::string_view::npos)
        {
            throw Spec_error{"the action's %{ block is never closed, which flex refuses", number};
        }

        const auto newline{code.find('\n', close)};

        return std::min(newline, code.size());
    }

    std::ptrdiff_t depth{0};

    for (std::size_t at{0}; at < code.size();)
    {
        const auto byte{code[at]};

        if (byte == '"' || byte == '\'')
        {
            const auto close{literal_end(code, at)};

            // A literal its quote closes is read past, and so is one the line's end leaves open inside a brace.
            if ((close < code.size() && code[close] == byte) || depth > 0)
            {
                at = close + 1;

                continue;
            }

            if (rule)
            {
                throw Spec_error{std::string{open_literal}, number};
            }

            return close;
        }

        if (code.substr(at).starts_with(comment_opener))
        {
            const auto close{code.find(comment_closer, at + comment_opener.size())};

            if (close == std::string_view::npos)
            {
                throw Spec_error{std::string{open_comment}, number};
            }

            at = close + comment_closer.size();

            continue;
        }

        if (byte == '{')
        {
            ++depth;
        }
        else if (byte == '}')
        {
            --depth;
        }
        else if (byte == '\n' && depth <= 0)
        {
            return at;
        }

        ++at;
    }

    if (depth > 0)
    {
        throw Spec_error{"the action's braces never close", number};
    }

    return code.size();
}

/**
 * @brief Moves the cursor past its line and the lines a stretch of code from that line on runs on to.
 * @param lines The cursor, at the line the code begins on.
 * @param code The code.
 */
void advance_past(Lines& lines, const std::string_view code)
{
    const auto newlines{std::ranges::count(code, '\n')};

    lines.advance(1 + static_cast<std::size_t>(newlines));
}

/**
 * @brief Skips code flex copies out of the rules section and drops, read to the same end an action is read to: a
 *        comment closing on a later line, or a brace block, runs the code on to that line, and nothing on those lines
 *        is a rule.
 * @param lines The cursor, at the line the code begins on; left past the last line it runs on to.
 * @param from The offset of the code's first byte from the line's first.
 * @throws Spec_error If the code leaves a comment or a brace open at the end of the file.
 */
void skip_copied_code(Lines& lines, const std::size_t from)
{
    const auto rest{lines.rest()};

    const auto end{action_end(rest.substr(from), lines.number(), false)};

    advance_past(lines, rest.substr(from, end));
}

/**
 * @brief Closes the innermost start-condition scope when the line opens with its `}`, the code after the brace skipped
 *        as the code after a scope's opener is.
 * @param lines The cursor, at the line; left past the code the line holds.
 * @param text The line, trimmed.
 * @param scoped The names of every open scope, the closed scope's dropped.
 * @param opened The open scopes, innermost last.
 * @return Whether the line closes a scope.
 * @throws Spec_error If the code after the brace is left open at the end of the file.
 */
[[nodiscard]] bool close_scope(
        Lines& lines, const std::string_view text, std::vector<std::string>& scoped, std::vector<Scope>& opened)
{
    if (opened.empty() || !text.starts_with('}'))
    {
        return false;
    }

    scoped.resize(opened.back().names);

    opened.pop_back();

    const auto brace{indentation(lines.current())};

    skip_copied_code(lines, brace + 1);

    return true;
}

/**
 * @brief Skips a comment on a line of its own, which flex copies out as code where the line is indented, and refuses at
 *        the margin, where its slash begins a rule: trailing context with nothing before it, an unrecognized rule to
 *        flex.
 * @param lines The cursor, at the line; left past the comment.
 * @param line The line, untrimmed.
 * @param text The line, trimmed.
 * @return Whether the line opens a comment.
 * @throws Spec_error If the comment stands at the margin or is never closed.
 */
[[nodiscard]] bool skip_rules_comment(Lines& lines, const std::string_view line, const std::string_view text)
{
    if (!text.starts_with(comment_opener))
    {
        return false;
    }

    if (line.front() != ' ' && line.front() != '\t')
    {
        throw Spec_error{
                "a comment at the margin of the rules section, which flex reads as a rule and refuses as "
                "unrecognized; indent it, as flex's manual asks",
                lines.number()};
    }

    const auto comment{indentation(lines.current())};

    skip_copied_code(lines, comment);

    return true;
}

/**
 * @brief Reads a line of the rules section's prologue, or a blank line after it: the indented code before the first
 *        rule is flex's to copy into the scanner, where it stands ahead of every action, so it holds the one thing the
 *        token language is not blind to just as the definitions do, a YY_USER_ACTION that moves the match. The lines it
 *        splices are read with it.
 * @param lines The cursor, at the line; left past it and the lines it continues on.
 * @param line The line, untrimmed.
 * @param text The line, trimmed.
 * @param prologue Whether the prologue is still open, no line at the margin read yet.
 * @param macros The macros the section defines, added to.
 * @param copied The stretches of copied code, the prologue's code appended.
 * @return Whether the line is prologue code or blank.
 */
[[nodiscard]] bool take_prologue_line(
        Lines& lines, const std::string_view line, const std::string_view text, const bool prologue, Macros_t& macros,
        std::vector<Copied>& copied)
{
    const auto prologue_code{prologue && is_code(line)};

    const auto blank_after{!prologue && text.empty()};

    if (!prologue_code && !blank_after)
    {
        return false;
    }

    if (prologue)
    {
        const auto first{lines.number()};

        const auto code{continued_code(lines)};

        copy_code(code, first, "the rules' prologue defines", macros, copied);
    }

    lines.advance();

    return true;
}

/**
 * @brief Reads a rule line's start-condition prefix when one stands, `<s1,s2>`, and the rule past it, on the next line
 *        that holds more than blanks where the prefix stands alone on its line: flex takes such a prefix as opening
 *        whatever the next line holds, since the newline after it yields no token, the `{` of a scope, as bison's
 *        scanners write it, or a rule.
 * @param lines The cursor, at the rule line; left at the line the rule's text stands on.
 * @param line The rule line's text, the cursor's line with its indentation and trailing blanks removed.
 * @return The conditions and where the rule's text stands.
 * @throws Spec_error If the prefix is not closed or ends the section.
 */
[[nodiscard]] Rule_start rule_start(Lines& lines, std::string_view line)
{
    const auto number{lines.number()};

    auto line_at{indentation(lines.current())};

    std::vector<std::string> conditions{};

    if (!line.starts_with('<') || line.starts_with(end_of_input))
    {
        return {.conditions = std::move(conditions), .text = line, .line_at = line_at, .number = number};
    }

    const auto close{line.find('>')};

    if (close == std::string_view::npos)
    {
        throw Spec_error{"the start-condition prefix is not closed", number};
    }

    for (const auto name : line.substr(1, close - 1) | std::views::split(','))
    {
        conditions.emplace_back(trimmed(std::string_view{name}));
    }

    line.remove_prefix(close + 1);

    line_at += close + 1;

    if (!trimmed(line).empty())
    {
        return {.conditions = std::move(conditions), .text = line, .line_at = line_at, .number = number};
    }

    do
    {
        lines.advance();
    } while (lines.more() && trimmed(lines.current()).empty());

    if (!lines.more())
    {
        throw Spec_error{"the start-condition prefix ends the section", number};
    }

    return {.conditions = std::move(conditions),
            .text = trimmed(lines.current()),
            .line_at = indentation(lines.current()),
            .number = lines.number()};
}

/**
 * @brief Returns a rule's action as flex copies it out: a `%{` block's code is what stands between its `%{` and its
 *        `%}`, the rest of the `%}` line dropped, so `%} return 7;` returns nothing; any other action is kept whole.
 * @param action The action as written, trimmed.
 * @return The action's code.
 */
[[nodiscard]] std::string block_code(const std::string_view action)
{
    if (!action.starts_with(block_opener))
    {
        return std::string{action};
    }

    const auto close{action.find(block_closer)};

    const auto inside{action.substr(block_opener.size(), close - block_opener.size())};

    return std::string{trimmed(inside)};
}

/**
 * @brief Reads one rule from the line under the cursor and the action lines it spans, leaving the cursor after it.
 * @param lines The cursor, at a rule line.
 * @param line The rule line's text, the cursor's line with its indentation and trailing blanks removed.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The rule, an `<<EOF>>` rule among them with what its action returns, since a `|` rule above it shares that
 *         action; the caller drops the `<<EOF>>` rules once the actions are shared, as no byte matches one.
 * @throws Spec_error If its start-condition prefix is not closed or ends the section, its pattern is missing or leaves
 *         a quote or a bracket open, its action is one action_end() refuses, or the action makes a call that moves or
 *         reruns the match, or an `<<EOF>>` action one that pushes a byte onto the input or takes one from it.
 */
[[nodiscard]] Lexer_spec::Rule read_rule(Lines& lines, const std::string_view line, const Returning_t& returning)
{
    auto [conditions, text, line_at, number]{rule_start(lines, line)};

    const auto length{pattern_length(text, number)};

    const std::string pattern{text.substr(0, length)};

    if (pattern.empty())
    {
        throw Spec_error{"a rule at the margin has no pattern", number};
    }

    // The action runs to the first end of a line at which its braces balance, as flex reads it, so one that opens a
    // brace anywhere on its line continues to the matching close, however many lines that takes; a `|` action is the
    // bar and whatever follows it on its line, which flex takes unread, so no brace or quote there counts.
    const auto after_pattern{text.substr(length)};

    const auto tail{trimmed(after_pattern)};

    const auto rest{lines.rest()};

    const auto opened{line_at + length + indentation(after_pattern)};

    const auto action_length{[&] {
        if (!tail.starts_with('|'))
        {
            return action_end(rest.substr(opened), number, pattern != "{");
        }

        const auto newline{rest.find('\n', opened)};

        return std::min(newline, rest.size()) - opened;
    }};

    const auto end{action_length()};

    // `<s>{` opens a start-condition scope rather than a rule, and what follows the brace on its line is code flex
    // copies out and drops, read to the same end an action is read to: a comment closing on a later line runs the code
    // on to that line, as flex 2.6.4 permits, and nothing on those lines is a rule. The caller reads the scope as such.
    if (pattern == "{")
    {
        advance_past(lines, rest.substr(opened, end));

        return {.pattern = pattern,
                .expression = pattern,
                .conditions = std::move(conditions),
                .action = {},
                .token = std::nullopt,
                .priority = std::nullopt,
                .line = number};
    }

    const auto written{trimmed(rest.substr(opened, end))};

    advance_past(lines, written);

    auto action{block_code(written)};

    // A `|` line is taken unread, as flex takes it, so what follows the bar is no call; an `<<EOF>>` action runs where
    // no match is, so what it calls moves no match, until a `|` rule above shares it, which share_actions() checks.
    const auto unread{action.starts_with('|') || pattern == end_of_input};

    if (const auto use{unread ? std::nullopt : stateful_use(action, false)})
    {
        throw Spec_error{action_refusal(*use), number};
    }

    // What an `<<EOF>>` action pushes onto the input is scanned after it, whatever rule matched before: flex takes
    // `<<EOF>> { unput('a'); return 9; }` on "b" through 8, 9, 7, 9, 7, the a arriving from the action. The calls that
    // move a match are another rule's concern, since an end-of-input action has none.
    if (const auto use{pattern == end_of_input ? stateful_use(action, true) : std::nullopt})
    {
        throw Spec_error{std::format("the end-of-input action {}", *use), number};
    }

    auto token{returned(action, returning)};

    return {.pattern = pattern,
            .expression = expression_of(pattern),
            .conditions = std::move(conditions),
            .action = std::move(action),
            .token = std::move(token),
            .priority = std::nullopt,
            .line = number};
}

} // namespace

void read_definitions(Lines& lines, Lexer_spec& spec, Settings& settings, Macros_t& macros, std::vector<Copied>& copied)
{
    for (; lines.more(); lines.advance())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (is_delimiter(line))
        {
            spec.line = lines.number();

            lines.advance();

            return;
        }

        // The section's code, a `%{` block, a `%top` block or an indented line, is flex's to copy through, and holds
        // one thing the token language is not blind to: a YY_USER_ACTION that moves the match. Each kind of line is
        // read by the first of these that takes it.
        const auto taken{
                take_code_block(lines, text, definitions_code, macros, copied) ||
                take_top_block(lines, text, macros, copied) || skip_definitions_comment(lines, text) ||
                take_code_line(lines, line, macros, copied) || take_options(lines, text, spec, settings) ||
                take_conditions(lines, text, spec)};

        // %array, %pointer and the like say nothing about the token set.
        if (taken || text.front() == '%')
        {
            continue;
        }

        take_definition(lines, text, spec);
    }

    throw Spec_error{"the file has no rules section: no line begins with %%", lines.number()};
}

std::size_t read_rules(
        Lines& lines, Lexer_spec& spec, const Returning_t& returning, Macros_t& macros, std::vector<Copied>& copied)
{
    // A start-condition scope, `<s>{` on a line of its own through a line opening with `}`, prefixes every rule inside
    // it. Scopes nest, and a rule inside one with a prefix of its own is active in the scope's conditions and its own
    // alike: flex keeps every open scope's names on one stack and a rule takes the whole stack.
    std::vector<std::string> scoped{};

    std::vector<Scope> opened{};

    // The section opens with a prologue, where an indented line is code flex copies into the scanner ahead of the
    // rules, which ends at the first line at the margin; from then on flex reads an indented line as a rule, in a scope
    // or out of one.
    auto prologue{true};

    while (lines.more())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (is_delimiter(line))
        {
            break;
        }

        // Each kind of line other than a rule is read by the first of these that takes it.
        const auto taken{
                take_rules_code_block(lines, text, macros, copied) || close_scope(lines, text, scoped, opened) ||
                skip_rules_comment(lines, line, text) ||
                take_prologue_line(lines, line, text, prologue, macros, copied)};

        if (taken)
        {
            continue;
        }

        prologue = false;

        auto rule{read_rule(lines, text, returning)};

        if (rule.pattern == "{" && rule.action.empty())
        {
            opened.push_back({.names = scoped.size(), .line = rule.line});

            std::ranges::move(rule.conditions, std::back_inserter(scoped));

            continue;
        }

        for (const auto& name : scoped)
        {
            if (!std::ranges::contains(rule.conditions, name))
            {
                rule.conditions.push_back(name);
            }
        }

        spec.rules.push_back(std::move(rule));
    }

    // flex reads the rest of the section as the scope's body and hits a parse error at its end, so a scope left open is
    // refused here rather than read as if its `}` stood at the section's end.
    if (!opened.empty())
    {
        throw Spec_error{"a start-condition scope is never closed", opened.back().line};
    }

    // The cursor stands on the `%%`, or one past the last line once the file has ended.
    return lines.more() ? lines.number() : lines.number() - 1;
}

void share_actions(std::vector<Lexer_spec::Rule>& rules)
{
    for (auto [sharing, shared] : rules | std::views::adjacent<2> | std::views::reverse)
    {
        if (!sharing.action.starts_with('|'))
        {
            continue;
        }

        // An `<<EOF>>` action is read here, once a `|` rule above shares it and a match runs it.
        const auto read{shared.pattern == end_of_input && !shared.action.starts_with('|')};

        if (const auto use{read ? stateful_use(shared.action, false) : std::nullopt})
        {
            throw Spec_error{action_refusal(*use), sharing.line};
        }

        sharing.token = shared.token;
    }
}

void add_default_rule(Lexer_spec& spec, const std::size_t line)
{
    spec.rules.push_back(
            {.pattern = R"(.|\n)",
             .expression = R"(.|\n)",
             .conditions = {std::string{every_condition}},
             .action = "ECHO;",
             .token = std::nullopt,
             .priority = std::nullopt,
             .line = line});
}

} // namespace munch::tools::audit

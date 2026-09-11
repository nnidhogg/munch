#include "munch/tools/audit/read_flex.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The source as lines, with the cursor the two section readers share.
 */
class Lines
{
public:
    /**
     * @brief Splits the source; a final line without a newline counts as a line.
     * @param source The file's text.
     */
    explicit Lines(std::string_view source);

    /**
     * @brief Whether a line remains.
     * @return True while the cursor is inside the file.
     */
    [[nodiscard]] bool more() const noexcept;

    /**
     * @brief Moves the cursor forward.
     * @param count How many lines, one unless told.
     */
    void advance(std::size_t count = 1) noexcept;

    /**
     * @brief The number of the line under the cursor, counted from one.
     * @return The line number.
     */
    [[nodiscard]] std::size_t number() const noexcept;

    /**
     * @brief The source from the line under the cursor to the end of the file, for an action that spans lines.
     * @return The remaining text.
     */
    [[nodiscard]] std::string_view rest() const noexcept;

    /**
     * @brief The line under the cursor.
     * @return The line, without its newline.
     */
    [[nodiscard]] std::string_view current() const noexcept;

    /**
     * @brief Skips lines through the first whose trimmed text is the given one, which a block delimiter is.
     * @param close The delimiter line, `%}` for a code block.
     * @param what What was open, named when the file ends first.
     * @throws Spec_error If the file ends before the delimiter.
     */
    void skip_through(std::string_view close, std::string_view what);

private:
    /**
     * @brief The file's text, which the lines view.
     */
    std::string_view source_;

    /**
     * @brief The lines.
     */
    std::vector<std::string_view> lines_;

    /**
     * @brief The index of the line under the cursor.
     */
    std::size_t at_{0};
};

/**
 * @brief The line with leading and trailing blanks removed.
 * @param line The line.
 * @return The trimmed view.
 */
[[nodiscard]] std::string_view trimmed(std::string_view line) noexcept
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
    {
        line.remove_prefix(1);
    }

    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    {
        line.remove_suffix(1);
    }

    return line;
}

/**
 * @brief Whether a line is code or blank rather than a declaration: it begins with a blank or is empty.
 * @param line The line.
 * @return True for an indented or empty line.
 */
[[nodiscard]] bool is_code(const std::string_view line) noexcept
{
    return line.empty() || line.front() == ' ' || line.front() == '\t' || trimmed(line).empty();
}

/**
 * @brief The words of a line after its first, split on blanks.
 * @param line The line.
 * @return The words.
 */
[[nodiscard]] std::vector<std::string> words_after_first(const std::string_view line)
{
    std::vector<std::string> words;

    auto rest{trimmed(line)};

    rest.remove_prefix(std::min(rest.find_first_of(" \t"), rest.size()));

    for (const auto word : rest | std::views::split(' '))
    {
        for (const auto piece : std::string_view{word} | std::views::split('\t'))
        {
            if (const std::string_view text{piece}; !text.empty())
            {
                words.emplace_back(text);
            }
        }
    }

    return words;
}

/**
 * @brief Whether a name is one flex accepts for a definition or a start condition.
 * @param name The candidate.
 * @return True for a letter or underscore followed by letters, digits, underscores and hyphens.
 */
[[nodiscard]] bool is_name(const std::string_view name) noexcept
{
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_'))
    {
        return false;
    }

    return std::ranges::all_of(name, [](const char byte) {
        return std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '_' || byte == '-';
    });
}

/**
 * @brief Where a pattern ends on a rule line: the first blank outside quotes and brackets, escapes honoured.
 * @param line The rule line, its `<...>` prefix already removed.
 * @param number The line number, for the error.
 * @return The pattern's length.
 * @throws Spec_error If a quote or bracket is left open.
 */
[[nodiscard]] std::size_t pattern_length(const std::string_view line, const std::size_t number)
{
    auto quoted{false};

    auto bracketed{false};

    // A ']' right after the '[' or the '[^' is a member, not the close, as in the pattern syntax itself.
    auto opening{false};

    for (std::size_t at{0}; at < line.size(); ++at)
    {
        const auto byte{line[at]};

        if (byte == '\\')
        {
            ++at;

            opening = false;

            continue;
        }

        if (quoted)
        {
            quoted = byte != '"';
        }
        else if (bracketed)
        {
            bracketed = byte != ']' || opening;

            opening = opening && byte == '^';
        }
        else if (byte == '"')
        {
            quoted = true;
        }
        else if (byte == '[')
        {
            bracketed = true;

            opening = true;
        }
        else if (byte == ' ' || byte == '\t')
        {
            return at;
        }
    }

    if (quoted || bracketed)
    {
        throw Spec_error{
                quoted ? "a quote is left open in the pattern" : "a bracket is left open in the pattern", number};
    }

    return line.size();
}

/**
 * @brief Reads the definitions section, up to and over its `%%`, whose line names the scanner.
 * @param lines The cursor, at the file's first line.
 * @param file The file being filled.
 * @throws Spec_error If the section never ends, a block is left open, or a definition has no pattern.
 */
void read_definitions(Lines& lines, Lexer_spec& file)
{
    for (; lines.more(); lines.advance())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (text == "%%")
        {
            file.line = lines.number();

            lines.advance();

            return;
        }

        if (text == "%{")
        {
            lines.skip_through("%}", "a %{ code block");

            continue;
        }

        if (text.starts_with("%top") && trimmed(text.substr(4)).starts_with('{'))
        {
            lines.skip_through("}", "a %top block");

            continue;
        }

        if (text.starts_with("/*"))
        {
            while (lines.more() && trimmed(lines.current()).find("*/") == std::string_view::npos)
            {
                lines.advance();
            }

            continue;
        }

        if (is_code(line))
        {
            continue;
        }

        if (text.starts_with("%option"))
        {
            std::ranges::move(words_after_first(text), std::back_inserter(file.options));

            continue;
        }

        if (text.starts_with("%s") || text.starts_with("%S") || text.starts_with("%x") || text.starts_with("%X"))
        {
            const auto exclusive{text[1] == 'x' || text[1] == 'X'};

            for (auto& name : words_after_first(text))
            {
                file.conditions.push_back({.name = std::move(name), .exclusive = exclusive});
            }

            continue;
        }

        if (text.front() == '%')
        {
            continue; // %array, %pointer and the like say nothing about the token set
        }

        // A definition: a name at the margin, blanks, and the pattern to the end of the line.
        const auto split{text.find_first_of(" \t")};

        const auto name{text.substr(0, split)};

        if (!is_name(name))
        {
            throw Spec_error{
                    "a definition name was expected at the margin, got '" + std::string{name} + "'", lines.number()};
        }

        const auto pattern{split == std::string_view::npos ? std::string_view{} : trimmed(text.substr(split))};

        if (pattern.empty())
        {
            throw Spec_error{"definition '" + std::string{name} + "' has no pattern", lines.number()};
        }

        file.definitions.insert_or_assign(std::string{name}, std::string{pattern});
    }

    throw Spec_error{"the file has no rules section: no line reads %%", lines.number()};
}

/**
 * @brief Reads one rule from the line under the cursor and the action lines it spans, leaving the cursor after it.
 * @param lines The cursor, at a rule line.
 * @param line The rule line's text, the cursor's line with its indentation removed inside a scope.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The rule, or std::nullopt for an `<<EOF>>` rule.
 * @throws Spec_error If the rule has no pattern or its action's braces never close.
 */
[[nodiscard]] std::optional<Lexer_spec::Rule> read_rule(
        Lines& lines, std::string_view line, const Returning_t& returning)
{
    const auto number{lines.number()};

    std::vector<std::string> conditions;

    if (line.starts_with('<') && !line.starts_with("<<EOF>>"))
    {
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
    }

    const auto length{pattern_length(line, number)};

    const std::string pattern{line.substr(0, length)};

    if (pattern.empty())
    {
        throw Spec_error{"a rule at the margin has no pattern", number};
    }

    // `<s>{` alone on the line opens a start-condition scope rather than a rule; the caller reads it as such.
    if (pattern == "{" && trimmed(line.substr(length)).empty())
    {
        lines.advance();

        return Lexer_spec::Rule{
                .pattern = pattern,
                .expression = pattern,
                .conditions = std::move(conditions),
                .action = {},
                .token = std::nullopt,
                .priority = std::nullopt,
                .line = number};
    }

    // The action runs to the first end of a line at which its braces balance, as flex reads it, so one that opens
    // a brace anywhere on its line continues to the matching close, however many lines that takes.
    const auto tail{trimmed(line.substr(length))};

    const auto rest{lines.rest()};

    const auto opened{static_cast<std::size_t>(tail.data() - rest.data())};

    const auto end{action_end(rest.substr(opened))};

    if (!end)
    {
        throw Spec_error{"the action's braces never close", number};
    }

    std::string action{trimmed(rest.substr(opened, *end))};

    lines.advance(1 + static_cast<std::size_t>(std::ranges::count(action, '\n')));

    if (pattern == "<<EOF>>")
    {
        return std::nullopt;
    }

    auto token{returned(action, returning)};

    return Lexer_spec::Rule{
            .pattern = pattern,
            .expression = pattern,
            .conditions = std::move(conditions),
            .action = std::move(action),
            .token = std::move(token),
            .priority = std::nullopt,
            .line = number};
}

/**
 * @brief Reads the rules section, up to its `%%` or the end of the file.
 * @param lines The cursor, at the section's first line.
 * @param file The file being filled.
 * @param returning The forms besides `return` an action returns a token through.
 */
void read_rules(Lines& lines, Lexer_spec& file, const Returning_t& returning)
{
    // A start-condition scope, `<s>{` on a line of its own through a line opening with `}`, prefixes every rule
    // inside it, and flex lets those rules be indented, so inside a scope an indented line is a rule rather than
    // code.
    std::vector<std::string> scope;

    while (lines.more())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (text == "%%")
        {
            return;
        }

        if (text == "%{")
        {
            lines.skip_through("%}", "a %{ code block");

            continue;
        }

        if (!scope.empty() && text.starts_with('}'))
        {
            scope.clear();

            lines.advance();

            continue;
        }

        // A comment on a line of its own, which flex copies out, wherever the rules section holds it.
        if (text.starts_with("/*"))
        {
            while (lines.more() && trimmed(lines.current()).find("*/") == std::string_view::npos)
            {
                lines.advance();
            }

            lines.advance();

            continue;
        }

        if (scope.empty() ? is_code(line) : text.empty())
        {
            lines.advance();

            continue;
        }

        auto rule{read_rule(lines, scope.empty() ? line : text, returning)};

        if (rule && rule->pattern == "{" && rule->action.empty())
        {
            scope = std::move(rule->conditions);

            continue;
        }

        if (rule)
        {
            if (!scope.empty())
            {
                rule->conditions = scope;
            }

            file.rules.push_back(std::move(*rule));
        }
    }
}

/**
 * @brief Gives every `|` action the token of the first rule below it that has an action of its own.
 * @param rules The rules, in file order.
 */
void share_actions(std::vector<Lexer_spec::Rule>& rules)
{
    for (auto at{rules.size()}; at > 1;)
    {
        --at;

        if (rules[at - 1].action == "|")
        {
            rules[at - 1].token = rules[at].token;
        }
    }
}

Lines::Lines(const std::string_view source) : source_{source}
{
    for (const auto line : source | std::views::split('\n'))
    {
        lines_.emplace_back(line);
    }

    if (!lines_.empty() && lines_.back().empty())
    {
        lines_.pop_back();
    }
}

bool Lines::more() const noexcept
{
    return at_ < lines_.size();
}

void Lines::advance(const std::size_t count) noexcept
{
    at_ += count;
}

std::size_t Lines::number() const noexcept
{
    return at_ + 1;
}

std::string_view Lines::rest() const noexcept
{
    return std::string_view{lines_[at_].data(), source_.data() + source_.size()};
}

std::string_view Lines::current() const noexcept
{
    return lines_[at_];
}

void Lines::skip_through(const std::string_view close, const std::string_view what)
{
    const auto opened{number()};

    for (advance(); more(); advance())
    {
        if (trimmed(current()) == close)
        {
            return;
        }
    }

    throw Spec_error{std::string{what} + " is never closed", opened};
}

} // namespace

Lexer_spec read_flex(const std::string_view source, const Returning_t& returning)
{
    Lexer_spec file;

    Lines lines{source};

    read_definitions(lines, file);

    read_rules(lines, file, returning);

    share_actions(file.rules);

    return file;
}

} // namespace munch::tools::audit

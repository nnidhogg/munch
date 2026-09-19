#include "munch/tools/audit/lexer_spec.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace munch::tools::audit
{
namespace
{
/**
 * @brief Whether a byte is one of a C word: a letter, a digit, an underscore, or a byte above ASCII, which is part of
 *        a UTF-8 identifier.
 * @param byte The byte.
 * @return True for a word byte.
 */
[[nodiscard]] bool is_word_byte(const char byte) noexcept
{
    return std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '_' ||
           static_cast<unsigned char>(byte) >= 0x80;
}

/**
 * @brief Whether a universal character name, `\u` or `\U` and its hexadecimal digits, opens at an index; it is part
 *        of the identifier around it, so that `return\u03B1` is a name of the file's own and no `return`.
 * @param code The stretch of C.
 * @param at The index.
 * @return True when one opens here.
 */
[[nodiscard]] bool at_universal_name(const std::string_view code, const std::size_t at) noexcept
{
    return at + 1 < code.size() && code[at] == '\\' && (code[at + 1] == 'u' || code[at + 1] == 'U');
}

/**
 * @brief The index just past the universal character name opening at an index: its `\u` or `\U` and the hexadecimal
 *        digits after it.
 * @param code The stretch of C.
 * @param at The index, where at_universal_name() holds.
 * @return The index past the name.
 */
[[nodiscard]] std::size_t past_universal_name(const std::string_view code, const std::size_t at) noexcept
{
    auto end{at + 2};

    while (end < code.size() && std::isxdigit(static_cast<unsigned char>(code[end])) != 0)
    {
        ++end;
    }

    return end;
}

/**
 * @brief The index just past the raw string literal opening at an index, when one does: an `R` after an optional
 *        `u8`, `u`, `U` or `L`, then `"`, a delimiter of up to sixteen bytes, `(`, the body, `)`, the same delimiter
 *        and `"`, as C++ reads one. Flex's C++ scanners hold C++ actions, so the word `R` followed by a quote is the
 *        prefix and not a name of the file's own; one left open runs to the end.
 * @param code The stretch of C.
 * @param at The index, of a byte that is no whitespace.
 * @return The index past the literal, or std::nullopt when none opens here.
 */
[[nodiscard]] std::optional<std::size_t> raw_string_end(const std::string_view code, const std::size_t at)
{
    const auto rest{code.substr(at)};

    std::size_t prefix{0};

    for (const std::string_view encoding : {"u8", "u", "U", "L", ""})
    {
        if ((at == 0 || !is_word_byte(code[at - 1])) && rest.starts_with(std::string{encoding} + "R\""))
        {
            prefix = encoding.size() + 2;

            break;
        }

        if (encoding.empty())
        {
            return std::nullopt;
        }
    }

    const auto open{code.find('(', at + prefix)};

    if (open == std::string_view::npos)
    {
        return code.size();
    }

    const auto delimiter{code.substr(at + prefix, open - at - prefix)};

    const auto close{code.find(")" + std::string{delimiter} + "\"", open + 1)};

    return close == std::string_view::npos ? code.size() : close + delimiter.size() + 2;
}

/**
 * @brief The index just past the token or the comment opening at an index of a stretch of C, as c_tokens() reads
 *        them: past a literal's closing quote, an escaped byte carried, past a comment's end, past the last byte of a
 *        word, and past the one or two bytes of a punctuator; a literal or a comment left open runs to the end.
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

    if (rest.starts_with("//"))
    {
        return std::min(code.find('\n', at), code.size());
    }

    if (rest.starts_with("/*"))
    {
        const auto close{code.find("*/", at + 2)};

        return close == std::string_view::npos ? code.size() : close + 2;
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

    // A number runs as the preprocessor's pp-number does: digits, letters, dots, a digit separator's apostrophe
    // before a digit or a letter, and a sign after an exponent's letter, so `1'000` is one token and its apostrophe
    // opens no character literal, and `0'0'0` is the one index 0.
    if (std::isdigit(static_cast<unsigned char>(rest.front())) != 0 ||
        (rest.front() == '.' && rest.size() > 1 && std::isdigit(static_cast<unsigned char>(rest[1])) != 0))
    {
        auto end{at + 1};

        while (end < code.size())
        {
            const auto byte{code[end]};

            const auto next{end + 1 < code.size() ? code[end + 1] : '\0'};

            const auto separator{byte == '\'' && std::isalnum(static_cast<unsigned char>(next)) != 0};

            const auto exponent{
                    (byte == 'e' || byte == 'E' || byte == 'p' || byte == 'P') && (next == '+' || next == '-')};

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

    if (is_word_byte(rest.front()) || at_universal_name(code, at))
    {
        auto end{at};

        while (end < code.size() && (is_word_byte(code[end]) || at_universal_name(code, end)))
        {
            end = at_universal_name(code, end) ? past_universal_name(code, end) : end + 1;
        }

        return end;
    }

    return at + (rest.starts_with("->") ? 2 : 1);
}

/**
 * @brief A stretch of C with its line splices removed, and where each byte of it stood.
 */
struct Spliced
{
    /**
     * @brief The text with every backslash-newline pair deleted.
     */
    std::string text;

    /**
     * @brief The offset in the stretch of each byte of the text, in order.
     */
    std::vector<std::size_t> place;
};

/**
 * @brief Splices the lines of a stretch of C as C does before it reads tokens: a backslash and the newline after it,
 *        a carriage return between them or not, are deleted, so a word, a literal or a comment may run over a
 *        physical line end.
 * @param code The stretch of C.
 * @return The spliced text and each byte's place.
 */
[[nodiscard]] Spliced spliced(const std::string_view code)
{
    Spliced result;

    const auto copy{[&result, code](const std::size_t at) {
        result.text.push_back(code[at]);

        result.place.push_back(at);
    }};

    // What the byte at hand stands in: code, a string or character literal, a line comment or a block comment. A
    // raw string opens in code alone, so the `R` of `"R"` or of a comment's text opens none, and its content is
    // what it says, splices and all: the language exempts it from the joining of lines. Everything else is joined
    // at a splice, a literal, a comment and code alike, as the compiler joins every line before it reads them.
    enum class Inside : std::uint8_t
    {
        code,
        literal,
        line_comment,
        block_comment
    };

    auto inside{Inside::code};

    char quote{};

    for (std::size_t at{0}; at < code.size(); ++at)
    {
        if (inside == Inside::code)
        {
            if (const auto raw{raw_string_end(code, at)})
            {
                for (; at < *raw; ++at)
                {
                    copy(at);
                }

                --at;

                continue;
            }
        }

        if (code[at] == '\\')
        {
            // gcc joins the lines where blanks stand between the backslash and the line's end, warning as it does
            // it, and the action is compiled by the compiler and not by the standard, so the splice is read the
            // way the compiler reads it: `ret\` with a space, then `urn 7;` on the next line, is one `return`.
            auto newline{std::min(code.find_first_not_of(" \t", at + 1), code.size())};

            newline += newline < code.size() && code[newline] == '\r' ? 1 : 0;

            if (newline < code.size() && code[newline] == '\n')
            {
                at = newline;

                continue;
            }

            // An escape inside a literal carries its byte, so an escaped quote closes nothing; the byte it carries
            // is the one after any splice, since the compiler joins the lines before it reads an escape, and so a
            // backslash before a backslash ending the line, `"x\\` and a newline, escapes the next line's first byte.
            if (inside == Inside::literal && at + 1 < code.size())
            {
                copy(at);

                auto escaped{at + 1};

                if (code[escaped] == '\\')
                {
                    auto joined{std::min(code.find_first_not_of(" \t", escaped + 1), code.size())};

                    joined += joined < code.size() && code[joined] == '\r' ? 1 : 0;

                    if (joined < code.size() && code[joined] == '\n')
                    {
                        escaped = joined + 1;
                    }
                }

                if (escaped < code.size())
                {
                    copy(escaped);
                }

                at = escaped;

                continue;
            }
        }

        const auto rest{code.substr(at)};

        switch (inside)
        {
        case Inside::code:
            if (code[at] == '"' || code[at] == '\'')
            {
                inside = Inside::literal;

                quote = code[at];
            }
            else if (rest.starts_with("//"))
            {
                inside = Inside::line_comment;
            }
            else if (rest.starts_with("/*"))
            {
                inside = Inside::block_comment;

                copy(at);

                ++at;
            }

            break;

        case Inside::literal:
            // A literal ends at its quote, or at the line's end when it is left open.
            if (code[at] == quote || code[at] == '\n')
            {
                inside = Inside::code;
            }

            break;

        case Inside::line_comment:
            if (code[at] == '\n')
            {
                inside = Inside::code;
            }

            break;

        case Inside::block_comment:
            if (rest.starts_with("*/"))
            {
                inside = Inside::code;

                copy(at);

                ++at;
            }

            break;
        }

        copy(at);
    }

    return result;
}

} // namespace

Spec_error::Spec_error(const std::string& message, const std::size_t line)
    : std::runtime_error{"line " + std::to_string(line) + ": " + message}, line_{line}
{}

std::size_t Spec_error::line() const noexcept
{
    return line_;
}

std::vector<C_token> c_tokens(const std::string_view code)
{
    std::vector<C_token> tokens;

    const auto [text, place]{spliced(code)};

    for (std::size_t at{0}; at < text.size();)
    {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0)
        {
            ++at;

            continue;
        }

        const auto end{token_end(text, at)};

        if (const auto rest{std::string_view{text}.substr(at)}; !rest.starts_with("//") && !rest.starts_with("/*"))
        {
            tokens.push_back({.at = place[at], .end = place[end - 1] + 1, .text = text.substr(at, end - at)});
        }

        at = end;
    }

    return tokens;
}

std::vector<C_token> java_tokens(const std::string_view code)
{
    std::string text{code};

    std::ranges::replace(text, '\r', '\n');

    std::vector<C_token> tokens;

    for (std::size_t at{0}; at < text.size();)
    {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0)
        {
            ++at;

            continue;
        }

        const auto end{token_end(text, at)};

        if (const auto rest{std::string_view{text}.substr(at)}; !rest.starts_with("//") && !rest.starts_with("/*"))
        {
            tokens.push_back({.at = at, .end = end, .text = text.substr(at, end - at)});
        }

        at = end;
    }

    return tokens;
}

std::size_t directive_end(const std::string_view code, const std::size_t from)
{
    // Whether a backslash at an index joins the line to the next, and where the newline it joins at stands: gcc
    // joins across blanks after the backslash, warning as it does it.
    const auto spliced_at{[code](const std::size_t at) -> std::optional<std::size_t> {
        auto past{std::min(code.find_first_not_of(" \t", at + 1), code.size())};

        past += past < code.size() && code[past] == '\r' ? 1 : 0;

        return past < code.size() && code[past] == '\n' ? std::optional{past} : std::nullopt;
    }};

    auto commented{false};

    for (auto scan{from}; scan < code.size(); ++scan)
    {
        if (!commented)
        {
            // A raw string's newlines are its own and end no directive, and neither do a block comment's, which
            // is one blank to the preprocessor however many lines it spans.
            if (const auto raw{raw_string_end(code, scan)})
            {
                scan = *raw - 1;

                continue;
            }

            if (code.substr(scan).starts_with("/*"))
            {
                const auto close{code.find("*/", scan + 2)};

                scan = close == std::string_view::npos ? code.size() : close + 1;

                continue;
            }

            // A line comment runs to the line's end, a splice carrying it on with the directive; nothing inside it
            // opens a literal, a comment or a raw string.
            if (code.substr(scan).starts_with("//"))
            {
                commented = true;

                ++scan;

                continue;
            }

            // An ordinary literal's escapes carry their byte and its splices join, the lines joined before any
            // escape is read, so a backslash before a backslash ending the line escapes the next line's first
            // byte; one left open ends with the line; the `R` inside `"R"` opens no raw string.
            if (code[scan] == '"' || code[scan] == '\'')
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

                    if (const auto past{spliced_at(scan)})
                    {
                        scan = *past;

                        continue;
                    }

                    ++scan;

                    if (scan < code.size() && code[scan] == '\\')
                    {
                        if (const auto past{spliced_at(scan)})
                        {
                            scan = *past + 1;
                        }
                    }
                }

                continue;
            }
        }

        if (code[scan] == '\\')
        {
            if (const auto past{spliced_at(scan)})
            {
                scan = *past;

                continue;
            }
        }

        if (code[scan] == '\n')
        {
            return scan;
        }
    }

    return code.size();
}

std::vector<Include_directive> includes_of(const std::string_view code)
{
    const auto tokens{c_tokens(code)};

    std::vector<Include_directive> includes;

    for (std::size_t at{0}; at + 2 < tokens.size(); ++at)
    {
        if (tokens[at].text != "#" || tokens[at + 1].text != "include")
        {
            continue;
        }

        const auto line{static_cast<std::size_t>(std::ranges::count(code.substr(0, tokens[at].at), '\n'))};

        const auto& name{tokens[at + 2].text};

        if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        {
            includes.push_back({.name = name.substr(1, name.size() - 2), .line = line, .form = Include_form::quoted});
        }
        else if (name == "<")
        {
            // The name between the brackets, its tokens joined as they stand, `sys/types.h` among them.
            std::string joined;

            auto close{at + 3};

            // The directive ends with its line, or with the stretch where no newline follows.
            const auto newline{code.find('\n', tokens[at].at)};

            const auto end{newline == std::string_view::npos ? code.size() : newline};

            for (; close < tokens.size() && tokens[close].text != ">" && tokens[close].at < end; ++close)
            {
                joined += tokens[close].text;
            }

            includes.push_back({.name = joined, .line = line, .form = Include_form::angled});
        }
        else if (std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')
        {
            includes.push_back({.name = name, .line = line, .form = Include_form::computed});
        }
    }

    return includes;
}

void take_macros(const std::string_view code, Macros_t& macros)
{
    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 2 < tokens.size(); ++at)
    {
        if (tokens[at].text != "#" || tokens[at + 1].text != "define")
        {
            continue;
        }

        const auto& name{tokens[at + 2].text};

        if (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_')
        {
            continue;
        }

        const auto end{directive_end(code, tokens[at + 2].end)};

        auto& macro{macros[name]};

        // A parameter list touches the name; its names are the macro's own and not words of the replacement.
        auto from{at + 3};

        if (from < tokens.size() && tokens[from].text == "(" && tokens[from].at == tokens[at + 2].end)
        {
            macro.function_like = true;

            for (auto depth{0}; from < tokens.size() && tokens[from].at < end; ++from)
            {
                depth += tokens[from].text == "(" ? 1 : tokens[from].text == ")" ? -1 : 0;

                if (depth == 0)
                {
                    ++from;

                    break;
                }
            }
        }

        std::vector<std::string> replacement;

        for (; from < tokens.size() && tokens[from].at < end; ++from)
        {
            replacement.push_back(tokens[from].text);
        }

        macro.words.insert(macro.words.end(), replacement.begin(), replacement.end());

        macro.replacements.push_back(std::move(replacement));
    }
}

namespace
{

/**
 * @brief The spellings the reading gives meaning to, each as the run of tokens it is: a word, or a pointer named by
 *        an expression, `in->cur`.
 */
using Spellings_t = std::vector<std::vector<std::string>>;

/**
 * @brief The meaningful spellings as runs of tokens.
 */
[[nodiscard]] Spellings_t runs_of(const std::span<const std::string_view> meaningful)
{
    Spellings_t runs;

    for (const auto& spelling : meaningful)
    {
        std::vector<std::string> run;

        for (const auto& token : c_tokens(spelling))
        {
            run.push_back(token.text);
        }

        if (!run.empty())
        {
            runs.push_back(std::move(run));
        }
    }

    return runs;
}

/**
 * @brief Whether a sequence of words holds a meaningful spelling as a run, or a word that makes tokens the text does
 *        not show.
 */
[[nodiscard]] bool loaded(const std::span<const std::string> words, const Spellings_t& runs)
{
    for (std::size_t at{0}; at < words.size(); ++at)
    {
        const auto& word{words[at]};

        if (word == "#" || word == "##" || word == "__VA_ARGS__" || word == "return")
        {
            return true;
        }

        for (const auto& run : runs)
        {
            if (at + run.size() <= words.size() &&
                std::equal(run.begin(), run.end(), words.begin() + static_cast<std::ptrdiff_t>(at)))
            {
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief Whether a macro's replacements, and those of the macros they name, hold such a spelling.
 * @param name The macro's name, one the table holds.
 * @param macros The table.
 * @param runs The meaningful spellings.
 * @param opacity The answers so far, a name on its way to an answer standing as not opaque, since a cycle adds
 *        nothing to what its members hold.
 * @return True when using the macro could put such a spelling in an action.
 */
[[nodiscard]] bool opaque(
        const std::string_view name, const Macros_t& macros, const Spellings_t& runs,
        std::map<std::string, bool, std::less<>>& opacity)
{
    if (const auto known{opacity.find(name)}; known != opacity.end())
    {
        return known->second;
    }

    opacity.emplace(std::string{name}, false);

    // A replacement that is a plain value, numbers, quoted literals, `true` and `false`, changes nothing an action
    // does whatever stands beside it. Anything else may: `#define SELF this` makes `SELF->yyinput()` the call the
    // action does not spell, and `#define STEP ++` makes `STEP YYCURSOR` a move, neither replacement holding a
    // word the reading gives meaning to. So a replacement holding any name or operator is opaque.
    const auto& words{macros.find(name)->second.words};

    const auto plain_value{[](const std::string& word) {
        return word == "true" || word == "false" || word.front() == '"' || word.front() == '\'' ||
               (std::isdigit(static_cast<unsigned char>(word.front())) != 0) ||
               (word.front() == '.' && word.size() > 1 && std::isdigit(static_cast<unsigned char>(word[1])) != 0);
    }};

    auto result{loaded(words, runs) || !std::ranges::all_of(words, plain_value)};

    for (auto at{words.begin()}; !result && at != words.end(); ++at)
    {
        result = macros.contains(*at) && opaque(*at, macros, runs, opacity);
    }

    opacity.insert_or_assign(std::string{name}, result);

    return result;
}

} // namespace

std::vector<std::vector<std::string>> plain_values(const std::string_view name, const Macros_t& macros)
{
    const auto macro{macros.find(name)};

    if (macro == macros.end())
    {
        return {};
    }

    std::map<std::string, bool, std::less<>> opacity;

    if (opaque(name, macros, Spellings_t{}, opacity))
    {
        return {};
    }

    // A transparent macro's words are plain values and name no macro, so each definition is one value as written.
    std::vector<std::vector<std::string>> values;

    for (const auto& replacement : macro->second.replacements)
    {
        if (!std::ranges::contains(values, replacement))
        {
            values.push_back(replacement);
        }
    }

    return values;
}

std::optional<std::string> macro_use(
        const std::string_view code, const Macros_t& macros, const std::span<const std::string_view> meaningful)
{
    const auto runs{runs_of(meaningful)};

    std::map<std::string, bool, std::less<>> opacity;

    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        const auto& word{tokens[at].text};

        if (!macros.contains(word))
        {
            continue;
        }

        if (opaque(word, macros, runs, opacity))
        {
            return std::format(
                    "calls the macro {}, whose replacement holds a word this reading gives meaning to, so what the "
                    "action does is out of sight until the macro is written out",
                    word);
        }

        // The arguments a call passes stand in place of the parameters, so a spelling in them is a spelling of the
        // action's; a group after any macro's name is read as its arguments, since an object-like alias of a
        // function-like macro takes the group with it.
        if (at + 1 < tokens.size() && tokens[at + 1].text == "(")
        {
            std::vector<std::string> arguments;

            auto scan{at + 1};

            for (auto depth{0UZ}; scan < tokens.size(); ++scan)
            {
                depth += tokens[scan].text == "(" ? 1 : tokens[scan].text == ")" ? -1 : 0;

                if (depth == 0)
                {
                    break;
                }

                arguments.push_back(tokens[scan].text);
            }

            const auto opaque_argument{std::ranges::any_of(arguments, [&](const std::string& argument) {
                return macros.contains(argument) && opaque(argument, macros, runs, opacity);
            })};

            if (loaded(arguments, runs) || opaque_argument)
            {
                return std::format(
                        "passes to the macro {} an argument it may put to a use the action does not spell, so what "
                        "the action does is out of sight until the macro is written out",
                        word);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> conditional_use(const std::string_view code)
{
    static constexpr std::string_view directives[]{"if", "ifdef", "ifndef", "elif", "else", "endif"};

    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 1 < tokens.size(); ++at)
    {
        if (tokens[at].text == "#" &&
            std::ranges::find(directives, tokens[at + 1].text) != std::ranges::end(directives))
        {
            return std::format(
                    "holds a #{} directive, and which arm of a conditional is live is the build's to decide, so "
                    "the action is out of sight until it is written without one",
                    tokens[at + 1].text);
        }
    }

    return std::nullopt;
}

std::optional<std::string> directive_use(const std::string_view code)
{
    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 1 < tokens.size(); ++at)
    {
        if (tokens[at].text == "#" && !tokens[at + 1].text.empty() &&
            std::isalpha(static_cast<unsigned char>(tokens[at + 1].text.front())) != 0)
        {
            return std::format(
                    "holds a #{} directive, which defines or conditions code this reading does not follow, so the "
                    "action is out of sight until it is written without one",
                    tokens[at + 1].text);
        }
    }

    return std::nullopt;
}

/**
 * @brief The tokens of an action with the body of every class, struct or union declared in it left out, since a
 *        `return` in a method of one returns from that method and not from the action.
 *
 * `{ struct Local { int f() { return 7; } }; Local local; (void)local.f(); }` returns nothing from the rule, and
 * the scanner flex builds from it discards the match; taking the method's return for the action's reported the
 * rule as emitting a token the scanner never emits. The body is the brace group that follows the keyword and its
 * name, so the braced value of `struct holder h = {custom};`, which follows an `=`, is no body and stays.
 * @param tokens The action's tokens.
 * @return The tokens outside every such body.
 */
[[nodiscard]] std::vector<C_token> outside_local_types(std::vector<C_token> tokens)
{
    static constexpr std::string_view types[]{"struct", "class", "union"};

    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        if (std::ranges::find(types, tokens[at].text) == std::ranges::end(types))
        {
            continue;
        }

        // A definition begins a statement, so the keyword stands first or after a `;`, a brace or a label's colon,
        // with declaration specifiers allowed between, `static struct Helper { ... } h;`; the `class` of a template
        // parameter list, `[]<class T>()`, follows a `<` or a `,` and defines nothing.
        static constexpr std::string_view specifiers[]{"typedef", "static",   "const",    "constexpr",
                                                       "inline",  "extern",   "volatile", "thread_local",
                                                       "mutable", "register", "constinit"};

        auto start{at};

        while (start > 0 && std::ranges::find(specifiers, tokens[start - 1].text) != std::ranges::end(specifiers))
        {
            --start;
        }

        if (start > 0 && tokens[start - 1].text != ";" && tokens[start - 1].text != "{" &&
            tokens[start - 1].text != "}" && tokens[start - 1].text != ":")
        {
            continue;
        }

        // The head runs from the keyword to the body's brace: a name, `final`, a base clause, an attribute or an
        // `alignas(...)` may stand between, their own groups stepped over, while an `=` or a `;` before any brace
        // says the keyword opened no class body at all, `struct holder h = {custom};` and `struct S;` among them.
        auto open{at + 1};

        for (auto groups{0}; open < tokens.size(); ++open)
        {
            const auto& piece{tokens[open].text};

            groups += piece == "(" || piece == "[" ? 1 : piece == ")" || piece == "]" ? -1 : 0;

            if (groups == 0 && (piece == "{" || piece == "=" || piece == ";"))
            {
                break;
            }
        }

        if (open >= tokens.size() || tokens[open].text != "{")
        {
            continue;
        }

        auto close{open};

        for (auto depth{0}; close < tokens.size(); ++close)
        {
            depth += tokens[close].text == "{" ? 1 : tokens[close].text == "}" ? -1 : 0;

            if (depth == 0)
            {
                break;
            }
        }

        if (close == tokens.size())
        {
            break;
        }

        tokens.erase(
                tokens.begin() + static_cast<std::ptrdiff_t>(open),
                tokens.begin() + static_cast<std::ptrdiff_t>(close) + 1);

        at = open > 0 ? open - 1 : 0;
    }

    return tokens;
}

[[nodiscard]] std::vector<C_token> outside_lambdas(std::vector<C_token> tokens)
{
    // A token an expression can end with: a name of the file's own, a number, a literal, or a closing parenthesis
    // or bracket. A closing brace depends on what it closes: a braced initializer's, whose `{` the type it
    // initializes stands before, ends an expression, so the `[` of `Predicates{}[0]()` subscripts; a block's, whose
    // `{` a parenthesis, a semicolon, another brace or a control keyword stands before, ends none, so the `[` of
    // `if (q) { } [&]{ ... }();` opens a capture list. A keyword of C's ends no expression either, so the `[` of
    // `return [](){ ... }()` opens a capture list where the `[` of `h[i](x)` subscripts.
    const auto ends_expression{[&tokens](const std::size_t at) {
        // A keyword of C's ends no expression, and neither do the alternative spellings C++ gives the operators:
        // `true and [] { ... }()` has an operator before the capture list, not a value, so the `[` opens a lambda
        // there as it does after `&&`.
        static constexpr std::string_view keywords[]{"return", "case",      "throw",    "else",     "do",     "new",
                                                     "delete", "co_return", "co_yield", "sizeof",   "and",    "or",
                                                     "not",    "xor",       "bitand",   "bitor",    "compl",  "and_eq",
                                                     "or_eq",  "xor_eq",    "not_eq",   "co_await", "alignof"};

        static constexpr std::string_view blocks[]{"if",    "else",   "for",   "while", "do",   "switch",   "try",
                                                   "catch", "struct", "class", "union", "enum", "namespace"};

        const auto named{[](const std::string_view text) {
            return std::isalnum(static_cast<unsigned char>(text.front())) != 0 || text.front() == '_';
        }};

        const auto& text{tokens[at].text};

        if (text == "}")
        {
            // The brace this one closes, and the token before it, which says which of the two it is.
            auto open{at};

            for (auto depth{0};; --open)
            {
                depth += tokens[open].text == "}" ? 1 : tokens[open].text == "{" ? -1 : 0;

                if (depth == 0 || open == 0)
                {
                    break;
                }
            }

            if (open == 0)
            {
                return false;
            }

            // The token before that brace, which says which of the two it is, a template argument list stepped
            // over on the way: a template-id ends with `>`, so the name stands before the list and not beside the
            // brace, and `std::array<bool, 1>{true}` initializes as `Predicates{}` does, the `[` after either one
            // subscripting rather than opening a capture list.
            auto head{open - 1};

            if (tokens[head].text == ">")
            {
                auto angles{0};

                auto groups{0};

                for (; head > 0; --head)
                {
                    const auto& text{tokens[head].text};

                    // An argument may be an expression with a comparison in it, `std::array<bool, (2 > 1)>`, whose
                    // signs are that comparison and not the list's, so only the ones outside every group count.
                    if (text == ")" || text == "]" || text == "}")
                    {
                        ++groups;
                    }
                    else if (text == "(" || text == "[" || text == "{")
                    {
                        groups -= groups > 0 ? 1 : 0;
                    }
                    else if (groups == 0)
                    {
                        angles += text == ">" ? 1 : text == "<" ? -1 : 0;
                    }

                    if (groups == 0 && angles == 0)
                    {
                        break;
                    }
                }

                if (angles != 0 || head == 0)
                {
                    return false;
                }

                --head;
            }

            // A parenthesised group before the brace is a type or a condition, and which it is the word before
            // the group says: `if (q) { }` and the loops open a block, while anything else opens a braced value,
            // `decltype(flags){true}` and the compound literal `(int[]){1}` among them, whose `[` after the brace
            // subscripts. A group opening a statement stands after one of the words that open a block and after
            // no other.
            if (tokens[head].text == ")")
            {
                auto scan{head};

                for (auto groups{0}; scan > 0; --scan)
                {
                    groups += tokens[scan].text == ")" ? 1 : tokens[scan].text == "(" ? -1 : 0;

                    if (groups == 0)
                    {
                        break;
                    }
                }

                if (scan == 0)
                {
                    return false;
                }

                static constexpr std::string_view conditions[]{"if", "while", "for", "switch", "catch"};

                // The group is a condition, so its brace opens a block and ends no expression; anything else
                // before the brace is a type, and the braced value it makes ends one. `if constexpr (q)` and
                // `if consteval` put a word between the `if` and the group, and are conditions all the same.
                const auto& before{tokens[scan - 1].text};

                const auto qualified{
                        (before == "constexpr" || before == "consteval") && scan >= 2 && tokens[scan - 2].text == "if"};

                return !qualified && std::ranges::find(conditions, before) == std::ranges::end(conditions);
            }

            const auto& before{tokens[head].text};

            // `if consteval { }` puts the word straight before the brace and opens a block all the same, and so do
            // its negations, `if !consteval` and `if not consteval`.
            if (before == "consteval" && head > 0)
            {
                const auto& prior{tokens[head - 1].text};

                if (prior == "if" || ((prior == "!" || prior == "not") && head > 1 && tokens[head - 2].text == "if"))
                {
                    return false;
                }
            }

            return named(before) && std::ranges::find(blocks, before) == std::ranges::end(blocks);
        }

        const auto literal{text.front() == '"' || text.front() == '\''};

        return (named(text) || literal || text == ")" || text == "]") &&
               std::ranges::find(keywords, text) == std::ranges::end(keywords);
    }};

    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        if (tokens[at].text != "[")
        {
            continue;
        }

        // An attribute opens with two brackets where a capture list opens with one, so `[[likely]] { ... }` opens
        // no lambda and the block after it is the function's own; the whole attribute is stepped over, wherever it
        // stands, since its inner bracket opens no capture list either and an expression may end before it.
        if (at + 1 < tokens.size() && tokens[at + 1].text == "[")
        {
            for (auto depth{0}; at < tokens.size(); ++at)
            {
                depth += tokens[at].text == "[" ? 1 : tokens[at].text == "]" ? -1 : 0;

                if (depth == 0)
                {
                    break;
                }
            }

            continue;
        }

        // A subscript's bracket, which an expression ends before.
        if (at > 0 && ends_expression(at - 1))
        {
            continue;
        }

        // The list's own `]`, its brackets matched.
        auto list{at};

        auto depth{0};

        for (auto depth{0}; list < tokens.size(); ++list)
        {
            depth += tokens[list].text == "[" ? 1 : tokens[list].text == "]" ? -1 : 0;

            if (depth == 0)
            {
                break;
            }
        }

        // A template parameter list stands after the capture list and nowhere else, so its angle brackets are a
        // group there and an operator anywhere after, `[](int x = (1 < 2)) { ... }` holding a comparison and not a
        // list; the brackets are stepped over before the body is looked for. A default argument of the list's own
        // may compare too, `[]<bool b = (1 < 2)>() { ... }`, so the angle brackets inside a group it opens are
        // that comparison and not the list's, and the list closes outside every group.
        auto from{list + 1};

        if (from < tokens.size() && tokens[from].text == "<")
        {
            for (auto angles{0}, groups{0}; from < tokens.size(); ++from)
            {
                const auto& text{tokens[from].text};

                if (text == "(" || text == "[" || text == "{")
                {
                    ++groups;
                }
                else if (text == ")" || text == "]" || text == "}")
                {
                    groups -= groups > 0 ? 1 : 0;
                }
                else if (groups == 0)
                {
                    angles += text == "<" ? 1 : text == ">" ? -1 : 0;
                }

                if (groups == 0 && angles == 0)
                {
                    ++from;

                    break;
                }
            }
        }

        // The body's brace: the first one outside every group after that, since a `(` or a `[` between the two
        // opens a group a brace inside belongs to, `[](int x = int{7}) { ... }` being the case that says so. A `;`
        // outside every group before it means the `[` opened no lambda at all. A requires clause stands between
        // the parameter list and the body, and its constraint may be a requires expression, whose own braces are
        // not the body's: `[]<class T>() requires requires { typename T::value_type; } { return 7; }` returns from
        // the lambda in the second group and not the first, so the clause is stepped over whole.
        auto open{tokens.end()};

        // Whether a requires clause has been opened, so that the `requires` words after it are the constraint's
        // own requires expressions and not another clause.
        auto constraining{false};

        for (auto at{from}; at < tokens.size(); ++at)
        {
            const auto& text{tokens[at].text};

            if (depth == 0 && text == "requires")
            {
                // The first `requires` opens the clause and what follows it is the constraint, an expression like
                // any other: a name, a parenthesised expression, or a requires expression, joined by `&&` and
                // `||`. Every `requires` inside that constraint opens a requires expression, whose own parameter
                // list and requirement braces are not the lambda's body, so each one is stepped over whole and
                // `requires true && requires { ... } { ... }` reaches the second group as its body.
                if (!constraining)
                {
                    constraining = true;

                    continue;
                }

                auto scan{at + 1};

                for (const auto opener : {"(", "{"})
                {
                    if (scan >= tokens.size() || tokens[scan].text != opener)
                    {
                        continue;
                    }

                    const auto closer{std::string_view{opener} == "(" ? ")" : "}"};

                    for (auto groups{0}; scan < tokens.size(); ++scan)
                    {
                        groups += tokens[scan].text == opener ? 1 : tokens[scan].text == closer ? -1 : 0;

                        if (groups == 0)
                        {
                            ++scan;

                            break;
                        }
                    }
                }

                at = scan - 1;

                continue;
            }

            if (text == "(" || text == "[")
            {
                ++depth;
            }
            else if (text == ")" || text == "]")
            {
                depth -= depth > 0 ? 1 : 0;
            }
            else if (depth == 0 && text == ";")
            {
                break;
            }
            else if (depth == 0 && text == "{")
            {
                open = tokens.begin() + static_cast<std::ptrdiff_t>(at);

                break;
            }
        }

        if (open == tokens.end())
        {
            continue;
        }

        auto close{open};

        for (auto depth{0}; close != tokens.end(); ++close)
        {
            depth += close->text == "{" ? 1 : close->text == "}" ? -1 : 0;

            if (depth == 0)
            {
                break;
            }
        }

        if (close == tokens.end())
        {
            break;
        }

        tokens.erase(open, std::next(close));
    }

    return tokens;
}

std::optional<std::string> returned(const std::string_view action, const Returning_t& returning)
{
    const auto tokens{outside_lambdas(outside_local_types(c_tokens(action)))};

    // The text from one token through the one before another, nothing between neighbours, spliced as the tokens are.
    const auto text{[action]<typename Iterator>(const Iterator from, const Iterator to) -> std::optional<std::string> {
        if (from >= to)
        {
            return std::nullopt;
        }

        return spliced(action.substr(from->at, std::prev(to)->end - from->at)).text;
    }};

    const auto returns{std::ranges::find_if(tokens, [&returning](const C_token& token) {
        return token.text == "return" ||
               std::ranges::any_of(returning, [&token](const std::string& name) { return name == token.text; });
    })};

    if (returns == tokens.end())
    {
        return std::nullopt;
    }

    const auto name{returns->text};

    const auto semicolon{std::ranges::find(returns, tokens.end(), ";", &C_token::text)};

    if (name == "return")
    {
        return text(std::next(returns), semicolon);
    }

    const auto next{std::next(returns)};

    if (next != tokens.end() && next->text == "=")
    {
        return text(std::next(next), semicolon);
    }

    if (next != tokens.end() && next->text == "(")
    {
        // The first argument: up to the comma or the close at depth one.
        std::size_t depth{0};

        for (auto inner{next}; inner != tokens.end(); ++inner)
        {
            if (inner->text == "(")
            {
                ++depth;
            }
            else if ((inner->text == ")" && --depth == 0) || (inner->text == "," && depth == 1))
            {
                return text(std::next(next), inner).value_or(std::string{name});
            }
        }
    }

    return std::string{name};
}

std::optional<std::string> returns_undecided(
        const std::string_view action, const Returning_t& returning, const bool break_discards,
        const std::set<std::string, std::less<>>& restarts, const bool chained)
{
    const auto tokens{outside_lambdas(outside_local_types(c_tokens(action)))};

    const auto names{[&returning](const C_token& token) {
        return token.text == "return" ||
               std::ranges::any_of(returning, [&token](const std::string& name) { return name == token.text; });
    }};

    // Every return of the action's own, each read as returned() reads the first, and the text of each.
    std::vector<std::string> values;

    for (auto at{tokens.begin()}; at != tokens.end(); ++at)
    {
        if (!names(*at))
        {
            continue;
        }

        const auto rest{action.substr(at->at)};

        values.push_back(returned(rest, returning).value_or(std::string{}));

        at = std::ranges::find(at, tokens.end(), ";", &C_token::text);

        if (at == tokens.end())
        {
            break;
        }
    }

    for (const auto& value : values)
    {
        if (value != values.front())
        {
            return "returns from more than one place and not the same token from each, so which token a match "
                   "emits is out of sight until the action returns one token";
        }
    }

    using Range = std::pair<std::size_t, std::size_t>;

    // The statements of a stretch, each from its first token through the one before the next: one ends at a `;`
    // or at a closing brace outside every brace of the stretch's own, unless an `else` follows, so that an `if`
    // and its `else`, and a chain of them, are one statement. A stretch of nothing but `;`, `}` and `\\` is no
    // statement, which is what flex passes on after an action's own text, a stray close or a backslash ending its
    // line.
    const auto statements{[&tokens](const Range range) {
        std::vector<Range> out;

        auto depth{0};

        auto begin{range.first};

        for (auto at{range.first}; at < range.second; ++at)
        {
            const auto& text{tokens[at].text};

            depth += text == "{" ? 1 : text == "}" ? -1 : 0;

            const auto continued{at + 1 < range.second && tokens[at + 1].text == "else"};

            const auto ends{(text == ";" || text == "}") && depth <= 0 && !continued};

            if (ends || at + 1 == range.second)
            {
                const auto garbage{std::ranges::all_of(
                        tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                        tokens.begin() + static_cast<std::ptrdiff_t>(at) + 1, [](const C_token& token) {
                            return token.text == ";" || token.text == "}" || token.text == "\\";
                        })};

                if (!garbage)
                {
                    out.emplace_back(begin, at + 1);
                }

                begin = at + 1;

                depth = std::max(depth, 0);
            }
        }

        return out;
    }};

    // Whether the last of a stretch's statements returns on every path through it, or the one before does and the
    // last is a jump, `token = BUILD; break;`, which leaves the scanner with the token stored.
    std::function<bool(Range)> returns;

    // The one jump that may follow a stored token, `token = BUILD; break;`; a `goto` is never it, its label being
    // out of sight.
    const auto jump{[&tokens, &restarts](const Range statement) {
        const auto& first{tokens[statement.first].text};

        return first == "break" || first == "continue" ||
               (first == "goto" && statement.second - statement.first > 1 &&
                restarts.contains(tokens[statement.first + 1].text));
    }};

    // The one jump that may follow the last return: a `break` after a stored token, `token = BUILD; break;`,
    // which leaves the loop with the token stored, or any jump after a `return`, which is dead code; a `continue`
    // or a restarting `goto` after a stored token rescans and drops the token, and is no such jump.
    const auto exempt{[&tokens, &jump, &returns](const Range before, const Range last) {
        return jump(last) && returns(before) &&
               (tokens[last.first].text == "break" || tokens[before.first].text == "return");
    }};

    const auto ends_returning{[&](const Range range) {
        auto own{statements(range)};

        if (own.size() >= 2 && exempt(own[own.size() - 2], own.back()))
        {
            own.pop_back();
        }

        return !own.empty() && returns(own.back());
    }};

    // Whether a statement returns on every path through it: a return of the action's own; a block whose last
    // statement does, dead code after a return counted as returning still; an `if` with an `else` whose both
    // branches do; a labelled statement that does. A loop, a `switch`, a `try` and an `if` without an `else` may
    // fall through, and are out of sight rather than followed.
    returns = [&](const Range range) -> bool {
        if (range.first >= range.second)
        {
            return false;
        }

        const auto& first{tokens[range.first].text};

        if (names(tokens[range.first]))
        {
            return true;
        }

        if (first == "{")
        {
            return tokens[range.second - 1].text == "}" && ends_returning({range.first + 1, range.second - 1});
        }

        if (first == "if")
        {
            // Past the condition: the words of `if constexpr` and the parenthesised condition, or `if !consteval`
            // and its kin, which have none.
            auto at{range.first + 1};

            for (; at < range.second && tokens[at].text != "(" && tokens[at].text != "{"; ++at)
            {
            }

            if (at < range.second && tokens[at].text == "(")
            {
                for (auto groups{0}; at < range.second; ++at)
                {
                    groups += tokens[at].text == "(" ? 1 : tokens[at].text == ")" ? -1 : 0;

                    if (groups == 0)
                    {
                        break;
                    }
                }

                ++at;
            }

            const auto branches{statements({at, range.second})};

            if (branches.size() != 1)
            {
                return false;
            }

            // The branches part at the `else` outside every brace of theirs, the one before it the then-statement
            // and what follows it the other, itself an `if` when the chain goes on.
            auto depth{0};

            for (auto split{branches.front().first}; split < branches.front().second; ++split)
            {
                depth += tokens[split].text == "{" ? 1 : tokens[split].text == "}" ? -1 : 0;

                if (depth == 0 && tokens[split].text == "else")
                {
                    return returns({branches.front().first, split}) && returns({split + 1, branches.front().second});
                }
            }

            return false;
        }

        // A label, `done: return 7;`.
        if (range.second - range.first > 2 && tokens[range.first + 1].text == ":")
        {
            return returns({range.first + 2, range.second});
        }

        return false;
    };

    // A jump anywhere but as the one after the last return ends the action on its path before any return: flex
    // writes each action as a case of a switch inside the scanning loop, so `break` and `continue` both leave it
    // with the match discarded, and re2c's actions stand in the loop the file wrote; `a+ { if (yyleng == 1) break;
    // return 7; }` returns 8 alone on "ab" under flex 2.6.4. A `goto` leaves for a label out of the action's sight,
    // `a+ { goto emit_token; }` reaching a `return 8` in the next rule's action, so it is refused whether or not
    // the action returns anywhere; a `break` or a `continue` in an action returning nowhere discards on every path.
    auto own{statements({0, tokens.size()})};

    // The one jump allowed stands last among the action's own statements, inside the braces that are the whole
    // action when it has them.
    while (own.size() == 1 && tokens[own.front().first].text == "{" && tokens[own.front().second - 1].text == "}")
    {
        own = statements({own.front().first + 1, own.front().second - 1});
    }

    const auto trailing{
            own.size() >= 2 && exempt(own[own.size() - 2], own.back()) ? own.back() :
                                                                         Range{tokens.size(), tokens.size()}};

    for (std::size_t at{0}; at < tokens.size(); ++at)
    {
        const auto& text{tokens[at].text};

        if (at >= trailing.first && at < trailing.second)
        {
            continue;
        }

        // A `goto` to a label that restarts the scan, standing before the block in the file, discards the match as
        // a `continue` does; any other label is out of sight.
        const auto restarting{text == "goto" && at + 1 < tokens.size() && restarts.contains(tokens[at + 1].text)};

        if (text == "goto" && !restarting)
        {
            return "leaves by `goto` on some path, for a label out of the action's sight, so whether a match emits "
                   "a token or is discarded is out of sight until every path returns";
        }

        if ((text == "break" || text == "continue" || restarting) && !values.empty())
        {
            return "leaves by `" + text + (restarting ? " " + tokens[at + 1].text : std::string{}) +
                   "` on some path before it returns, which the scanner takes as the action's end with the match "
                   "discarded, so whether a match emits a token or is discarded is out of sight until every path "
                   "returns";
        }

        if (text == "break" && !break_discards)
        {
            return "leaves by `break` the loop the file wrote around the scanner, so whether the scan goes on past "
                   "the match is out of sight";
        }
    }

    if (values.empty())
    {
        // flex writes `YY_BREAK` after every action, so one returning nowhere discards its match; re2c writes the
        // actions one after another and control falls from an action's end into the next rule's action, so one
        // returning nowhere must leave by a jump, `continue;` or a restarting `goto`, `"a"+ { ++count; }` before
        // `"b" { return 8; }` returning 8 for "a" under re2c 3.1.
        if (chained && !(!own.empty() && jump(own.back())))
        {
            return "ends without returning or leaving, and re2c writes the next rule's action right after it, so "
                   "what a match of this rule does is out of sight until the action leaves by a jump or a return";
        }

        return std::nullopt;
    }

    if (ends_returning({0, tokens.size()}))
    {
        return std::nullopt;
    }

    return "returns " + values.front() +
           " on some path and ends without returning on another, so whether a match emits a token or is "
           "discarded is out of sight until the action ends by returning one";
}

core::Lexer build(const Lexer_spec& spec, const std::string_view condition)
{
    return compile(token_set(spec, condition));
}

std::optional<std::size_t> brace_close(const std::string_view code)
{
    std::size_t depth{0};

    for (const auto& token : c_tokens(code))
    {
        if (token.text == "{")
        {
            ++depth;
        }
        else if (token.text == "}" && --depth == 0)
        {
            return token.end;
        }
    }

    return std::nullopt;
}

Token_set token_set(const Lexer_spec& spec, const std::string_view condition)
{
    // A generator's own number, higher winning, lands below every index on the builder's lower-wins scale.
    constexpr auto top{std::numeric_limits<std::size_t>::max() / 2};

    Token_set set;

    for (const auto index : active_rules(spec, condition))
    {
        const auto& rule{spec.rules[index]};

        if (rule.priority && *rule.priority > top)
        {
            throw Spec_error{"the priority " + std::to_string(*rule.priority) + " lies beyond the scale", rule.line};
        }

        try
        {
            set.rules.push_back(
                    {.regex = regex::parse(rule.expression, spec.definitions, spec.parse),
                     .id = index,
                     .priority = rule.priority ? top - *rule.priority : index,
                     .discarded = !rule.token.has_value()});
        }
        catch (const regex::Syntax_error& refused)
        {
            throw Spec_error{"the pattern '" + rule.pattern + "' is refused: " + refused.what(), rule.line};
        }
    }

    return set;
}

std::vector<std::size_t> active_rules(const Lexer_spec& spec, const std::string_view condition)
{
    const auto inclusive{
            condition == "INITIAL" ||
            std::ranges::any_of(spec.conditions, [condition](const Lexer_spec::Condition& declared) {
                return declared.name == condition && !declared.exclusive;
            })};

    std::vector<std::size_t> active;

    for (std::size_t index{0}; index < spec.rules.size(); ++index)
    {
        const auto& conditions{spec.rules[index].conditions};

        const auto named{std::ranges::any_of(
                conditions, [condition](const std::string& name) { return name == condition || name == "*"; })};

        if (named || (conditions.empty() && inclusive))
        {
            active.push_back(index);
        }
    }

    return active;
}

} // namespace munch::tools::audit

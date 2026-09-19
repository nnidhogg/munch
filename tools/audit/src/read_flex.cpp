#include "munch/tools/audit/read_flex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <iterator>
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
     * @param close The delimiter line, `}` for a %top block.
     * @param what What was open, named when the file ends first.
     * @throws Spec_error If the file ends before the delimiter.
     */
    void skip_through(std::string_view close, std::string_view what);

    /**
     * @brief Moves to the first line holding the given mark anywhere, the line under the cursor included, which is
     *        how flex closes a `%{` block: at the line that holds `%}`, wherever on it.
     * @param mark The mark, `%}`.
     * @param what What was open, named when the file ends first.
     * @throws Spec_error If the file ends before the mark.
     */
    void skip_to(std::string_view mark, std::string_view what);

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
 * @brief The words of a line after its first, split on blanks outside double quotes.
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
    std::vector<std::string> words;

    auto rest{trimmed(line)};

    rest.remove_prefix(std::min(rest.find_first_of(" \t"), rest.size()));

    std::string word;

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
 * @brief Where the scanner of a rule's pattern stands, which decides what a `]` is and what ends the pattern.
 */
enum class Pattern_at
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
    bracket,
};

/**
 * @brief Whether a byte is an ASCII letter, which is what a POSIX class's name is made of, tested directly so no
 *        locale is consulted.
 * @param byte The byte.
 * @return True for a to z and A to Z.
 */
[[nodiscard]] constexpr bool is_letter(const char byte) noexcept
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

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

    while (past < line.size() && is_letter(line[past]))
    {
        ++past;
    }

    return past > name && line.substr(past).starts_with(":]") ? past + 2 - at : 0;
}

/**
 * @brief Where a pattern ends on a rule line: the first blank outside a quote and a bracket, escapes honoured.
 *
 * A bracket expression holds no bracket of its own, so the scan tracks which state of one it stands in rather than
 * a depth: a `]` closes the bracket unless it is its first member, `[]a]` and `[^]a]`, a `^` negates the bracket
 * only where it opens one, so the second caret of `[^^]` is a member, and a `[:class:]` is one token whose `]` is
 * no close, which is what keeps the blank of `[[:alpha:] ]+` inside the pattern.
 * @param line The rule line, its `<...>` prefix already removed.
 * @param number The line number, for the error.
 * @return The pattern's length.
 * @throws Spec_error If a quote or bracket is left open.
 */
[[nodiscard]] std::size_t pattern_length(const std::string_view line, const std::size_t number)
{
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < line.size(); ++at)
    {
        const auto byte{line[at]};

        // An escape carries whatever byte follows it, and uses up a bracket's first position.
        if (byte == '\\')
        {
            ++at;

            state = state == Pattern_at::opened || state == Pattern_at::negated ? Pattern_at::bracket : state;

            continue;
        }

        if (state == Pattern_at::quoted)
        {
            state = byte == '"' ? Pattern_at::pattern : state;

            continue;
        }

        if (state == Pattern_at::pattern)
        {
            if (byte == ' ' || byte == '\t')
            {
                return at;
            }

            state = byte == '"' ? Pattern_at::quoted : byte == '[' ? Pattern_at::opened : state;

            continue;
        }

        if (byte == ']' && state == Pattern_at::bracket)
        {
            state = Pattern_at::pattern;

            continue;
        }

        if (byte == '^' && state == Pattern_at::opened)
        {
            state = Pattern_at::negated;

            continue;
        }

        if (byte == '[')
        {
            if (const auto length{class_length(line, at)}; length > 0)
            {
                at += length - 1;
            }
        }

        state = Pattern_at::bracket;
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

/**
 * @brief One `%option` the reader refuses by name, as the file spells it, and the line it stands on.
 */
struct Standing
{
    /**
     * @brief The word, as the file spells it.
     */
    std::string word;

    /**
     * @brief The line it stands on, counted from one.
     */
    std::size_t line;
};

/**
 * @brief What the `%option` words read so far leave standing of the settings that decide what a rule matches.
 *
 * flex sets every option before it parses a rule, so what governs the file is what the last word naming a setting
 * left standing: `%option caseless` and then `%option nocaseless` scans case-sensitively. Each `no` before a name
 * flips the setting's sense, and the case setting has off-spellings of its own, `caseful` and `case-sensitive`,
 * which a `no` turns back on.
 */
struct Settings
{
    /**
     * @brief Whether every ASCII letter of every pattern matches in either case.
     */
    bool caseless{false};

    /**
     * @brief The standing `lex-compat`, under which a counted repetition binds the whole expression before it
     *        rather than the one atom, so that `ab{3}` matches "ababab"; std::nullopt while it does not stand.
     */
    std::optional<Standing> lex_compat{};

    /**
     * @brief The standing `posix-compat`, which binds a counted repetition the same way; flex keeps the two as
     *        flags of its own and takes that binding while either stands, so `lex-compat posix-compat
     *        nolex-compat` still has it.
     */
    std::optional<Standing> posix_compat{};

    /**
     * @brief The word that left flex building its tables over the 128 bytes of ASCII, and the line it stands on;
     *        std::nullopt while they are built over all 256.
     */
    std::optional<Standing> narrowed{};

    /**
     * @brief Whether the alphabet's width was named outright and as the narrow one: `7bit` and `no8bit` name it
     *        narrow, `8bit` and `no7bit` wide, and the last word naming it decides; std::nullopt while none does,
     *        where flex's own default settles it.
     */
    std::optional<bool> named_width{};

    /**
     * @brief Whether a `full` or `fast` table was asked for, which flex reads whatever the word's sense, so that
     *        `nofull` asks for one as `full` does.
     */
    bool full_table{false};

    /**
     * @brief Whether the equivalence classes stand, which flex defaults to and `full`, `fast` and `noecs` drop.
     */
    bool classes{true};

    /**
     * @brief Whether the default rule stands, the one flex adds after the file's own that matches one byte where
     *        no rule of the file's does and echoes it; `%option nodefault` drops it, so that such a byte stops the
     *        scanner with a fatal error instead, and `%option default` restores it.
     */
    bool default_rule{true};

    /**
     * @brief The standing `reject`, which declares that an action uses REJECT where flex cannot see it, through a
     *        macro or code of the file's own; std::nullopt while it does not stand.
     */
    std::optional<Standing> reject{};

    /**
     * @brief The standing `yymore`, which declares a call of yymore() out of sight the same way.
     */
    std::optional<Standing> yymore{};
};

/**
 * @brief Whether flex builds its tables over the 128 bytes of ASCII rather than all 256, resolved as flex's own
 *        check_options() resolves it: a width named outright decides, and otherwise a full or fast table with the
 *        equivalence classes off narrows it.
 * @param settings The settings so far.
 * @return True for the narrow alphabet.
 */
[[nodiscard]] bool narrow(const Settings& settings) noexcept
{
    return settings.named_width.value_or(settings.full_table && !settings.classes);
}

/**
 * @brief Takes one `%option` word into the settings.
 * @param settings The settings so far.
 * @param word The word, its `no` prefix included.
 * @param line The line the word stands on.
 */
void take(Settings& settings, const std::string_view word, const std::size_t line)
{
    // flex lexes a `no` inside the word as a token of its own that flips the sense, and no option's name begins
    // with one, so `nonocaseless` sets the case option and `nononocaseless` clears it again. A `no` standing as a
    // word of its own reaches no name and says nothing, which is where the sense begins afresh for each word.
    auto sense{true};

    auto name{word};

    while (name.starts_with("no"))
    {
        sense = !sense;

        name.remove_prefix(2);
    }

    if (name == "caseless" || name == "case-insensitive")
    {
        settings.caseless = sense;
    }
    else if (name == "caseful" || name == "case-sensitive")
    {
        settings.caseless = !sense;
    }
    else if (name == "default")
    {
        settings.default_rule = sense;
    }
    else if (name == "lex-compat" || name == "posix-compat" || name == "reject" || name == "yymore")
    {
        auto& standing{
                name == "lex-compat"   ? settings.lex_compat :
                name == "posix-compat" ? settings.posix_compat :
                name == "reject"       ? settings.reject :
                                         settings.yymore};

        standing = sense ? std::optional{Standing{.word = std::string{word}, .line = line}} : std::nullopt;
    }
    else if (name == "7bit" || name == "8bit" || name == "full" || name == "fast" || name == "ecs")
    {
        if (name == "7bit" || name == "8bit")
        {
            settings.named_width = (name == "7bit") == sense;
        }
        else if (name == "ecs")
        {
            settings.classes = sense;
        }
        else
        {
            settings.full_table = true;

            settings.classes = false;
        }

        // The word that narrowed the alphabet is the one the refusal names, so it stands until one widens it again.
        if (!narrow(settings))
        {
            settings.narrowed.reset();
        }
        else if (!settings.narrowed)
        {
            settings.narrowed = Standing{.word = std::string{word}, .line = line};
        }
    }
}

/**
 * @brief Refuses the file when a standing option changes what its rules match in a way the byte-level reading
 *        cannot follow, naming the option and the line it stands on rather than recording it and reading on.
 * @param settings The settings the definitions section left standing.
 * @throws Spec_error If such an option stands.
 */
void refuse_unmodelled(const Settings& settings)
{
    // Either flag standing takes flex's other binding, so either one refuses the file.
    if (const auto& compat{settings.lex_compat ? settings.lex_compat : settings.posix_compat})
    {
        throw Spec_error{
                "%option " + compat->word +
                        " binds a counted repetition to the whole expression before it, so that 'ab{3}' matches "
                        "\"ababab\", which the pattern parser does not read",
                compat->line};
    }

    if (settings.narrowed)
    {
        throw Spec_error{
                "%option " + settings.narrowed->word +
                        " leaves flex building its tables over the 128 bytes of ASCII, and refusing outright a "
                        "pattern that names a byte above 127, while the reading is over all 256",
                settings.narrowed->line};
    }

    // The two options exist for a use flex cannot see, through a macro or code of the file's own, and such a use is
    // out of the reading's sight too.
    if (settings.reject)
    {
        throw Spec_error{
                "%option " + settings.reject->word +
                        " declares that an action uses REJECT where flex cannot see it, which drops a match for the "
                        "next rule's, so which rule matches is not the rules' longest match and first rule",
                settings.reject->line};
    }

    if (settings.yymore)
    {
        throw Spec_error{
                "%option " + settings.yymore->word +
                        " declares that an action calls yymore() where flex cannot see it, which appends the next "
                        "match to this one, so the next token begins where this match did",
                settings.yymore->line};
    }
}

/**
 * @brief One of flex's calls that moves the bounds of a match or reruns it, which the token language has no place
 *        for, and what it does.
 */
struct Stateful
{
    /**
     * @brief The name, `REJECT` in flex's own spelling and the rest as the macros are named.
     */
    std::string_view name;

    /**
     * @brief Whether it is written as a call, a parenthesis after the name; `REJECT` stands alone.
     */
    bool call;

    /**
     * @brief What it does to the tokens, for the refusal.
     */
    std::string_view does;
};

/**
 * @brief flex's calls that move the bounds of a match or rerun it: what each does is what the refusal says.
 */
constexpr std::array stateful{
        Stateful{
                .name = "REJECT",
                .call = false,
                .does = "drops the match for the next rule's, so which rule matches is not the rules' longest match "
                        "and first rule"},
        Stateful{
                .name = "yymore",
                .call = true,
                .does = "appends the next match to this one, so the next token begins where this match did"},
        Stateful{
                .name = "yyless",
                .call = true,
                .does = "gives the end of the match back to be matched again, so the next token begins inside this "
                        "match"},
        Stateful{
                .name = "unput",
                .call = true,
                .does = "pushes a byte onto the input, so the next token is matched against bytes the input may not "
                        "hold"},
        Stateful{
                .name = "input",
                .call = true,
                .does = "consumes bytes no rule matched, so the next token begins past them"},
        Stateful{
                .name = "yyinput",
                .call = true,
                .does = "consumes bytes no rule matched, so the next token begins past them"},
};

/**
 * @brief The index just past a string or character literal or a comment opening at an index of a C action, or the
 *        index itself when none opens there, as C reads them: a literal to its closing quote, escapes carried, a
 *        `//` comment to its line's end and a block comment to its star-slash; one left open runs to the end.
 * @param code The action's text.
 * @param at The index.
 * @return The index to resume at.
 */
[[nodiscard]] std::size_t past_literal_or_comment(const std::string_view code, const std::size_t at) noexcept
{
    const auto byte{code[at]};

    if (byte == '"' || byte == '\'')
    {
        auto close{at + 1};

        while (close < code.size() && code[close] != byte)
        {
            close += code[close] == '\\' ? 2 : 1;
        }

        return std::min(close + 1, code.size());
    }

    if (byte == '/' && code.substr(at + 1).starts_with('/'))
    {
        return std::min(code.find('\n', at), code.size());
    }

    if (byte == '/' && code.substr(at + 1).starts_with('*'))
    {
        const auto close{code.find("*/", at + 2)};

        return close == std::string_view::npos ? code.size() : close + 2;
    }

    return at;
}

/**
 * @brief The first of flex's calls in an action that moves the bounds of the match or reruns it: `yymore()`,
 *        `REJECT`, `yyless()`, `unput()` and `input()` or `yyinput()`, which flex's C++ scanners name it.
 *
 * The action is read as C reads it, a comment or a literal holding none of them, and a call is a whole word followed
 * by a parenthesis and not reached through `.` or `->`, so that a field or a function of the file's own named
 * `input` is none of them; `REJECT` is a whole word in flex's spelling, capitals throughout, since flex takes
 * `reject` and `Reject` as names of the file's own. `BEGIN`, `yy_push_state`, `yy_pop_state` and `yy_top_state`
 * change the start condition and nothing else, which a token set per condition already stands for, so they are no
 * concern here.
 * @param action The action's text.
 * @return The call found, or std::nullopt when the action holds none.
 */
[[nodiscard]] std::optional<Stateful> stateful_call(const std::string_view action) noexcept
{
    const auto is_word_byte{
            [](const char byte) { return std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '_'; }};

    for (std::size_t at{0}; at < action.size();)
    {
        if (const auto past{past_literal_or_comment(action, at)}; past != at)
        {
            at = past;

            continue;
        }

        if (!is_word_byte(action[at]))
        {
            ++at;

            continue;
        }

        auto end{at};

        while (end < action.size() && is_word_byte(action[end]))
        {
            ++end;
        }

        const auto word{action.substr(at, end - at)};

        const auto through_member{at > 0 && (action[at - 1] == '.' || action[at - 1] == '>')};

        const auto next{action.find_first_not_of(" \t", end)};

        const auto called{next != std::string_view::npos && action[next] == '('};

        for (const auto& candidate : stateful)
        {
            if (word == candidate.name && (candidate.call ? called && !through_member : true))
            {
                return candidate;
            }
        }

        at = end;
    }

    return std::nullopt;
}

/**
 * @brief Whether a line is a section delimiter, as flex lexes one: `%%` at the margin, and after it anything at all,
 *        a comment, text or blanks, which flex drops with the rest of the line; an indented `%%` is no delimiter.
 * @param line The line, untrimmed.
 * @return True for a delimiter.
 */
[[nodiscard]] bool is_delimiter(const std::string_view line) noexcept
{
    return line.starts_with("%%");
}

/**
 * @brief Reads the definitions section, up to and over its `%%`, whose line names the scanner.
 * @param lines The cursor, at the file's first line.
 * @param file The file being filled.
 * @param settings The settings the `%option` lines resolve to, filled as they are read.
 * @throws Spec_error If the section never ends, a block is left open, or a definition has no pattern.
 */
void read_definitions(Lines& lines, Lexer_spec& file, Settings& settings)
{
    for (; lines.more(); lines.advance())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (is_delimiter(line))
        {
            file.line = lines.number();

            lines.advance();

            return;
        }

        if (text.starts_with("%{"))
        {
            lines.skip_to("%}", "a %{ code block");

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
            for (auto& word : words_after_first(text, lines.number()))
            {
                take(settings, word, lines.number());

                file.options.push_back(std::move(word));
            }

            continue;
        }

        if (text.starts_with("%s") || text.starts_with("%S") || text.starts_with("%x") || text.starts_with("%X"))
        {
            const auto exclusive{text[1] == 'x' || text[1] == 'X'};

            for (auto& name : words_after_first(text, lines.number()))
            {
                file.conditions.push_back({.name = std::move(name), .exclusive = exclusive});
            }

            continue;
        }

        if (text.front() == '%')
        {
            continue; // %array, %pointer and the like say nothing about the token set
        }

        // A definition: a name at the margin, blanks, and the pattern to the end of the line, a comment there
        // included, since flex takes the definition to the line's end and reads such a comment as part of it.
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

    throw Spec_error{"the file has no rules section: no line begins with %%", lines.number()};
}

/**
 * @brief Where a flex action ends, as flex 2.6.4's action scanner reads one: at the first end of a line at which its
 *        braces balance, whether it opens with a brace or reaches one later on its line, a stray close counting
 *        below zero.
 *
 * What hides a brace is what that scanner has a state for. A block comment runs to its star-slash over any number of
 * lines. A string or character literal runs to its closing quote or to the end of its line, whichever comes first, an
 * escape carrying the byte after it and a backslash before the newline carrying the literal on to the next line, as C
 * splices lines; a literal the line's end closes leaves the action open where a brace is, and where none is, flex ends
 * a rule's action there in the literal's state and never closes the code it emits for it, so that the m4 it runs stops
 * with an end of file in string, which is refused by name, while a scope's opener or close line, whose code flex
 * copies out as no rule's action, ends there like any other line. There is no state for a `//` comment, so a brace
 * after one on the line counts and a quote there opens a literal. An action opening with `%{` is a code block
 * instead, read in a state of its own with no comments or literals, which runs to the end of the first line holding
 * `%}`.
 * @param code The stretch of C the action opens, from its first byte to the end of the file.
 * @param number The line the action begins on, for the refusals.
 * @param rule Whether the code is a rule's action rather than the code on a scope's opener or close line.
 * @return The offset of the newline ending the action, or the stretch's size when the file ends it balanced.
 * @throws Spec_error If a brace, a comment or a `%{` block is left open at the end of the file, which flex refuses
 *         as an end of file inside an action, or a rule's action leaves a literal open at the end of a line where its
 *         braces balance.
 */
[[nodiscard]] std::size_t action_end(const std::string_view code, const std::size_t number, const bool rule)
{
    if (code.starts_with("%{"))
    {
        const auto close{code.find("%}")};

        if (close == std::string_view::npos)
        {
            throw Spec_error{"the action's %{ block is never closed, which flex refuses", number};
        }

        return std::min(code.find('\n', close), code.size());
    }

    std::ptrdiff_t depth{0};

    for (std::size_t at{0}; at < code.size();)
    {
        const auto byte{code[at]};

        if (byte == '"' || byte == '\'')
        {
            auto close{at + 1};

            // A backslash before a newline splices the lines; any other carries the byte after it, with whatever
            // splices stand between, and a newline after those ends the literal as it ends any other.
            while (close < code.size() && code[close] != byte && code[close] != '\n')
            {
                if (code[close] != '\\')
                {
                    ++close;
                }
                else if (code.substr(close).starts_with("\\\n"))
                {
                    close += 2;
                }
                else
                {
                    ++close;

                    while (code.substr(close).starts_with("\\\n"))
                    {
                        close += 2;
                    }

                    if (close < code.size() && code[close] != '\n')
                    {
                        ++close;
                    }
                }
            }

            if (close < code.size() && code[close] == byte)
            {
                at = close + 1;

                continue;
            }

            if (depth > 0)
            {
                at = close + 1;

                continue;
            }

            if (rule)
            {
                throw Spec_error{
                        "a quote is left open at the end of the action's line, where flex ends the action inside the "
                        "literal and never closes the code it emits for it, so that the m4 it runs stops with an end "
                        "of file in string",
                        number};
            }

            return close;
        }

        if (byte == '/' && code.substr(at + 1).starts_with('*'))
        {
            const auto close{code.find("*/", at + 2)};

            if (close == std::string_view::npos)
            {
                throw Spec_error{
                        "the action's comment is never closed, which flex refuses as an end of file inside an action",
                        number};
            }

            at = close + 2;

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
 * @brief Reads one rule from the line under the cursor and the action lines it spans, leaving the cursor after it.
 * @param lines The cursor, at a rule line.
 * @param line The rule line's text, the cursor's line with its indentation and trailing blanks removed.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The rule, or std::nullopt for an `<<EOF>>` rule.
 * @throws Spec_error If the rule has no pattern, or its action is one action_end() refuses.
 */
[[nodiscard]] std::optional<Lexer_spec::Rule> read_rule(
        Lines& lines, std::string_view line, const Returning_t& returning)
{
    auto number{lines.number()};

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

        // flex takes a prefix alone on its line as opening whatever the next line holds, since the newline after
        // it yields no token: the `{` of a scope, as bison's scanners write it, or a rule.
        if (trimmed(line).empty())
        {
            do
            {
                lines.advance();
            } while (lines.more() && trimmed(lines.current()).empty());

            if (!lines.more())
            {
                throw Spec_error{"the start-condition prefix ends the section", number};
            }

            line = trimmed(lines.current());

            number = lines.number();
        }
    }

    const auto length{pattern_length(line, number)};

    const std::string pattern{line.substr(0, length)};

    if (pattern.empty())
    {
        throw Spec_error{"a rule at the margin has no pattern", number};
    }

    // The action runs to the first end of a line at which its braces balance, as flex reads it, so one that opens
    // a brace anywhere on its line continues to the matching close, however many lines that takes; a `|` action is
    // the bar and whatever follows it on its line, which flex takes unread, so no brace or quote there counts.
    const auto tail{trimmed(line.substr(length))};

    const auto rest{lines.rest()};

    const auto opened{static_cast<std::size_t>(tail.data() - rest.data())};

    const auto end{
            tail.starts_with('|') ? std::min(rest.find('\n', opened), rest.size()) - opened :
                                    action_end(rest.substr(opened), number, pattern != "{")};

    // `<s>{` opens a start-condition scope rather than a rule, and what follows the brace on its line is code flex
    // copies out and drops, read to the same end an action is read to: a comment closing on a later line runs the
    // code on to that line, as flex 2.6.4 permits, and nothing on those lines is a rule. The caller reads the scope
    // as such.
    if (pattern == "{")
    {
        lines.advance(1 + static_cast<std::size_t>(std::ranges::count(rest.substr(opened, end), '\n')));

        return Lexer_spec::Rule{
                .pattern = pattern,
                .expression = pattern,
                .conditions = std::move(conditions),
                .action = {},
                .token = std::nullopt,
                .priority = std::nullopt,
                .line = number};
    }

    std::string action{trimmed(rest.substr(opened, end))};

    lines.advance(1 + static_cast<std::size_t>(std::ranges::count(action, '\n')));

    if (pattern == "<<EOF>>")
    {
        return std::nullopt;
    }

    // A `|` line is taken unread, as flex takes it, so what follows the bar is no call.
    if (const auto call{action.starts_with('|') ? std::nullopt : stateful_call(action)})
    {
        throw Spec_error{
                std::format(
                        "the action {} {}{}, which {}", call->call ? "calls" : "uses", call->name,
                        call->call ? "()" : "", call->does),
                number};
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
 * @brief One open start-condition scope, `<s>{` through the line opening with `}`.
 */
struct Scope
{
    /**
     * @brief How many names the scope stack held before this scope opened, which closing it restores.
     */
    std::size_t names;

    /**
     * @brief The line the scope opened on, named when the section ends with it still open.
     */
    std::size_t line;
};

/**
 * @brief Reads the rules section, up to its `%%` or the end of the file.
 * @param lines The cursor, at the section's first line.
 * @param file The file being filled.
 * @param returning The forms besides `return` an action returns a token through.
 * @return The line the section ends on: its `%%`, or the file's last line when the file ends first.
 * @throws Spec_error If a rule is malformed, a comment stands at the margin, which flex refuses as an unrecognized
 *         rule, or a start-condition scope is never closed, which flex refuses as a parse error at the section's end.
 */
[[nodiscard]] std::size_t read_rules(Lines& lines, Lexer_spec& file, const Returning_t& returning)
{
    // A start-condition scope, `<s>{` on a line of its own through a line opening with `}`, prefixes every rule
    // inside it. Scopes nest, and a rule inside one with a prefix of its own is active in the scope's conditions and
    // its own alike: flex keeps every open scope's names on one stack and a rule takes the whole stack.
    std::vector<std::string> scoped;

    std::vector<Scope> opened;

    // The section opens with a prologue, where an indented line is code flex copies into the scanner ahead of the
    // rules, which ends at the first line at the margin; from then on flex reads an indented line as a rule, in a
    // scope or out of one.
    auto prologue{true};

    while (lines.more())
    {
        const auto line{lines.current()};

        const auto text{trimmed(line)};

        if (is_delimiter(line))
        {
            break;
        }

        if (text.starts_with("%{"))
        {
            lines.skip_to("%}", "a %{ code block");

            lines.advance();

            continue;
        }

        // The close of a scope, and after it whatever code its line holds, which flex copies out and drops as it does
        // the code after a scope's opener, read to the same end an action is read to: a comment closing on a later
        // line, or a brace block, runs the code on to that line, and nothing on those lines is a rule.
        if (!opened.empty() && text.starts_with('}'))
        {
            scoped.resize(opened.back().names);

            opened.pop_back();

            const auto rest{lines.rest()};

            const auto after{static_cast<std::size_t>(text.data() - rest.data()) + 1};

            const auto end{action_end(rest.substr(after), lines.number(), false)};

            lines.advance(1 + static_cast<std::size_t>(std::ranges::count(rest.substr(after, end), '\n')));

            continue;
        }

        // A comment on a line of its own, which flex copies out as code where the line is indented, read to the same
        // end an action is read to, and refuses at the margin, where its slash begins a rule: trailing context with
        // nothing before it, an unrecognized rule to flex.
        if (text.starts_with("/*"))
        {
            if (line.front() != ' ' && line.front() != '\t')
            {
                throw Spec_error{
                        "a comment at the margin of the rules section, which flex reads as a rule and refuses as "
                        "unrecognized; indent it, as flex's manual asks",
                        lines.number()};
            }

            const auto rest{lines.rest()};

            const auto from{static_cast<std::size_t>(text.data() - rest.data())};

            const auto end{action_end(rest.substr(from), lines.number(), false)};

            lines.advance(1 + static_cast<std::size_t>(std::ranges::count(rest.substr(from, end), '\n')));

            continue;
        }

        if (prologue ? is_code(line) : text.empty())
        {
            lines.advance();

            continue;
        }

        prologue = false;

        auto rule{read_rule(lines, text, returning)};

        if (rule && rule->pattern == "{" && rule->action.empty())
        {
            opened.push_back({.names = scoped.size(), .line = rule->line});

            std::ranges::move(rule->conditions, std::back_inserter(scoped));

            continue;
        }

        if (rule)
        {
            for (const auto& name : scoped)
            {
                if (!std::ranges::contains(rule->conditions, name))
                {
                    rule->conditions.push_back(name);
                }
            }

            file.rules.push_back(std::move(*rule));
        }
    }

    // flex reads the rest of the section as the scope's body and hits a parse error at its end, so a scope left
    // open is refused here rather than read as if its `}` stood at the section's end.
    if (!opened.empty())
    {
        throw Spec_error{"a start-condition scope is never closed", opened.back().line};
    }

    // The cursor stands on the `%%`, or one past the last line once the file has ended.
    return lines.more() ? lines.number() : lines.number() - 1;
}

/**
 * @brief Appends the default rule flex adds after the file's own once the section is read: one byte, `.|\n`, in
 *        every start condition and at the lowest priority, whose action `ECHO;` returns nothing, so that a byte no
 *        rule of the file's matches is a one-byte discarded token wherever the scanner stands.
 * @param file The file, its own rules read.
 * @param line The line the rules section ends on, which the rule is given, standing on none of its own.
 */
void add_default_rule(Lexer_spec& file, const std::size_t line)
{
    file.rules.push_back(
            {.pattern = ".|\\n",
             .expression = ".|\\n",
             .conditions = {"*"},
             .action = "ECHO;",
             .token = std::nullopt,
             .priority = std::nullopt,
             .line = line});
}

/**
 * @brief Gives every `|` action the token of the first rule below it that has an action of its own; flex takes a
 *        `|` and whatever follows it on the line, a comment usually, as that continuation.
 * @param rules The rules, in file order.
 */
void share_actions(std::vector<Lexer_spec::Rule>& rules)
{
    for (auto at{rules.size()}; at > 1;)
    {
        --at;

        if (rules[at - 1].action.starts_with('|'))
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

void Lines::skip_to(const std::string_view mark, const std::string_view what)
{
    const auto opened{number()};

    // The opening line counts, so that `%{ code %}` on one line is that line.
    for (; more(); advance())
    {
        if (current().contains(mark))
        {
            return;
        }
    }

    throw Spec_error{std::string{what} + " is never closed", opened};
}

} // namespace

std::vector<Lexer_spec> read_flex(const std::string_view source, const Returning_t& returning)
{
    Lexer_spec file;

    Lines lines{source};

    Settings settings;

    read_definitions(lines, file, settings);

    refuse_unmodelled(settings);

    file.parse = {.caseless = settings.caseless};

    const auto end{read_rules(lines, file, returning)};

    share_actions(file.rules);

    // flex adds the default rule once the section is read, after every rule of the file's, unless `%option
    // nodefault` stands, under which a byte no rule matches stops the scanner with a fatal error, which is what a
    // token set answers of itself where no rule matches.
    if (settings.default_rule)
    {
        add_default_rule(file, end);
    }

    return {std::move(file)};
}

} // namespace munch::tools::audit

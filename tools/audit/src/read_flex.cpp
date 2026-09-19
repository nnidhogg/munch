#include "munch/tools/audit/read_flex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <deque>
#include <format>
#include <iterator>
#include <map>
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
 * @brief Whether a line continues on the next, its last byte a backslash, the line's own ending disregarded.
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
 * @brief The first negated POSIX class in a pattern, `[:^alpha:]`, or nothing.
 *
 * flex fills a class with its ASCII members whatever locale it runs under, its CCL_EXPR testing isascii() first,
 * and fills the negation without that test, so `[[:^alpha:]]` drops the letters of the locale flex ran under beside
 * the ASCII ones: flex 2.6.4 under fr_FR.ISO8859-1 leaves `\xE9` out of it, where under the C locale it is a
 * member. The file does not decide the locale, so the class is refused by name.
 * @param pattern The pattern or definition text.
 * @return The class as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> negated_class(const std::string_view pattern) noexcept
{
    // The states pattern_length() walks: a quote opens text only outside a bracket, where inside one it is a
    // member, `[^"[:^print:]]` holding the quote and then the class.
    auto state{Pattern_at::pattern};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        const auto byte{pattern[at]};

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
            if (const auto length{class_length(pattern, at)}; length > 0)
            {
                if (pattern.substr(at).starts_with("[:^"))
                {
                    return std::string{pattern.substr(at, length)};
                }

                at += length - 1;
            }
        }

        state = Pattern_at::bracket;
    }

    return std::nullopt;
}

/**
 * @brief The first group in a pattern that turns the case option on, `(?i:`, with `s`, `x` and `r` beside it in any
 *        order and a `-` turning what follows off, `(?s-i:` counting as none; or nothing.
 * @param pattern The pattern or definition text.
 * @return The group's opening as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> folding_group(const std::string_view pattern) noexcept
{
    for (std::size_t at{0}; at + 2 < pattern.size(); ++at)
    {
        if (pattern[at] == '\\')
        {
            ++at;

            continue;
        }

        if (!pattern.substr(at).starts_with("(?"))
        {
            continue;
        }

        auto on{true};

        for (auto flag{at + 2}; flag < pattern.size(); ++flag)
        {
            if (pattern[flag] == ':')
            {
                break;
            }

            if (pattern[flag] == '-')
            {
                on = false;
            }
            else if (pattern[flag] == 'i' && on)
            {
                return std::string{pattern.substr(at, pattern.find(':', at) - at + 1)};
            }
            else if (pattern[flag] != 's' && pattern[flag] != 'x' && pattern[flag] != 'r')
            {
                break;
            }
        }
    }

    return std::nullopt;
}

/**
 * @brief The first byte beyond ASCII a pattern spells, written out, as `\xHH` or as an octal escape, or nothing.
 *
 * Under the case option flex folds every byte of a pattern with the C library's case functions under the locale it
 * runs under, so a byte beyond ASCII gains its other case there and not under the C locale: flex 2.6.4 under
 * fr_FR.ISO8859-1 with `\xE9 return 7;` before `\xC9 return 8;` returns 7 on `\xC9`, where the C locale returns
 * 8, and a bracket member folds the same way. The file does not decide the locale, so such a pattern is refused by
 * name under the case option.
 * @param pattern The pattern or definition text.
 * @return The byte as written, or nothing.
 */
[[nodiscard]] std::optional<std::string> beyond_ascii(const std::string_view pattern) noexcept
{
    const auto hex{[](const char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
    }};

    for (std::size_t at{0}; at < pattern.size(); ++at)
    {
        if (static_cast<unsigned char>(pattern[at]) >= 0x80)
        {
            return std::string{pattern.substr(at, 1)};
        }

        if (pattern[at] != '\\' || at + 1 >= pattern.size())
        {
            continue;
        }

        // flex takes `\x` with one or two hex digits and an octal escape of one to three digits.
        if (pattern[at + 1] == 'x')
        {
            std::size_t past{at + 2};

            while (past < pattern.size() && past < at + 4 && hex(pattern[past]))
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
            ++at;
        }
    }

    return std::nullopt;
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
 * flex sets every option before it parses a rule, so what governs the file is what the last word naming a
 * setting left standing: `%option caseless` and then `%option nocaseless` scans case-sensitively. Each `no`
 * before a name flips the setting's sense, and the case setting has off-spellings of its own, `caseful` and
 * `case-sensitive`, which a `no` turns back on.
 */

/**
 * @brief A stretch of code flex copies into the scanner ahead of every action: a definitions block, an indented
 *        definitions line, the rules section's prologue or one of its code blocks. Every such stretch is read for
 *        the hook once the whole file's macros are known, since the generated scanner expands the hook under all of
 *        them, wherever they were defined.
 */
struct Copied
{
    /**
     * @brief The code.
     */
    std::string_view code;

    /**
     * @brief The line the stretch begins at.
     */
    std::size_t first;

    /**
     * @brief Which kind of stretch it is, as a refusal names it.
     */
    std::string_view what;

    /**
     * @brief The path the stretch was read from as the include reader spells it, empty for the file audited, which
     *        the stretch's own includes are resolved beside.
     */
    std::string_view path;
};

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
        Stateful{
                .name = "yyterminate",
                .call = true,
                .does = "ends the scan with no token, so whether a match emits a token is out of sight"},
        Stateful{
                .name = "YY_FLUSH_BUFFER",
                .call = false,
                .does = "discards the buffer's remaining input, so the next token is not matched against the "
                        "input's next bytes"},
        Stateful{
                .name = "yy_flush_buffer",
                .call = true,
                .does = "discards a buffer's remaining input, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yyrestart",
                .call = true,
                .does = "restarts the scan on another input, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_string",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_bytes",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_scan_buffer",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yy_switch_to_buffer",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yypush_buffer_state",
                .call = true,
                .does = "switches the scan to another buffer, so the next token is not matched against the input's "
                        "next bytes"},
        Stateful{
                .name = "yypop_buffer_state",
                .call = true,
                .does = "switches the scan back to an earlier buffer, so the next token is not matched against the "
                        "input's next bytes"}};

/**
 * @brief The first use in an action of one of flex's calls that move the bounds of the match or rerun it, `yymore()`,
 *        `REJECT`, `yyless()`, `unput()` and `input()` or `yyinput()`, which flex's C++ scanners name it, as the
 *        refusal states it: what the action does and what that does to the tokens.
 *
 * The action is read as c_tokens() reads it, a comment or a literal holding none of them. A call is a word whose next
 * token is a parenthesis, whatever blanks or comments stand between; the word standing otherwise, called through
 * parentheses, `(input)()`, taken as a pointer or declared anew, is named apart from a call, and what the action does
 * with it is out of sight, so it is refused as a use the reading cannot follow. A word whose previous token is `.` or
 * `->` is a member, a field or a function of the file's own named `input`, and none of them, unless the member is
 * `this`'s, `this->yyinput()` and `(*this).yyinput()` being the call as flex's C++ scanners, whose `yyinput` is a
 * member of the lexer class, write it; `REJECT` is a whole word in flex's spelling, capitals throughout, since flex
 * takes `reject` and `Reject` as names of the file's own. `BEGIN`, `yy_push_state`, `yy_pop_state` and `yy_top_state`
 * change the start condition and nothing else, which a token set per condition already stands for, so they are no
 * concern here.
 * @param action The action's text.
 * @param injecting_only Whether to name only the calls that push a byte onto the input or take one from it, which
 *        is what an end-of-input action, having no match to move, can still do.
 * @return What the refusal says after "the action", or std::nullopt when the action holds no such use.
 */
[[nodiscard]] std::optional<std::string> stateful_use(const std::string_view action, const bool injecting_only)
{
    const auto tokens{c_tokens(action)};

    for (std::size_t index{0}; index < tokens.size(); ++index)
    {
        const auto& word{tokens[index].text};

        const auto called{index + 1 < tokens.size() && tokens[index + 1].text == "("};

        // A member of `this`, `this->yyinput()` or `(*this).yyinput()`, is the call as flex's C++ scanners write it.
        const auto of_this{[&tokens](const std::size_t dot) {
            if (dot > 0 && tokens[dot - 1].text == "this")
            {
                return true;
            }

            // `(this)` and `(*this)`, the parentheses and the dereference stepped over.
            if (dot < 3 || tokens[dot - 1].text != ")" || tokens[dot - 2].text != "this")
            {
                return false;
            }

            const auto before{tokens[dot - 3].text == "*" ? dot - 3 : dot - 2};

            return before > 0 && tokens[before - 1].text == "(";
        }};

        // A member of the name on anything, called or parenthesised to be called, `(self->yyinput)()`, is the
        // call: flex's C++ scanners write `this->yyinput()`, and an alias of `this`, `auto* self = this;
        // self->yyinput();`, is the same call under a name the text does not resolve, so `self->yyinput()` and
        // `s.input()` alike are refused by name; a member of that name read and not called, `yylval.input = 1`, is
        // none.
        const auto through_member{
                index > 0 && (tokens[index - 1].text == "." || tokens[index - 1].text == "->") && !of_this(index - 1)};

        const auto closed{index + 1 < tokens.size() && tokens[index + 1].text == ")"};

        for (const auto& [name, call, does] : stateful)
        {
            // The calls that push a byte onto the input or take one from it, which change what is scanned next
            // wherever they stand; the others move a match, which an action without one cannot do.
            const auto injecting{name == "unput" || name == "input" || name == "yyinput"};

            if (word != name || (through_member && !called && !closed) || (injecting_only && !injecting))
            {
                continue;
            }

            if (!call)
            {
                return std::format("uses {}, which {}", name, does);
            }

            if (called)
            {
                return std::format("calls {}(), which {}", name, does);
            }

            return std::format(
                    "names {} apart from a call, so what it does with it is out of sight; called, it {}", name, does);
        }
    }

    return std::nullopt;
}

/**
 * @brief The words this reading gives meaning to in a flex action, which a macro's replacement may not hide: the
 *        calls that move or feed the match, and the one that ends the scan.
 */
constexpr std::string_view meaningful_words[]{
        "REJECT",
        "yymore",
        "yyless",
        "unput",
        "input",
        "yyinput",
        "yyterminate",
        "YY_FLUSH_BUFFER",
        "yy_flush_buffer",
        "yyrestart",
        "yy_scan_string",
        "yy_scan_bytes",
        "yy_scan_buffer",
        "yy_switch_to_buffer",
        "yypush_buffer_state",
        "yypop_buffer_state"};

/**
 * @brief The first match-moving call in a `YY_USER_ACTION` the definitions section's code defines, which flex runs
 *        before every rule's own action, so a call in it is a call in every action: under `#define YY_USER_ACTION
 *        input();` flex 2.6.4 takes "aab" through `a+` and `b` as the one token 7, the b consumed by the hook.
 *
 * The directive is found as C reads it, token by token as c_tokens() reads them, so a comment between its words and
 * a line splice inside one are no hiding place, and its replacement runs to the end of the line and on past a
 * backslash before the newline; a comment naming the macro defines nothing.
 * @param code A stretch of the definitions section's C.
 * @param macros The macros the file defines, whose opaque ones the hook may not use.
 * @param returning The forms besides `return` an action returns a token through, which the hook may not hold.
 * @return What the refusal says after "the action" and how many lines into the stretch the directive stands, or
 *         std::nullopt when no such hook is defined.
 */
[[nodiscard]] std::optional<std::pair<std::string, std::size_t>> user_action_use(
        const std::string_view code, const Macros_t& macros, const Returning_t& returning)
{
    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 2 < tokens.size(); ++at)
    {
        // flex writes `YY_BREAK` after every action as well, a `break` by default, so a definition of it of the
        // file's own runs after every action too, and what it does there is out of this reading's sight.
        if (tokens[at].text == "#" && tokens[at + 1].text == "define" && tokens[at + 2].text == "YY_BREAK")
        {
            return std::pair{
                    std::string{"is not the only hook the code defines: YY_BREAK is defined too, which flex writes "
                                "after every action, so what an action leaves is out of sight"},
                    static_cast<std::size_t>(std::ranges::count(code.substr(0, tokens[at].at), '\n'))};
        }

        if (tokens[at].text != "#" || tokens[at + 1].text != "define" || tokens[at + 2].text != "YY_USER_ACTION")
        {
            continue;
        }

        const auto end{directive_end(code, tokens[at + 2].end)};

        const auto from{tokens[at + 2].end};

        const auto replacement{code.substr(from, end - from)};

        const auto line{static_cast<std::size_t>(std::ranges::count(code.substr(0, tokens[at].at), '\n'))};

        // The hook runs before the rule's own action, so a return in it returns before the action can, a `break`
        // or a `continue` ends the rule's case without the action, and a `goto` leaves for somewhere out of sight:
        // flex 2.6.4 under `#define YY_USER_ACTION return 9;` returns 9 for every match of `a+ return 7;`.
        for (const auto& token : c_tokens(replacement))
        {
            const auto leaves{
                    token.text == "return" || token.text == "break" || token.text == "continue" ||
                    token.text == "goto" || token.text == "YY_BREAK" || std::ranges::contains(returning, token.text)};

            if (leaves)
            {
                return std::pair{
                        "holds `" + token.text +
                                "`, which ends the rule's case before its own action runs, so which token a match "
                                "emits is out of sight",
                        line};
            }
        }

        if (const auto use{stateful_use(replacement, false)})
        {
            return std::pair{*use, line};
        }

        if (const auto use{macro_use(replacement, macros, meaningful_words)})
        {
            return std::pair{*use, line};
        }
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
void read_definitions(Lines& lines, Lexer_spec& file, Settings& settings, Macros_t& macros, std::vector<Copied>& copied)
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

        // The section's code, a `%{` block, a `%top` block or an indented line, is flex's to copy through, and
        // holds one thing the token language is not blind to: a YY_USER_ACTION that moves the match.
        // The macros a stretch defines are taken now and the hook it defines is read once the section is: a helper
        // the hook calls may be defined in a later block, and the generated scanner expands the hook where it runs,
        // under every definition the section leaves. Reading each stretch as it arrives had let a hook slip through
        // whose helper stood below it.
        const auto hooked{[&macros, &copied](const std::string_view code, const std::size_t first) {
            take_macros(code, macros);

            copied.push_back({.code = code, .first = first, .what = "the definitions define", .path = {}});
        }};

        if (text.starts_with("%{"))
        {
            const auto before{lines.rest()};

            const auto first{lines.number()};

            lines.skip_to("%}", "a %{ code block");

            hooked(before.substr(0, before.size() - lines.rest().size()), first);

            continue;
        }

        if (text.starts_with("%top") && trimmed(text.substr(4)).starts_with('{'))
        {
            const auto before{lines.rest()};

            const auto first{lines.number()};

            lines.skip_through("}", "a %top block");

            hooked(before.substr(0, before.size() - lines.rest().size()), first);

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
            // A directive the section splices over several lines is one line to the compiler that reads it, so the
            // lines it continues on are read with it: flex copies them through as they stand, and a backslash at a
            // line's end joins it to the next before any of it means anything.
            const auto first{lines.number()};

            const auto before{lines.rest()};

            while (continues(lines.current()) && lines.more())
            {
                lines.advance();
            }

            hooked(before.substr(0, before.size() - lines.rest().size() + lines.current().size()), first);

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

        if (const auto negated{negated_class(pattern)})
        {
            throw Spec_error{
                    "the definition '" + std::string{name} + "' holds " + *negated +
                            ", which flex fills under the locale it runs under, dropping the bytes that locale counts "
                            "in the class beside the ASCII ones, which the file does not decide",
                    lines.number()};
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
 * after one on the line counts and a quote there opens a literal. An action opening with `%{` is a code block instead,
 * read in a state of its own with no comments or literals, which runs to the end of the first line holding `%}`.
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
 * @return The rule, an `<<EOF>>` rule among them with what its action returns, since a `|` rule above it shares that
 *         action; the caller drops the `<<EOF>>` rules once the actions are shared, as no byte matches one.
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

    // A `%{` block's code is what stands between its `%{` and its `%}`, the rest of the `%}` line dropped, as flex
    // copies it out: `%} return 7;` returns nothing.
    if (action.starts_with("%{"))
    {
        action = std::string{trimmed(action.substr(2, action.find("%}") - 2))};
    }

    // A `|` line is taken unread, as flex takes it, so what follows the bar is no call; an `<<EOF>>` action runs
    // where no match is, so what it calls moves no match, until a `|` rule above shares it, which share_actions()
    // checks.
    const auto unread{action.starts_with('|') || pattern == "<<EOF>>"};

    if (const auto use{unread ? std::nullopt : stateful_use(action, false)})
    {
        throw Spec_error{"the action " + *use, number};
    }

    // What an `<<EOF>>` action pushes onto the input is scanned after it, whatever rule matched before: flex takes
    // `<<EOF>> { unput('a'); return 9; }` on "b" through 8, 9, 7, 9, 7, the a arriving from the action. The calls
    // that move a match are another rule's concern, since an end-of-input action has none.
    if (pattern == "<<EOF>>")
    {
        if (const auto use{stateful_use(action, true)})
        {
            throw Spec_error{"the end-of-input action " + *use, number};
        }
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
[[nodiscard]] std::size_t read_rules(
        Lines& lines, Lexer_spec& file, const Returning_t& returning, Macros_t& macros, std::vector<Copied>& copied)
{
    // A start-condition scope, `<s>{` on a line of its own through a line opening with `}`, prefixes every rule
    // inside it. Scopes nest, and a rule inside one with a prefix of its own is active in the scope's conditions and
    // its own alike: flex keeps every open scope's names on one stack and a rule takes the whole stack.
    std::vector<std::string> scoped;

    std::vector<Scope> opened;

    // The section opens with a prologue, where an indented line is code flex copies into the scanner ahead of
    // the rules, which ends at the first line at the margin; from then on flex reads an indented line as a
    // rule, in a scope or out of one.
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
            // A code block in the rules section is copied into the scanner as the definitions' blocks are, ahead
            // of every action, so a YY_USER_ACTION defined there is the same hook and refused by the same name.
            const auto first{lines.number()};

            const auto before{lines.rest()};

            lines.skip_to("%}", "a %{ code block");

            const auto code{before.substr(0, before.size() - lines.rest().size())};

            take_macros(code, macros);

            copied.push_back({.code = code, .first = first, .what = "the rules' code block defines", .path = {}});

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
            // The indented code before the first rule is flex's to copy into the scanner, where it stands ahead of
            // every action, so it holds the one thing the token language is not blind to just as the definitions
            // do: a YY_USER_ACTION that moves the match. The lines it splices are read with it.
            if (prologue)
            {
                const auto first{lines.number()};

                const auto before{lines.rest()};

                while (continues(lines.current()) && lines.more())
                {
                    lines.advance();
                }

                const auto code{before.substr(0, before.size() - lines.rest().size() + lines.current().size())};

                take_macros(code, macros);

                copied.push_back({.code = code, .first = first, .what = "the rules' prologue defines", .path = {}});
            }

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
 *        `|` and whatever follows it on the line, a comment usually, as that continuation. An `<<EOF>>` rule's action
 *        was read unchecked, since it runs where no match is; shared, it runs on the `|` rule's match, `a |` over
 *        `<<EOF>> { yymore(); }` returning one token for "ab" in flex 2.6.4, so it is checked here as any action is,
 *        at the line of the rule that runs it.
 * @param rules The rules, in file order.
 * @throws Spec_error If a `|` rule shares an `<<EOF>>` action that moves the match.
 */
void share_actions(std::vector<Lexer_spec::Rule>& rules)
{
    for (auto at{rules.size()}; at > 1;)
    {
        --at;

        if (!rules[at - 1].action.starts_with('|'))
        {
            continue;
        }

        if (rules[at].pattern == "<<EOF>>" && !rules[at].action.starts_with('|'))
        {
            if (const auto use{stateful_use(rules[at].action, false)})
            {
                throw Spec_error{"the action " + *use, rules[at - 1].line};
            }
        }

        rules[at - 1].token = rules[at].token;
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

std::vector<Lexer_spec> read_flex(
        const std::string_view source, const Returning_t& returning, const Include_reader_t& includes)
{
    Lexer_spec file;

    Lines lines{source};

    Settings settings;

    Macros_t macros;

    std::vector<Copied> copied;

    read_definitions(lines, file, settings, macros, copied);

    refuse_unmodelled(settings);

    file.parse = {.caseless = settings.caseless};

    const auto end{read_rules(lines, file, returning, macros, copied)};

    share_actions(file.rules);

    // The hook and the actions are read for what the file's macros could put in them only now, with every
    // definition the scanner is compiled under collected, wherever it stood; and a conditional in any of them
    // is refused rather than decided.
    // A file the copied code includes defines what the file's own code would: `YY_USER_ACTION`, `YY_BREAK` and the
    // macros the actions use. Its text is read as copied code of the file's, its own includes after it and beside
    // it, or the include is refused where no reader reaches the file, since what it defines is out of sight; an
    // include in angle brackets the reader does not find names a system header, which defines nothing of the
    // scanner's, and one named by a macro is out of sight.
    std::deque<std::string> texts;

    auto files{0UZ};

    for (std::size_t at{0}; at < copied.size(); ++at)
    {
        for (const auto& [name, line, form] : includes_of(copied[at].code))
        {
            if (form == Include_form::computed)
            {
                throw Spec_error{
                        "the code includes a file named by the macro " + name +
                                ", which the reading does not expand, so what the file defines is out of sight, "
                                "YY_USER_ACTION and YY_BREAK among what it may define",
                        copied[at].first + line};
            }

            const auto included{includes ? includes(name, copied[at].path, form) : std::nullopt};

            if (!included && form == Include_form::angled)
            {
                continue;
            }

            if (!included)
            {
                throw Spec_error{
                        "the code includes \"" + name + "\", a file of its own " +
                                (includes ? "not found beside the file including it or on the include path" :
                                            "the reading does not reach") +
                                ", whose definitions are out of sight, YY_USER_ACTION and YY_BREAK among what it may "
                                "define",
                        copied[at].first + line};
            }

            if (++files > 64)
            {
                throw Spec_error{"the code includes more files than the reading follows", copied[at].first + line};
            }

            texts.push_back(included->text);

            take_macros(texts.back(), macros);

            texts.push_back("the included file \"" + name + "\" defines");

            texts.push_back(included->path);

            copied.push_back(
                    {.code = texts[texts.size() - 3],
                     .first = copied[at].first + line,
                     .what = texts[texts.size() - 2],
                     .path = texts.back()});
        }
    }

    for (const auto& [code, first, what, path] : copied)
    {
        if (const auto use{conditional_use(code)})
        {
            throw Spec_error{"the code " + std::string{what} + " " + *use, first};
        }

        if (const auto use{user_action_use(code, macros, returning)})
        {
            throw Spec_error{
                    "the YY_USER_ACTION " + std::string{what} + ", run before every action, " + use->first,
                    first + use->second};
        }
    }

    // The case option is the one the definitions section left standing, and a `(?i:` group turns it on inside
    // itself, for the definitions it names too; the file is held to the option where either stands, since a byte
    // a group folds may stand in a definition the group names, so once the file folds case anywhere every byte
    // beyond ASCII in it is refused, the refusal naming what folds.
    std::optional<std::string> group;

    for (const auto& [name, pattern] : file.definitions)
    {
        group = group ? group : folding_group(pattern);
    }

    for (const auto& rule : file.rules)
    {
        group = group ? group : folding_group(rule.pattern);
    }

    const auto folding{
            settings.caseless ? std::optional{std::string{"under the case option"}} :
            group             ? std::optional{"beside the group " + *group + " that folds case"} :
                                std::nullopt};

    for (const auto& [name, pattern] : file.definitions)
    {
        if (const auto beyond{folding ? beyond_ascii(pattern) : std::nullopt})
        {
            throw Spec_error{
                    "the definition '" + name + "' spells the byte " + *beyond + " " + *folding +
                            ", which flex folds under the locale it runs under, giving the byte its other case there "
                            "and not under the C locale, which the file does not decide",
                    file.line};
        }
    }

    for (const auto& rule : file.rules)
    {
        if (const auto beyond{folding ? beyond_ascii(rule.pattern) : std::nullopt})
        {
            throw Spec_error{
                    "the pattern spells the byte " + *beyond + " " + *folding +
                            ", which flex folds under the locale it runs under, giving the byte its other case there "
                            "and not under the C locale, which the file does not decide",
                    rule.line};
        }
    }

    for (const auto& rule : file.rules)
    {
        if (const auto negated{negated_class(rule.pattern)})
        {
            throw Spec_error{
                    "the pattern holds " + *negated +
                            ", which flex fills under the locale it runs under, dropping the bytes that locale counts "
                            "in the class beside the ASCII ones, which the file does not decide",
                    rule.line};
        }

        if (rule.action.starts_with('|'))
        {
            continue;
        }

        if (const auto use{directive_use(rule.action)})
        {
            throw Spec_error{"the action " + *use, rule.line};
        }

        if (const auto use{returns_undecided(rule.action, returning, true)})
        {
            throw Spec_error{"the action " + *use, rule.line};
        }

        // flex defines `YY_BREAK` as `break;` and writes it after every action; an action spelling it itself leaves
        // the rule's case on that path with the match discarded, as a `break` of its own would: flex 2.6.4 with
        // `a+ { if (yyleng == 1) YY_BREAK; return 7; }` before `b return 8;` returns 8 alone on "ab".
        for (const auto& token : c_tokens(rule.action))
        {
            if (token.text == "YY_BREAK")
            {
                throw Spec_error{
                        "the action uses YY_BREAK, which flex defines as `break;`, so it leaves the rule's case on "
                        "some path with the match discarded, and whether a match emits a token is out of sight",
                        rule.line};
            }
        }

        if (const auto use{macro_use(rule.action, macros, meaningful_words)})
        {
            throw Spec_error{"the action " + *use, rule.line};
        }
    }

    // An `<<EOF>>` rule matches no byte; it stayed until here for the action a `|` rule above it shares.
    std::erase_if(file.rules, [](const Lexer_spec::Rule& rule) { return rule.pattern == "<<EOF>>"; });

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

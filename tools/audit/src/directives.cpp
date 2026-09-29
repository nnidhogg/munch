#include "munch/tools/audit/directives.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/c_tokens.hpp"
#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements directives.hpp: which macros are opaque to the reading and the spellings they are tested against are
// private to this unit.

/**
 * @brief The spellings the reading gives meaning to, each as the run of tokens it is: a word, or a pointer named by
 *        an expression, `in->cur`.
 */
using Spellings_t = std::vector<std::vector<std::string>>;

/**
 * @brief Whether a sequence of words holds a meaningful spelling as a run, or a word that makes tokens the text does
 *        not show.
 * @param words The words, a replacement's or a call's arguments.
 * @param runs The meaningful spellings.
 * @return True when the words hold one.
 */
[[nodiscard]] bool is_loaded(const std::span<const std::string> words, const Spellings_t& runs)
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
[[nodiscard]] bool is_opaque(
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
               is_digit(word.front()) || (word.front() == '.' && word.size() > 1 && is_digit(word[1]));
    }};

    auto result{is_loaded(words, runs) || !std::ranges::all_of(words, plain_value)};

    for (auto at{words.begin()}; !result && at != words.end(); ++at)
    {
        result = macros.contains(*at) && is_opaque(*at, macros, runs, opacity);
    }

    opacity.insert_or_assign(std::string{name}, result);

    return result;
}

/**
 * @brief The meaningful spellings as runs of tokens.
 * @param meaningful The spellings, each as the text it is written in.
 * @return Each spelling's tokens, the spellings holding none left out.
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

} // namespace

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

        if (!starts_name(name))
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
        else if (starts_name(name))
        {
            includes.push_back({.name = name, .line = line, .form = Include_form::computed});
        }
    }

    return includes;
}

std::optional<Included> included_file(
        const Include_directive& directive, const std::string_view from, const Include_reader_t& includes,
        const std::string_view needed, const std::size_t line)
{
    const auto& name{directive.name};

    const auto form{directive.form};

    if (form == Include_form::computed)
    {
        throw Spec_error{
                "the code includes a file named by the macro " + name +
                        ", which the reading does not expand, so what the file defines is out of sight, " +
                        std::string{needed},
                line};
    }

    auto included{includes ? includes(name, from, form) : std::nullopt};

    if (!included && form == Include_form::angled)
    {
        return std::nullopt;
    }

    if (!included)
    {
        throw Spec_error{
                "the code includes \"" + name + "\", a file of its own " +
                        (includes ? "not found beside the file including it or on the include path" :
                                    "the reading does not reach") +
                        ", whose definitions are out of sight, " + std::string{needed},
                line};
    }

    return included;
}

std::vector<std::vector<std::string>> plain_values(const std::string_view name, const Macros_t& macros)
{
    const auto macro{macros.find(name)};

    if (macro == macros.end())
    {
        return {};
    }

    std::map<std::string, bool, std::less<>> opacity;

    if (is_opaque(name, macros, Spellings_t{}, opacity))
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

        if (is_opaque(word, macros, runs, opacity))
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
                return macros.contains(argument) && is_opaque(argument, macros, runs, opacity);
            })};

            if (is_loaded(arguments, runs) || opaque_argument)
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
            is_letter(static_cast<unsigned char>(tokens[at + 1].text.front())))
        {
            return std::format(
                    "holds a #{} directive, which defines or conditions code this reading does not follow, so the "
                    "action is out of sight until it is written without one",
                    tokens[at + 1].text);
        }
    }

    return std::nullopt;
}

} // namespace munch::tools::audit

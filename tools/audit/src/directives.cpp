#include "munch/tools/audit/directives.hpp"

#include <algorithm>
#include <array>
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
/**
 * @brief The spellings the reading gives meaning to, each as the run of tokens it is: a word, or a pointer named by an
 *        expression, `in->cur`.
 */
using Spellings_t = std::vector<std::vector<std::string>>;

/**
 * @brief Whether each macro asked about is opaque, as is_opaque() answers it, by the macro's name.
 */
using Opacity_t = std::map<std::string, bool, std::less<>>;

/**
 * @brief Returns the name an angled include spells between its brackets, its tokens joined as they stand, `sys/types.h`
 *        among them, read up to the `>` or the directive's end, which is the end of its line, or of the stretch where
 *        no newline follows.
 * @param code The stretch of C.
 * @param tokens The stretch's tokens.
 * @param at The index of the directive's `#`.
 * @return The name.
 */
[[nodiscard]] std::string angled_name(
        const std::string_view code, const std::vector<C_token>& tokens, const std::size_t at)
{
    std::string joined{};

    const auto newline{code.find('\n', tokens[at].at)};

    const auto end{newline == std::string_view::npos ? code.size() : newline};

    for (auto close{at + 3}; close < tokens.size() && tokens[close].text != ">" && tokens[close].at < end; ++close)
    {
        joined += tokens[close].text;
    }

    return joined;
}

/**
 * @brief Returns whether a sequence of words holds a meaningful spelling as a run, or a word that makes tokens the text
 *        does not show.
 * @param words The words, a replacement's or a call's arguments.
 * @param runs The meaningful spellings.
 * @return True when the words hold one.
 */
[[nodiscard]] bool is_loaded(const std::span<const std::string> words, const Spellings_t& runs)
{
    static constexpr std::array<std::string_view, 4> generative{"#", "##", "__VA_ARGS__", "return"};

    for (std::size_t at{0}; at < words.size(); ++at)
    {
        if (std::ranges::contains(generative, words[at]))
        {
            return true;
        }

        const auto stands_here{[&words, at](const std::vector<std::string>& run) {
            return at + run.size() <= words.size() && std::ranges::equal(run, words.subspan(at, run.size()));
        }};

        if (std::ranges::any_of(runs, stands_here))
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Returns whether a macro's replacements, and those of the macros they name, hold such a spelling.
 *
 * A replacement that is a plain value, numbers, quoted literals, `true` and `false`, changes nothing an action does
 * whatever stands beside it. Anything else may: `#define SELF this` makes `SELF->yyinput()` the call the action does
 * not spell, and `#define STEP ++` makes `STEP YYCURSOR` a move, neither replacement holding a word the reading gives
 * meaning to. So a replacement holding any name or operator is opaque.
 * @param name The macro's name, one the table holds.
 * @param macros The table.
 * @param runs The meaningful spellings.
 * @param opacity The answers so far, a name on its way to an answer standing as not opaque, since a cycle adds nothing
 *        to what its members hold.
 * @return True when using the macro could put such a spelling in an action.
 */
[[nodiscard]] bool is_opaque(
        const std::string_view name, const Macros_t& macros, const Spellings_t& runs, Opacity_t& opacity)
{
    if (const auto known{opacity.find(name)}; known != opacity.end())
    {
        const auto& [known_name, opaque]{*known};

        return opaque;
    }

    opacity.emplace(std::string{name}, false);

    const auto& [macro_name, macro]{*macros.find(name)};

    const auto& words{macro.words};

    const auto plain_value{[](const std::string& word) {
        return word == "true" || word == "false" || word.front() == '"' || word.front() == '\'' ||
               is_digit(word.front()) || (word.front() == '.' && word.size() > 1 && is_digit(word[1]));
    }};

    const auto names_opaque{
            [&](const std::string& word) { return macros.contains(word) && is_opaque(word, macros, runs, opacity); }};

    const auto result{
            is_loaded(words, runs) || !std::ranges::all_of(words, plain_value) ||
            std::ranges::any_of(words, names_opaque)};

    opacity.insert_or_assign(std::string{name}, result);

    return result;
}

/**
 * @brief Returns the meaningful spellings as runs of tokens.
 * @param meaningful The spellings, each as the text it is written in.
 * @return Each spelling's tokens, the spellings holding none left out.
 */
[[nodiscard]] Spellings_t runs_of(const std::span<const std::string_view> meaningful)
{
    Spellings_t runs{};

    for (const auto& spelling : meaningful)
    {
        const auto tokens{c_tokens(spelling)};

        std::vector<std::string> run{};

        std::ranges::transform(tokens, std::back_inserter(run), &C_token::text);

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

    const auto past_parameters{[&tokens](std::size_t from, const std::size_t end) {
        for (auto depth{0}; from < tokens.size() && tokens[from].at < end; ++from)
        {
            const auto& text{tokens[from].text};

            depth += depth_step(text, "(", ")");

            if (depth == 0)
            {
                return from + 1;
            }
        }

        return from;
    }};

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

            from = past_parameters(from, end);
        }

        std::vector<std::string> replacement{};

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

    std::vector<Include_directive> includes{};

    for (std::size_t at{0}; at + 2 < tokens.size(); ++at)
    {
        if (tokens[at].text != "#" || tokens[at + 1].text != "include")
        {
            continue;
        }

        const auto line{lines_before(code, tokens[at].at)};

        const auto& name{tokens[at + 2].text};

        if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        {
            const auto included{without_delimiters(name)};

            includes.push_back({.name = std::string{included}, .line = line, .form = Include_form::quoted});
        }
        else if (name == "<")
        {
            includes.push_back({.name = angled_name(code, tokens, at), .line = line, .form = Include_form::angled});
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
                std::format(
                        "the code includes a file named by the macro {}, which the reading does not expand, so what "
                        "the file defines is out of sight, {}",
                        name, needed),
                line};
    }

    auto included{includes ? includes(name, from, form) : std::nullopt};

    if (!included && form == Include_form::angled)
    {
        return std::nullopt;
    }

    if (!included)
    {
        const std::string_view missed{
                includes ? "not found beside the file including it or on the include path" :
                           "the reading does not reach"};

        throw Spec_error{
                std::format(
                        "the code includes \"{}\", a file of its own {}, whose definitions are out of sight, {}", name,
                        missed, needed),
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

    Opacity_t opacity{};

    if (is_opaque(name, macros, Spellings_t{}, opacity))
    {
        return {};
    }

    // A transparent macro's words are plain values and name no macro, so each definition is one value as written.
    std::vector<std::vector<std::string>> values{};

    const auto& [macro_name, definition]{*macro};

    for (const auto& replacement : definition.replacements)
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

    Opacity_t opacity{};

    const auto tokens{c_tokens(code)};

    // The words from the opening parenthesis to the close, the opening one kept and the closing one left out.
    const auto arguments_of{[&tokens](const std::size_t open) {
        const auto close{group_close(tokens, open, "(", ")")};

        const auto group{std::span{tokens}.subspan(open, close - open)};

        std::vector<std::string> arguments{};

        std::ranges::transform(group, std::back_inserter(arguments), &C_token::text);

        return arguments;
    }};

    const auto opaque{[&](const std::string& argument) {
        return macros.contains(argument) && is_opaque(argument, macros, runs, opacity);
    }};

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
        if (at + 1 >= tokens.size() || tokens[at + 1].text != "(")
        {
            continue;
        }

        const auto arguments{arguments_of(at + 1)};

        const auto opaque_argument{std::ranges::any_of(arguments, opaque)};

        if (is_loaded(arguments, runs) || opaque_argument)
        {
            return std::format(
                    "passes to the macro {} an argument it may put to a use the action does not spell, so what the "
                    "action does is out of sight until the macro is written out",
                    word);
        }
    }

    return std::nullopt;
}

std::optional<std::string> conditional_use(const std::string_view code)
{
    const auto tokens{c_tokens(code)};

    static constexpr std::array<std::string_view, 6> directives{"if", "ifdef", "ifndef", "elif", "else", "endif"};

    for (std::size_t at{0}; at + 1 < tokens.size(); ++at)
    {
        const auto& directive{tokens[at + 1].text};

        if (tokens[at].text == "#" && std::ranges::contains(directives, directive))
        {
            return std::format(
                    "holds a #{} directive, and which arm of a conditional is live is the build's to decide, so "
                    "the action is out of sight until it is written without one",
                    directive);
        }
    }

    return std::nullopt;
}

std::optional<std::string> directive_use(const std::string_view code)
{
    const auto tokens{c_tokens(code)};

    for (std::size_t at{0}; at + 1 < tokens.size(); ++at)
    {
        const auto& directive{tokens[at + 1].text};

        if (tokens[at].text == "#" && !directive.empty() && is_letter(static_cast<unsigned char>(directive.front())))
        {
            return std::format(
                    "holds a #{} directive, which defines or conditions code this reading does not follow, so the "
                    "action is out of sight until it is written without one",
                    directive);
        }
    }

    return std::nullopt;
}

} // namespace munch::tools::audit

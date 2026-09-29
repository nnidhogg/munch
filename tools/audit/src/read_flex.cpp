#include "munch/tools/audit/read_flex.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/tools/audit/flex_actions.hpp"
#include "munch/tools/audit/flex_lines.hpp"
#include "munch/tools/audit/flex_options.hpp"
#include "munch/tools/audit/flex_pattern.hpp"
#include "munch/tools/audit/flex_sections.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements read_flex.hpp: following the copied code's includes and the refusals read over the whole file are private
// to this unit.

/**
 * @brief Follows the includes of the code flex copies into the scanner: a file the copied code includes defines what
 *        the file's own code would, `YY_USER_ACTION`, `YY_BREAK` and the macros the actions use, so its text is read
 *        for its macros and joins the stretches as copied code of the file's, its own includes after it and beside it.
 *
 * An include is refused where no reader reaches the file, since what it defines is out of sight; an include in angle
 * brackets the reader does not find names a system header, which defines nothing of the scanner's, and one named by a
 * macro is out of sight.
 * @param copied The stretches of copied code, each file reached appended.
 * @param texts Where the texts and the names of the files reached are kept, which the appended stretches view.
 * @param macros The macros the file defines, the included files' added.
 * @param includes How the caller reaches an included file.
 * @throws Spec_error If an include is refused, or the code includes more files than the reading follows.
 */
void follow_includes(
        std::vector<Copied>& copied, std::deque<std::string>& texts, Macros_t& macros, const Include_reader_t& includes)
{
    auto files{0UZ};

    for (std::size_t at{0}; at < copied.size(); ++at)
    {
        for (const auto& directive : includes_of(copied[at].code))
        {
            const auto line{copied[at].first + directive.line};

            const auto included{included_file(
                    directive, copied[at].path, includes, "YY_USER_ACTION and YY_BREAK among what it may define",
                    line)};

            if (!included)
            {
                continue;
            }

            if (++files > 64)
            {
                throw Spec_error{"the code includes more files than the reading follows", line};
            }

            texts.push_back(included->text);

            take_macros(texts.back(), macros);

            texts.push_back("the included file \"" + directive.name + "\" defines");

            texts.push_back(included->path);

            copied.push_back(
                    {.code = texts[texts.size() - 3],
                     .first = line,
                     .what = texts[texts.size() - 2],
                     .path = texts.back()});
        }
    }
}

/**
 * @brief Refuses the file when a stretch of its copied code holds a conditional, whose live arm the build decides,
 *        or defines a `YY_USER_ACTION` that does what an action may not, since flex runs the hook before every action.
 * @param copied The stretches of copied code, the included files' among them.
 * @param macros The macros the file and the files it includes define.
 * @param returning The forms besides `return` an action returns a token through.
 * @throws Spec_error If a stretch is refused.
 */
void refuse_hooks(const std::vector<Copied>& copied, const Macros_t& macros, const Returning_t& returning)
{
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
}

/**
 * @brief Refuses the file when it folds case anywhere and spells a byte beyond ASCII, which flex folds under the
 *        locale it runs under.
 *
 * The case option is the one the definitions section left standing, and a `(?i:` group turns it on inside itself, for
 * the definitions it names too; the file is held to the option where either stands, since a byte a group folds may
 * stand in a definition the group names, so once the file folds case anywhere every byte beyond ASCII in it is
 * refused, the refusal naming what folds.
 * @param spec The specification, its definitions and rules read.
 * @param caseless Whether the case option stands.
 * @throws Spec_error If a definition or a pattern spells such a byte.
 */
void refuse_folded_bytes(const Lexer_spec& spec, const bool caseless)
{
    std::optional<std::string> group;

    for (const auto& [name, pattern] : spec.definitions)
    {
        group = group ? group : folding_group(pattern);
    }

    for (const auto& rule : spec.rules)
    {
        group = group ? group : folding_group(rule.pattern);
    }

    const auto folding{
            caseless ? std::optional{std::string{"under the case option"}} :
            group    ? std::optional{"beside the group " + *group + " that folds case"} :
                       std::nullopt};

    if (!folding)
    {
        return;
    }

    const auto tail{
            " " + *folding +
            ", which flex folds under the locale it runs under, giving the byte its other case there and not under the "
            "C locale, which the file does not decide"};

    for (const auto& [name, pattern] : spec.definitions)
    {
        if (const auto beyond{beyond_ascii(pattern)})
        {
            throw Spec_error{"the definition '" + name + "' spells the byte " + *beyond + tail, spec.line};
        }
    }

    for (const auto& rule : spec.rules)
    {
        if (const auto beyond{beyond_ascii(rule.pattern)})
        {
            throw Spec_error{"the pattern spells the byte " + *beyond + tail, rule.line};
        }
    }
}

} // namespace

std::vector<Lexer_spec> read_flex(
        const std::string_view source, const Returning_t& returning, const Include_reader_t& includes,
        const bool case_insensitive)
{
    Lexer_spec spec;

    Lines lines{source};

    // The command line's `-i` is the case option's setting before the definitions section speaks: flex 2.6.4 with `-i`
    // and `%option caseful` scans case-sensitively, the file's word standing last.
    Settings settings{.caseless = case_insensitive};

    // Recorded as the first option word, so that the file's own words stand after it and the token set is built under
    // whichever stands last, as flex settles the option.
    if (case_insensitive)
    {
        spec.options.emplace_back("case-insensitive");
    }

    Macros_t macros;

    std::vector<Copied> copied;

    read_definitions(lines, spec, settings, macros, copied);

    refuse_unmodelled(settings);

    spec.parse = {.caseless = settings.caseless};

    const auto end{read_rules(lines, spec, returning, macros, copied)};

    share_actions(spec.rules);

    // The hook and the actions are read for what the file's macros could put in them only now, with every definition
    // the scanner is compiled under collected, wherever it stood, the included files' among them; and a conditional in
    // any of them is refused rather than decided.
    std::deque<std::string> texts;

    follow_includes(copied, texts, macros, includes);

    refuse_hooks(copied, macros, returning);

    refuse_folded_bytes(spec, settings.caseless);

    for (const auto& rule : spec.rules)
    {
        if (const auto negated{negated_class(rule.pattern)})
        {
            throw Spec_error{"the pattern " + negated_class_refusal(*negated), rule.line};
        }

        refuse_action(rule, macros, returning);
    }

    // An `<<EOF>>` rule matches no byte; it stayed until here for the action a `|` rule above it shares.
    std::erase_if(spec.rules, [](const Lexer_spec::Rule& rule) { return rule.pattern == "<<EOF>>"; });

    // flex adds the default rule once the section is read, after every rule of the file's, unless `%option nodefault`
    // stands, under which a byte no rule matches stops the scanner with a fatal error, which is what a token set
    // answers of itself where no rule matches.
    if (settings.default_rule)
    {
        add_default_rule(spec, end);
    }

    return {std::move(spec)};
}

} // namespace munch::tools::audit

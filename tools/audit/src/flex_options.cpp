#include "munch/tools/audit/flex_options.hpp"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
void take_option(Settings& settings, const std::string_view word, const std::size_t line)
{
    // As flex's check_options() resolves it: a named width decides, else a full table without classes narrows.
    const auto is_narrow{
            [&settings] { return settings.named_width.value_or(settings.full_table && !settings.classes); }};

    auto sense{true};

    auto name{word};

    // flex lexes a `no` inside the word as a token of its own that flips the sense, and no option's name begins with
    // one, so `nonocaseless` sets the case option and `nononocaseless` clears it again. A `no` standing as a word of
    // its own reaches no name and says nothing, which is where the sense begins afresh for each word.
    static constexpr std::string_view negation{"no"};

    while (name.starts_with(negation))
    {
        sense = !sense;

        name.remove_prefix(negation.size());
    }

    // The word that narrowed the alphabet stands, for the refusal to name, until one widens it again.
    const auto track_narrowing{[&settings, &is_narrow, word, line] {
        if (!is_narrow())
        {
            settings.narrowed.reset();

            return;
        }

        if (!settings.narrowed)
        {
            settings.narrowed = Standing{.word = std::string{word}, .line = line};
        }
    }};

    const auto stand{[sense, word, line](std::optional<Standing>& standing) {
        standing = sense ? std::optional{Standing{.word = std::string{word}, .line = line}} : std::nullopt;
    }};

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
    else if (name == "lex-compat")
    {
        stand(settings.lex_compat);
    }
    else if (name == "posix-compat")
    {
        stand(settings.posix_compat);
    }
    else if (name == "reject")
    {
        stand(settings.reject);
    }
    else if (name == "yymore")
    {
        stand(settings.yymore);
    }
    else if (name == "7bit" || name == "8bit")
    {
        settings.named_width = (name == "7bit") == sense;

        track_narrowing();
    }
    else if (name == "ecs")
    {
        settings.classes = sense;

        track_narrowing();
    }
    else if (name == "full" || name == "fast")
    {
        settings.full_table = true;

        settings.classes = false;

        track_narrowing();
    }
}

void refuse_unmodelled(const Settings& settings)
{
    const auto refuse_standing{[](const Standing& standing, const std::string_view consequence) {
        const auto& [word, line]{standing};

        throw Spec_error{std::format("%option {}{}", word, consequence), line};
    }};

    // Either flag standing takes flex's other binding, so either one refuses the file.
    if (const auto& compat{settings.lex_compat ? settings.lex_compat : settings.posix_compat})
    {
        refuse_standing(
                *compat,
                " binds a counted repetition to the whole expression before it, so that 'ab{3}' matches \"ababab\", "
                "which the pattern parser does not read");
    }

    if (settings.narrowed)
    {
        refuse_standing(
                *settings.narrowed,
                " leaves flex building its tables over the 128 bytes of ASCII, and refusing outright a pattern that "
                "names a byte above 127, while the reading is over all 256");
    }

    // The two options exist for a use flex cannot see, through a macro or code of the file's own, and such a use is out
    // of the reading's sight too.
    if (settings.reject)
    {
        refuse_standing(
                *settings.reject,
                " declares that an action uses REJECT where flex cannot see it, which drops a match for the next "
                "rule's, so which rule matches is not the rules' longest match and first rule");
    }

    if (settings.yymore)
    {
        refuse_standing(
                *settings.yymore,
                " declares that an action calls yymore() where flex cannot see it, which appends the next match to "
                "this one, so the next token begins where this match did");
    }
}

} // namespace munch::tools::audit

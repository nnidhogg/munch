#include "munch/tools/audit/flex_options.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
namespace
{
// Implements flex_options.hpp: the width flex builds its tables over, which two of the words decide together, and the
// refusal of a standing option are private to this unit.

/**
 * @brief Whether flex builds its tables over the 128 bytes of ASCII rather than all 256, resolved as flex's own
 *        check_options() resolves it: a width named outright decides, and otherwise a full or fast table with the
 *        equivalence classes off narrows it.
 * @param settings The settings so far.
 * @return True for the narrow alphabet.
 */
[[nodiscard]] bool is_narrow(const Settings& settings) noexcept
{
    return settings.named_width.value_or(settings.full_table && !settings.classes);
}

/**
 * @brief Refuses the file for an option word left standing, naming the word and what it does that the reading does not
 *        follow.
 * @param standing The word and its line.
 * @param consequence What the option does, from the word on: " binds ...".
 * @throws Spec_error Always, at the word's line.
 */
[[noreturn]] void refuse_standing(const Standing& standing, const std::string_view consequence)
{
    throw Spec_error{"%option " + standing.word + std::string{consequence}, standing.line};
}

} // namespace

void take_option(Settings& settings, const std::string_view word, const std::size_t line)
{
    // flex lexes a `no` inside the word as a token of its own that flips the sense, and no option's name begins with
    // one, so `nonocaseless` sets the case option and `nononocaseless` clears it again. A `no` standing as a word of
    // its own reaches no name and says nothing, which is where the sense begins afresh for each word.
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
        if (!is_narrow(settings))
        {
            settings.narrowed.reset();
        }
        else if (!settings.narrowed)
        {
            settings.narrowed = Standing{.word = std::string{word}, .line = line};
        }
    }
}

void refuse_unmodelled(const Settings& settings)
{
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

#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_OPTIONS_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_OPTIONS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief The `%option` words of a flex file resolved in file order into the settings that decide what a rule matches,
 *        Settings and take_option(), and the refusals a standing option causes, refuse_unmodelled().
 *
 * flex sets every option before it parses a rule, so what governs the file is what the last word naming a setting left
 * standing, each `no` before a name flipping its sense; the settings the byte-level reading cannot follow are recorded
 * with the word and the line that set them, Standing, so that the refusal names them.
 */
namespace munch::tools::audit
{
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
 * flex sets every option before it parses a rule, so what governs the file is what the last word naming a setting left
 * standing: `%option caseless` and then `%option nocaseless` scans case-sensitively. Each `no` before a name flips the
 * setting's sense, and the case setting has off-spellings of its own, `caseful` and `case-sensitive`, which a `no`
 * turns back on.
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
 * @brief Takes one `%option` word into the settings.
 * @param settings The settings so far.
 * @param word The word, its `no` prefix included.
 * @param line The line the word stands on.
 */
void take_option(Settings& settings, std::string_view word, std::size_t line);

/**
 * @brief Refuses the file when a standing option changes what its rules match in a way the byte-level reading
 *        cannot follow, naming the option and the line it stands on rather than recording it and reading on.
 * @param settings The settings the definitions section left standing.
 * @throws Spec_error If such an option stands.
 */
void refuse_unmodelled(const Settings& settings);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_FLEX_OPTIONS_HPP

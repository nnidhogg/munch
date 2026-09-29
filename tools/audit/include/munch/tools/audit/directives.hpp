#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_DIRECTIVES_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_DIRECTIVES_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief What the preprocessor directives around a scanner define and reach, read without expanding anything: the
 *        macros a stretch of C defines, take_macros(), the files it includes and reaches, includes_of() and
 *        included_file(), and what the macros and the directives make of an action, plain_values(), macro_use(),
 *        conditional_use() and directive_use().
 *
 * No macro is expanded and no conditional decided. What the reading asks of a macro is whether using it could put in an
 * action something the action's own text does not show, which the words of its replacements decide, every definition's
 * together; a conditional or any other directive inside an action is refused rather than followed. The flex and re2c
 * readers ask it of the code the generator copies into the scanner and of the files that code includes, which the
 * caller's Include_reader_t reaches, and the ANTLR reader of the macros its actions define.
 */
namespace munch::tools::audit
{
/**
 * @brief One macro the C around a scanner defines, as far as this reading asks about it: the words of every
 *        replacement a definition gives it, and whether any definition takes parameters.
 *
 * The reading expands nothing. What it asks of a macro is whether using it could put a call, a return or a directive in
 * an action that the action's own text does not show, and that is decided from the words the replacements hold, every
 * definition's together, without deciding which definition is live where.
 */
struct Macro
{
    /**
     * @brief Whether a definition of it takes parameters, `#define CALL(f) f()`.
     */
    bool function_like{false};

    /**
     * @brief The tokens of its replacements, every definition's appended, the parameters left out.
     */
    std::vector<std::string> words;

    /**
     * @brief The tokens of each definition's replacement on its own, in the order the definitions stand, since which
     *        one is live where an action uses the name is not decided by this reading.
     */
    std::vector<std::vector<std::string>> replacements;
};

/**
 * @brief The macros a stretch of C defines, by name.
 */
using Macros_t = std::map<std::string, Macro, std::less<>>;

/**
 * @brief Adds the macros a stretch of C defines to a table: every `#define`, in every arm of every conditional,
 *        since which arm is live is not this reading's to decide. A replacement runs to the end of its logical
 *        line, past a splice and past a block comment.
 * @param code The stretch of C.
 * @param macros The table, added to.
 */
void take_macros(std::string_view code, Macros_t& macros);

/**
 * @brief A file a reader reached through an include: its text and the path it was read from, which its own
 *        includes are resolved beside.
 */
struct Included
{
    /**
     * @brief The file's text.
     */
    std::string text;

    /**
     * @brief The path the file was read from, as the reader spells it, handed back as `from` for the file's own
     *        includes.
     */
    std::string path;
};

/**
 * @brief How an include directive names its file.
 */
enum class Include_form : std::uint8_t
{
    /**
     * @brief `#include "name"`, looked for beside the including file first.
     */
    quoted,

    /**
     * @brief `#include <name>`, looked for on the compiler's include path, a system header unless the reader finds
     *        one beside the file.
     */
    angled,

    /**
     * @brief `#include NAME`, the file named by a macro, which the reading does not expand.
     */
    computed
};

/**
 * @brief How a reader reaches a file the copied code includes, `#include "hooks.h"` or `#include <hooks.h>`: the
 *        file, or std::nullopt when no such file can be found. `from` is the path of the including file as the
 *        reader returned it, empty for the file audited, and `form` how the directive names the file, so that a
 *        quoted name is looked for beside the file including it first and an angle-bracket name on the include
 *        path alone, as a compiler resolves them. The command line does so with its `--include` directories; a
 *        caller giving no reader has every quoted include refused, since what the file defines is out of sight, and
 *        an angle-bracket include the reader does not find is taken for a system header's, which defines nothing of
 *        the scanner's.
 */
using Include_reader_t =
        std::function<std::optional<Included>(std::string_view name, std::string_view from, Include_form form)>;

/**
 * @brief An include directive of a stretch of C, as c_tokens() reads it.
 */
struct Include_directive
{
    /**
     * @brief The name between the quotes or the brackets, or the macro's name for a computed include.
     */
    std::string name;

    /**
     * @brief How many lines into the stretch the directive stands.
     */
    std::size_t line;

    /**
     * @brief How the file is named.
     */
    Include_form form;
};

/**
 * @brief The include directives of a stretch of C, in order.
 * @param code The stretch.
 * @return The directives.
 */
[[nodiscard]] std::vector<Include_directive> includes_of(std::string_view code);

/**
 * @brief The file an include directive of a scanner's code reaches, as the flex and re2c readers follow one: the file
 *        the caller's reader returns, resolved beside the including file, or std::nullopt for an include in angle
 *        brackets the reader does not find, which names a system header and defines nothing of the scanner's.
 *
 * An include named by a macro is refused, since the reading does not expand the macro, and so is a quoted include where
 * the caller gives no reader or the reader does not find the file, since what the file defines is out of sight. Each
 * refusal ends with what the caller's reading needs of the file, which is where the two readers' refusals differ.
 * @param directive The directive.
 * @param from The path of the including file as the reader returned it, empty for the file audited.
 * @param includes The caller's reader, empty when it gives none.
 * @param needed What the reading needs of the file's definitions, the refusal's last clause.
 * @param line The line the refusal points at.
 * @return The file, or std::nullopt for a system header.
 * @throws Spec_error If the include is refused.
 */
[[nodiscard]] std::optional<Included> included_file(
        const Include_directive& directive, std::string_view from, const Include_reader_t& includes,
        std::string_view needed, std::size_t line);

/**
 * @brief The values a macro stands for when it is transparent to the reading: the plain values each definition's
 *        replacement holds, numbers, quoted literals, `true` and `false`, one value per definition, so that
 *        `cursors[SLOT]` under `#define SLOT 0` is `cursors[0]` to a reading that compares spellings, and under a
 *        second `#define SLOT 1` is either, since which definition is live where the name is used is not decided
 *        here. A function-like macro whose replacement is plain values stands for them whatever its arguments.
 * @param name The macro's name.
 * @param macros The macros defined.
 * @return The distinct values, in definition order; none for a name that is no macro or for an opaque one, which
 *         macro_use() refuses using where a word of meaning may stand.
 */
[[nodiscard]] std::vector<std::vector<std::string>> plain_values(std::string_view name, const Macros_t& macros);

/**
 * @brief Why a stretch of C is out of this reading's sight for what its macros could put in it, or std::nullopt
 *        when nothing about them is.
 *
 * A macro is opaque when a replacement of it holds a word the reading gives meaning to in an action, a `return`
 * or a call that moves the match among them, or a `#` or `##` that makes new tokens, or `__VA_ARGS__`, or the
 * name of another opaque macro, transitively. Using an opaque macro is refused; so is passing to any macro of the
 * file's an argument that holds such a word or such a name, since `#define CALL(f) f()` makes `CALL(input)` the
 * call the action does not spell. A macro whose replacements hold none of that, `#define MAX 10`, changes nothing
 * the reading looks for and its uses are read as ordinary text.
 * @param code The stretch of C.
 * @param macros The macros the file defines.
 * @param meaningful The words the reading gives meaning to in an action.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> macro_use(
        std::string_view code, const Macros_t& macros, std::span<const std::string_view> meaningful);

/**
 * @brief Why a stretch of C is out of this reading's sight for the conditional directives in it, or std::nullopt
 *        when it holds none: which arm of a `#if` is live is the build's to decide, so a stretch holding one is
 *        refused rather than read with one arm guessed.
 * @param code The stretch of C.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> conditional_use(std::string_view code);

/**
 * @brief Why an action is out of this reading's sight for the preprocessor directives in it, or std::nullopt when it
 *        holds none: a directive inside an action defines or conditions code the reading does not follow, so an
 *        action holding one is refused rather than read with the directive taken for code.
 * @param code The action's text.
 * @return What the refusal says after "the action", or std::nullopt.
 */
[[nodiscard]] std::optional<std::string> directive_use(std::string_view code);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_DIRECTIVES_HPP

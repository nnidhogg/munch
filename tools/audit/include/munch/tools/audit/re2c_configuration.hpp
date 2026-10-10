#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CONFIGURATION_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CONFIGURATION_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/read_re2c.hpp"

/**
 * @brief A `re2c:` configuration as re2c honours it, configure(): the case flags and the encoding it sets for the whole
 *        block it stands in, the API the scanner advances by, and the names it gives the scan pointers, Pointers_t,
 *        with the encodings a reading over bytes cannot follow refused by their own reasons, unreadable(), and what
 *        re2c reads each configuration's value as, Configuration_syntax, by the names it knows, syntax_of().
 *
 * A configuration governs the whole block wherever in it it stands, and of two assignments to one name the last is the
 * one that governs, so what configure() sets is what the block leaves, which the next pass over the block reads every
 * pattern under; the pointer names and the API carry on to the blocks after it.
 */
namespace munch::tools::audit
{
/**
 * @brief A scan pointer's name: re2c's own and the one the configurations give it.
 */
struct Pointer_name
{
    /**
     * @brief The name re2c gives the pointer, `YYCURSOR`, `YYMARKER` or `YYCTXMARKER`, which a `define:` configuration
     *        renames.
     */
    std::string canonical{};

    /**
     * @brief The name the configurations leave it under, an expression as the actions write it.
     */
    std::string name{};

    /**
     * @brief The line of the configuration that gave it the name, zero while it is re2c's own.
     */
    std::size_t line{};
};

/**
 * @brief What the file's blocks call the scan pointers, each under the canonical name a configuration renames it by:
 *        re2c carries a `define:` configuration from the block it stands in to the blocks after it, so the names are
 *        the file's and not one block's, and a block renaming none writes what the one above it left.
 */
using Pointers_t = std::vector<Pointer_name>;

/**
 * @brief What re2c 3.1 reads a configuration's value as.
 */
enum class Configuration_value : std::uint8_t
{
    /**
     * @brief A number: `0` or a decimal opening with a digit other than zero, a minus before it or none, within the
     *        range of an `int`.
     */
    number,

    /**
     * @brief A number that is not negative.
     */
    nonnegative_number,

    /**
     * @brief A string: quoted, the quote closing it on the same line, bare, a run of bytes other than a space, a tab,
     *        a newline, a NUL and `;` opening with no quote, or nothing.
     */
    string,

    /**
     * @brief A number where one follows the `=`, a string where none does, which is how re2c reads the start label's.
     */
    number_or_string,

    /**
     * @brief One of the words the configuration chooses among.
     */
    choice,
};

/**
 * @brief What re2c 3.1 reads a configuration's value as, and the words it chooses among where it chooses.
 */
struct Configuration_syntax
{
    /**
     * @brief What the value is read as.
     */
    Configuration_value value{};

    /**
     * @brief The words a choice is among, in the order re2c lists them, none for a value of another kind.
     */
    std::span<const std::string_view> choices{};
};

/**
 * @brief Returns how re2c 3.1 reads a configuration's value, by the name the configuration goes by, when re2c knows the
 *        name.
 *
 * The names are the ones re2c's configuration lexer matches, every alias among them, and nothing else: a name it does
 * not match it refuses as an unrecognized configuration, `re2c:flags:F` and `re2c:eofx` among them.
 * @param name The name, between `re2c:` and the blanks or `=` after it.
 * @return The syntax, or std::nullopt when re2c knows no configuration by the name.
 */
[[nodiscard]] std::optional<Configuration_syntax> syntax_of(std::string_view name);

/**
 * @brief Returns why an encoding is one no reading over bytes can follow, when it is: its own reason, named.
 * @param encoding The encoding.
 * @return The refusal's words, empty for the encodings the reading models, ASCII and UTF-8.
 */
[[nodiscard]] std::string unreadable(Re2c_encoding encoding);

/**
 * @brief Applies one `re2c:` configuration to what the block's configurations leave: a case flag or an encoding, under
 *        every name the manual gives it, the API the scanner advances by, and a `define:` renaming a scan pointer.
 *
 * The flags that change the reading are honoured when set in the file, under every spelling the manual's configuration
 * list gives them. A configuration governs the whole block wherever in it it stands, and the last assignment is the one
 * that governs, so what is set here is what the block leaves and the next pass reads every pattern of it under; nothing
 * here changes this pass. The reading models re2c's default API, where the scanner advances by stepping a pointer an
 * action can be seen to step too. Under the custom API it advances by calling `YYSKIP`, and backs up and restores by
 * calling `YYBACKUP` and `YYRESTORE`, which an action may call as plainly as the scanner does and which name no pointer
 * at all, so an action moving the match is out of this reading's sight. Which API the block reads under is the block's
 * last word on it, as every configuration is, so the choice is recorded and the block is refused at its end, and `api =
 * custom;` followed by `api = default;` reads under the default. The style names the spelling of those operations and
 * decides nothing while the API is the default one.
 * @param option The configuration as read, its name, `=` and its value, a number in decimal and anything else as
 *        written, `define:YYCURSOR=cur`.
 * @param line The line it stands on, where a refusal of it points.
 * @param flags The flags the block's configurations leave, the configuration's applied.
 * @param encoding_line The line of the configuration that last set the encoding, this one's where it sets one.
 * @param api_custom The line of the configuration that left the scanner under an API other than the default one, none
 *        while it reads under the default.
 * @param pointers The names the configurations give the scan pointers, the one this configuration renames renamed.
 * @throws Spec_error If the configuration sets an encoding policy other than the default.
 */
void configure(
        const std::string& option, std::size_t line, Re2c_flags& flags, std::size_t& encoding_line,
        std::optional<std::size_t>& api_custom, Pointers_t& pointers);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_RE2C_CONFIGURATION_HPP

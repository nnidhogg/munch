#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_COMMAND_LINE_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_COMMAND_LINE_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "munch/tools/audit/action_return.hpp"
#include "munch/tools/audit/file_kind.hpp"
#include "munch/tools/audit/read_re2c.hpp"
#include "munch/tools/audit/report.hpp"

/**
 * @brief The command line as munch-audit reads it: what it asked for, Options, with the bytes it requires certified,
 *        Requirement, read by parse_options(), and the usage text a command-line error prints, usage().
 */
namespace munch::tools::audit
{
/**
 * @brief A byte the command line requires every audited scanner and condition to certify.
 */
struct Requirement
{
    /**
     * @brief The byte.
     */
    unsigned char byte{};

    /**
     * @brief Whether the byte need certify only once the discarded tokens are deleted, rather than exactly.
     */
    bool modulo{};

    /**
     * @brief The byte as the command line spelled it, which the line naming an unmet requirement repeats.
     */
    std::string text{};
};

/**
 * @brief What the command line asked for.
 */
struct Options
{
    /**
     * @brief The kind every file is read as, or by its name when not given.
     */
    std::optional<Kind> kind{};

    /**
     * @brief re2c's command-line flags, for its files.
     */
    Re2c_flags re2c_flags{};

    /**
     * @brief flex's `-i`, for its files: the case option on before the file's own `%option` words.
     */
    bool flex_case_insensitive{false};

    /**
     * @brief The forms besides `return` an action returns a token through.
     */
    Returning_t returning{};

    /**
     * @brief The start conditions to audit, every one with rules when empty.
     */
    std::vector<std::string> conditions{};

    /**
     * @brief The bytes to price besides the newline and the near misses.
     */
    std::vector<unsigned char> priced{};

    /**
     * @brief The files.
     */
    std::vector<std::string> files{};

    /**
     * @brief The directories an include is looked for in, as the compiler's `-I` names them, in order.
     */
    std::vector<std::string> include_dirs{};

    /**
     * @brief The path of the input the certified-anchor supply is measured on, none when empty.
     */
    std::string input{};

    /**
     * @brief The longest window tried.
     */
    std::size_t window_limit{default_window_limit};

    /**
     * @brief Whether the output is JSON rather than text.
     */
    bool json{false};

    /**
     * @brief The bytes every audited scanner and condition must certify, in the order given.
     */
    std::vector<Requirement> required{};
};

/**
 * @brief Reads the command line.
 * @param arguments The arguments after the program's name.
 * @return The options.
 * @throws std::invalid_argument If an option is unknown, lacks its value, or no file is named.
 */
[[nodiscard]] Options parse_options(std::span<const std::string_view> arguments);

/**
 * @brief Returns the usage text, printed on a command-line error: every option, and what each exit status means.
 * @return The text.
 */
[[nodiscard]] std::string_view usage() noexcept;

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_COMMAND_LINE_HPP

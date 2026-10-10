#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ARGUMENTS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ARGUMENTS_HPP

#include <cstddef>
#include <optional>
#include <string_view>

/**
 * @brief The command-line figures a probe reads, positive_count.
 */
namespace munch::tools::probes
{
/**
 * @brief Reads a positive whole decimal count, refusing a text that is not wholly one, a zero, and a value past the
 *        range of std::size_t.
 * @param text The argument.
 * @return The count, std::nullopt for a refusal.
 */
[[nodiscard]] std::optional<std::size_t> positive_count(std::string_view text);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ARGUMENTS_HPP

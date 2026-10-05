#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GENERATED_IDENTIFIERS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GENERATED_IDENTIFIERS_HPP

#include <array>
#include <string_view>

#include "munch/tools/probes/lcg64.hpp"

/**
 * @brief The identifiers the probes' generated C-like statements draw, generated_identifiers and pick_identifier.
 */
namespace munch::tools::probes
{
/**
 * @brief The identifiers a generated statement draws from.
 */
inline constexpr std::array<std::string_view, 8> generated_identifiers{"count", "buffer", "index", "state",
                                                                       "value", "table",  "next",  "size"};

/**
 * @brief Draws one of the generated identifiers.
 * @param lcg The stream drawn from, advanced once.
 * @return The identifier.
 */
[[nodiscard]] std::string_view pick_identifier(Lcg64& lcg);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_GENERATED_IDENTIFIERS_HPP

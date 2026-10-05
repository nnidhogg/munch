#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CHUNKS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CHUNKS_HPP

#include <cstddef>
#include <string_view>
#include <vector>

/**
 * @brief The cut of a planned input into its chunks, chunks_of.
 */
namespace munch::tools::probes
{
/**
 * @brief Cuts an input into the chunks between consecutive boundaries of a plan.
 * @param input The input.
 * @param bounds The plan's boundaries, both ends included, ascending.
 * @return The chunks, in order.
 */
[[nodiscard]] std::vector<std::string_view> chunks_of(std::string_view input, const std::vector<std::size_t>& bounds);

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_CHUNKS_HPP

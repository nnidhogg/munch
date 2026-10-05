#include "munch/tools/probes/chunks.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <string_view>
#include <vector>

namespace munch::tools::probes
{
std::vector<std::string_view> chunks_of(const std::string_view input, const std::vector<std::size_t>& bounds)
{
    const auto chunk_between{
            [input](const std::size_t begin, const std::size_t end) { return input.substr(begin, end - begin); }};

    std::vector<std::string_view> chunks{};

    std::ranges::copy(bounds | std::views::adjacent_transform<2>(chunk_between), std::back_inserter(chunks));

    return chunks;
}

} // namespace munch::tools::probes

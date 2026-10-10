#include "munch/tools/probes/arguments.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace munch::tools::probes
{
std::optional<std::size_t> positive_count(const std::string_view text)
{
    std::size_t value{0};

    const auto [stopped, error]{std::from_chars(text.data(), text.data() + text.size(), value)};

    if (error != std::errc{} || stopped != text.data() + text.size() || value == 0)
    {
        return std::nullopt;
    }

    return value;
}

} // namespace munch::tools::probes

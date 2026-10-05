#include "munch/tools/probes/lcg64.hpp"

#include <cstddef>
#include <cstdint>

namespace munch::tools::probes
{
Lcg64::Lcg64(const std::uint64_t seed) noexcept : state_{seed}
{}

std::size_t Lcg64::next(const std::size_t bound) noexcept
{
    state_ = state_ * multiplier + increment;

    return static_cast<std::size_t>((state_ >> 33U) % bound);
}

} // namespace munch::tools::probes

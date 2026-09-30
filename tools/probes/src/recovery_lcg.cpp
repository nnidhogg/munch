#include "munch/tools/probes/recovery_lcg.hpp"

#include <cstdint>

namespace munch::tools::probes
{
Lcg::Lcg(const std::uint32_t seed) noexcept : state_{seed}
{}

std::uint32_t Lcg::bounded(const std::uint32_t span) noexcept
{
    while (true)
    {
        const auto x{next()};

        const auto m{static_cast<std::uint64_t>(x) * span};

        if (static_cast<std::uint32_t>(m) >= span || static_cast<std::uint32_t>(m) >= (0U - span) % span)
        {
            return static_cast<std::uint32_t>(m >> 32U);
        }
    }
}

std::uint32_t Lcg::next() noexcept
{
    state_ = state_ * 1664525U + 1013904223U;

    return state_ ^ (state_ >> 16U);
}

char Lcg::byte() noexcept
{
    state_ = state_ * 1664525U + 1013904223U;

    return static_cast<char>((state_ >> 16U) & 0xffU);
}

} // namespace munch::tools::probes

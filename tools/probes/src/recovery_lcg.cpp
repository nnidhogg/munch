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
        const auto draw{next()};

        const auto product{static_cast<std::uint64_t>(draw) * span};

        const auto low{static_cast<std::uint32_t>(product)};

        if (low >= span || low >= (0U - span) % span)
        {
            return static_cast<std::uint32_t>(product >> 32U);
        }
    }
}

std::uint32_t Lcg::next() noexcept
{
    const auto state{advance()};

    return state ^ (state >> 16U);
}

std::uint32_t Lcg::advance() noexcept
{
    state_ = state_ * multiplier + increment;

    return state_;
}

char Lcg::byte() noexcept
{
    const auto state{advance()};

    return static_cast<char>((state >> 16U) & 0xFFU);
}

} // namespace munch::tools::probes

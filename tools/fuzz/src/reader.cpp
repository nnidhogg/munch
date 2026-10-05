#include "munch/tools/fuzz/reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace munch::tools::fuzz
{
Reader::Reader(const std::span<const std::uint8_t> data)
    : bytes_{reinterpret_cast<const char*>(data.data()), data.size()}
{}

std::uint8_t Reader::byte() noexcept
{
    return position_ < bytes_.size() ? static_cast<std::uint8_t>(bytes_[position_++]) : 0;
}

std::string Reader::take(const std::size_t count)
{
    const auto taken{bytes_.substr(position_, count)};

    position_ += taken.size();

    return std::string{taken};
}

std::string Reader::remainder() const
{
    return std::string{bytes_.substr(position_)};
}

void require(const bool condition)
{
    if (!condition)
    {
        __builtin_trap();
    }
}

} // namespace munch::tools::fuzz

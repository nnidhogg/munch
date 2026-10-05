#include "munch/core/exceptions/state_limit_error.hpp"

#include <format>

namespace munch::core
{
State_limit_error::State_limit_error(const std::size_t limit)
    : std::runtime_error{std::format("Determinization exceeded the configured state limit of {}", limit)}, limit_{limit}
{}

std::size_t State_limit_error::limit() const noexcept
{
    return limit_;
}

} // namespace munch::core

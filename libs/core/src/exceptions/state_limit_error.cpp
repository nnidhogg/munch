#include "munch/core/exceptions/state_limit_error.hpp"

#include <string>

namespace munch::core
{
State_limit_error::State_limit_error(const std::size_t limit)
    : std::runtime_error{"Determinization exceeded the configured state limit of " + std::to_string(limit)}
    , limit_{limit}
{}

std::size_t State_limit_error::limit() const noexcept
{
    return limit_;
}

} // namespace munch::core

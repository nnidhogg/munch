#include "munch/tools/probes/assertions.hpp"

#include <iostream>
#include <string_view>

namespace munch::tools::probes
{
void Assertions::expect(const bool condition, const std::string_view what)
{
    if (!condition)
    {
        ++failures_;

        std::cout << "fail: " << what << "\n";
    }
}

bool Assertions::has_failures() const noexcept
{
    return failures_ != 0;
}

} // namespace munch::tools::probes

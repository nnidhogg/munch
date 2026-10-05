#include "munch/tools/probes/generated_identifiers.hpp"

#include <string_view>

#include "munch/tools/probes/lcg64.hpp"

namespace munch::tools::probes
{
std::string_view pick_identifier(Lcg64& lcg)
{
    const auto drawn{lcg.next(generated_identifiers.size())};

    return generated_identifiers[drawn];
}

} // namespace munch::tools::probes

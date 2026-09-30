#include "munch/tools/probes/gate_totals.hpp"

#include <cstddef>
#include <optional>
#include <string>

#include "munch/core/lexer.hpp"

namespace munch::tools::probes
{
void Gate_totals::cross_check(
        const core::Lexer& lexer, const std::string& window, const std::optional<std::size_t>& model)
{
    ++port_checks;

    const auto library{lexer.is_split_window(window)};

    const auto agrees{library == model};

    if (!agrees)
    {
        ++port_disagreements;
    }
}

} // namespace munch::tools::probes

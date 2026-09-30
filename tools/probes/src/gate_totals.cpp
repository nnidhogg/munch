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

    port_disagreements += lexer.is_split_window(window) == model ? 0 : 1;
}

} // namespace munch::tools::probes

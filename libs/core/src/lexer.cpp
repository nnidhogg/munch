#include "munch/core/lexer.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace munch::core
{
std::optional<std::size_t> Lexer::next_certified_start(const std::string_view input, const std::size_t from) const
{
    const auto found{next_certified_evidence(input, from)};

    const auto start_of{[](const Certified_start& certified) { return certified.start; }};

    return found.transform(start_of);
}

std::optional<Lexer::Certified_start> Lexer::next_certified_evidence(
        const std::string_view input, const std::size_t from) const
{
    const auto has_byte_certificates{simulator_.has_split_points()};

    Window_planner planner{};

    for (std::size_t at{from}; at < input.size(); ++at)
    {
        if (has_byte_certificates && is_split_point(input[at]))
        {
            return Certified_start{.start = at, .evidence_begin = at, .evidence_end = at + 1, .window = false};
        }

        const auto found{planner.window_at(simulator_, input.begin(), input.size(), at)};

        if (!found)
        {
            continue;
        }

        const auto [origin, length]{*found};

        return Certified_start{.start = at + origin, .evidence_begin = at, .evidence_end = at + length, .window = true};
    }

    return std::nullopt;
}

bool Lexer::rescue_free() const
{
    const auto [witness, exhaustive]{rescue()};

    return exhaustive && witness.empty();
}

} // namespace munch::core

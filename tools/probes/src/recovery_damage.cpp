#include "munch/tools/probes/recovery_damage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/tools/probes/recovery_lcg.hpp"

namespace munch::tools::probes
{
std::size_t failure_offset(const core::Lexer& lexer, const std::string_view input)
{
    return lexer.tokenize_all<figures::Token>(input, [](const figures::Token, const std::size_t) {});
}

std::vector<std::size_t> boundaries(const core::Lexer& lexer, const std::string_view input)
{
    std::vector<std::size_t> begins{};

    std::size_t at{0};

    const auto consumed{lexer.tokenize_all<figures::Token>(input, [&](const figures::Token, const std::size_t length) {
        begins.push_back(at);

        at += length;
    })};

    if (consumed != input.size())
    {
        std::fprintf(stderr, "corpus not completely tokenizable: %zu of %zu\n", consumed, input.size());

        std::exit(EXIT_FAILURE);
    }

    return begins;
}

std::string_view name(const Op op)
{
    switch (op)
    {
    case Op::Substitute:
        return "substitute";

    case Op::Delete:
        return "delete";

    default:
        return "insert";
    }
}

Damage damage(const std::string& pristine, const Op op, const std::size_t p, const std::size_t k, Lcg& random)
{
    switch (op)
    {
    case Op::Substitute:
    {
        std::string out{pristine};

        for (std::size_t i{0}; i < k; ++i)
        {
            out[p + i] = random.byte();
        }

        return Damage{.input = std::move(out), .end = p + k, .shift = 0, .low = p, .cut = p + k};
    }

    case Op::Delete:
    {
        std::string out{pristine.substr(0, p)};

        out += pristine.substr(p + k);

        return Damage{
                .input = std::move(out),
                .end = p,
                .shift = -static_cast<std::ptrdiff_t>(k),
                .low = p,
                .cut = p + k};
    }

    default:
    {
        std::string out{pristine.substr(0, p)};

        for (std::size_t i{0}; i < k; ++i)
        {
            out += random.byte();
        }

        out += pristine.substr(p);

        return Damage{
                .input = std::move(out),
                .end = p + k,
                .shift = static_cast<std::ptrdiff_t>(k),
                .low = p,
                .cut = p};
    }
    }
}

bool is_landed(const std::vector<std::size_t>& pristine, const Damage& damaged, const std::size_t at)
{
    if (at < damaged.low)
    {
        return std::binary_search(pristine.begin(), pristine.end(), at);
    }

    if (static_cast<std::ptrdiff_t>(at) < static_cast<std::ptrdiff_t>(damaged.cut) + damaged.shift)
    {
        return false;
    }

    const auto preimage{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) - damaged.shift)};

    return preimage >= damaged.cut && std::binary_search(pristine.begin(), pristine.end(), preimage);
}

std::optional<std::size_t> first_true_boundary(const std::vector<std::size_t>& pristine, const Damage& damaged)
{
    const auto from{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(damaged.end) - damaged.shift)};

    const auto found{std::lower_bound(pristine.begin(), pristine.end(), std::max(from, damaged.cut))};

    if (found == pristine.end())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*found) + damaged.shift);
}

} // namespace munch::tools::probes

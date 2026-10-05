#include "munch/tools/probes/recovery_damage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
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
    const auto ignore_token{[](const figures::Token, const std::size_t) {}};

    return lexer.tokenize_all<figures::Token>(input, ignore_token);
}

Token_starts token_starts(const core::Lexer& lexer, const std::string_view input, const std::size_t base)
{
    std::vector<std::size_t> starts{};

    std::size_t at{base};

    const auto note_start{[&starts, &at](const figures::Token, const std::size_t length) {
        starts.push_back(at);

        at += length;
    }};

    const auto segment{input.substr(base)};

    const auto consumed{lexer.tokenize_all<figures::Token>(segment, note_start)};

    return {.starts = std::move(starts), .consumed = consumed};
}

std::vector<std::size_t> boundaries(const core::Lexer& lexer, const std::string_view input)
{
    const auto [begins, consumed]{token_starts(lexer, input, 0)};

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
    case Op::substitution:
        return "substitute";

    case Op::deletion:
        return "delete";

    case Op::insertion:
        return "insert";
    }

    std::unreachable();
}

Damage damage(
        const std::string& pristine, const Op op, const std::size_t position, const std::size_t width, Lcg& random)
{
    const auto payload_byte{[&random] { return random.byte(); }};

    switch (op)
    {
    case Op::substitution:
    {
        std::string out{pristine};

        const auto window{out.begin() + static_cast<std::ptrdiff_t>(position)};

        std::ranges::generate_n(window, static_cast<std::ptrdiff_t>(width), payload_byte);

        return Damage{
                .input = std::move(out),
                .end = position + width,
                .shift = 0,
                .low = position,
                .cut = position + width};
    }

    case Op::deletion:
    {
        std::string out{pristine.substr(0, position)};

        out += pristine.substr(position + width);

        return Damage{
                .input = std::move(out),
                .end = position,
                .shift = -static_cast<std::ptrdiff_t>(width),
                .low = position,
                .cut = position + width};
    }

    case Op::insertion:
    {
        std::string out{pristine.substr(0, position)};

        std::ranges::generate_n(std::back_inserter(out), static_cast<std::ptrdiff_t>(width), payload_byte);

        out += pristine.substr(position);

        return Damage{
                .input = std::move(out),
                .end = position + width,
                .shift = static_cast<std::ptrdiff_t>(width),
                .low = position,
                .cut = position};
    }
    }

    std::unreachable();
}

std::size_t shifted(const std::size_t at, const std::ptrdiff_t shift)
{
    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) + shift);
}

bool is_landed(const std::vector<std::size_t>& pristine, const Damage& damaged, const std::size_t at)
{
    const auto& [input, end, shift, low, cut]{damaged};

    if (at < low)
    {
        return std::ranges::binary_search(pristine, at);
    }

    if (at < shifted(cut, shift))
    {
        return false;
    }

    const auto preimage{shifted(at, -shift)};

    return preimage >= cut && std::ranges::binary_search(pristine, preimage);
}

std::optional<std::size_t> first_true_boundary(const std::vector<std::size_t>& pristine, const Damage& damaged)
{
    const auto& [input, end, shift, low, cut]{damaged};

    const auto from{shifted(end, -shift)};

    const auto found{std::ranges::lower_bound(pristine, std::max(from, cut))};

    if (found == pristine.end())
    {
        return std::nullopt;
    }

    return shifted(*found, shift);
}

} // namespace munch::tools::probes

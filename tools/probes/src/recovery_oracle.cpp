#include "munch/tools/probes/recovery_oracle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "munch/core/lexer.hpp"
#include "munch/tools/probes/recovery_arms.hpp"
#include "munch/tools/probes/recovery_damage.hpp"
#include "munch/tools/probes/recovery_lcg.hpp"

namespace munch::tools::probes
{
namespace
{
/**
 * @brief The shortest window the certificate scans consult.
 */
constexpr std::size_t shortest_window{2};

/**
 * @brief The longest window the certificate scans consult, beside the single byte.
 */
constexpr std::size_t longest_window{4};

/**
 * @brief The next mapped pristine boundary at or below an index, walking backward past the imageless window.
 */
struct Mapped
{
    /**
     * @brief The boundary's image, std::nullopt when the walk ran out or reached an image below the floor.
     */
    std::optional<std::size_t> image{};

    /**
     * @brief The boundary's index, -1 when there is no image.
     */
    std::ptrdiff_t index{-1};
};

/**
 * @brief Where the backward walk of both streams stopped.
 */
struct Common_suffix
{
    /**
     * @brief The index of the last emitted start not matched, -1 when every one was.
     */
    std::ptrdiff_t start{-1};

    /**
     * @brief The index of the pristine boundary the walk would compare next, -1 when none is left.
     */
    std::ptrdiff_t boundary{-1};

    /**
     * @brief The smallest position both suffixes agree on, std::nullopt when they share no suffix.
     */
    std::optional<std::size_t> agreed{};
};

/**
 * @brief Returns the window lengths the certificate scans consult at an offset: two up to the longest window, cut at
 *        the end of the input.
 * @param input The input.
 * @param at The offset, below the input's size.
 * @return The lengths, ascending.
 */
auto window_lengths(const std::string_view input, const std::size_t at)
{
    const auto limit{std::min(longest_window, input.size() - at)};

    return std::views::iota(shortest_window, limit + 1);
}

/**
 * @brief Returns a pristine boundary's damaged image.
 * @param damaged The damaged input's coordinate map.
 * @param boundary The pristine boundary.
 * @return The boundary itself below low, the boundary plus the shift at or past cut, std::nullopt inside the damaged
 *         window.
 */
std::optional<std::size_t> image_of(const Damage& damaged, const std::size_t boundary)
{
    const auto& [input, end, shift, low, cut]{damaged};

    if (boundary < low)
    {
        return boundary;
    }

    if (boundary >= cut)
    {
        return shifted(boundary, shift);
    }

    return std::nullopt;
}

/**
 * @brief Walks the pristine boundaries backward from an index to the first one with an image, the largest image at or
 *        below the index since images ascend with their boundaries.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param index The index the walk starts at, -1 for none.
 * @return The image and its index, or no image when the walk runs out.
 */
Mapped first_mapped(const std::vector<std::size_t>& pristine, const Damage& damaged, const std::ptrdiff_t index)
{
    for (auto j{index}; j >= 0; --j)
    {
        const auto image{image_of(damaged, pristine[static_cast<std::size_t>(j)])};

        if (image)
        {
            return Mapped{.image = image, .index = j};
        }
    }

    return Mapped{.image = std::nullopt, .index = -1};
}

/**
 * @brief Walks the pristine boundaries backward from an index to the first one with an image at or above a floor.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param index The index the walk starts at, -1 for none.
 * @param floor The smallest image matched.
 * @return The image and its index, or no image when the walk runs out or the first image lies below the floor.
 */
Mapped next_mapped(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::ptrdiff_t index,
        const std::size_t floor)
{
    const auto mapped{first_mapped(pristine, damaged, index)};

    const auto& [image, image_index]{mapped};

    if (!image || *image < floor)
    {
        return Mapped{.image = std::nullopt, .index = -1};
    }

    return mapped;
}

/**
 * @brief Walks the emitted starts at or above the floor and the mapped pristine boundaries backward from their ends, in
 *        step, to their first disagreement.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param starts The emitted starts, ascending.
 * @param floor The smallest position matched.
 * @return Where each walk stopped and the smallest agreed position.
 */
Common_suffix common_suffix(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::vector<std::size_t>& starts,
        const std::size_t floor)
{
    Common_suffix walk{
            .start = static_cast<std::ptrdiff_t>(starts.size()) - 1,
            .boundary = static_cast<std::ptrdiff_t>(pristine.size()) - 1,
            .agreed = std::nullopt};

    auto& [start, boundary, agreed]{walk};

    while (start >= 0 && starts[static_cast<std::size_t>(start)] >= floor)
    {
        const auto [image, index]{next_mapped(pristine, damaged, boundary, floor)};

        boundary = index;

        if (!image || *image != starts[static_cast<std::size_t>(start)])
        {
            break;
        }

        agreed = image;

        --start;

        --boundary;
    }

    return walk;
}

} // namespace

std::optional<Evidence> evidence_of(const core::Lexer& lexer, const std::string_view input, const std::size_t from)
{
    for (std::size_t at{from}; at < input.size(); ++at)
    {
        if (lexer.is_split_point(input[at]))
        {
            return Evidence{.begin = at, .byte = true, .length = 1, .origin = 0};
        }

        for (const auto length : window_lengths(input, at))
        {
            const auto window{input.substr(at, length)};

            if (const auto origin{lexer.is_split_window(window)})
            {
                return Evidence{.begin = at, .byte = false, .length = length, .origin = *origin};
            }
        }
    }

    return std::nullopt;
}

std::size_t minimal_answer(
        const core::Lexer& lexer, const std::string_view input, const std::size_t from, const std::size_t answer)
{
    auto minimal{answer};

    for (std::size_t at{from}; at <= answer && at < input.size(); ++at)
    {
        if (lexer.is_split_point(input[at]))
        {
            minimal = std::min(minimal, at);

            continue;
        }

        for (const auto length : window_lengths(input, at))
        {
            const auto window{input.substr(at, length)};

            if (const auto origin{lexer.is_split_window(window)})
            {
                minimal = std::min(minimal, at + *origin);
            }
        }
    }

    return minimal;
}

std::size_t pristine_oracle(const Row& row, const std::size_t samples)
{
    Lcg random{0x5EED0003U};

    std::size_t failures{0};

    const auto& [label, lexer, corpus, begins, generated]{row};

    // The final byte is excluded, so this oracle never checks the last starting offset; there the split-friendly row
    // answers with a final newline's one-byte certificate, while the other rows refuse.
    const auto span{static_cast<std::uint32_t>(corpus.size() - 1)};

    for (std::size_t sample{0}; sample < samples; ++sample)
    {
        const auto from{static_cast<std::size_t>(random.bounded(span))};

        const auto found{lexer.next_certified_start(corpus, from)};

        if (found && (!std::ranges::binary_search(begins, *found) || *found < from))
        {
            std::fprintf(
                    stderr, "pristine oracle violation: %s from %zu answered %zu\n", std::string{label}.c_str(), from,
                    *found);

            ++failures;
        }
    }

    return failures;
}

Convergence converge(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::vector<std::size_t>& starts,
        const std::size_t floor)
{
    const auto [unmatched, boundary_index, agreed]{common_suffix(pristine, damaged, starts, floor)};

    Convergence result{};

    auto& [at, lost, spurious]{result};

    // Agreement down to the floor on both sides converges at the floor; no common suffix converges only at the end of
    // input.
    const auto starts_matched{unmatched < 0 || starts[static_cast<std::size_t>(unmatched)] < floor};

    const auto [mapped_above_floor, mapped_index]{next_mapped(pristine, damaged, boundary_index, floor)};

    if (starts_matched && !mapped_above_floor)
    {
        at = floor;
    }
    else
    {
        at = agreed.value_or(damaged.input.size());
    }

    const auto is_lost{[&damaged, at](const std::size_t boundary) {
        const auto image{image_of(damaged, boundary)};

        return image && *image >= damaged.end && *image < at;
    }};

    const auto is_spurious{[&pristine, &damaged, at](const std::size_t start) {
        return start >= damaged.end && start < at && !is_landed(pristine, damaged, start);
    }};

    lost = static_cast<std::size_t>(std::ranges::count_if(pristine, is_lost));

    spurious = static_cast<std::size_t>(std::ranges::count_if(starts, is_spurious));

    if (at <= damaged.end && (lost != 0 || spurious != 0))
    {
        std::fprintf(stderr, "convergence region violation\n");

        std::exit(EXIT_FAILURE);
    }

    return result;
}

Score score_of(const Row& row, const Damage& damaged, const Incident& incident)
{
    Score score{};

    if (incident.first && *incident.first < damaged.input.size())
    {
        score.first_landed = is_landed(row.begins, damaged, *incident.first);
    }

    if (incident.terminal && *incident.terminal < damaged.input.size())
    {
        score.terminal_landed = is_landed(row.begins, damaged, *incident.terminal);
    }

    if (incident.outcome == Outcome::completed)
    {
        if (incident.first)
        {
            score.convergence = converge(row.begins, damaged, incident.starts, *incident.first);
        }
        else
        {
            score.convergence = Convergence{.at = damaged.input.size(), .lost = 0, .spurious = 0};
        }
    }

    return score;
}

} // namespace munch::tools::probes

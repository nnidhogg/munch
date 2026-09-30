#include "munch/tools/probes/recovery_oracle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
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
// Implements recovery_oracle.hpp: the pristine-to-damaged image and the backward walks of converge are private to this
// unit.

/**
 * @brief A pristine boundary's damaged image.
 * @param damaged The damaged input's coordinate map.
 * @param boundary The pristine boundary.
 * @return The boundary itself below low, the boundary plus the shift at or past cut, std::nullopt inside the damaged
 *         window.
 */
std::optional<std::size_t> image_of(const Damage& damaged, const std::size_t boundary)
{
    if (boundary < damaged.low)
    {
        return boundary;
    }

    if (boundary >= damaged.cut)
    {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(boundary) + damaged.shift);
    }

    return std::nullopt;
}

/**
 * @brief The next mapped pristine boundary at or below an index, walking backward past the imageless window.
 */
struct Mapped
{
    /**
     * @brief The boundary's image, std::nullopt when the walk ran out or reached an image below the floor.
     */
    std::optional<std::size_t> image;

    /**
     * @brief The boundary's index, -1 when there is no image.
     */
    std::ptrdiff_t index{-1};
};

/**
 * @brief Walks the pristine boundaries backward from an index to the first one with an image.
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
    for (auto j{index}; j >= 0; --j)
    {
        const auto image{image_of(damaged, pristine[static_cast<std::size_t>(j)])};

        if (image && *image < floor)
        {
            return Mapped{.image = std::nullopt, .index = -1};
        }

        if (image)
        {
            return Mapped{.image = image, .index = j};
        }
    }

    return Mapped{.image = std::nullopt, .index = -1};
}

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
    std::optional<std::size_t> agreed;
};

/**
 * @brief Walks the emitted starts at or above the floor and the mapped pristine boundaries backward from their ends,
 *        in step, to their first disagreement.
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

    while (walk.start >= 0 && starts[static_cast<std::size_t>(walk.start)] >= floor)
    {
        const auto mapped{next_mapped(pristine, damaged, walk.boundary, floor)};

        walk.boundary = mapped.index;

        if (!mapped.image || *mapped.image != starts[static_cast<std::size_t>(walk.start)])
        {
            break;
        }

        walk.agreed = mapped.image;

        --walk.start;

        --walk.boundary;
    }

    return walk;
}

/**
 * @brief Whether a pristine boundary at or below an index maps to an image at or above the floor.
 * @param pristine The pristine boundaries, ascending.
 * @param damaged The damaged input's coordinate map.
 * @param index The index the backward search starts at, -1 for none.
 * @param floor The floor.
 * @return True when such a boundary exists.
 */
bool has_mapped_at_or_above(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::ptrdiff_t index,
        const std::size_t floor)
{
    for (auto j{index}; j >= 0; --j)
    {
        const auto image{image_of(damaged, pristine[static_cast<std::size_t>(j)])};

        if (image && *image >= floor)
        {
            return true;
        }
    }

    return false;
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

        const auto limit{std::min<std::size_t>(4, input.size() - at)};

        for (std::size_t length{2}; length <= limit; ++length)
        {
            if (const auto origin{lexer.is_split_window(input.substr(at, length))})
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

        const auto limit{std::min<std::size_t>(4, input.size() - at)};

        for (std::size_t length{2}; length <= limit; ++length)
        {
            if (const auto origin{lexer.is_split_window(input.substr(at, length))})
            {
                minimal = std::min(minimal, at + *origin);
            }
        }
    }

    return minimal;
}

std::size_t pristine_oracle(const Row& row, const std::size_t samples)
{
    Lcg random{0x5eed0003U};

    std::size_t failures{0};

    for (std::size_t sample{0}; sample < samples; ++sample)
    {
        // The final byte is excluded, so this oracle never checks the last starting offset; there the split-friendly
        // row answers with a final newline's one-byte certificate, while the other rows refuse.
        const auto from{static_cast<std::size_t>(random.bounded(static_cast<std::uint32_t>(row.corpus.size() - 1)))};

        const auto found{row.lexer.next_certified_start(row.corpus, from)};

        if (found && (!std::binary_search(row.begins.begin(), row.begins.end(), *found) || *found < from))
        {
            std::fprintf(
                    stderr, "PRISTINE ORACLE VIOLATION: %s from %zu answered %zu\n", std::string{row.label}.c_str(),
                    from, *found);

            ++failures;
        }
    }

    return failures;
}

Convergence converge(
        const std::vector<std::size_t>& pristine, const Damage& damaged, const std::vector<std::size_t>& starts,
        const std::size_t floor)
{
    const auto walk{common_suffix(pristine, damaged, starts, floor)};

    Convergence result{};

    // Agreement down to the floor on both sides converges at the floor; no common suffix converges only at the end of
    // input.
    if ((walk.start < 0 || starts[static_cast<std::size_t>(walk.start)] < floor) &&
        !has_mapped_at_or_above(pristine, damaged, walk.boundary, floor))
    {
        result.at = floor;
    }
    else
    {
        result.at = walk.agreed ? *walk.agreed : damaged.input.size();
    }

    // Both counts range over the divergence region, from the corruption end to the convergence point, so the initial
    // jump's skipped boundaries count as lost and emitted starts before the corruption end never count.
    for (const auto boundary : pristine)
    {
        const auto image{image_of(damaged, boundary)};

        if (image && *image >= damaged.end && *image < result.at)
        {
            ++result.lost;
        }
    }

    for (const auto start : starts)
    {
        if (start >= damaged.end && start < result.at && !is_landed(pristine, damaged, start))
        {
            ++result.spurious;
        }
    }

    if (result.at <= damaged.end && (result.lost != 0 || result.spurious != 0))
    {
        std::fprintf(stderr, "CONVERGENCE REGION VIOLATION\n");

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

    if (incident.outcome == Outcome::Completed)
    {
        score.convergence = incident.first ? converge(row.begins, damaged, incident.starts, *incident.first) :
                                             Convergence{.at = damaged.input.size(), .lost = 0, .spurious = 0};
    }

    return score;
}

} // namespace munch::tools::probes

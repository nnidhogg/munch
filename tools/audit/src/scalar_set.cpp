#include "munch/tools/audit/scalar_set.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "munch/tools/audit/expression.hpp"

namespace munch::tools::audit
{
namespace
{
/**
 * @brief The last scalar below the surrogates.
 */
constexpr char32_t below_surrogates{first_surrogate - 1};

/**
 * @brief The first scalar above the surrogates.
 */
constexpr char32_t above_surrogates{last_surrogate + 1};

} // namespace

void Scalar_set::add(const char32_t low, const char32_t high)
{
    spans_gap_ = spans_gap_ || (low <= below_surrogates && high >= above_surrogates);

    ranges_.emplace_back(low, high);

    std::ranges::sort(ranges_);

    std::vector<Range_t> merged{};

    for (const auto& [from, to] : ranges_)
    {
        if (merged.empty())
        {
            merged.emplace_back(from, to);

            continue;
        }

        auto& [kept_low, kept_high]{merged.back()};

        if (from > kept_high + 1)
        {
            merged.emplace_back(from, to);

            continue;
        }

        kept_high = std::max(kept_high, to);
    }

    ranges_ = std::move(merged);
}

void Scalar_set::add(const Scalar_set& other)
{
    spans_gap_ = spans_gap_ || other.spans_gap_;

    for (const auto& [low, high] : other.ranges_)
    {
        add(low, high);
    }
}

Scalar_set Scalar_set::minus(const Scalar_set& other) const
{
    Scalar_set difference{};

    difference.spans_gap_ = spans_gap_ && !other.contains(below_surrogates) && !other.contains(above_surrogates);

    for (const auto& [low, high] : ranges_)
    {
        auto from{low};

        for (const auto& [cut_low, cut_high] : other.ranges_)
        {
            if (cut_high < from)
            {
                continue;
            }

            if (cut_low > high)
            {
                break;
            }

            if (cut_low > from)
            {
                difference.add(from, cut_low - 1);
            }

            if (cut_high >= high)
            {
                from = high + 1;

                break;
            }

            from = std::max(from, static_cast<char32_t>(cut_high + 1));
        }

        if (from <= high)
        {
            difference.add(from, high);
        }
    }

    return difference;
}

const std::vector<Scalar_set::Range_t>& Scalar_set::ranges() const noexcept
{
    return ranges_;
}

bool Scalar_set::contains(const char32_t value) const noexcept
{
    const auto holds{[value](const Range_t& range) {
        const auto& [low, high]{range};

        return low <= value && value <= high;
    }};

    return std::ranges::any_of(ranges_, holds);
}

bool Scalar_set::empty() const noexcept
{
    return ranges_.empty();
}

std::optional<char32_t> Scalar_set::single() const noexcept
{
    if (ranges_.size() != 1)
    {
        return std::nullopt;
    }

    const auto& [low, high]{ranges_.front()};

    return low == high ? std::optional{low} : std::nullopt;
}

bool Scalar_set::spans_gap() const noexcept
{
    return spans_gap_;
}

Scalar_set universe_of(const bool unicode)
{
    Scalar_set set{};

    if (!unicode)
    {
        set.add(0, last_byte);

        return set;
    }

    // Made as the crate makes its dot, one range across the surrogate gap, with the surrogates then taken out.
    set.add(0, last_scalar);

    Scalar_set surrogates{};

    surrogates.add(first_surrogate, last_surrogate);

    return set.minus(surrogates);
}

} // namespace munch::tools::audit

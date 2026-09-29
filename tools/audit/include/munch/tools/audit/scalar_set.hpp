#ifndef MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SCALAR_SET_HPP
#define MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SCALAR_SET_HPP

#include <optional>
#include <utility>
#include <vector>

/**
 * @brief A set of scalars, or of bytes, as ascending disjoint ranges, Scalar_set: union and difference, and whether a
 *        range runs across the surrogate gap as the regex crate holds its classes; with the set every class is
 *        complemented against, universe_of().
 *
 * The logos reader holds every class of a pattern as such a set, a bracket, the dot, an escape's class and a letter
 * folded under `i` among them, and writes it for the pattern parser once the pattern is read.
 */
namespace munch::tools::audit
{
/**
 * @brief A set of scalars, or of bytes when the pattern is over bytes, as sorted disjoint ranges.
 */
class Scalar_set
{
public:
    /**
     * @brief An inclusive range.
     */
    using Range_t = std::pair<char32_t, char32_t>;

    /**
     * @brief Adds a range, merging it with the ranges it touches.
     * @param low The first member.
     * @param high The last member, at least the first.
     */
    void add(char32_t low, char32_t high);

    /**
     * @brief Adds every member of another set.
     * @param other The set.
     */
    void add(const Scalar_set& other);

    /**
     * @brief The members of this set that are not in another.
     * @param other The set to remove.
     * @return The difference.
     */
    [[nodiscard]] Scalar_set minus(const Scalar_set& other) const;

    /**
     * @brief The ranges, ascending and disjoint.
     * @return The ranges.
     */
    [[nodiscard]] const std::vector<Range_t>& ranges() const noexcept;

    /**
     * @brief Whether a value is a member.
     * @param value The value.
     * @return True when it is.
     */
    [[nodiscard]] bool contains(char32_t value) const noexcept;

    /**
     * @brief Whether the set has no member.
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief The one member, when there is exactly one.
     * @return The member, or std::nullopt.
     */
    [[nodiscard]] std::optional<char32_t> single() const noexcept;

    /**
     * @brief Whether the regex crate keeps a range of this set running across the surrogate gap, from below U+D800 to
     *        above U+DFFF.
     *
     * The crate's classes hold their ranges as the crate built them, and it never merges a range ending at U+D7FF with
     * one beginning at U+E000, so a class admitting every scalar is the dot to it, one range, only when made from a
     * range across the gap: the dot itself, a negation, or a range written across it, while
     * `[\x00-\x{D7FF}\x{E000}-\x{10FFFF}]` stays two ranges. The flag follows the crate's construction: a range added
     * across the gap sets it, a union keeps it from either side, and a difference keeps it unless the removed set holds
     * U+D7FF or U+E000, which is where the crate's negation puts a boundary.
     * @return True when one does.
     */
    [[nodiscard]] bool spans_gap() const noexcept;

private:
    /**
     * @brief Whether a range runs across the surrogate gap as the crate holds the set.
     */
    bool spans_gap_{false};

    /**
     * @brief The ranges, kept ascending, disjoint and apart.
     */
    std::vector<Range_t> ranges_;
};

/**
 * @brief The set every class is complemented against in a mode: the scalars less the surrogates, or the bytes.
 * @param unicode Whether the mode is Unicode.
 * @return The universe.
 */
[[nodiscard]] Scalar_set universe_of(bool unicode);

} // namespace munch::tools::audit

#endif // MUNCH_TOOLS_AUDIT_INCLUDE_MUNCH_TOOLS_AUDIT_SCALAR_SET_HPP

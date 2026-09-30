#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_LCG64_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_LCG64_HPP

#include <cstddef>
#include <cstdint>

/**
 * @brief The 64-bit linear congruential stream the generated corpora are drawn from, Lcg64.
 */
namespace munch::tools::probes
{
/**
 * @brief A deterministic 64-bit linear congruential stream: each draw advances the state to
 *        `state * 6364136223846793005 + 1442695040888963407` and returns `(state >> 33) % bound`, so one seed always
 *        draws the same sequence.
 */
class Lcg64
{
public:
    /**
     * @brief Starts the stream at a seed.
     * @param seed The initial state.
     */
    explicit Lcg64(std::uint64_t seed) noexcept;

    /**
     * @brief Advances the state and draws a value below a bound.
     * @param bound The exclusive upper bound, at least one.
     * @return The advanced state's high 31 bits modulo the bound.
     */
    [[nodiscard]] std::size_t next(std::size_t bound) noexcept;

private:
    /**
     * @brief The current state.
     */
    std::uint64_t state_;
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_LCG64_HPP

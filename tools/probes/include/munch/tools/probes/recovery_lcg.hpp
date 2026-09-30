#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_LCG_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_LCG_HPP

#include <cstdint>

/**
 * @brief The recovery study's 32-bit linear congruential stream, Lcg.
 */
namespace munch::tools::probes
{
/**
 * @brief A deterministic 32-bit linear congruential stream: each draw advances the state to
 *        `state * 1664525 + 1013904223`, so one seed always draws the same sequence. The study keeps one instance per
 *        purpose, so the corpus, sampling, schedule and payload streams never share a draw.
 */
class Lcg
{
public:
    /**
     * @brief Starts the stream at a seed.
     * @param seed The initial state.
     */
    explicit Lcg(std::uint32_t seed) noexcept;

    /**
     * @brief Advances the state and draws a full-width value, so a position sampled from it can reach every offset of
     *        a span.
     * @return The advanced state xor that state shifted right by 16.
     */
    [[nodiscard]] std::uint32_t next() noexcept;

    /**
     * @brief Draws an unbiased value below a bound by Lemire's multiply-shift: draws next() until the low half of the
     *        draw times the span is at least `(2^32 - span) % span`, then answers the high half.
     * @param span The exclusive upper bound, at least one.
     * @return The accepted product shifted right by 32, in [0, span).
     */
    [[nodiscard]] std::uint32_t bounded(std::uint32_t span) noexcept;

    /**
     * @brief Advances the state and draws a byte with every value admitted, the damage payloads' alphabet.
     * @return Bits 16 to 23 of the advanced state.
     */
    [[nodiscard]] char byte() noexcept;

private:
    /**
     * @brief The current state.
     */
    std::uint32_t state_;
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_RECOVERY_LCG_HPP

#ifndef MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ASSERTIONS_HPP
#define MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ASSERTIONS_HPP

#include <cstddef>
#include <string_view>

/**
 * @brief The assertions a probe checks, Assertions: each failed one printed as it fails, and whether any failed.
 */
namespace munch::tools::probes
{
/**
 * @brief The assertions of one probe run: a failed one prints `FAIL: <what>` on standard output and is counted, and the
 *        count decides the probe's verdict.
 */
class Assertions
{
public:
    /**
     * @brief Checks one assertion, printing `FAIL: ` and its description on standard output when it does not hold.
     * @param condition Whether the assertion holds.
     * @param what What the assertion states.
     */
    void expect(bool condition, std::string_view what);

    /**
     * @brief Whether any assertion checked so far failed.
     * @return True after the first failure.
     */
    [[nodiscard]] bool has_failures() const noexcept;

private:
    /**
     * @brief The number of assertions that failed.
     */
    std::size_t failures_{0};
};

} // namespace munch::tools::probes

#endif // MUNCH_TOOLS_PROBES_INCLUDE_MUNCH_TOOLS_PROBES_ASSERTIONS_HPP

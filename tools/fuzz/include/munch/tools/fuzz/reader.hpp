#ifndef MUNCH_TOOLS_FUZZ_INCLUDE_MUNCH_TOOLS_FUZZ_READER_HPP
#define MUNCH_TOOLS_FUZZ_INCLUDE_MUNCH_TOOLS_FUZZ_READER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/**
 * @brief What both fuzz harnesses share: the input read as a byte stream, Reader, and the invariant check, require().
 */
namespace munch::tools::fuzz
{
/**
 * @brief Reads the fuzz input as a byte stream, yielding zeros once exhausted.
 *
 * Exhaustion yielding zeros keeps every decode total: a truncated input decodes to a small grammar and an empty scan
 * instead of a rejected run, so the fuzzer never wastes inputs on decode failures.
 */
class Reader
{
public:
    /**
     * @brief Reads from the start of the input.
     * @param data The fuzz input.
     */
    explicit Reader(std::span<const std::uint8_t> data);

    /**
     * @brief Returns the next byte of the input.
     * @return The byte, zero once the input is exhausted.
     */
    [[nodiscard]] std::uint8_t byte() noexcept;

    /**
     * @brief Returns the next bytes of the input.
     * @param count The bytes wanted.
     * @return Up to that many bytes, fewer once the input runs out.
     */
    [[nodiscard]] std::string take(std::size_t count);

    /**
     * @brief Returns the bytes not yet read, used as the text to scan.
     * @return The rest of the input.
     */
    [[nodiscard]] std::string remainder() const;

private:
    /**
     * @brief The fuzz input, viewed as characters.
     */
    std::string_view bytes_{};

    /**
     * @brief The offset of the next byte to read, never past the end of the input.
     */
    std::size_t position_{0};
};

/**
 * @brief Aborts on a violated invariant, which the fuzzer reports as a crash on this input.
 * @param condition The invariant.
 */
void require(bool condition);

} // namespace munch::tools::fuzz

#endif // MUNCH_TOOLS_FUZZ_INCLUDE_MUNCH_TOOLS_FUZZ_READER_HPP

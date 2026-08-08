#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "munch/tools/tokenizer/raw_string.hpp"

namespace
{
/**
 * @brief Aborts on a violated invariant, which the fuzzer reports as a crash on this input.
 */
void require(const bool condition)
{
    if (!condition)
    {
        __builtin_trap();
    }
}

/**
 * @brief The longest delimiter the scanner accepts, mirrored here so the harness can assert the bound.
 */
constexpr std::size_t max_delimiter_length{16};

/**
 * @brief Reads the fuzz input as a byte stream, yielding zeros once exhausted.
 */
class Reader
{
public:
    Reader(const std::uint8_t* const data, const std::size_t size) : data_{data}, size_{size} {}

    [[nodiscard]] std::uint8_t byte() noexcept { return position_ < size_ ? data_[position_++] : 0; }

    [[nodiscard]] std::string take(const std::size_t count)
    {
        const auto available{position_ < size_ ? std::min(count, size_ - position_) : std::size_t{0}};

        std::string result{reinterpret_cast<const char*>(data_) + position_, available};

        position_ += available;

        return result;
    }

    [[nodiscard]] std::string remainder() const
    {
        return position_ < size_ ? std::string{reinterpret_cast<const char*>(data_) + position_, size_ - position_} :
                                   std::string{};
    }

private:
    const std::uint8_t* data_;

    std::size_t size_;

    std::size_t position_{0};
};

/**
 * @brief Checks everything the scanner promises about a successful scan.
 *
 * Success is the interesting direction: a failure only has to not crash, but a length is a claim about the input,
 * and a wrong one sends a driver's seek() into the middle of a literal or past the end of the buffer.
 */
void check_success(const std::string_view input, const std::size_t offset, const std::size_t length)
{
    // The whole point of the returned length is that a driver seeks by it, so it must stay inside the buffer.
    require(length > 0);
    require(offset + length <= input.size());

    const auto literal{input.substr(offset, length)};

    // R" opens it and the delimiter runs to the first '(', bounded by the documented maximum.
    require(literal.size() >= 5);
    require(literal.starts_with("R\""));

    const auto open{literal.find('(')};

    require(open != std::string_view::npos);

    const auto delimiter{literal.substr(2, open - 2)};

    require(delimiter.size() <= max_delimiter_length);

    // Every accepted delimiter character must be a C++23 d-char: a letter, a digit, or basic-set punctuation.
    // Stated here independently of the scanner's own predicate, so a loosened predicate fails the harness.
    for (const char accepted : delimiter)
    {
        constexpr std::string_view d_chars{R"(!"#%&'*+,-./:;<=>?[]^_{|}~)"};

        require((accepted >= 'A' && accepted <= 'Z') || (accepted >= 'a' && accepted <= 'z') ||
                (accepted >= '0' && accepted <= '9') || d_chars.find(accepted) != std::string_view::npos);
    }

    // The literal ends with exactly the closing sequence the delimiter dictates.
    std::string closing{")"};
    closing += delimiter;
    closing += '"';

    require(literal.ends_with(closing));

    // ... and closes at the first opportunity: any earlier occurrence would mean the scan ran past the real end,
    // swallowing text that belongs to the tokens after it.
    const auto body{literal.substr(open + 1, literal.size() - (open + 1) - closing.size())};

    require(body.find(closing) == std::string_view::npos);

    // Rescanning the literal alone must agree, so the answer depends on the literal and not on its surroundings.
    const auto rescan{munch::tools::tokenizer::scan_raw_string(literal, 0)};

    require(rescan.has_value());
    require(*rescan == length);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Reader reader{data, size};

    const auto mode{reader.byte() % 2U};

    const auto offset_byte{static_cast<std::size_t>(reader.byte())};

    std::string input;

    if (mode == 0)
    {
        // Arbitrary bytes at an arbitrary offset: the malformed direction, where the scanner must reject rather
        // than read past the end of a truncated delimiter or an unterminated body.
        input = reader.remainder();
    }
    else
    {
        // A literal assembled from fuzz bytes, so the success path is reached often rather than by accident. The
        // pieces are still fuzz-controlled, so delimiters containing ')' or '"', bodies containing the closing
        // sequence, and over-long delimiters all arise on their own.
        const auto delimiter{reader.take(reader.byte() % (max_delimiter_length + 4U))};

        input = "R\"" + delimiter + "(" + reader.remainder() + ")" + delimiter + "\"";
    }

    // Kept inside the buffer: an out-of-range offset is echoed back in the error unchanged, which says nothing
    // about the scanner and would only assert what the caller already passed in. It is exercised separately below.
    const auto offset{offset_byte % (input.size() + 1)};

    const auto result{munch::tools::tokenizer::scan_raw_string(input, offset)};

    if (result)
    {
        check_success(input, offset, *result);
    }
    else
    {
        // A failure reports where it gave up, and that position has to be inside the input to be usable.
        require(result.error().position() <= input.size());
    }

    // Scanning at or past the end is the boundary a driver hits at the end of a buffer, and must be a clean
    // rejection rather than a read. No claim is made about the reported position here, only that nothing is read.
    require(!munch::tools::tokenizer::scan_raw_string(input, input.size()));
    require(!munch::tools::tokenizer::scan_raw_string(input, input.size() + offset_byte + 1));

    return 0;
}

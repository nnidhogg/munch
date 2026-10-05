#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "munch/tools/fuzz/reader.hpp"
#include "munch/tools/tokenizer/raw_string.hpp"

namespace
{
using munch::tools::fuzz::Reader;
using munch::tools::fuzz::require;

/**
 * @brief The longest delimiter the scanner accepts, mirrored here so the harness can assert the bound.
 */
constexpr std::size_t max_delimiter_length{16};

/**
 * @brief The most bytes a decoded delimiter may run past the longest one accepted, so over-long delimiters arise.
 */
constexpr std::size_t delimiter_overshoot{4};

/**
 * @brief The prefix that opens every raw string literal.
 */
constexpr std::string_view raw_opener{R"(R")"};

/**
 * @brief The shortest raw string literal, with an empty delimiter and an empty body.
 */
constexpr std::string_view shortest_literal{R"x(R"()")x"};

/**
 * @brief The punctuation C++23 admits in a raw string delimiter, beside letters and digits.
 */
constexpr std::string_view d_char_punctuation{R"(!"#%&'*+,-./:;<=>?[]^_{|}~)"};

/**
 * @brief Checks everything the scanner promises about a successful scan.
 *
 * Success is the interesting direction: a failure only has to not crash, but a length is a claim about the input, and a
 * wrong one sends a driver's seek() into the middle of a literal or past the end of the buffer.
 * @param input The scanned input.
 * @param offset The offset the scan started at.
 * @param length The length the scan returned.
 */
void check_success(const std::string_view input, const std::size_t offset, const std::size_t length)
{
    // The whole point of the returned length is that a driver seeks by it, so it must stay inside the buffer.
    require(length > 0);

    require(offset + length <= input.size());

    const auto literal{input.substr(offset, length)};

    // R" opens it and the delimiter runs to the first '(', bounded by the documented maximum.
    require(literal.size() >= shortest_literal.size());

    require(literal.starts_with(raw_opener));

    const auto open{literal.find('(')};

    require(open != std::string_view::npos);

    const auto delimiter{literal.substr(raw_opener.size(), open - raw_opener.size())};

    require(delimiter.size() <= max_delimiter_length);

    // The C++23 d-char test, stated apart from the scanner's own predicate.
    const auto is_d_char{[](const char symbol) {
        return (symbol >= 'A' && symbol <= 'Z') || (symbol >= 'a' && symbol <= 'z') ||
               (symbol >= '0' && symbol <= '9') || d_char_punctuation.contains(symbol);
    }};

    for (const char accepted : delimiter)
    {
        require(is_d_char(accepted));
    }

    // The literal ends with exactly the closing sequence the delimiter dictates.
    const auto closing{std::format(R"(){}")", delimiter)};

    require(literal.ends_with(closing));

    // ... and closes at the first opportunity: any earlier occurrence would mean the scan ran past the real end,
    // swallowing text that belongs to the tokens after it.
    const auto body{literal.substr(open + 1, literal.size() - (open + 1) - closing.size())};

    require(!body.contains(closing));

    // Rescanning the literal alone must agree, so the answer depends on the literal and not on its surroundings.
    const auto rescan{munch::tools::tokenizer::scan_raw_string(literal, 0)};

    require(rescan.has_value());

    require(*rescan == length);
}

} // namespace

/**
 * @brief Checks one fuzz input, the fuzz entry point run once per input.
 *
 * The first byte chooses how the text to scan is decoded from the rest. Mode zero takes arbitrary bytes at an arbitrary
 * offset: the malformed direction, where the scanner must reject rather than read past the end of a truncated delimiter
 * or an unterminated body. Mode one assembles a literal from fuzz bytes, so the success path is reached often rather
 * than by accident. The pieces are still fuzz-controlled, so delimiters containing ')' or '"', bodies containing the
 * closing sequence, and over-long delimiters all arise on their own.
 * @param data The fuzz input.
 * @param size The input's size.
 * @return Zero; a violated invariant traps instead.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    Reader reader{std::span{data, size}};

    const auto mode{reader.byte() % 2U};

    const auto offset_byte{static_cast<std::size_t>(reader.byte())};

    const auto decode_input{[&reader, mode] {
        if (mode == 0)
        {
            return reader.remainder();
        }

        const auto delimiter_length{reader.byte() % (max_delimiter_length + delimiter_overshoot)};

        const auto delimiter{reader.take(delimiter_length)};

        const auto body{reader.remainder()};

        return std::format(R"(R"{}({}){}")", delimiter, body, delimiter);
    }};

    const auto input{decode_input()};

    // Kept inside the buffer: an out-of-range offset is echoed back in the error unchanged, which says nothing about
    // the scanner and would only assert what the caller already passed in. It is exercised separately below.
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

    // Scanning at or past the end is the boundary a driver hits at the end of a buffer, and must be a clean rejection
    // rather than a read. No claim is made about the reported position here, only that nothing is read.
    const auto at_end{munch::tools::tokenizer::scan_raw_string(input, input.size())};

    require(!at_end);

    const auto past_end{munch::tools::tokenizer::scan_raw_string(input, input.size() + offset_byte + 1)};

    require(!past_end);

    return 0;
}

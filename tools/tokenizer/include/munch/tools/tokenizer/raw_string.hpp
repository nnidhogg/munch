#ifndef MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_RAW_STRING_HPP
#define MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_RAW_STRING_HPP

#include <cstddef>
#include <expected>
#include <string_view>

#include "munch/tools/tokenizer/error.hpp"

namespace munch::tools::tokenizer
{
/**
 * @brief Scans a C++ raw string literal by hand.
 *
 * Raw string literals are not regular: the closing sequence must repeat the opening delimiter, which no automaton
 * can express. A driver recognizes the `R"` prefix with its lexer, calls this scanner on the tokenizer's input, and
 * seeks past the returned length; see Tokenizer::seek() and Tokenizer::input().
 * @param input The input text.
 * @param offset The offset of the literal's `R`; an encoding prefix such as `u8` lies before it.
 * @return The length of the whole literal from `offset`, or an Error describing why the scan failed.
 */
[[nodiscard]] std::expected<std::size_t, Error> scan_raw_string(std::string_view input, std::size_t offset);

} // namespace munch::tools::tokenizer

#endif // MUNCH_TOOLS_TOKENIZER_INCLUDE_MUNCH_TOOLS_TOKENIZER_RAW_STRING_HPP

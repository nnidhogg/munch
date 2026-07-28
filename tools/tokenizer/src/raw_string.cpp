#include "munch/tools/tokenizer/raw_string.hpp"

#include <string>

namespace munch::tools::tokenizer
{
namespace
{
/**
 * @brief The longest delimiter the standard allows.
 */
constexpr std::size_t max_delimiter_length{16};

/**
 * @brief Checks whether a character may appear in a raw string delimiter.
 *
 * The standard allows any basic character except spaces, parentheses, backslashes, and control characters; the
 * opening parenthesis never reaches this check, as it ends the delimiter.
 */
bool is_delimiter_character(const char c)
{
    return c != ' ' && c != ')' && c != '\\' && static_cast<unsigned char>(c) > 0x1F && c != 0x7F;
}

} // namespace

std::expected<std::size_t, Error> scan_raw_string(const std::string_view input, const std::size_t offset)
{
    if (offset >= input.size() || input.size() - offset < 2 || input[offset] != 'R' || input[offset + 1] != '"')
    {
        return std::unexpected{Error{"Not a raw string literal", offset}};
    }

    const auto delimiter_start{offset + 2};

    auto position{delimiter_start};

    while (position < input.size() && input[position] != '(')
    {
        if (!is_delimiter_character(input[position]) || position - delimiter_start >= max_delimiter_length)
        {
            return std::unexpected{Error{"Invalid raw string delimiter", position}};
        }

        ++position;
    }

    if (position == input.size())
    {
        return std::unexpected{Error{"Unterminated raw string literal", offset}};
    }

    // The literal closes at ')' delimiter '"'; repeating the delimiter makes any other content, including `)"`,
    // plain characters.
    std::string closing{")"};
    closing += input.substr(delimiter_start, position - delimiter_start);
    closing += '"';

    const auto end{input.find(closing, position + 1)};

    if (end == std::string_view::npos)
    {
        return std::unexpected{Error{"Unterminated raw string literal", offset}};
    }

    return end + closing.size() - offset;
}

} // namespace munch::tools::tokenizer

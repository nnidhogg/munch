#include "munch/tools/tokenizer/raw_string.hpp"

#include <string>
#include <string_view>

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
 * C++23 restricts a d-char to the basic character set less spaces, parentheses, backslashes, and control
 * characters, so the check is a whitelist of exactly those members: letters, digits, and the set's punctuation.
 * Dollar, at-sign, and grave accent become d-chars only when P2558 lands in a later standard, and bytes outside
 * ASCII are never d-chars; the opening parenthesis never reaches this check, as it ends the delimiter.
 */
bool is_delimiter_character(const char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
    {
        return true;
    }

    constexpr std::string_view punctuation{R"(!"#%&'*+,-./:;<=>?[]^_{|}~)"};

    return punctuation.find(c) != std::string_view::npos;
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

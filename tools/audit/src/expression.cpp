#include "munch/tools/audit/expression.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace munch::tools::audit
{
std::string quoted(const std::string_view bytes)
{
    std::string text{'"'};

    for (const auto byte : bytes)
    {
        const auto value{static_cast<unsigned char>(byte)};

        if (byte == '"' || byte == '\\')
        {
            text += std::string{'\\', byte};
        }
        else if (!is_printable(value))
        {
            text += bracket_member(value);
        }
        else
        {
            text.push_back(byte);
        }
    }

    return text + '"';
}

std::string reference(const std::string_view name)
{
    return std::format("{{{}}}", name);
}

std::string bracket(const regex::Set& set)
{
    std::string text{'['};

    for (unsigned first{0U}; first < byte_values; ++first)
    {
        if (!set.symbols().contains(static_cast<char>(first)))
        {
            continue;
        }

        auto last{first};

        while (last + 1U < byte_values && set.symbols().contains(static_cast<char>(last + 1U)))
        {
            ++last;
        }

        text += bracket_run(static_cast<unsigned char>(first), static_cast<unsigned char>(last));

        first = last;
    }

    return text + ']';
}

std::string bracket_run(const unsigned char first, const unsigned char last)
{
    auto text{bracket_member(first)};

    if (last > first + 1)
    {
        text += '-';
    }

    if (last > first)
    {
        text += bracket_member(last);
    }

    return text;
}

std::string code_point_member(const char32_t first, const char32_t last)
{
    const auto low{static_cast<std::uint32_t>(first)};

    if (first == last)
    {
        return std::format(R"(\u{{{:x}}})", low);
    }

    const auto high{static_cast<std::uint32_t>(last)};

    return std::format(R"(\u{{{:x}}}-\u{{{:x}}})", low, high);
}

std::string bracket_member(const unsigned char byte)
{
    switch (byte)
    {
    case '\n':
        return R"(\n)";
    case '\t':
        return R"(\t)";
    case '\r':
        return R"(\r)";
    case '\\':
    case ']':
    case '[':
    case '^':
    case '-':
        return std::string{'\\', static_cast<char>(byte)};
    default:
        break;
    }

    if (!is_printable(byte))
    {
        return std::format(R"(\x{:02x})", byte);
    }

    return {static_cast<char>(byte)};
}

std::string without_trailing_blanks(std::string text)
{
    while (!text.empty() && is_blank(text.back()))
    {
        text.pop_back();
    }

    return text;
}

std::string_view without_delimiters(const std::string_view text) noexcept
{
    return text.substr(1, text.size() - 2);
}

std::size_t lines_before(const std::string_view text, const std::size_t offset) noexcept
{
    const auto before{text.substr(0, offset)};

    return static_cast<std::size_t>(std::ranges::count(before, '\n'));
}

} // namespace munch::tools::audit

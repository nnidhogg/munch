#include "munch/tools/audit/expression.hpp"

#include <format>
#include <string>
#include <string_view>

namespace munch::tools::audit
{
std::string encoded(const char32_t scalar)
{
    std::string bytes;

    if (scalar < 0x80)
    {
        bytes.push_back(static_cast<char>(scalar));
    }
    else if (scalar < 0x800)
    {
        bytes.push_back(static_cast<char>(0xC0 | (scalar >> 6U)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }
    else if (scalar < 0x10000)
    {
        bytes.push_back(static_cast<char>(0xE0 | (scalar >> 12U)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }
    else
    {
        bytes.push_back(static_cast<char>(0xF0 | (scalar >> 18U)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 12U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | ((scalar >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80 | (scalar & 0x3FU)));
    }

    return bytes;
}

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
        else if (value < 0x20 || value > 0x7E)
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

std::string bracket(const regex::Set& set)
{
    std::string text{'['};

    for (unsigned first{0}; first < 256; ++first)
    {
        if (!set.symbols().contains(static_cast<char>(first)))
        {
            continue;
        }

        auto last{first};

        while (last + 1 < 256 && set.symbols().contains(static_cast<char>(last + 1)))
        {
            ++last;
        }

        text += bracket_member(static_cast<unsigned char>(first));

        if (last > first + 1)
        {
            text += '-';
        }

        if (last > first)
        {
            text += bracket_member(static_cast<unsigned char>(last));
        }

        first = last;
    }

    return text + ']';
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

    if (byte < 0x20 || byte > 0x7E)
    {
        return std::format(R"(\x{:02x})", byte);
    }

    return {static_cast<char>(byte)};
}

} // namespace munch::tools::audit

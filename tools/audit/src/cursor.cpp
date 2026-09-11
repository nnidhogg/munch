#include "munch/tools/audit/cursor.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
Cursor::Cursor(const std::string_view text, const std::size_t begin, const std::size_t end)
    : text_{text}, at_{begin}, end_{end}
{}

Cursor::Cursor(const std::string_view text) : Cursor{text, 0, text.size()}
{}

char Cursor::next(const std::string_view what)
{
    if (at_ >= end_)
    {
        fail("expected " + std::string{what} + " before the end of the text");
    }

    return text_[at_++];
}

bool Cursor::done() const noexcept
{
    return at_ >= end_;
}

bool Cursor::accept(const char byte) noexcept
{
    if (peek() != byte)
    {
        return false;
    }

    ++at_;

    return true;
}

void Cursor::skip_blanks()
{
    for (;;)
    {
        while (peek() && (*peek() == ' ' || *peek() == '\t' || *peek() == '\n' || *peek() == '\r'))
        {
            ++at_;
        }

        if (at("//"))
        {
            while (peek() && *peek() != '\n')
            {
                ++at_;
            }
        }
        else if (at("/*"))
        {
            const auto close{text_.find("*/", at_ + 2)};

            if (close == std::string_view::npos || close + 2 > end_)
            {
                fail("a comment is never closed");
            }

            at_ = close + 2;
        }
        else
        {
            return;
        }
    }
}

void Cursor::expect(const char byte, const std::string_view what)
{
    if (!accept(byte))
    {
        fail("expected " + std::string{what});
    }
}

std::size_t Cursor::line() const noexcept
{
    return line_of(std::min(at_, text_.empty() ? 0 : text_.size() - 1));
}

std::size_t Cursor::offset() const noexcept
{
    return at_;
}

std::optional<char> Cursor::peek() const noexcept
{
    return at_ < end_ ? std::optional{text_[at_]} : std::nullopt;
}

void Cursor::fail(const std::string& message) const
{
    throw Spec_error{message, line()};
}

bool Cursor::at(const std::string_view prefix) const noexcept
{
    return text_.substr(at_, end_ - std::min(at_, end_)).starts_with(prefix);
}

std::size_t Cursor::line_of(const std::size_t offset) const noexcept
{
    return 1 + static_cast<std::size_t>(std::ranges::count(text_.substr(0, offset), '\n'));
}

} // namespace munch::tools::audit

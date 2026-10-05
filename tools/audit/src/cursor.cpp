#include "munch/tools/audit/cursor.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "munch/tools/audit/expression.hpp"
#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
Cursor::Cursor(
        const std::string_view text, const std::size_t begin, const std::size_t end,
        const Line_comment_end line_comment_end)
    : text_{text}, at_{begin}, end_{end}, line_comment_end_{line_comment_end}
{}

Cursor::Cursor(const std::string_view text, const Line_comment_end line_comment_end)
    : Cursor{text, 0, text.size(), line_comment_end}
{}

char Cursor::next(const std::string_view what)
{
    if (done())
    {
        fail(std::format("expected {} before the end of the text", what));
    }

    return text_[at_++];
}

bool Cursor::done() const noexcept
{
    return at_ >= end_;
}

void Cursor::skip_blanks()
{
    for (;;)
    {
        while (peek() && is_blank(*peek()))
        {
            ++at_;
        }

        if (at(line_comment_opener))
        {
            while (peek() && *peek() != '\n' && (line_comment_end_ == Line_comment_end::newline || *peek() != '\r'))
            {
                ++at_;
            }

            continue;
        }

        if (!at(comment_opener))
        {
            return;
        }

        const auto close{text_.find(comment_closer, at_ + comment_opener.size())};

        if (close == std::string_view::npos || close + comment_closer.size() > end_)
        {
            fail("a comment is never closed");
        }

        at_ = close + comment_closer.size();
    }
}

void Cursor::expect(const char byte, const std::string_view what)
{
    if (!accept(byte))
    {
        fail(std::format("expected {}", what));
    }
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

std::size_t Cursor::offset() const noexcept
{
    return at_;
}

std::optional<char> Cursor::peek() const noexcept
{
    return !done() ? std::optional{text_[at_]} : std::nullopt;
}

void Cursor::fail(const std::string& message) const
{
    throw Spec_error{message, line()};
}

std::size_t Cursor::line() const noexcept
{
    const auto last{text_.empty() ? 0 : text_.size() - 1};

    return line_of(std::min(at_, last));
}

bool Cursor::at(const std::string_view prefix) const noexcept
{
    const auto rest{text_.substr(at_, end_ - std::min(at_, end_))};

    return rest.starts_with(prefix);
}

std::size_t Cursor::line_of(const std::size_t offset) const noexcept
{
    return 1 + lines_before(text_, offset);
}

} // namespace munch::tools::audit

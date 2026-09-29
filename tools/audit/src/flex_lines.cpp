#include "munch/tools/audit/flex_lines.hpp"

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>

#include "munch/tools/audit/lexer_spec.hpp"

namespace munch::tools::audit
{
Lines::Lines(const std::string_view source) : source_{source}
{
    for (const auto line : source | std::views::split('\n'))
    {
        lines_.emplace_back(line);
    }

    if (!lines_.empty() && lines_.back().empty())
    {
        lines_.pop_back();
    }
}

bool Lines::more() const noexcept
{
    return at_ < lines_.size();
}

void Lines::advance(const std::size_t count) noexcept
{
    at_ += count;
}

std::size_t Lines::number() const noexcept
{
    return at_ + 1;
}

std::string_view Lines::rest() const noexcept
{
    return std::string_view{lines_[at_].data(), source_.data() + source_.size()};
}

std::string_view Lines::current() const noexcept
{
    return lines_[at_];
}

void Lines::skip_through(const std::string_view close, const std::string_view what)
{
    const auto opened{number()};

    for (advance(); more(); advance())
    {
        if (trimmed(current()) == close)
        {
            return;
        }
    }

    throw Spec_error{std::string{what} + " is never closed", opened};
}

void Lines::skip_to(const std::string_view mark, const std::string_view what)
{
    const auto opened{number()};

    // The opening line counts, so that `%{ code %}` on one line is that line.
    for (; more(); advance())
    {
        if (current().contains(mark))
        {
            return;
        }
    }

    throw Spec_error{std::string{what} + " is never closed", opened};
}

std::string_view trimmed(std::string_view line) noexcept
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
    {
        line.remove_prefix(1);
    }

    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    {
        line.remove_suffix(1);
    }

    return line;
}

} // namespace munch::tools::audit

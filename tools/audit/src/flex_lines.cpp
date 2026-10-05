#include "munch/tools/audit/flex_lines.hpp"

#include <cstddef>
#include <format>
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

    throw Spec_error{std::format("{} is never closed", what), opened};
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

    throw Spec_error{std::format("{} is never closed", what), opened};
}

std::string_view trimmed(const std::string_view line) noexcept
{
    const auto first{line.find_first_not_of(line_blanks)};

    if (first == std::string_view::npos)
    {
        return {};
    }

    const auto last{line.find_last_not_of(line_blanks)};

    return line.substr(first, last + 1 - first);
}

} // namespace munch::tools::audit

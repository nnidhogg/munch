#include "lexer/regex/set.hpp"

#include <algorithm>
#include <ranges>

namespace lexer::regex
{
Set::Set(const std::initializer_list<Symbol_t> symbols) : symbols_{symbols}
{}

Set::Set(const Symbols_t& symbols) : symbols_{symbols}
{}

Set::Set(Symbols_t&& symbols) : symbols_{std::move(symbols)}
{}

const Set::Symbols_t& Set::symbols() const noexcept
{
    return symbols_;
}

Set Set::from(const Symbol_t s)
{
    return Set({s});
}

Set Set::from(std::initializer_list<Symbol_t> symbols)
{
    return Set({symbols.begin(), symbols.end()});
}

Set Set::range(const Symbol_t start, const Symbol_t end)
{
    const auto range{
            std::views::iota(static_cast<unsigned>(start), static_cast<unsigned>(end) + 1) |
            std::views::transform([](const auto i) { return static_cast<Symbol_t>(i); })};

    return Set({range.begin(), range.end()});
}

Set Set::digits()
{
    return range('0', '9');
}

Set Set::alpha()
{
    return range('a', 'z') + range('A', 'Z');
}

Set Set::alphanum()
{
    return alpha() + digits();
}

Set Set::printable()
{
    return range(' ', '~');
}

Set Set::escape()
{
    return {'\n', '\t', '\r', '\'', '\"', '\\'};
}

Set Set::newline()
{
    return {'\n', '\r'};
}

Set Set::whitespace()
{
    return {' ', '\t'};
}

Set Set::all()
{
    return range(0, 127);
}

Set& Set::operator+=(const Set& other)
{
    symbols_.insert(other.symbols_.begin(), other.symbols_.end());

    return *this;
}

Set& Set::operator+=(const Symbol_t s)
{
    symbols_.insert(s);

    return *this;
}

Set& Set::operator-=(const Set& other)
{
    std::ranges::for_each(other.symbols_, [this](const Symbol_t s) { symbols_.erase(s); });

    return *this;
}

Set& Set::operator-=(const Symbol_t s)
{
    symbols_.erase(s);

    return *this;
}

Set operator+(Set lhs, const Set& rhs)
{
    lhs += rhs;

    return lhs;
}

Set operator+(Set lhs, const Set::Symbol_t s)
{
    lhs += s;

    return lhs;
}

Set operator-(Set lhs, const Set& rhs)
{
    lhs -= rhs;

    return lhs;
}

Set operator-(Set lhs, const Set::Symbol_t s)
{
    lhs -= s;

    return lhs;
}

Set operator+(const Set::Symbol_t s, Set rhs)
{
    rhs += s;

    return rhs;
}

} // namespace lexer::regex

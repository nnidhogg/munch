#include "munch/regex/set.hpp"

#include <ranges>
#include <stdexcept>
#include <utility>

namespace munch::regex
{
Set::Set(const std::initializer_list<Symbol_t> symbols) : symbols_{symbols}
{}

Set::Set(const Symbols_t& symbols) : symbols_{symbols}
{}

Set::Set(Symbols_t&& symbols) : symbols_{std::move(symbols)}
{}

Set Set::from(const Symbol_t symbol)
{
    return {symbol};
}

Set Set::from(const std::initializer_list<Symbol_t> symbols)
{
    Symbols_t chosen{symbols.begin(), symbols.end()};

    return Set{std::move(chosen)};
}

Set Set::range(const Symbol_t start, const Symbol_t end)
{
    // views::iota requires its bound to order at or after its value, so a reversed range would be undefined rather than
    // empty. Symbols are ordered as unsigned bytes here, matching how the rest of the library indexes them.
    if (static_cast<unsigned char>(end) < static_cast<unsigned char>(start))
    {
        throw std::invalid_argument{"A symbol range may not end before it starts"};
    }

    const unsigned first{static_cast<unsigned char>(start)};

    const unsigned last{static_cast<unsigned char>(end)};

    const auto symbol_of{[](const unsigned value) { return static_cast<Symbol_t>(value); }};

    const auto symbols{std::views::iota(first, last + 1U) | std::views::transform(symbol_of)};

    Symbols_t chosen{symbols.begin(), symbols.end()};

    return Set{std::move(chosen)};
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
    return {'\n', '\t', '\r', '\'', '"', '\\'};
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
    return range(0, static_cast<Symbol_t>(0xFF));
}

Set& Set::operator+=(const Set& other)
{
    symbols_.insert(other.symbols_.begin(), other.symbols_.end());

    return *this;
}

Set& Set::operator+=(const Symbol_t symbol)
{
    symbols_.insert(symbol);

    return *this;
}

Set& Set::operator-=(const Set& other)
{
    // Subtracting a set from itself would erase the elements being iterated; the answer is the empty set.
    if (this == &other)
    {
        symbols_.clear();

        return *this;
    }

    for (const auto symbol : other.symbols_)
    {
        symbols_.erase(symbol);
    }

    return *this;
}

Set& Set::operator-=(const Symbol_t symbol)
{
    symbols_.erase(symbol);

    return *this;
}

Set operator+(Set lhs, const Set& rhs)
{
    lhs += rhs;

    return lhs;
}

Set operator+(Set lhs, const Set::Symbol_t symbol)
{
    lhs += symbol;

    return lhs;
}

Set operator+(const Set::Symbol_t symbol, Set rhs)
{
    rhs += symbol;

    return rhs;
}

Set operator-(Set lhs, const Set& rhs)
{
    lhs -= rhs;

    return lhs;
}

Set operator-(Set lhs, const Set::Symbol_t symbol)
{
    lhs -= symbol;

    return lhs;
}

const Set::Symbols_t& Set::symbols() const noexcept
{
    return symbols_;
}

} // namespace munch::regex

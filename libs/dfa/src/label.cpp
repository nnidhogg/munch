#include "munch/dfa/label.hpp"

#include <functional>

namespace munch::dfa
{
Label::Label(const Symbol_t s) noexcept : symbol_{s}
{}

bool Label::operator==(const Label& other) const noexcept
{
    return symbol_ == other.symbol_;
}

Label::Symbol_t Label::symbol() const noexcept
{
    return symbol_;
}

std::size_t Label::Hash::operator()(const Label& label) const noexcept
{
    return std::hash<Symbol_t>{}(label.symbol());
}

} // namespace munch::dfa

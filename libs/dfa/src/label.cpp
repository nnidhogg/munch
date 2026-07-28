#include "munch/dfa/label.hpp"

namespace munch::dfa
{
Label::Label(const Symbol_t s) noexcept : symbol_{s}
{}

Label::Symbol_t Label::symbol() const noexcept
{
    return symbol_;
}

bool Label::operator==(const Label& other) const noexcept
{
    return symbol_ == other.symbol_;
}

std::size_t Label::Hash::operator()(const Label& label) const noexcept
{
    return std::hash<Symbol_t>{}(label.symbol());
}

} // namespace munch::dfa

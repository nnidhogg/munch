#include "munch/dfa/label.hpp"

#include <functional>

namespace munch::dfa
{
std::size_t Label::Hash::operator()(const Label& label) const noexcept
{
    return std::hash<Symbol_t>{}(label.symbol());
}

Label::Label(const Symbol_t symbol) noexcept : symbol_{symbol}
{}

Label::Symbol_t Label::symbol() const noexcept
{
    return symbol_;
}

} // namespace munch::dfa

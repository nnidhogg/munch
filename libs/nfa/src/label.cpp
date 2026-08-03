#include "munch/nfa/label.hpp"

#include <stdexcept>

namespace munch::nfa
{
bool Epsilon::operator==(const Epsilon&) const noexcept
{
    return true;
}

std::size_t Epsilon::Hash::operator()(const Epsilon&) const noexcept
{
    return 0;
}

Label::Label(const Symbol_t s) noexcept : variant_{s}
{}

Label::Label(const Epsilon e) noexcept : variant_{e}
{}

bool Label::operator==(const Label& other) const noexcept
{
    return variant_ == other.variant_;
}

Label Label::epsilon() noexcept
{
    return Label{Epsilon{}};
}

bool Label::is_symbol() const noexcept
{
    return std::holds_alternative<Symbol_t>(variant_);
}

bool Label::is_epsilon() const noexcept
{
    return std::holds_alternative<Epsilon>(variant_);
}

Label::Symbol_t Label::symbol() const
{
    return std::get<Symbol_t>(variant_);
}

const Label::Variant_t& Label::variant() const noexcept
{
    return variant_;
}

std::size_t Label::Hash::operator()(const Label& label) const noexcept
{
    return std::visit(
            []<typename T>(const T& arg) {
                if constexpr (std::is_same_v<T, Epsilon>)
                {
                    return Epsilon::Hash{}(arg);
                }
                else
                {
                    return std::hash<T>{}(arg);
                }
            },
            label.variant());
}

} // namespace munch::nfa

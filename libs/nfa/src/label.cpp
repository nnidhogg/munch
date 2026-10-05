#include "munch/nfa/label.hpp"

#include <functional>
#include <type_traits>

namespace munch::nfa
{
std::size_t Epsilon::Hash::operator()(const Epsilon&) const noexcept
{
    return 0;
}

bool Epsilon::operator==(const Epsilon&) const noexcept
{
    return true;
}

std::size_t Label::Hash::operator()(const Label& label) const noexcept
{
    const auto hash_of{[]<typename T>(const T& arg) {
        if constexpr (std::is_same_v<T, Epsilon>)
        {
            return Epsilon::Hash{}(arg);
        }
        else
        {
            return std::hash<T>{}(arg);
        }
    }};

    return std::visit(hash_of, label.variant());
}

Label::Label(const Symbol_t symbol) noexcept : variant_{symbol}
{}

Label::Label(const Epsilon epsilon) noexcept : variant_{epsilon}
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

} // namespace munch::nfa

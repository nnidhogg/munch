#include "munch/nfa/token.hpp"

namespace munch::nfa
{
Token::Token(const std::size_t id, const std::size_t priority) noexcept : id_{id}, priority_{priority}
{}

bool Token::operator<(const Token& other) const noexcept
{
    return priority_ != other.priority_ ? priority_ < other.priority_ : id_ < other.id_;
}

bool Token::operator==(const Token& other) const noexcept
{
    return id_ == other.id_ && priority_ == other.priority_;
}

std::size_t Token::id() const noexcept
{
    return id_;
}

std::size_t Token::priority() const noexcept
{
    return priority_;
}

} // namespace munch::nfa

#include "lexer/nfa/token.hpp"

namespace lexer::nfa
{
Token::Token(const std::size_t id, const std::size_t priority) noexcept : id_{id}, priority_{priority}
{}

std::size_t Token::id() const noexcept
{
    return id_;
}

std::size_t Token::priority() const noexcept
{
    return priority_;
}

bool Token::operator<(const Token& other) const noexcept
{
    return priority_ < other.priority_;
}

bool Token::operator==(const Token& other) const noexcept
{
    return id_ == other.id_ && priority_ == other.priority_;
}

} // namespace lexer::nfa

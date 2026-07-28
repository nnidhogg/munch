#include "lexer/dfa/token.hpp"

namespace lexer::dfa
{
Token::Token(const std::size_t id) noexcept : id_{id}
{}

std::size_t Token::id() const noexcept
{
    return id_;
}

bool Token::operator==(const Token& other) const noexcept
{
    return id_ == other.id_;
}

} // namespace lexer::dfa

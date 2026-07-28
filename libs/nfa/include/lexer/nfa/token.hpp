#ifndef LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_TOKEN_HPP
#define LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_TOKEN_HPP

#include <cstddef>

namespace lexer::nfa
{
/**
 * @brief Represents a token in the NFA, identified by a unique ID and priority.
 */
class Token
{
public:
    /**
     * @brief Constructs a token with the given ID and priority.
     * @param id The unique identifier for the token.
     * @param priority The priority of the token (lower means higher priority).
     */
    Token(std::size_t id, std::size_t priority) noexcept;

    /**
     * @brief Returns the unique identifier of the token.
     * @return The token's ID.
     */
    [[nodiscard]] std::size_t id() const noexcept;

    /**
     * @brief Returns the priority of the token.
     * @return The token's priority.
     */
    [[nodiscard]] std::size_t priority() const noexcept;

    /**
     * @brief Less-than comparison operator for tokens (by priority, then ID).
     * @param other The token to compare with.
     * @return True if this token has lower priority or same priority but lower ID.
     */
    bool operator<(const Token& other) const noexcept;

    /**
     * @brief Equality comparison operator for tokens.
     * @param other The token to compare with.
     * @return True if the IDs and priorities are equal, false otherwise.
     */
    bool operator==(const Token& other) const noexcept;

private:
    std::size_t id_;

    std::size_t priority_;
};

} // namespace lexer::nfa

#endif // LEXER_LIBS_NFA_INCLUDE_LEXER_NFA_TOKEN_HPP

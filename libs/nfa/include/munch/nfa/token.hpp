#ifndef MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_TOKEN_HPP
#define MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_TOKEN_HPP

#include <cstddef>

namespace munch::nfa
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
     * @brief Orders tokens by priority, then ID.
     * @param other The token to compare with.
     * @return True if this token has lower priority or same priority but lower ID.
     */
    bool operator<(const Token& other) const noexcept;

    /**
     * @brief Equal when both IDs and both priorities are equal.
     */
    bool operator==(const Token&) const noexcept = default;

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

private:
    /**
     * @brief The unique identifier of the token.
     */
    std::size_t id_;

    /**
     * @brief The priority of the token (lower means higher priority).
     */
    std::size_t priority_;
};

} // namespace munch::nfa

#endif // MUNCH_LIBS_NFA_INCLUDE_MUNCH_NFA_TOKEN_HPP

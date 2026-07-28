#ifndef MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TOKEN_HPP
#define MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TOKEN_HPP

#include <cstddef>

namespace munch::dfa
{
/**
 * @brief Represents a token in the DFA, identified by a unique ID.
 *
 * Unlike NFA tokens, DFA tokens do not store priority information. This is by design:
 * priority resolution happens during subset construction (NFA → DFA conversion) when
 * multiple NFA accept states with different priorities are merged into a single DFA state.
 * At that point, the highest-priority (lowest priority value) token is selected, and only
 * its ID is preserved in the resulting DFA accept state.
 */
class Token
{
public:
    /**
     * @brief Constructs a token with the given ID.
     * @param id The unique identifier for the token.
     */
    explicit Token(std::size_t id) noexcept;

    /**
     * @brief Returns the unique identifier of the token.
     * @return The token's ID.
     */
    [[nodiscard]] std::size_t id() const noexcept;

    /**
     * @brief Equality comparison operator for tokens.
     * @param other The token to compare with.
     * @return True if the IDs are equal, false otherwise.
     */
    bool operator==(const Token& other) const noexcept;

private:
    std::size_t id_;
};

} // namespace munch::dfa

#endif // MUNCH_LIBS_DFA_INCLUDE_MUNCH_DFA_TOKEN_HPP

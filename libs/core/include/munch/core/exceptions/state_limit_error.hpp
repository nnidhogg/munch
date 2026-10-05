#ifndef MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_EXCEPTIONS_STATE_LIMIT_ERROR_HPP
#define MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_EXCEPTIONS_STATE_LIMIT_ERROR_HPP

#include <cstddef>
#include <stdexcept>

namespace munch::core
{
/**
 * @brief Thrown when determinization discovers more DFA states than the configured limit allows.
 *
 * Derives from std::runtime_error, so a handler for that type catches it too; a caller sandboxing untrusted token sets
 * catches this type to distinguish a rejected grammar from any other failure. See Builder::set_state_limit().
 */
class State_limit_error : public std::runtime_error
{
public:
    /**
     * @brief Constructs the error from the limit that was exceeded.
     * @param limit The cap determinization ran into.
     */
    explicit State_limit_error(std::size_t limit);

    /**
     * @brief Returns the configured state limit the construction ran into.
     * @return The limit.
     */
    [[nodiscard]] std::size_t limit() const noexcept;

private:
    /**
     * @brief The configured state limit the construction ran into.
     */
    std::size_t limit_;
};

} // namespace munch::core

#endif // MUNCH_LIBS_CORE_INCLUDE_MUNCH_CORE_EXCEPTIONS_STATE_LIMIT_ERROR_HPP

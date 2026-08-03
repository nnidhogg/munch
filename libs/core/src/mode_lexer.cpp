#include "munch/core/mode_lexer.hpp"

namespace munch::core
{
void Mode_lexer::check(const std::size_t mode) const
{
    if (mode >= lexers_.size())
    {
        throw std::out_of_range{"Mode_lexer: the mode stack names a mode this lexer does not have"};
    }
}

Mode_action Mode_lexer::action_of(const std::size_t mode, const std::size_t token) const noexcept
{
    for (const auto& registered : actions_)
    {
        if (registered.mode == mode && registered.token == token)
        {
            return unpack(registered.action);
        }
    }

    return {};
}

} // namespace munch::core

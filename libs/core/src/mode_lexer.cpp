#include "munch/core/mode_lexer.hpp"

#include <stdexcept>
#include <string>

namespace munch::core
{
Mode_lexer::Mode_lexer(std::vector<Lexer> lexers) : Mode_lexer{std::move(lexers), {}}
{
    if (lexers_.empty())
    {
        throw std::invalid_argument("A mode lexer needs at least one lexer");
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

void Mode_lexer::reject(const std::size_t mode)
{
    throw std::out_of_range{"Mode_lexer: mode " + std::to_string(mode) + " is not a mode of this lexer"};
}

} // namespace munch::core

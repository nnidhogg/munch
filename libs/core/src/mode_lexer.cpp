#include "munch/core/mode_lexer.hpp"

#include <format>
#include <stdexcept>

namespace munch::core
{
Mode_lexer::Mode_lexer(std::vector<Lexer> lexers) : Mode_lexer{std::move(lexers), {}}
{
    if (lexers_.empty())
    {
        throw std::invalid_argument{"A mode lexer needs at least one lexer"};
    }
}

Mode_lexer::Mode_lexer(std::vector<Lexer> lexers, std::vector<Registered> actions)
    : lexers_{std::move(lexers)}, actions_{std::move(actions)}, acting_(lexers_.size(), 0)
{
    for (const auto& [mode, token, action] : actions_)
    {
        acting_[mode] = 1;
    }
}

void Mode_lexer::reject(const std::size_t mode)
{
    const auto message{std::format("Mode_lexer: mode {} is not a mode of this lexer", mode)};

    throw std::out_of_range{message};
}

Mode_action Mode_lexer::action_of(const std::size_t mode, const std::size_t token) const noexcept
{
    for (const auto& [registered_mode, registered_token, action] : actions_)
    {
        if (registered_mode == mode && registered_token == token)
        {
            return unpack(action);
        }
    }

    return {};
}

} // namespace munch::core

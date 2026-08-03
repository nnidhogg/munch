#include "munch/core/mode_builder.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace munch::core
{
Mode_builder::Mode_diagnostics Mode_builder::diagnose() const
{
    Mode_diagnostics out;

    out.per_mode.reserve(modes_.size());

    for (auto builder : modes_)
    {
        builder.set_state_limit(state_limit_);

        out.per_mode.push_back(builder.diagnose());
    }

    // Reachability is a walk from mode 0: a target named only by an unreachable mode is not reached.
    std::vector<bool> entered(modes_.size(), false);

    std::vector<bool> leaves(modes_.size(), false);

    // A token no reachable state awards can never fire, so an action on it neither leaves a mode nor enters one.
    const auto live{[&out](const std::size_t mode, const std::size_t token) {
        return !std::ranges::contains(out.per_mode[mode].dead_tokens, token);
    }};

    for (std::size_t mode{0}; mode < registered_.size(); ++mode)
    {
        for (const auto& [token, action] : registered_[mode])
        {
            if (!live(mode, token))
            {
                continue;
            }

            switch (action.kind)
            {
            case Mode_action_kind::go_to:
            case Mode_action_kind::push:
                leaves[mode] = leaves[mode] || action.target != mode;

                break;

            case Mode_action_kind::pop:
                leaves[mode] = true;

                break;

            case Mode_action_kind::stay:
                break;
            }
        }
    }

    if (!entered.empty())
    {
        entered[0] = true;

        std::vector<std::size_t> pending{0};

        while (!pending.empty())
        {
            const auto mode{pending.back()};

            pending.pop_back();

            if (mode >= registered_.size())
            {
                continue;
            }

            for (const auto& [token, action] : registered_[mode])
            {
                if (!live(mode, token))
                {
                    continue;
                }

                const auto targeted{action.kind == Mode_action_kind::go_to || action.kind == Mode_action_kind::push};

                if (targeted && action.target < entered.size() && !entered[action.target])
                {
                    entered[action.target] = true;

                    pending.push_back(action.target);
                }
            }
        }
    }

    for (std::size_t mode{0}; mode < modes_.size(); ++mode)
    {
        if (mode != 0 && !entered[mode])
        {
            out.unreachable_modes.push_back(mode);
        }

        if (!leaves[mode])
        {
            out.inescapable_modes.push_back(mode);
        }
    }

    return out;
}

Mode_lexer Mode_builder::build() const
{
    if (modes_.empty())
    {
        throw std::invalid_argument{"Mode_builder::build: no tokens were registered"};
    }

    for (std::size_t mode{0}; mode < modes_.size(); ++mode)
    {
        // A skipped index would otherwise compile to a lexer matching nothing, so every scan reaching that mode
        // would fail at its first byte with no indication that the grammar, rather than the input, was wrong.
        if (!populated_[mode])
        {
            throw std::invalid_argument{"Mode_builder::build: mode " + std::to_string(mode) + " has no tokens"};
        }
    }

    // Checked here, not in add_token: a target may legitimately name a mode registered later.
    for (std::size_t mode{0}; mode < registered_.size(); ++mode)
    {
        for (const auto& [token, action] : registered_[mode])
        {
            const auto& [kind, target]{action};

            const auto targeted{kind == Mode_action_kind::go_to || kind == Mode_action_kind::push};

            if (targeted && target >= modes_.size())
            {
                throw std::invalid_argument{
                        "Mode_builder::build: token " + std::to_string(token) + " in mode " + std::to_string(mode) +
                        " targets mode " + std::to_string(target) + ", but only " + std::to_string(modes_.size()) +
                        " were registered"};
            }
        }
    }

    std::vector<Lexer> lexers;

    lexers.reserve(modes_.size());

    // Each token's action rides on its own accepting states, so the driver never looks one up by token ID.
    std::vector<Mode_lexer::Registered> mode_actions;

    std::vector<bool> acting(modes_.size(), false);

    for (std::size_t mode{0}; mode < modes_.size(); ++mode)
    {
        auto builder{modes_[mode]};

        builder.set_state_limit(state_limit_);

        if (mode < registered_.size())
        {
            for (const auto& [token, action] : registered_[mode])
            {
                if (action.kind == Mode_action_kind::stay)
                {
                    continue;
                }

                builder.set_token_payload(token, pack(action));

                mode_actions.push_back({.mode = mode, .token = token, .action = pack(action)});

                acting[mode] = true;
            }
        }

        lexers.push_back(builder.build());
    }

    return Mode_lexer{std::move(lexers), std::move(mode_actions), std::move(acting)};
}

} // namespace munch::core

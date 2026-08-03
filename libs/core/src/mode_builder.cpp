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

    for (std::size_t mode{0}; mode < actions_.size(); ++mode)
    {
        for (const auto& action : actions_[mode])
        {
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

            if (mode >= actions_.size())
            {
                continue;
            }

            for (const auto& action : actions_[mode])
            {
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
    for (std::size_t mode{0}; mode < actions_.size(); ++mode)
    {
        for (std::size_t token{0}; token < actions_[mode].size(); ++token)
        {
            const auto& [kind, target]{actions_[mode][token]};

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

    for (auto builder : modes_)
    {
        builder.set_state_limit(state_limit_);

        lexers.push_back(builder.build());
    }

    // Choose a lookup per mode by how many action tokens it has. Nothing is indexed by public token value, so a
    // sparse enumeration costs only its own pairs.
    std::vector<Mode_dispatch> dispatch(modes_.size());

    std::vector<std::pair<std::size_t, Mode_action>> pairs;

    for (std::size_t mode{0}; mode < modes_.size(); ++mode)
    {
        std::vector<std::pair<std::size_t, Mode_action>> mine;

        if (mode < actions_.size())
        {
            for (std::size_t token{0}; token < actions_[mode].size(); ++token)
            {
                if (actions_[mode][token].kind != Mode_action_kind::stay)
                {
                    mine.emplace_back(token, actions_[mode][token]);
                }
            }
        }

        auto& chosen{dispatch[mode]};

        if (mine.empty())
        {
            chosen.shape = Mode_dispatch::Shape::none;

            continue;
        }

        if (mine.size() == 1)
        {
            chosen.shape = Mode_dispatch::Shape::one;

            chosen.token = mine.front().first;

            chosen.action = mine.front().second;

            continue;
        }

        const auto maskable{std::ranges::all_of(mine, [](const auto& pair) { return pair.first < 64; })};

        chosen.shape = maskable ? Mode_dispatch::Shape::masked : Mode_dispatch::Shape::sparse;

        chosen.first = pairs.size();

        chosen.count = mine.size();

        for (const auto& [token, action] : mine)
        {
            if (maskable)
            {
                chosen.mask |= std::uint64_t{1} << token;
            }

            pairs.push_back({token, action});
        }
    }

    return Mode_lexer{std::move(lexers), std::move(dispatch), std::move(pairs)};
}

} // namespace munch::core

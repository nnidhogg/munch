#include "munch/core/mode_builder.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace munch::core
{
namespace
{
/**
 * @brief Every registered token and its normalized action, per mode, as Mode_builder holds them.
 */
using Registered_t = std::vector<std::vector<std::pair<std::size_t, Mode_action>>>;

/**
 * @brief Rejects a grammar with a skipped mode index.
 *
 * A skipped index would otherwise compile to a lexer matching nothing, so every scan reaching that mode would fail at
 * its first byte with no indication that the grammar, rather than the input, was wrong.
 * @param registered The registered tokens and actions, per mode.
 * @throws std::invalid_argument If some mode received none.
 */
void require_populated(const Registered_t& registered)
{
    const auto skipped{std::ranges::find_if(registered, [](const auto& tokens) { return tokens.empty(); })};

    if (skipped == registered.cend())
    {
        return;
    }

    const auto mode{std::distance(registered.cbegin(), skipped)};

    const auto message{std::format("Mode_builder::build: mode {} has no tokens", mode)};

    throw std::invalid_argument{message};
}

/**
 * @brief Returns whether an action names a target mode: a go_to or a push.
 * @param action The action.
 * @return Whether the action is a go_to or a push.
 */
[[nodiscard]] bool targets_a_mode(const Mode_action& action) noexcept
{
    return action.kind == Mode_action_kind::go_to || action.kind == Mode_action_kind::push;
}

/**
 * @brief Rejects an action whose target names no registered mode.
 *
 * Checked at build time, not in add_token: a target may legitimately name a mode registered later.
 * @param registered The registered tokens and actions, per mode.
 * @param modes The number of registered modes.
 * @throws std::invalid_argument If a go_to or push targets a mode past the last one.
 */
void require_targets_exist(const Registered_t& registered, const std::size_t modes)
{
    for (std::size_t mode{0}; mode < registered.size(); ++mode)
    {
        for (const auto& [token, action] : registered[mode])
        {
            if (!targets_a_mode(action) || action.target < modes)
            {
                continue;
            }

            const auto message{std::format(
                    "Mode_builder::build: token {} in mode {} targets mode {}, but only {} were registered", token,
                    mode, action.target, modes)};

            throw std::invalid_argument{message};
        }
    }
}

/**
 * @brief Rejects an action on the token a mode's lexer matches the empty string with.
 *
 * A token matching the empty string is the one the initial state accepts, which is what matching an empty input
 * reports. The two drivers disagree about such a token, since the batch one stops without reporting it at all, and an
 * action on it would make them disagree about the mode as well.
 * @param lexer The mode's compiled lexer.
 * @param actions The mode's registered tokens and their actions.
 * @param mode The mode, for the message of the rejection.
 * @throws std::invalid_argument If the empty-matching token carries an action other than stay.
 */
void reject_nullable_actions(
        const Lexer& lexer, const std::span<const std::pair<std::size_t, Mode_action>> actions, const std::size_t mode)
{
    const auto [nullable, length]{lexer.tokenize<std::size_t>(std::string_view{})};

    if (!nullable)
    {
        return;
    }

    for (const auto& [token, action] : actions)
    {
        if (token != *nullable || action.kind == Mode_action_kind::stay)
        {
            continue;
        }

        const auto message{std::format(
                "Mode_builder::build: token {} in mode {} matches the empty string and carries an action", token,
                mode)};

        throw std::invalid_argument{message};
    }
}

/**
 * @brief Returns whether a token can fire in a mode: one no reachable state awards can never fire, so an action on it
 *        neither leaves a mode nor enters one.
 * @param per_mode The per-mode diagnostics, indexed by mode.
 * @param mode The mode the token is registered in.
 * @param token The token.
 * @return Whether the token is live in the mode.
 */
[[nodiscard]] bool live(
        const std::vector<Builder::Diagnostics>& per_mode, const std::size_t mode, const std::size_t token)
{
    return !std::ranges::contains(per_mode[mode].dead_tokens, token);
}

/**
 * @brief What one walk of the live go_to and push actions from mode 0 establishes.
 */
struct Closure
{
    /**
     * @brief Whether each mode is entered, indexed by mode; a target named only by an unreachable mode is not.
     */
    std::vector<bool> entered{};

    /**
     * @brief For each mode, the modes its frames can name.
     */
    std::vector<std::vector<bool>> framed{};
};

/**
 * @brief Walks the live go_to and push actions from mode 0, marking every mode entered and closing the frames each
 *        mode's outstanding pushes can name, to one fixpoint.
 *
 * Which modes the outstanding frames can name matters since that is where a pop returns: presence alone would let a
 * self-push fake an escape. Closing over push and go_to reaches every frame a pop can expose, because those buried
 * under one naming f are a stack f once held.
 * @param registered The registered tokens and actions, per mode.
 * @param per_mode The per-mode diagnostics, indexed by mode.
 * @return The modes entered and the modes each mode's frames can name.
 */
[[nodiscard]] Closure frame_closure(const Registered_t& registered, const std::vector<Builder::Diagnostics>& per_mode)
{
    const auto modes{per_mode.size()};

    std::vector<bool> entered(modes, false);

    if (modes > 0)
    {
        entered[0] = true;
    }

    std::vector<std::vector<bool>> framed(modes, std::vector<bool>(modes, false));

    const auto carry{[&framed](const std::size_t mode, const Mode_action& action) {
        auto& target{framed[action.target]};

        auto changed{false};

        for (std::size_t named{0}; named < framed[mode].size(); ++named)
        {
            if (!framed[mode][named] || target[named])
            {
                continue;
            }

            target[named] = true;

            changed = true;
        }

        if (action.kind == Mode_action_kind::push && !target[mode])
        {
            target[mode] = true;

            changed = true;
        }

        return changed;
    }};

    const auto sweep_mode{[&](const std::size_t mode) {
        auto changed{false};

        for (const auto& [token, action] : registered[mode])
        {
            if (!live(per_mode, mode, token) || !targets_a_mode(action) || action.target >= framed.size())
            {
                continue;
            }

            if (!entered[action.target])
            {
                entered[action.target] = true;

                changed = true;
            }

            changed = carry(mode, action) || changed;
        }

        return changed;
    }};

    const auto sweep{[&] {
        auto changed{false};

        for (std::size_t mode{0}; mode < registered.size(); ++mode)
        {
            if (!entered[mode])
            {
                continue;
            }

            changed = sweep_mode(mode) || changed;
        }

        return changed;
    }};

    // Sweeps until a pass changes nothing.
    while (sweep())
    {
    }

    return {.entered = std::move(entered), .framed = std::move(framed)};
}

/**
 * @brief Marks every mode a live action can leave: a non-self go_to or push, or a pop whose frames name another mode.
 * @param registered The registered tokens and actions, per mode.
 * @param per_mode The per-mode diagnostics, indexed by mode.
 * @param framed For each mode, the modes its frames can name.
 * @return Whether each mode can be left, indexed by mode.
 */
[[nodiscard]] std::vector<bool> escaping_modes(
        const Registered_t& registered, const std::vector<Builder::Diagnostics>& per_mode,
        const std::vector<std::vector<bool>>& framed)
{
    const auto frames_another{[&framed](const std::size_t mode) {
        const auto names_other{
                [&framed, mode](const std::size_t named) { return named != mode && framed[mode][named]; }};

        return std::ranges::any_of(std::views::iota(std::size_t{0}, framed[mode].size()), names_other);
    }};

    std::vector<bool> leaves(per_mode.size(), false);

    for (std::size_t mode{0}; mode < registered.size(); ++mode)
    {
        for (const auto& [token, action] : registered[mode])
        {
            if (!live(per_mode, mode, token))
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
                leaves[mode] = leaves[mode] || frames_another(mode);

                break;

            case Mode_action_kind::stay:
                break;
            }
        }
    }

    return leaves;
}

} // namespace

Mode_lexer Mode_builder::build() const
{
    if (modes_.empty())
    {
        throw std::invalid_argument{"Mode_builder::build: no tokens were registered"};
    }

    require_populated(registered_);

    require_targets_exist(registered_, modes_.size());

    std::vector<Lexer> lexers{};

    lexers.reserve(modes_.size());

    // Each token's action rides on its own accepting states, so the batch driver never looks one up by token ID; the
    // per-token driver does.
    std::vector<Mode_lexer::Registered> mode_actions{};

    for (std::size_t mode{0}; mode < modes_.size(); ++mode)
    {
        auto builder{modes_[mode]};

        builder.set_state_limit(state_limit_);

        for (const auto& [token, action] : registered_[mode])
        {
            if (action.kind == Mode_action_kind::stay)
            {
                continue;
            }

            const auto packed{pack(action)};

            builder.set_token_payload(token, packed);

            mode_actions.push_back({.mode = mode, .token = token, .action = packed});
        }

        lexers.push_back(builder.build());

        reject_nullable_actions(lexers.back(), registered_[mode], mode);
    }

    return Mode_lexer{std::move(lexers), std::move(mode_actions)};
}

Mode_builder::Mode_diagnostics Mode_builder::diagnose() const
{
    Mode_diagnostics out{};

    out.per_mode.reserve(modes_.size());

    for (auto builder : modes_)
    {
        builder.set_state_limit(state_limit_);

        out.per_mode.push_back(builder.diagnose());
    }

    const auto [entered, framed]{frame_closure(registered_, out.per_mode)};

    const auto leaves{escaping_modes(registered_, out.per_mode, framed)};

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

void Mode_builder::register_token(
        const std::size_t index, const regex::Regex& regex, const std::size_t id, const std::size_t priority,
        const Mode_action action)
{
    // An action kind outside the enumeration reaches apply(), which rejects it, while the batch driver ignores that
    // rejection: the two drivers would disagree on the same input.
    switch (action.kind)
    {
    case Mode_action_kind::stay:
    case Mode_action_kind::go_to:
    case Mode_action_kind::push:
    case Mode_action_kind::pop:
        break;

    default:
        throw std::invalid_argument{"Mode_builder::add_token: the action kind is not one of the four"};
    }

    // Stay and pop carry target zero, so two otherwise identical actions compare equal, and a go_to onto its own mode
    // is a stay, so a mode whose only action is that one takes the driver's no-action path.
    const auto self_go_to{action.kind == Mode_action_kind::go_to && action.target == index};

    const Mode_action normalized{
            .kind = self_go_to ? Mode_action_kind::stay : action.kind,
            .target = targets_a_mode(action) && !self_go_to ? action.target : std::size_t{0}};

    const auto conflicts{[&](const std::pair<std::size_t, Mode_action>& entry) {
        const auto& [declared, previous]{entry};

        const auto same{previous.kind == normalized.kind && previous.target == normalized.target};

        return declared == id && !same;
    }};

    // Patterns may share a token ID; conflicting actions for it cannot, since the scanner reports only the ID. Checked
    // before anything is resized or registered, so a caught exception leaves the builder as it was.
    const auto conflicting{index < registered_.size() && std::ranges::any_of(registered_[index], conflicts)};

    if (conflicting)
    {
        const auto message{std::format(
                "Mode_builder::add_token: token {} in mode {} already carries a different action", id, index)};

        throw std::invalid_argument{message};
    }

    if (index >= modes_.size())
    {
        modes_.resize(index + 1);

        registered_.resize(index + 1);
    }

    modes_[index].add_token(regex, id, priority);

    const auto known{std::ranges::contains(registered_[index] | std::views::keys, id)};

    if (!known)
    {
        registered_[index].emplace_back(id, normalized);
    }
}

} // namespace munch::core

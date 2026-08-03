#include "munch/core/mode.hpp"

namespace munch::core
{
bool Mode_stack::apply(const Mode_action& action)
{
    switch (action.kind)
    {
    case Mode_action_kind::stay:
        return true;

    case Mode_action_kind::go_to:
        current = action.target;

        return true;

    case Mode_action_kind::push:
        saved.push_back(current);

        current = action.target;

        return true;

    case Mode_action_kind::pop:
        if (saved.empty())
        {
            return false;
        }

        current = saved.back();

        saved.pop_back();

        return true;
    }

    return false;
}

} // namespace munch::core

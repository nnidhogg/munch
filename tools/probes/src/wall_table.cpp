#include "munch/tools/probes/wall_table.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "munch/dfa/dfa.hpp"

namespace munch::tools::probes
{
Table table_of(
        const std::size_t states, std::vector<char> accept, const std::function<int(std::size_t state, int byte)>& row)
{
    Table table{
            .states = states,
            .init = 0,
            .next = std::vector<std::array<int, 256>>(states),
            .accept = std::move(accept)};

    for (std::size_t state{0}; state < states; ++state)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            table.next[state][static_cast<std::size_t>(byte)] = row(state, byte);
        }
    }

    return table;
}

Table extract(const dfa::Dfa& dfa)
{
    std::map<dfa::Dfa::State_t, std::size_t> index{};

    std::vector<dfa::Dfa::State_t> order{};

    const auto intern{[&](const dfa::Dfa::State_t state) {
        if (const auto found{index.find(state)}; found != index.end())
        {
            return found->second;
        }

        index.emplace(state, order.size());

        order.push_back(state);

        return order.size() - 1;
    }};

    intern(dfa.init_state());

    std::vector<std::array<int, 256>> raw{};

    for (std::size_t at{0}; at < order.size(); ++at)
    {
        raw.emplace_back();

        raw.back().fill(kDead);

        for (int byte{0}; byte < 256; ++byte)
        {
            if (const auto to{dfa.advance(order[at], static_cast<char>(byte))})
            {
                raw[at][static_cast<std::size_t>(byte)] = static_cast<int>(intern(*to));
            }
        }
    }

    std::vector<char> accept(order.size(), 0);

    for (std::size_t at{0}; at < order.size(); ++at)
    {
        accept[at] = dfa.has_accept_token(order[at]) ? 1 : 0;
    }

    std::vector<char> co{accept};

    for (auto grew{true}; grew;)
    {
        grew = false;

        for (std::size_t at{0}; at < raw.size(); ++at)
        {
            if (co[at])
            {
                continue;
            }

            for (int byte{0}; byte < 256 && !co[at]; ++byte)
            {
                if (const auto to{raw[at][static_cast<std::size_t>(byte)]};
                    to != kDead && co[static_cast<std::size_t>(to)])
                {
                    co[at] = 1;

                    grew = true;
                }
            }
        }
    }

    Table table{.states = raw.size(), .init = 0, .next = std::move(raw), .accept = std::move(accept)};

    for (auto& row : table.next)
    {
        for (auto& to : row)
        {
            if (to != kDead && !co[static_cast<std::size_t>(to)])
            {
                to = kDead;
            }
        }
    }

    return table;
}

std::vector<std::size_t> live_states(const Table& table)
{
    std::vector<std::size_t> states{};

    for (std::size_t state{0}; state < table.states; ++state)
    {
        auto live{table.accept[state] != 0};

        for (int byte{0}; byte < 256 && !live; ++byte)
        {
            live = table.next[state][static_cast<std::size_t>(byte)] != kDead;
        }

        if (live)
        {
            states.push_back(state);
        }
    }

    return states;
}

bool is_init_reentrant(const Table& table)
{
    for (std::size_t state{0}; state < table.states; ++state)
    {
        for (int byte{0}; byte < 256; ++byte)
        {
            if (table.next[state][static_cast<std::size_t>(byte)] == static_cast<int>(table.init))
            {
                return true;
            }
        }
    }

    return false;
}

std::optional<std::size_t> scan_step(const Table& table, const std::size_t state, const unsigned char byte)
{
    if (const auto to{table.next[state][byte]}; to != kDead)
    {
        return static_cast<std::size_t>(to);
    }

    if (table.accept[state] == 0)
    {
        return std::nullopt;
    }

    if (const auto to{table.next[table.init][byte]}; to != kDead)
    {
        return static_cast<std::size_t>(to);
    }

    return std::nullopt;
}

std::optional<std::size_t> scan_word(const Table& table, std::size_t state, const std::string_view word)
{
    for (const auto letter : word)
    {
        const auto next{scan_step(table, state, static_cast<unsigned char>(letter))};

        if (!next)
        {
            return std::nullopt;
        }

        state = *next;
    }

    return state;
}

std::optional<std::vector<std::size_t>> serial_boundaries(const Table& table, const std::string_view input)
{
    std::vector<std::size_t> starts{0};

    std::size_t state{table.init};

    auto consumed_first{false};

    for (std::size_t at{0}; at < input.size(); ++at)
    {
        const auto byte{static_cast<unsigned char>(input[at])};

        if (const auto to{table.next[state][byte]}; to != kDead)
        {
            state = static_cast<std::size_t>(to);
        }
        else
        {
            if (table.accept[state] == 0)
            {
                return std::nullopt;
            }

            starts.push_back(at);

            const auto restart{table.next[table.init][byte]};

            if (restart == kDead)
            {
                return std::nullopt;
            }

            state = static_cast<std::size_t>(restart);
        }

        consumed_first = true;
    }

    if (consumed_first && table.accept[state] == 0)
    {
        return std::nullopt;
    }

    starts.push_back(input.size());

    return starts;
}

} // namespace munch::tools::probes

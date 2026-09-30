#include "munch/tools/probes/gate_evidence.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grammars.hpp"
#include "munch/core/lexer.hpp"
#include "munch/dfa/dfa.hpp"
#include "munch/tools/probes/gate_totals.hpp"
#include "munch/tools/probes/window_model.hpp"

namespace munch::tools::probes
{
namespace
{
// Implements gate_evidence.hpp: the prefix family, the covering-token scan, the token completion and the rewind count
// are private to this unit.

using dfa::Dfa;
using figures::Token;

/**
 * @brief A live automaton state and how many bytes the scan has run past its last accepting position.
 */
using Distance_key_t = std::pair<Dfa::State_t, std::size_t>;

/**
 * @brief The shortest word reaching every live (state, distance past the last accepting position) pair from the
 *        initial state, breadth first, with the distance at most a bound; an accepting state resets the distance.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param max_distance The largest distance kept.
 * @return Per reached pair, the shortest word reaching it, the initial pair's word empty.
 */
std::map<Distance_key_t, std::string> distance_prefixes(
        const Dfa& dfa, const States_t& live, const std::size_t max_distance)
{
    std::map<Distance_key_t, std::string> prefix{};

    std::deque<Distance_key_t> pending{};

    const auto seed{Distance_key_t{dfa.init_state(), 0}};

    prefix[seed] = "";

    pending.push_back(seed);

    while (!pending.empty())
    {
        const auto [state, distance]{pending.front()};

        pending.pop_front();

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{dfa.advance(state, static_cast<char>(symbol))};

            if (!next || !live.contains(*next))
            {
                continue;
            }

            const auto moved{dfa.has_accept_token(*next) ? std::size_t{0} : distance + 1};

            if (moved > max_distance)
            {
                continue;
            }

            if (const auto key{Distance_key_t{*next, moved}}; !prefix.contains(key))
            {
                prefix[key] = prefix[{state, distance}] + static_cast<char>(symbol);

                pending.push_back(key);
            }
        }
    }

    return prefix;
}

/**
 * @brief One scan of an input and the token covering one of its positions.
 */
struct Covering
{
    /**
     * @brief The bytes the scan consumed.
     */
    std::size_t consumed{0};

    /**
     * @brief Where the scanned token containing the position begins, std::nullopt when the scan stopped before it.
     */
    std::optional<std::size_t> start;
};

/**
 * @brief Scans an input and finds where the token containing one position begins.
 * @param lexer The lexer.
 * @param input The input.
 * @param last The position.
 * @return The consumed byte count and the covering token's start.
 */
Covering covering_start(const core::Lexer& lexer, const std::string& input, const std::size_t last)
{
    Covering covering{};

    std::size_t offset{0};

    covering.consumed = lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t length) {
        if (offset <= last && last < offset + length)
        {
            covering.start = offset;
        }

        offset += length;
    });

    return covering;
}

/**
 * @brief Counts the rewinds of a full maximal-munch scan over the automaton: the tokens whose scan ran past their last
 *        accepting position before stopping.
 * @param dfa The automaton.
 * @param input The input.
 * @return The rewinds, counted up to the first position no token accepts from.
 */
std::size_t rewinds(const Dfa& dfa, const std::string& input)
{
    std::size_t count{0};

    std::size_t at{0};

    while (at < input.size())
    {
        auto state{dfa.init_state()};

        auto accepted{false};

        auto last_accept{at};

        auto reached{at};

        while (reached < input.size())
        {
            const auto next{dfa.advance(state, input[reached])};

            if (!next)
            {
                break;
            }

            state = *next;

            ++reached;

            if (dfa.has_accept_token(state))
            {
                last_accept = reached;

                accepted = true;
            }
        }

        if (!accepted)
        {
            break;
        }

        count += reached > last_accept ? 1 : 0;

        at = last_accept;
    }

    return count;
}

/**
 * @brief The shortest bytes completing the token a window begins at its origin: walks the window from the origin in
 *        the automaton and, when the walk stays live and ends in a state that does not accept, searches breadth first
 *        for the nearest accepting state.
 * @param dfa The automaton.
 * @param live The automaton's trim states.
 * @param window The window.
 * @param origin The offset inside the window at which the token begins.
 * @return The completion, std::nullopt when the walk dies, already accepts, or reaches no accepting state.
 */
std::optional<std::string> completion(
        const Dfa& dfa, const States_t& live, const std::string& window, const std::size_t origin)
{
    auto state{dfa.init_state()};

    for (std::size_t i{origin}; i < window.size(); ++i)
    {
        const auto next{dfa.advance(state, window[i])};

        if (!next || !live.contains(*next))
        {
            return std::nullopt;
        }

        state = *next;
    }

    if (dfa.has_accept_token(state))
    {
        return std::nullopt;
    }

    std::map<Dfa::State_t, std::string> suffix{{state, ""}};

    std::deque<Dfa::State_t> walk{state};

    while (!walk.empty())
    {
        const auto at{walk.front()};

        walk.pop_front();

        if (dfa.has_accept_token(at))
        {
            return suffix[at];
        }

        for (int symbol{0}; symbol < 256; ++symbol)
        {
            const auto next{dfa.advance(at, static_cast<char>(symbol))};

            if (next && live.contains(*next) && !suffix.contains(*next))
            {
                suffix[*next] = suffix[at] + static_cast<char>(symbol);

                walk.push_back(*next);
            }
        }
    }

    return std::nullopt;
}
} // namespace

Backup backup_disagreements(
        const Dfa& dfa, const core::Lexer& lexer, const States_t& live, const std::vector<std::string>& windows,
        const std::optional<std::size_t> force_origin)
{
    Backup backup{};

    const auto reentrant{is_init_reentrant(dfa, live)};

    const auto prefix{distance_prefixes(dfa, live, 9)};

    // An accepted word ahead of a prefix varies the distance from the last potential boundary to the window.
    std::vector<std::string> completed{""};

    for (const auto& [key, head] : prefix)
    {
        if (dfa.has_accept_token(key.first) && !head.empty() && completed.size() < 6)
        {
            completed.push_back(head);
        }
    }

    std::vector<std::string> heads{};

    for (const auto& done : completed)
    {
        for (const auto& [key, head] : prefix)
        {
            heads.push_back(done + head);
        }
    }

    backup.prefixes = heads.size();

    constexpr std::string_view tails[]{"", " ", "\n", " x", ";\n", "\n}\n", " 1 ", "\"s\"\n"};

    for (const auto& window : windows)
    {
        const auto at{predicted(dfa, live, window, reentrant)};

        if (!at)
        {
            continue;
        }

        for (const auto& head : heads)
        {
            for (const auto tail : tails)
            {
                auto input{head + window};

                input += tail;

                const auto covering{covering_start(lexer, input, head.size() + window.size() - 1)};

                if (covering.consumed < head.size() + window.size())
                {
                    continue;
                }

                const auto rewound{rewinds(dfa, input) > 0};

                backup.exercised += rewound ? 1 : 0;

                backup.tokenizable += rewound && covering.consumed == input.size() ? 1 : 0;

                backup.disagreements += covering.start == head.size() + force_origin.value_or(*at) ? 0 : 1;
            }
        }
    }

    return backup;
}

std::optional<std::pair<std::string, std::string>> find_witness(
        Gate_totals& totals, const Dfa& dfa, const core::Lexer& lexer, const States_t& live,
        const std::vector<Certified_window>& words)
{
    const auto prefix{distance_prefixes(dfa, live, 6)};

    std::vector<std::string> heads{""};

    std::vector<std::string> tails{""};

    for (const auto& [key, head] : prefix)
    {
        heads.push_back(head);

        if (dfa.has_accept_token(key.first) && !head.empty() && tails.size() < 8)
        {
            tails.push_back(head);
        }
    }

    for (const std::string_view generic : {" ", "\n", " x", ";\n", " 1 "})
    {
        tails.emplace_back(generic);
    }

    for (const auto& [window, origin] : words)
    {
        auto tried{tails};

        if (const auto ending{completion(dfa, live, window, origin)})
        {
            tried.insert(tried.begin(), *ending);
        }

        for (const auto& head : heads)
        {
            for (const auto& tail : tried)
            {
                const auto input{head + window + tail};

                const auto covering{covering_start(lexer, input, head.size() + window.size() - 1)};

                if (covering.consumed != input.size())
                {
                    continue;
                }

                if (covering.start != head.size() + origin)
                {
                    ++totals.witness_disagreements;

                    continue;
                }

                return std::pair{input, window};
            }
        }
    }

    return std::nullopt;
}

std::pair<std::size_t, std::size_t> covering_violations(
        const core::Lexer& lexer, const std::string& window, const std::size_t origin, const std::string& alphabet,
        const std::size_t max_length)
{
    std::size_t occurrences{0};

    std::size_t violations{0};

    std::string input{};

    for (std::size_t length{window.size()}; length <= max_length; ++length)
    {
        std::size_t count{1};

        for (std::size_t i{0}; i < length; ++i)
        {
            count *= alphabet.size();
        }

        for (std::size_t index{0}; index < count; ++index)
        {
            input.assign(length, alphabet[0]);

            for (std::size_t i{0}, rest{index}; i < length; ++i, rest /= alphabet.size())
            {
                input[i] = alphabet[rest % alphabet.size()];
            }

            std::vector<std::size_t> covering(length, 0);

            std::size_t offset{0};

            const auto consumed{lexer.tokenize_all<Token>(input, [&](const Token, const std::size_t token_length) {
                for (std::size_t inside{0}; inside < token_length; ++inside)
                {
                    covering[offset + inside] = offset;
                }

                offset += token_length;
            })};

            if (consumed < length)
            {
                continue;
            }

            for (std::size_t at{0}; at + window.size() <= length; ++at)
            {
                if (input.compare(at, window.size(), window) == 0)
                {
                    ++occurrences;

                    violations += covering[at + window.size() - 1] == at + origin ? 0 : 1;
                }
            }
        }
    }

    return {occurrences, violations};
}

} // namespace munch::tools::probes

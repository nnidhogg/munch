#include "munch/tools/probes/gate_evidence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
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
using dfa::Dfa;
using figures::Token;

/**
 * @brief A live automaton state and how many bytes the scan has run past its last accepting position.
 */
using Distance_key_t = std::pair<Dfa::State_t, std::size_t>;

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
    std::optional<std::size_t> start{};
};

/**
 * @brief Returns the shortest word reaching every live (state, distance past the last accepting position) pair from the
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

    const Distance_key_t seed{dfa.init_state(), 0};

    prefix[seed] = "";

    pending.push_back(seed);

    while (!pending.empty())
    {
        const auto [state, distance]{pending.front()};

        pending.pop_front();

        for (const auto symbol : every_byte())
        {
            const auto next{dfa.advance(state, symbol)};

            if (!next || !live.contains(*next))
            {
                continue;
            }

            const auto moved{dfa.has_accept_token(*next) ? std::size_t{0} : distance + 1};

            if (moved > max_distance)
            {
                continue;
            }

            const Distance_key_t key{*next, moved};

            if (prefix.contains(key))
            {
                continue;
            }

            const auto word{prefix[{state, distance}] + symbol};

            prefix[key] = word;

            pending.push_back(key);
        }
    }

    return prefix;
}

/**
 * @brief Returns the empty head followed by every non-empty prefix whose state accepts, in the prefixes' order, until
 *        the list holds a cap of entries.
 * @param dfa The automaton.
 * @param prefix The prefixes, by state and distance.
 * @param cap The most entries the list holds.
 * @return The heads.
 */
std::vector<std::string> accepted_heads(
        const Dfa& dfa, const std::map<Distance_key_t, std::string>& prefix, const std::size_t cap)
{
    std::vector<std::string> heads{""};

    for (const auto& [key, head] : prefix)
    {
        const auto& [state, distance]{key};

        if (dfa.has_accept_token(state) && !head.empty() && heads.size() < cap)
        {
            heads.push_back(head);
        }
    }

    return heads;
}

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

    const auto locate{[&covering, &offset, last](const Token, const std::size_t length) {
        if (offset <= last && last < offset + length)
        {
            covering.start = offset;
        }

        offset += length;
    }};

    covering.consumed = lexer.tokenize_all<Token>(input, locate);

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

        if (reached > last_accept)
        {
            ++count;
        }

        at = last_accept;
    }

    return count;
}

/**
 * @brief Returns the shortest bytes completing the token a window begins at its origin: walks the window from the
 *        origin in the automaton and, when the walk stays live and ends in a state that does not accept, searches
 *        breadth first for the nearest accepting state.
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

        for (const auto symbol : every_byte())
        {
            const auto next{dfa.advance(at, symbol)};

            if (!next || !live.contains(*next) || suffix.contains(*next))
            {
                continue;
            }

            const auto word{suffix[at] + symbol};

            suffix[*next] = word;

            walk.push_back(*next);
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

    constexpr std::size_t prefix_distance{9};

    const auto prefix{distance_prefixes(dfa, live, prefix_distance)};

    constexpr std::size_t completed_heads{6};

    // An accepted word ahead of a prefix varies the distance from the last potential boundary to the window.
    const auto completed{accepted_heads(dfa, prefix, completed_heads)};

    std::vector<std::string> heads{};

    for (const auto& done : completed)
    {
        for (const auto& head : prefix | std::views::values)
        {
            heads.push_back(done + head);
        }
    }

    backup.prefixes = heads.size();

    const auto scan_one{[&](const std::string& head, const std::string& window, const std::size_t origin,
                            const std::string_view tail) {
        auto input{head + window};

        input += tail;

        const auto last{head.size() + window.size() - 1};

        const auto [consumed, start]{covering_start(lexer, input, last)};

        if (consumed < head.size() + window.size())
        {
            return;
        }

        const auto rewound{rewinds(dfa, input) > 0};

        if (rewound)
        {
            ++backup.exercised;
        }

        const auto complete{consumed == input.size()};

        if (rewound && complete)
        {
            ++backup.tokenizable;
        }

        const auto starts_at_origin{start == head.size() + origin};

        if (!starts_at_origin)
        {
            ++backup.disagreements;
        }
    }};

    for (const auto& window : windows)
    {
        const auto at{predicted(dfa, live, window, reentrant)};

        if (!at)
        {
            continue;
        }

        const auto origin{force_origin.value_or(*at)};

        constexpr std::array<std::string_view, 8> tails{"", " ", "\n", " x", ";\n", "\n}\n", " 1 ", "\"s\"\n"};

        for (const auto& [head, tail] : std::views::cartesian_product(heads, tails))
        {
            scan_one(head, window, origin, tail);
        }
    }

    return backup;
}

std::optional<Occurrence_witness> find_witness(
        Gate_totals& totals, const Dfa& dfa, const core::Lexer& lexer, const States_t& live,
        const std::vector<Certified_window>& words)
{
    constexpr std::size_t prefix_distance{6};

    const auto prefix{distance_prefixes(dfa, live, prefix_distance)};

    std::vector<std::string> heads{""};

    std::ranges::copy(prefix | std::views::values, std::back_inserter(heads));

    constexpr std::size_t accepted_tails{8};

    auto tails{accepted_heads(dfa, prefix, accepted_tails)};

    constexpr std::array<std::string_view, 5> generic_tails{" ", "\n", " x", ";\n", " 1 "};

    for (const auto generic : generic_tails)
    {
        tails.emplace_back(generic);
    }

    const auto first_agreeing{
            [&](const std::string& window, const std::size_t origin,
                const std::vector<std::string>& tried) -> std::optional<std::string> {
                for (const auto& [head, tail] : std::views::cartesian_product(heads, tried))
                {
                    const auto input{head + window + tail};

                    const auto last{head.size() + window.size() - 1};

                    const auto [consumed, start]{covering_start(lexer, input, last)};

                    if (consumed != input.size())
                    {
                        continue;
                    }

                    if (start != head.size() + origin)
                    {
                        ++totals.witness_disagreements;

                        continue;
                    }

                    return input;
                }

                return std::nullopt;
            }};

    for (const auto& [window, origin] : words)
    {
        auto tried{tails};

        if (const auto ending{completion(dfa, live, window, origin)})
        {
            tried.insert(tried.begin(), *ending);
        }

        if (const auto input{first_agreeing(window, origin, tried)})
        {
            return Occurrence_witness{.input = *input, .window = window};
        }
    }

    return std::nullopt;
}

Coverage covering_violations(
        const core::Lexer& lexer, const std::string& window, const std::size_t origin, const std::string& alphabet,
        const std::size_t max_length)
{
    std::size_t occurrences{0};

    std::size_t violations{0};

    std::string input{};

    const auto count_occurrences{[&](const std::size_t length) {
        std::vector<std::size_t> covering(length, 0);

        std::size_t offset{0};

        const auto mark{[&covering, &offset](const Token, const std::size_t token_length) {
            for (std::size_t inside{0}; inside < token_length; ++inside)
            {
                covering[offset + inside] = offset;
            }

            offset += token_length;
        }};

        const auto consumed{lexer.tokenize_all<Token>(input, mark)};

        if (consumed < length)
        {
            return;
        }

        for (std::size_t at{0}; at + window.size() <= length; ++at)
        {
            if (input.compare(at, window.size(), window) != 0)
            {
                continue;
            }

            ++occurrences;

            const auto covered_from_origin{covering[at + window.size() - 1] == at + origin};

            if (!covered_from_origin)
            {
                ++violations;
            }
        }
    }};

    const auto inputs_of_length{[&alphabet](const std::size_t length) {
        std::size_t count{1};

        for (std::size_t i{0}; i < length; ++i)
        {
            count *= alphabet.size();
        }

        return count;
    }};

    const auto spell{[&alphabet, &input](const std::size_t length, const std::size_t index) {
        input.assign(length, alphabet[0]);

        for (std::size_t i{0}, rest{index}; i < length; ++i, rest /= alphabet.size())
        {
            input[i] = alphabet[rest % alphabet.size()];
        }
    }};

    for (std::size_t length{window.size()}; length <= max_length; ++length)
    {
        const auto count{inputs_of_length(length)};

        for (std::size_t index{0}; index < count; ++index)
        {
            spell(length, index);

            count_occurrences(length);
        }
    }

    return {.occurrences = occurrences, .violations = violations};
}

} // namespace munch::tools::probes

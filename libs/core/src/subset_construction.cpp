#include <algorithm>
#include <array>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include "munch/core/builder.hpp"
#include "munch/core/exceptions/state_limit_error.hpp"
#include "munch/dfa/builder.hpp"

namespace
{
/**
 * @brief Determinizes an NFA by subset construction over dense bit sets.
 *
 * A reachable state set of a byte-expanded Unicode class holds thousands of members, and the construction visits
 * every set once per distinct symbol, so set membership, union, and identity must not cost a tree node each. The
 * states are projected onto dense indices once; a set is then a vector of words, a closure is a precomputed
 * member list unioned by setting bits, and a set's identity is the hash of its words. Closure is transitive, so
 * a target already present contributes nothing and its closure is skipped whole.
 */
class Determinizer
{
public:
    explicit Determinizer(const munch::nfa::Nfa& nfa, const std::size_t state_limit) : state_limit_{state_limit}
    {
        auto count{nfa.init_state() + 1};

        for (const auto& [key, targets] : nfa.transitions())
        {
            count = std::max(count, key.first + 1);

            for (const auto target : targets)
            {
                count = std::max(count, target + 1);
            }
        }

        for (const auto state : std::views::keys(nfa.accept_states()))
        {
            count = std::max(count, state + 1);
        }

        // Dense indices are 32 bits: an NFA approaching four billion states exhausts memory orders of
        // magnitude earlier, so the narrowing casts onto them cannot truncate in practice.
        words_ = (count + 63) / 64;

        init_ = static_cast<std::uint32_t>(nfa.init_state());

        moves_.resize(count);

        epsilon_.resize(count);

        closures_.resize(count);

        accepts_.resize(count);

        for (const auto& [key, targets] : nfa.transitions())
        {
            const auto& [state, label]{key};

            Members_t list{targets.cbegin(), targets.cend()};

            if (label.is_symbol())
            {
                moves_[state].push_back(
                        {.symbol = static_cast<unsigned char>(label.symbol()), .targets = std::move(list)});
            }
            else
            {
                epsilon_[state] = std::move(list);
            }
        }

        for (const auto& [state, token] : nfa.accept_states())
        {
            accepts_[state] = token;
        }
    }

    /**
     * @brief Runs the construction and returns the built DFA.
     * @throws munch::core::State_limit_error If a non-zero state limit is exceeded.
     */
    [[nodiscard]] munch::dfa::Dfa run() { return walk(false); }

    /**
     * @brief Runs the construction and returns the accepting candidates of every reachable subset instead.
     *
     * The single traversal serves both construction and diagnostics, so diagnostics see exactly the subsets the
     * build discovers by definition rather than by a mirrored reimplementation; the DFA built along the way is
     * simply not kept.
     */
    [[nodiscard]] std::vector<std::vector<munch::nfa::Token>> candidates()
    {
        walk(true);

        return std::move(candidates_);
    }

private:
    /**
     * @brief Runs the subset walk, building the DFA and, when collecting, the candidates of accepting subsets.
     */
    munch::dfa::Dfa walk(const bool collect)
    {
        munch::dfa::Builder dfa;

        Bits_t initial(words_, 0);

        for (const auto state : closure_of(init_))
        {
            insert(initial, state);
        }

        std::unordered_map<Bits_t, munch::dfa::Dfa::State_t, Words_hash> ids{{initial, dfa.init_state()}};

        std::queue<std::pair<Bits_t, munch::dfa::Dfa::State_t>> queue;

        queue.emplace(std::move(initial), dfa.init_state());

        Members_t members;

        std::vector<unsigned char> symbols;

        std::array<std::vector<const Members_t*>, 256> buckets;

        Bits_t scratch(words_, 0);

        while (!queue.empty())
        {
            const auto [bits, dfa_state]{std::move(queue.front())};

            queue.pop();

            members.clear();

            for (std::size_t word{0}; word < words_; ++word)
            {
                for (auto rest{bits[word]}; rest != 0; rest &= rest - 1)
                {
                    members.push_back(
                            static_cast<std::uint32_t>(word * 64 + static_cast<std::size_t>(std::countr_zero(rest))));
                }
            }

            if (const auto token{accept_of(members)}; token)
            {
                dfa.add_accept_state(dfa_state, munch::dfa::Token{token->id()});
            }

            if (collect)
            {
                std::vector<munch::nfa::Token> candidates;

                for (const auto member : members)
                {
                    if (accepts_[member])
                    {
                        candidates.push_back(*accepts_[member]);
                    }
                }

                if (!candidates.empty())
                {
                    candidates_.push_back(std::move(candidates));
                }
            }

            symbols.clear();

            for (const auto member : members)
            {
                for (const auto& move : moves_[member])
                {
                    if (buckets[move.symbol].empty())
                    {
                        symbols.push_back(move.symbol);
                    }

                    buckets[move.symbol].push_back(&move.targets);
                }
            }

            // Ascending symbol order keeps state discovery, and with it the DFA's numbering, deterministic.
            std::ranges::sort(symbols);

            for (const auto symbol : symbols)
            {
                std::ranges::fill(scratch, 0);

                for (const auto* targets : buckets[symbol])
                {
                    for (const auto target : *targets)
                    {
                        if (!contains(scratch, target))
                        {
                            for (const auto state : closure_of(target))
                            {
                                insert(scratch, state);
                            }
                        }
                    }
                }

                buckets[symbol].clear();

                auto iterator{ids.find(scratch)};

                // A state identifier is only allocated when the state set is new, keeping the identifiers dense.
                if (iterator == ids.cend())
                {
                    if (state_limit_ != 0 && ids.size() >= state_limit_)
                    {
                        throw munch::core::State_limit_error{state_limit_};
                    }

                    iterator = ids.emplace(scratch, dfa.next_state()).first;

                    queue.emplace(scratch, iterator->second);
                }

                dfa.add_transition(dfa_state, munch::dfa::Label{static_cast<char>(symbol)}, iterator->second);
            }
        }

        return std::move(dfa).build();
    }

    /**
     * @brief A set of NFA states as one bit per dense state index.
     */
    using Bits_t = std::vector<std::uint64_t>;

    /**
     * @brief A list of NFA states as dense indices.
     */
    using Members_t = std::vector<std::uint32_t>;

    /**
     * @brief The symbol transition of one state: the consumed symbol and the target states.
     */
    struct Move
    {
        /**
         * @brief The consumed symbol, as a table row index.
         */
        unsigned char symbol;

        /**
         * @brief The states the symbol leads to.
         */
        Members_t targets;
    };

    /**
     * @brief Hasher for a set of states, combining the hashes of its words.
     */
    struct Words_hash
    {
        std::size_t operator()(const Bits_t& bits) const noexcept
        {
            return boost::hash_range(bits.cbegin(), bits.cend());
        }
    };

    /**
     * @brief Returns whether the set holds the state.
     */
    static bool contains(const Bits_t& bits, const std::uint32_t state) noexcept
    {
        return ((bits[state / 64] >> (state % 64)) & 1U) != 0;
    }

    /**
     * @brief Inserts the state into the set.
     */
    static void insert(Bits_t& bits, const std::uint32_t state) noexcept { bits[state / 64] |= 1ULL << (state % 64); }

    /**
     * @brief The memoized epsilon closure of one state, itself included, as a dense member list.
     *
     * A closure always contains its own state, so an empty slot marks one not yet computed.
     */
    const Members_t& closure_of(const std::uint32_t state)
    {
        if (closures_[state].empty())
        {
            Members_t result{state};

            Bits_t seen(words_, 0);

            insert(seen, state);

            for (std::size_t index{0}; index < result.size(); ++index)
            {
                for (const auto next : epsilon_[result[index]])
                {
                    if (!contains(seen, next))
                    {
                        insert(seen, next);

                        result.push_back(next);
                    }
                }
            }

            closures_[state] = std::move(result);
        }

        return closures_[state];
    }

    /**
     * @brief The winning token among the accepting members, if any; the minimum resolves priority then identifier.
     */
    [[nodiscard]] std::optional<munch::nfa::Token> accept_of(const Members_t& members) const
    {
        std::optional<munch::nfa::Token> best;

        for (const auto member : members)
        {
            const auto& candidate{accepts_[member]};

            if (candidate && (!best || *candidate < *best))
            {
                best = candidate;
            }
        }

        return best;
    }

    /**
     * @brief The determinization cap; zero means unlimited.
     */
    std::size_t state_limit_;

    /**
     * @brief The number of 64-bit words a state set occupies.
     */
    std::size_t words_;

    /**
     * @brief The NFA's initial state as a dense index.
     */
    std::uint32_t init_;

    /**
     * @brief The symbol transitions of each state.
     */
    std::vector<std::vector<Move>> moves_;

    /**
     * @brief The epsilon targets of each state.
     */
    std::vector<Members_t> epsilon_;

    /**
     * @brief The memoized epsilon closures; see closure_of().
     */
    std::vector<Members_t> closures_;

    /**
     * @brief The accepting token of each state, if any.
     */
    std::vector<std::optional<munch::nfa::Token>> accepts_;

    /**
     * @brief The accepting candidates collected per subset when walking for candidates().
     */
    std::vector<std::vector<munch::nfa::Token>> candidates_;
};

} // namespace

namespace munch::core
{
dfa::Dfa Builder::subset_construction(const nfa::Nfa& nfa, const std::size_t state_limit)
{
    return Determinizer{nfa, state_limit}.run();
}

std::vector<std::vector<nfa::Token>> Builder::reachable_candidates(const nfa::Nfa& nfa, const std::size_t state_limit)
{
    return Determinizer{nfa, state_limit}.candidates();
}

} // namespace munch::core
